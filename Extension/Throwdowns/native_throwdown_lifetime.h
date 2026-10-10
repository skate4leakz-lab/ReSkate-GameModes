#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace dingosdk::multiplayer {
// Own the offline UIPlayerInfo flags from queue creation through the native
// activity's confirmed end. Destroying its registration flag at Start is not
// an end; removal of this player from this activity is.
class NativeThrowdownLifetime {
public:
    bool active() const noexcept { return flags_.load(std::memory_order_acquire) != 0; }
    bool host() const noexcept { return (flags_.load(std::memory_order_acquire) & hosting) != 0; }

    bool create(std::uint32_t player) {
        std::lock_guard lock(mutex_);
        completed_solo_ = 0;
        menu_cleanup_ = false;
        player_ = player;
        activity_ = 0;
        player_limit_ = 0;
        return flags_.exchange(hosting, std::memory_order_acq_rel) == 0;
    }
    bool join(std::uint32_t player) {
        std::lock_guard lock(mutex_);
        const auto before = flags_.load(std::memory_order_relaxed);
        if (!before) { player_ = player; activity_ = 0; player_limit_ = 0; completed_solo_ = 0; menu_cleanup_ = false; }
        flags_.store(before | joined, std::memory_order_release);
        return (before & joined) == 0;
    }
    void leave_queue() {
        std::lock_guard lock(mutex_);
        // An in-progress leave is a request. Wait for participant removal.
        if (!activity_) flags_.fetch_and(~joined, std::memory_order_acq_rel);
    }
    // Called only after ThrowdownRegistration.QueueFilled created the event
    // containing the local player. Other activity callbacks cannot bind it.
    bool activity_started(std::uintptr_t activity, std::uint32_t player) {
        std::lock_guard lock(mutex_);
        if (!active() || !activity || !player || (player_ && player_ != player) ||
            (activity_ && activity_ != activity)) return false;
        player_ = player;
        activity_ = activity;
        return true;
    }
    void player_limit(std::uint32_t limit) {
        std::lock_guard lock(mutex_);
        // Only the host's final, stock Throwdown parameters set this. A
        // temporary 1-Up picker and a guest's queue never acquire solo cleanup.
        if (host() && !activity_) player_limit_ = limit;
    }
    std::uintptr_t solo_activity(std::uint32_t player) {
        std::lock_guard lock(mutex_);
        return host() && player_limit_ == 1 && player && player == player_ ? activity_ : 0;
    }
    bool participant_left(std::uintptr_t activity, std::uint32_t player) {
        std::lock_guard lock(mutex_);
        if (!activity_ || activity != activity_ || !player || player != player_) return false;
        if (host() && player_limit_ == 1) { completed_solo_ = player_; menu_cleanup_ = true; }
        return clear_locked();
    }
    // A native client EnterEnd also proves completion. LeaveInProgress is
    // merely a request and must wait for the exact server participant removal.
    bool ended() {
        std::lock_guard lock(mutex_);
        if (activity_ && host() && player_limit_ == 1) { completed_solo_ = player_; menu_cleanup_ = true; }
        return clear_locked();
    }
    std::uint32_t take_completed_solo() {
        std::lock_guard lock(mutex_);
        const auto player = completed_solo_;
        completed_solo_ = 0;
        return player;
    }
    // The Toolbox can be created after the activity's client state cleared.
    // Keep its independent pending-queue cleanup until that model is present.
    bool menu_cleanup_pending() {
        std::lock_guard lock(mutex_);
        return menu_cleanup_ && !active();
    }
    void menu_cleaned() {
        std::lock_guard lock(mutex_);
        if (!active()) menu_cleanup_ = false;
    }
    bool clear() {
        std::lock_guard lock(mutex_);
        completed_solo_ = 0;
        menu_cleanup_ = false;
        return clear_locked();
    }
private:
    bool clear_locked() {
        player_ = 0;
        activity_ = 0;
        player_limit_ = 0;
        return flags_.exchange(0, std::memory_order_acq_rel) != 0;
    }
    static constexpr unsigned hosting = 1, joined = 2;
    std::atomic<unsigned> flags_{};
    std::mutex mutex_;
    std::uint32_t player_{};
    std::uint32_t player_limit_{};
    std::uint32_t completed_solo_{};
    bool menu_cleanup_{};
    std::uintptr_t activity_{};
};
}
