#include "Extension/Throwdowns/native_throwdown_lifetime.h"
#include "Extension/Throwdowns/one_up_input_contract.h"
#include <cstdio>
#include <stdexcept>

using dingosdk::multiplayer::NativeThrowdownLifetime;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void solo_skate_leave() {
    // Captured native sequence: local ID 2, server activity 0xb4b5f240;
    // the client mirrors it at a different address, 0xaa64ec40. Registration
    // teardown happens at Start; neither client EnterEnd graph ran at Leave.
    NativeThrowdownLifetime lifetime;
    check(lifetime.create(2), "Creating a solo queue must acquire ownership");
    check(lifetime.active() && lifetime.host(), "Waiting host must own the UI lock");
    check(lifetime.activity_started(0xb4b5f240, 2), "QueueFilled must bind the real native activity");
    // Destroying the registration doesn't call clear: it is still playing.
    check(lifetime.active(), "Removing the Start flag must retain activity ownership");
    check(!lifetime.participant_left(0xb4b5f240, 3), "Another player's removal must not unlock the local challenge");
    check(!lifetime.participant_left(0xaa64ec40, 2), "A different activity address must not clear server ownership");
    check(!lifetime.participant_left(0x999, 2), "A cooperative challenge must not unlock a running Throwdown");
    check(lifetime.participant_left(0xb4b5f240, 2), "Confirmed local removal must unlock even without EnterEnd");
    check(!lifetime.active() && !lifetime.host(), "Both UI flags must clear together after Leave");
    check(!lifetime.participant_left(0xb4b5f240, 2), "Duplicate confirmation must be inert");
    check(lifetime.create(2), "The next challenge must be available after a solo leave");
    check(lifetime.activity_started(0xabc, 2), "The next challenge must have its own activity");
    check(!lifetime.participant_left(0xb4b5f240, 2) && lifetime.active(),
        "A late confirmation from the previous challenge must not unlock the new one");
    check(lifetime.participant_left(0xabc, 2), "The second challenge must also clean up");
}

void joined_and_teardown() {
    NativeThrowdownLifetime lifetime;
    check(lifetime.join(2) && lifetime.active() && !lifetime.host(), "Joining must track a participant, not a host");
    lifetime.leave_queue();
    check(!lifetime.active(), "Leaving a joined waiting queue must release its flag");
    lifetime.join(2);
    check(!lifetime.activity_started(0x123, 3), "QueueFilled for a foreign player must not acquire the activity");
    check(lifetime.activity_started(0x123, 2), "Joined queue must bind its activity at start");
    lifetime.leave_queue();
    check(lifetime.active(), "In-progress leave requests must wait for native confirmation");
    check(lifetime.participant_left(0x123, 2) && !lifetime.active(), "Joined activity exit must release its lock");
    lifetime.create(0); // Identity can become available after queue creation.
    check(lifetime.activity_started(0x456, 2), "Start must resolve an initially unavailable native identity");
    check(!lifetime.activity_started(0x789, 2), "An unrelated event must not replace the bound activity");
    check(lifetime.clear() && !lifetime.active(), "Leaving the map must clear flags and activity addresses");
    check(!lifetime.clear(), "Repeated level teardown must be inert");
    check(!lifetime.activity_started(0x456, 2), "Expired callbacks must not recreate ownership after teardown");
    lifetime.create(2);
    check(lifetime.clear() && !lifetime.host(), "1-Up's temporary flag queue must still release ownership");
}

