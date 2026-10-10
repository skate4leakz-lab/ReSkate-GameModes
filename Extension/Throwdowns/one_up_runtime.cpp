#include "one_up_runtime.h"
#include "one_up_native.h"
#include "native_throwdowns.h"
#include "throwdown_wire.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

namespace dingosdk::multiplayer::one_up {
namespace {
struct Seen { Message message; Time at{}; };
struct Runtime {
    std::mutex mutex;
    View published;
    std::vector<std::string> commands;
    std::optional<SpawnRequest> spawn_request;
    std::vector<std::pair<PlayerId, Message>> inbox;
    std::vector<std::vector<std::uint8_t>> outgoing;
    std::optional<Match> hosted;
    State selected;
    std::array<float, 3> spot{};
    Facing facing=default_facing;
    std::map<PlayerId, Seen> offers;
    std::map<PlayerId, InputOrder> inputs;
    std::map<std::pair<PlayerId, std::uint64_t>, std::uint64_t> sequences;
    std::map<PlayerId, std::string> names;
    ThrowdownRelayInput input;
    Time snapshot_at{}, next_send{}, ready_until{}, ready_sent_at{}, position_until{}, turn_at{};
    std::uint64_t sequence{}, wire_revision{};
    std::uint32_t remaining{};
    Token placed, clock_token;
    bool ready_pending{}, start_pending{}, positioned{};
    std::string status;
    std::atomic<bool> waiting{}, playing{}, countdown_input{}, markers_restricted{};
    std::atomic<PlayerId> active{};
};
Runtime& runtime() { static auto* r = new Runtime; return *r; }
bool member(const State& s, PlayerId id) {
    return std::any_of(s.players.begin(), s.players.end(), [id](const Player& p) { return p.id == id && p.connected; });
}
bool running(const State& s) { return s.phase != Phase::lobby && s.phase != Phase::finished && s.phase != Phase::cancelled; }
float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    float result{}; for (unsigned i = 0; i < 3; ++i) result += (a[i] - b[i]) * (a[i] - b[i]); return std::sqrt(result);
}
bool present(const Runtime& r, PlayerId id) {
    return id == r.input.local || std::any_of(r.input.peers.begin(), r.input.peers.end(), [id](const ThrowdownPeer& p) { return p.id == id; });
}
std::optional<std::array<float, 3>> position(const Runtime& r, PlayerId id) {
    if (id == r.input.local) return r.input.position;
    const auto p = std::find_if(r.input.peers.begin(), r.input.peers.end(), [id](const ThrowdownPeer& x) { return x.id == id; });
    return p == r.input.peers.end() ? std::nullopt : p->position;
}
void emit(Runtime& r, Message m) {
    if(r.input.local_only) return;
    if(r.hosted && r.hosted->solo_test()) return;
    ThrowdownMessage envelope; envelope.kind = ThrowdownMessage::Kind::one_up; envelope.leader = m.leader;
    envelope.id = static_cast<std::uint32_t>(m.match) | 1U; envelope.one_up = encode(m);
    r.outgoing.push_back(encode_throwdown(envelope));
}
void apply_input(Runtime& r, PlayerId sender, const Message& m, Time now) {
    if (!r.hosted || m.match != r.hosted->state().match || m.world != r.input.world || m.leader != r.input.local || !present(r, sender)) return;
    auto& match = *r.hosted;
    std::vector<Message> ready;
    if(r.input.local_only && sender==r.input.local) ready.push_back(m);
    else {
        auto [stream, inserted] = r.inputs.try_emplace(sender, m.action == Action::join ? m.sequence : 1);
        (void)inserted;ready=stream->second.push(m);
    }
    for (const auto& item : ready) {
        const auto at = position(r, sender);
        const Token token{item.match, item.turn, sender};
        switch (item.action) {
        case Action::join: match.join(sender); break;
        case Action::ready: if (!item.flag || (at && distance(*at, r.spot) <= 2)) match.ready(sender, item.flag); break;
        case Action::leave: match.leave(sender, now); break;
        case Action::trick_score: match.trick_scored(token,item.line,item.score,item.elapsed,now); break;
        case Action::snapshot: break;
        }
    }
}
void send_input(Runtime& r, Message m, Time now) {
    if (!r.selected.match) return;
    m.match = r.selected.match; m.world = r.selected.world; m.leader = r.selected.leader; m.sequence = ++r.sequence;
    r.sequences[{m.leader, m.match}] = r.sequence;
    if (r.hosted) apply_input(r, r.input.local, m, now); else emit(r, std::move(m));
}
void clear(Runtime& r) {
    r.hosted.reset(); r.selected = {}; r.inputs.clear(); r.sequence = r.wire_revision = 0;
    r.ready_pending = r.start_pending = false; r.placed = {}; r.clock_token = {}; r.waiting.store(false); r.playing.store(false); r.countdown_input.store(false); r.markers_restricted.store(false); r.active.store(0);
    arm_native_lines({}, false, 0); spectate_party_member(0);
}
void publish(Runtime& r, Time now) {
    View v; v.state = r.selected; v.local = r.input.local; v.world = r.input.world; v.names = r.names;
    v.remaining = r.hosted ? r.hosted->remaining(now) : (now - r.snapshot_at < r.remaining ? r.remaining - static_cast<std::uint32_t>(now - r.snapshot_at) : 0);
    v.positioning = r.selected.phase == Phase::countdown && (r.hosted ? !r.positioned : r.remaining==0);
    if (v.positioning) v.remaining = r.selected.config.countdown_ms;
    v.published = now; v.scoring_ready = native_lines_available(); v.participant = member(r.selected, r.input.local);
    v.can_create = r.input.local && r.input.world && r.input.in_world && r.input.position && !r.input.barred && v.scoring_ready && !local_throwdown_active();
    v.status = r.status; v.solo_test=r.hosted && r.hosted->solo_test();
    for (const auto& [id, offer] : r.offers)
        if (id != r.input.local && present(r, id) && now - offer.at < 4000 && offer.message.state.phase != Phase::finished && offer.message.state.phase != Phase::cancelled)
            v.offers.push_back({id, offer.message.match, v.name(id), offer.message.state.phase != Phase::lobby});
    std::lock_guard lock(r.mutex); r.published = std::move(v);
}
void command(Runtime& r, std::string_view text, Time now) {
    std::istringstream args{std::string(text)}; std::string action; args >> action;
    if (action == "leave") {
        if (r.selected.match) { Message m; m.action = Action::leave; send_input(r, m, now); }
        if (r.hosted) {
            Message m; m.match = r.selected.match; m.world = r.selected.world; m.leader = r.selected.leader;
            m.state = r.hosted->state(); m.state.revision = ++r.wire_revision; m.spot = r.spot;m.facing=r.facing; emit(r, m);
        }
        clear(r); r.status = "You left 1-Up."; return;
    }
    if (!r.input.local || !r.input.world || !r.input.in_world || !r.input.position || r.input.barred) { r.status = "Join a session and load the map before playing 1-Up."; return; }
    if (!native_lines_available()) { r.status = "Waiting for the game's scoring system."; return; }
    if (local_throwdown_active()) { r.status = "Leave your current throwdown before playing 1-Up."; return; }
    if (action == "create") {
        if (r.selected.match) { r.status = "Exit your current 1-Up match first."; return; }
        unsigned seconds = 20; if (!(args >> seconds)) seconds = 20;
        unsigned players = 6; if (!(args >> players)) players = 6;
        if (seconds < 10 || seconds > 120) { r.status = "Choose a turn from 10 to 120 seconds."; return; }
        Config config; config.turn_ms = seconds * 1000; config.max_players = players;
        // Native Throwdowns announce the activity before their turn countdown.
        // Both happen while the player is positioned on the board and locked.
        config.countdown_ms = 6000;
        if (!config.valid()) { r.status = "Choose 1 to 6 players and a turn from 10 to 120 seconds."; return; }
        std::random_device random;
        auto identity = (std::uint64_t{random()} << 32) | random(); if (!identity) identity = now | 1;
        r.hosted.emplace(identity, r.input.world, r.input.local, config); r.selected = r.hosted->state(); r.spot = *r.input.position;r.facing=default_facing;
        r.sequence = r.wire_revision = 0; r.next_send = 0; r.status = "Spot placed. Invite a nearby skater, then choose Ready.";
    } else if (action == "join") {
        PlayerId leader{}; std::uint64_t identity{}; if (!(args >> leader >> identity) || r.selected.match) return;
        const auto it = r.offers.find(leader);
        if (it == r.offers.end() || it->second.message.match != identity || now - it->second.at >= 4000) { r.status = "That match is no longer available."; return; }
        const auto& m = it->second.message;
        r.selected = m.state; r.spot = m.spot;r.facing=m.facing; r.remaining = m.remaining; r.snapshot_at = now;
        r.sequence = r.sequences[{m.leader, m.match}];
        if (r.selected.phase == Phase::lobby) { Message input; input.action = Action::join; send_input(r, input, now); r.status = "Joining the match..."; }
        else r.status = "Watching this match. Join the next lobby after the host chooses Rematch.";
    } else if (action == "enroll" && r.selected.phase == Phase::lobby && !member(r.selected, r.input.local)) {
        Message m; m.action = Action::join; send_input(r, m, now); r.status = "Joining the next match...";
    } else if (action == "ready" && r.selected.phase == Phase::lobby && member(r.selected, r.input.local)) {
        if (teleport_local_skater_transform(spawn_transform(r.spot,r.facing))) { r.ready_pending = true; r.ready_sent_at = 0; r.ready_until = now + 6000; r.status = "Moving to the starting spot..."; }
        else r.status = "Could not move to the spot. Wait for the map to finish loading.";
    } else if (action == "start" && r.hosted) {
        if(r.hosted->state().phase==Phase::lobby && r.ready_pending && !r.hosted->state().players.front().ready) {
            r.start_pending=true;r.status="Moving to the flag. Starting when ready.";return;
        }
        r.start_pending=false;
        const bool solo=(r.hosted->state().config.max_players==1 || r.input.peers.empty()) && r.hosted->state().players.size()==1;
        const bool started=solo?r.hosted->start_solo_test(now):r.hosted->start(now);
        r.status=started?(solo?"Solo test started. Get ready!":"Get ready!"):
            (solo?"Choose Ready at your flag before starting.":"At least two skaters must join and choose Ready.");
        logging::log(logging::Level::info,logging::Channel::ui,"1-Up: {}",r.status);
    } else if(action=="solo") {
        if(!r.hosted) { r.status="Place a 1-Up flag and choose Ready before starting a solo test."; return; }
        if(!r.input.peers.empty() && r.hosted->state().config.max_players!=1) { r.status="Select Players 1 to practice alone in a shared session."; return; }
        r.status=r.hosted->start_solo_test(now)?"Solo test started. Beat your previous line; three penalties end the test.":
            "Solo testing needs exactly one ready skater in a waiting 1-Up lobby.";
        logging::log(logging::Level::info,logging::Channel::ui,"1-Up: {}",r.status);
    } else if (action == "rematch" && r.hosted) {
        if (r.hosted->rematch(now)) { r.placed = {}; r.status = "Rematch opened. Everyone must choose Ready again."; }
    }
}
void create_selected_spawn(Runtime& r, const SpawnRequest& request, Time now) {
    if(r.selected.match || request.world!=r.input.world || request.local!=r.input.local ||
       !valid_facing(request.facing) || !std::all_of(request.position.begin(),request.position.end(),[](float n){return std::isfinite(n) && std::abs(n)<100000;})) {
        r.status="The selected flag is no longer available. Place it again."; return;
    }
    // Reuse all creation eligibility and configuration checks, then publish the
    // selected spawn before the first snapshot can leave this client tick.
    command(r,"create "+std::to_string(request.seconds)+" "+std::to_string(request.players),now);
    if(r.hosted) {
        r.spot=request.position;
        r.facing=request.facing;
        logging::log(logging::Level::info,logging::Channel::ui,"1-Up: flag handed to elimination rules, Players {}, turn {} seconds.",request.players,request.seconds);
        // Ready Up launches the native picker. After confirmation use the
        // existing arrival check, then wait for the host's explicit Start.
        command(r,"ready",now);
    }
}
}
std::string View::name(PlayerId id) const {
    const auto it = names.find(id); return it == names.end() || it->second.empty() ? "Skater" : it->second;
}
View view() { auto& r = runtime(); std::lock_guard lock(r.mutex); return r.published; }
void queue(std::string_view text) { auto& r = runtime(); std::lock_guard lock(r.mutex); if (text.size() <= 128 && r.commands.size() < 8) r.commands.emplace_back(text); }
void create_at_spawn(const SpawnRequest& request) { auto& r=runtime(); std::lock_guard lock(r.mutex); r.spawn_request=request; }
void receive(PlayerId sender, std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.front() != static_cast<std::uint8_t>(ThrowdownMessage::Kind::one_up)) return;
    const auto envelope = decode_throwdown(bytes); if (!envelope) return;
    const auto message = decode(envelope->one_up);
    if (!message || envelope->leader != message->leader || envelope->id != (static_cast<std::uint32_t>(message->match) | 1U)) return;
    auto& r = runtime(); if (r.inbox.size() < 256) r.inbox.emplace_back(sender, *message);
}
std::vector<std::vector<std::uint8_t>> tick(std::uintptr_t base, const ThrowdownRelayInput& input) {
    auto& r = runtime(); const Time now = GetTickCount64();
    const Token before{r.selected.match,r.selected.turn,r.selected.active};
    const auto before_phase=r.selected.phase;
    prepare_native_lines(base, input.world ? input.world : 1, input.in_world);
    if (r.selected.match && (input.world != r.selected.world || !input.local || !input.in_world || input.barred || local_throwdown_active())) {
        clear(r); r.status = "1-Up ended because the session, map, or eligibility changed.";
    }
    r.input = input;
    if(!input.local_name.empty() && input.local_name!="Unknown Player")r.names[input.local] = input.local_name;
    if(r.hosted && r.hosted->solo_test() && r.hosted->state().config.max_players!=1 && !input.peers.empty()) {
        clear(r); r.status="Solo test ended because another player joined. Create a normal 1-Up match to play together.";
    }
    for (const auto& p : input.peers)
        if(!p.name.empty() && p.name!="Unknown Player")r.names[p.id] = p.name;
    if (r.hosted) {
        auto players = r.hosted->state().players;
        for (const auto& p : players) if (p.connected && !present(r, p.id)) { r.hosted->leave(p.id, now); r.inputs.erase(p.id); }
    } else if (r.selected.match && !present(r, r.selected.leader)) {
        r.selected.phase = Phase::cancelled; r.selected.active = r.selected.winner = 0;
        r.selected.notice = "The host left. Exit and create a new match.";
    }
    for (const auto& [sender, m] : r.inbox) {
        if (!present(r, sender) || m.world != input.world) continue;
        if (m.action == Action::snapshot) {
            if (sender != m.leader) continue;
            if (r.offers.size() < 64 || r.offers.contains(sender)) {
                auto& offer = r.offers[sender];
                if (offer.message.match != m.match || m.state.revision > offer.message.state.revision) offer = {m, now};
            }
            if (!r.hosted && r.selected.match == m.match && apply_snapshot(r.selected, m, sender, input.world)) {
                r.spot = m.spot;r.facing=m.facing; r.remaining = m.remaining; r.snapshot_at = now;
            }
        } else apply_input(r, sender, m, now);
    }
    r.inbox.clear();
    std::vector<std::string> commands;
    std::optional<SpawnRequest> spawn;
    { std::lock_guard lock(r.mutex); commands = std::exchange(r.commands, {}); spawn=std::exchange(r.spawn_request,{}); }
    if(spawn) create_selected_spawn(r,*spawn,now);
    for (const auto& text : commands) command(r, text, now);
    if (r.hosted) r.selected = r.hosted->state();
    if (r.ready_pending) {
        const auto me = std::find_if(r.selected.players.begin(), r.selected.players.end(), [&](const Player& p) { return p.id == input.local; });
        if (me != r.selected.players.end() && me->ready) { r.ready_pending = false; r.status = "Ready at the spot."; }
        else if (input.position && distance(*input.position, r.spot) <= 2 && !local_skater_teleport_pending() && now - r.ready_sent_at >= 500) {
            Message m; m.action = Action::ready; m.flag = true; send_input(r, m, now); r.ready_sent_at = now;
        } else if (now >= r.ready_until) { r.ready_pending = false; r.status = "Could not reach the starting spot. Try Ready again."; }
    }
    if(r.start_pending && !r.ready_pending) {
        r.start_pending=false;
        if(r.hosted && r.hosted->state().phase==Phase::lobby) {
            const auto& players=r.hosted->state().players;
            if(std::all_of(players.begin(),players.end(),[](const auto& p){return p.ready && p.connected;}))
                command(r,"start",now);
        }
    }
    // Deferred Start can change the turn after the earlier snapshot refresh.
    // Publish that turn before positioning and arming its countdown.
    if (r.hosted) r.selected = r.hosted->state();
    const Token token{r.selected.match, r.selected.turn, r.selected.active};
    r.markers_restricted.store(r.selected.match && running(r.selected) && member(r.selected,input.local));
    // Teleport may execute synchronously before the new View is published.
    // Arm board-start and input locking before requesting the native move.
    r.countdown_input.store(r.selected.match && r.selected.phase==Phase::countdown &&
        r.selected.active==input.local && member(r.selected,input.local));
    if (r.selected.match && r.selected.phase == Phase::countdown && !(r.placed == token)) {
        r.placed = token; r.positioned = false; r.position_until = now + 8000;
        if (r.selected.active == input.local && member(r.selected, input.local)) {
            if (!teleport_local_skater_transform(spawn_transform(r.spot,r.facing))) r.status = "Starting position unavailable.";
        }
    }
    if (r.hosted && r.selected.phase == Phase::countdown && !r.positioned) {
        const auto at = position(r, r.selected.active);
        if (at && distance(*at, r.spot) <= 2 &&
            (r.selected.active != input.local || !local_skater_teleport_pending())) {
            r.positioned = true; r.hosted->position_confirmed(token, now);
        }
        else if (now >= r.position_until) r.hosted->cancel("The active skater could not reach the starting spot. Exit and try again.");
    }
    if (r.hosted && (r.selected.phase != Phase::countdown || r.positioned)) { r.hosted->tick(now); r.selected = r.hosted->state(); }
    // Feedback can advance to a new countdown during this tick. Hold its
    // presenters immediately; the next frame will queue the new token's move.
    if(r.hosted && r.selected.phase==Phase::countdown &&
       !(r.placed==Token{r.selected.match,r.selected.turn,r.selected.active}))r.positioned=false;
    const Token armed{r.selected.match, r.selected.turn, r.selected.active};
    if (r.selected.phase == Phase::playing && !(r.clock_token == armed)) {
        r.clock_token = armed;
        const auto left = r.hosted ? r.hosted->remaining(now) : r.remaining;
        r.turn_at = now - (r.selected.config.turn_ms - std::min(left, r.selected.config.turn_ms));
    }
    const bool participating = member(r.selected, input.local);
    update_native_scoring_rule(base,input.world,input.in_world && r.selected.match &&
        r.selected.phase!=Phase::finished && r.selected.phase!=Phase::cancelled && participating);
    const bool on_turn = participating && r.selected.active == input.local && (r.selected.phase == Phase::playing || r.selected.phase == Phase::settling);
    const auto left = r.hosted ? r.hosted->remaining(now) : (now - r.snapshot_at < r.remaining ? r.remaining - static_cast<std::uint32_t>(now - r.snapshot_at) : 0);
    arm_native_lines(on_turn ? armed : Token{}, on_turn && r.selected.phase == Phase::playing && left > 0, r.turn_at);
    for (const auto& event : take_native_lines()) if (on_turn && event.token == armed && event.scored_trick) {
        Message m; m.action = Action::trick_score;
        m.turn = event.token.turn; m.line = event.line; m.elapsed = event.elapsed; m.score = event.score; m.flag = event.landed;
        send_input(r, m, now);
    }
    if (r.hosted) {
        r.selected = r.hosted->state();
        if (now >= r.next_send) {
            r.next_send = now + 250;
            Message m; m.state = r.selected; m.state.revision = ++r.wire_revision;
            m.match = m.state.match; m.world = m.state.world; m.leader = m.state.leader;
            // Zero holds remote startup presenters until the host confirms arrival.
            m.remaining = r.selected.phase==Phase::countdown && !r.positioned ? 0 : r.hosted->remaining(now); m.spot = r.spot;m.facing=r.facing;
            emit(r, std::move(m));
        }
    }
    const bool showing = r.selected.match && running(r.selected);
    r.markers_restricted.store(showing && participating);
    r.playing.store(showing); r.active.store(showing ? r.selected.active : 0);
    // The active skater waits on the board during countdown. Other skaters
    // remain off board until their own turn starts.
    r.waiting.store(showing && r.selected.active != input.local);
    r.countdown_input.store(showing && participating && r.selected.active==input.local && r.selected.phase==Phase::countdown);
    if (r.selected.match) spectate_party_member(showing && r.selected.active != input.local ? r.selected.active : 0);
    std::erase_if(r.offers, [now](const auto& item) { return now - item.second.at >= 4000; });
    if(r.selected.match && (!(Token{r.selected.match,r.selected.turn,r.selected.active}==before) || r.selected.phase!=before_phase)) {
        constexpr std::array<const char*,7> phase_names{"lobby","countdown","playing","settling","feedback","finished","cancelled"};
        logging::log(logging::Level::info,logging::Channel::ui,
            "1-Up: turn {} {}, target {}, total {}. {}",r.selected.turn,
            phase_names[static_cast<unsigned>(r.selected.phase)],r.selected.target,r.selected.best,r.selected.notice);
    }
    publish(r, now); return std::exchange(r.outgoing, {});
}
bool waits_offboard() noexcept { return runtime().waiting.load(); }
bool countdown_locks_input() noexcept { return runtime().countdown_input.load(); }
bool restricts_session_markers() noexcept { return runtime().markers_restricted.load(); }
bool hides(PlayerId id) noexcept { return runtime().playing.load() && runtime().active.load() != id; }
} // namespace dingosdk::multiplayer::one_up