void solo_event_cleanup_scope() {
    NativeThrowdownLifetime lifetime;
    lifetime.create(2);
    lifetime.player_limit(1);
    check(!lifetime.solo_activity(2), "Waiting queues must not destroy an activity");
    lifetime.activity_started(0x123, 2);
    check(!lifetime.solo_activity(0) && !lifetime.solo_activity(3),
        "An unknown or different player must not destroy the solo host's activity");
    check(lifetime.solo_activity(2) == 0x123, "The one-player host must destroy its exact running event on exit");
    lifetime.player_limit(6);
    check(lifetime.solo_activity(2) == 0x123, "Late parameters must not replace a running event's player limit");
    lifetime.participant_left(0x123, 2);
    check(!lifetime.solo_activity(2), "Confirmed exit must retire the solo cleanup target");

    for (const auto limit : {2u, 6u, 10u}) {
        lifetime.create(2);
        lifetime.player_limit(limit);
        lifetime.activity_started(0x456, 2);
        check(!lifetime.solo_activity(2), "Multiplayer-sized events must retain the normal participant leave path");
    }
    lifetime.create(2);
    lifetime.activity_started(0x789, 2);
    check(!lifetime.solo_activity(2), "A new queue must not inherit a previous one-player limit");
    lifetime.clear();
    lifetime.join(2);
    lifetime.player_limit(1);
    lifetime.activity_started(0xabc, 2);
    check(!lifetime.solo_activity(2), "A joined guest must never destroy the host's event");
    lifetime.clear();
    lifetime.create(0);
    lifetime.player_limit(1);
    lifetime.activity_started(0xdef, 2);
    check(lifetime.solo_activity(2) == 0xdef, "Native identity resolved at start must still bind solo cleanup");
    check(!lifetime.participant_left(0x123, 2) && lifetime.solo_activity(2) == 0xdef,
        "A stale leave must not retire the next solo activity");
    lifetime.clear();
    lifetime.create(2); // The temporary 1-Up picker never sets a stock host limit.
    lifetime.activity_started(0xfff, 2);
    check(!lifetime.solo_activity(2), "1-Up's temporary native picker must not acquire stock solo cleanup");
}
void confirmed_solo_reset() {
    NativeThrowdownLifetime lifetime;
    lifetime.create(2);lifetime.player_limit(1);lifetime.activity_started(0x123,2);
    lifetime.leave_queue();
    check(!lifetime.take_completed_solo(),"A quit request cannot reset client activity before confirmation");
    check(!lifetime.participant_left(0x456,2) && !lifetime.take_completed_solo(),
        "A stale activity cannot arm client cleanup");
    lifetime.participant_left(0x123,2);
    check(lifetime.take_completed_solo()==2,"Confirmed solo removal must also release the native client state");
    check(!lifetime.take_completed_solo(),"Native client cleanup must be consumed exactly once");
    for(const auto limit:{1u,2u,6u}) {
        lifetime.create(2);lifetime.player_limit(limit);lifetime.activity_started(0x789,2);
        lifetime.ended();
        check(lifetime.take_completed_solo()==(limit==1?2u:0u),
            "Only an ended solo host may reset the native client activity");
    }
    lifetime.create(2);lifetime.player_limit(1);lifetime.activity_started(0x123,2);lifetime.ended();
    lifetime.create(2);
    check(!lifetime.take_completed_solo(),"Starting a new queue must discard the previous activity's reset");
    lifetime.player_limit(1);lifetime.activity_started(0x456,2);lifetime.ended();lifetime.clear();
    check(!lifetime.take_completed_solo(),"Level teardown must discard borrowed player cleanup");
    lifetime.join(2);lifetime.player_limit(1);lifetime.activity_started(0x789,2);lifetime.ended();
    check(!lifetime.take_completed_solo(),"A guest ending must not reset host-owned native state");
}
void native_marker_entries() {
    using dingosdk::multiplayer::one_up::marker_action;
    const std::array<std::uint32_t,10> return_layout{208,7512,5956,5915,24,0,100,1638716,9437220,17042434};
    check(marker_action(0x177dbf1c,return_layout,0),"The real marker Return action must be denied at entry");
    check(!marker_action(0x177dbf1c,return_layout,16),"A resumed interpreter must not be abandoned");
    auto changed=return_layout;changed[2]++;
    check(!marker_action(0x177dbf1c,changed,0),"An unsupported graph layout must remain native");
    check(!marker_action(0x6998ec4b,return_layout,0),"Quit Match must not be confused with a marker action");
    check(marker_action(0x12daba2a,{128,3792,2978,2958,13,0,50,1638558,7340060,17042434},0),
        "The real marker Set action must be denied at entry");
    check(marker_action(0x0d6708f4,{208,7528,5957,5920,23,0,100,1638717,9437220,17042434},0),
        "Marker Undo must not bypass the return restriction");
}
void deferred_toolbox_cleanup() {
    NativeThrowdownLifetime lifetime;
    lifetime.create(2);lifetime.player_limit(1);lifetime.activity_started(0x123,2);
    lifetime.leave_queue();
    check(!lifetime.menu_cleanup_pending(),"A quit request must not unlock the wheel before confirmation");
    check(!lifetime.participant_left(0x456,2) && !lifetime.menu_cleanup_pending(),
        "A foreign activity cannot change the wheel's pending queue");
    lifetime.participant_left(0x123,2);
    check(lifetime.take_completed_solo()==2,"Client activity cleanup must still be delivered");
    check(lifetime.menu_cleanup_pending(),"Opening the wheel later must retain the separate UI cleanup");
    check(!lifetime.take_completed_solo() && lifetime.menu_cleanup_pending(),
        "Consuming the client cleanup must not consume an unavailable wheel model");
    lifetime.menu_cleaned();
    check(!lifetime.menu_cleanup_pending(),"Published wheel cleanup must be consumed once");
    lifetime.create(2);lifetime.player_limit(1);lifetime.activity_started(0x789,2);lifetime.ended();
    lifetime.create(2);
    check(!lifetime.menu_cleanup_pending(),"A new queue must never inherit a previous wheel unlock");
    lifetime.player_limit(1);lifetime.activity_started(0xabc,2);lifetime.ended();lifetime.clear();
    check(!lifetime.menu_cleanup_pending(),"A level change must discard old UI cleanup");
    for(const auto limit:{2u,6u}) {
        lifetime.create(2);lifetime.player_limit(limit);lifetime.activity_started(0xdef,2);lifetime.ended();
        check(!lifetime.menu_cleanup_pending(),"A multiplayer event must retain its native wheel behavior");
    }
}
}
int main() {
    try {
        solo_skate_leave();
        joined_and_teardown();
        solo_event_cleanup_scope();
        confirmed_solo_reset();
        native_marker_entries();
        deferred_toolbox_cleanup();
        std::puts("Native Throwdown ownership regression checks passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
