#include "one_up_native.h"
#include "one_up_skating_score.h"
#include "native_type_scan.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/one_up_scoring.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>
#include <stdexcept>
#include <utility>

namespace dingosdk::multiplayer::one_up {
namespace {
namespace engine = game::build::v20260929::engine;
using Address = std::uintptr_t;
using namespace game::build::v20260929::one_up_scoring;
template<class T> T read(Address at) {
    T v{}; if (!memory::peek(at, v)) throw std::runtime_error("1-Up scoring memory unavailable"); return v;
}
struct Field { Address type{}; std::uint16_t offset{}, size{}; unsigned kind{}; };
std::optional<Field> field(Address type, std::uint32_t hash) {
    const auto record = read<Address>(type);
    const auto record_kind=(read<std::uint16_t>(record+4)>>5)&31;
    if(record_kind!=2 && record_kind!=3)return {};
    const auto count = read<std::uint16_t>(record + 0x2a);
    if (count > 96) return {};
    const auto fields = read<Address>(record + (record_kind==3?0x38:0x60));
    for (unsigned i = 0; i < count; ++i) {
        const auto at = fields + i * 24;
        if (read<std::uint32_t>(at) != hash) continue;
        Field f; f.offset = read<std::uint16_t>(at + 8); f.type = read<Address>(at + 16);
        const auto meta = read<Address>(f.type);f.kind = (read<std::uint16_t>(meta + 4) >> 5) & 31; f.size = f.kind==3?8:read<std::uint16_t>(meta + 6);
        if (f.offset + f.size > read<std::uint16_t>(record + 6)) return {};
        return f;
    }
    if(record_kind==3)if(const auto parent=read<Address>(record+0x30))return field(parent,hash);
    return {};
}
struct Binding { Address event{}, line{}; Field data, reason, score; Address sequence_event{}, sequence{}; Field sequence_data, sequence_score, success, sequence_kind, actions, current_action, category, action_score, base_score, landing; };
struct Capture { Token token; bool starts{}; Time at{}; };
struct Runtime {
    std::mutex mutex;
    std::atomic<Address> base{};
    std::atomic<std::uint64_t> world{};
    std::atomic<bool> ready{}, scanning{};
    std::atomic<bool> multiplier_disabled{};
    std::uint64_t next_scan{}, next_line{};
    unsigned end_failures{};
    std::optional<Binding> binding;
    Capture armed, running;
    std::uint64_t line{};
    std::vector<NativeLine> events;
};
Runtime& runtime() { static auto* r = new Runtime; return *r; }
// These handles belong to the scoring world, not to a temporary menu page.
// Only the client tick accesses this state, under the native model lock.
struct ScoringRule {
    Address manager{};
    std::uint64_t world{}, retry{}, report{};
    std::map<menu_data::Handle, std::pair<menu_data::Value,bool>> originals;
};
ScoringRule& scoring_rule() { static ScoringRule rule; return rule; }
void restore_scoring_rule(const menu_data::Context& c, ScoringRule& rule) {
    for (const auto& [id, saved] : rule.originals) {
        // The engine retires scoring models during world teardown. Never write
        // a snapshot into a reused handle or require old embedded storage.
        if (c.type_of(id) != saved.first.type || !game::native_data().models.value(c.manager,id,0,1)) continue;
        if (menu_data::read<bool>(c.address(saved.first))) c.set(saved.first,saved.second);
    }
    rule.originals.clear();
}
bool client(Address base) {
    Address context{};
    reinterpret_cast<Address (*)(Address*)>(base + engine::current_context)(&context);
    const auto offset = read<std::uint32_t>(base + engine::context_type_offset);
    return context && offset < 0x1000000 && read<std::uint32_t>(context + offset) == 0xbf0f9789;
}
bool contract(Address resource, std::uint32_t hash) {
    const auto frame = read<std::uint32_t>(resource + 0x20);
    const auto constants = read<std::uint32_t>(resource + 0x24);
    const auto code = read<std::uint32_t>(resource + 0x2c);
    return graph_contract(hash, frame, constants, code);
}
}
void prepare_native_lines(Address base, std::uint64_t world, bool in_world) noexcept {
    auto& r = runtime(); r.base.store(base);
    const auto wanted = in_world ? world : 0;
    if (r.world.exchange(wanted) != wanted) {
        r.ready.store(false); std::lock_guard lock(r.mutex); r.binding.reset(); r.running = {}; r.armed = {}; r.line = 0; r.events.clear(); r.next_scan = 0;
    }
    if (!wanted || r.ready.load() || GetTickCount64() < r.next_scan || r.scanning.exchange(true)) return;
    r.next_scan = GetTickCount64() + 2000;
    try {
        std::thread([wanted] {
            auto& s = runtime();
            try {
                std::array<NativeTypeQuery, 4> queries{{{end_type, 0, 2}, {line_type, 0, 2}, {sequence_end_type,0,2}, {sequence_type,0,2}}};
                if (!find_native_types(queries, true)) {
                    const auto data = field(queries[0].object, line_data_field), reason = field(queries[0].object, reason_field), score = field(queries[1].object, score_field);
                    const auto sequence_data=field(queries[2].object,sequence_data_field), success=field(queries[2].object,success_field), sequence_score=field(queries[3].object,sequence_score_field);
                    const auto sequence_kind=field(queries[3].object,2702987617U),actions=field(queries[3].object,382266934U),current_action=field(queries[3].object,333924837U);
                    const auto category=current_action?field(current_action->type,0x26e74827):std::nullopt;
                    const auto action_score=field(queries[3].object,3836815661U),base_score=field(queries[3].object,3691294820U),landing=field(queries[3].object,2088787037U);
                    if (data && reason && score && data->type == queries[1].object && reason->size == 4 && score->size == 4 &&
                        (reason->kind == 8 || reason->kind == 15 || reason->kind == 16) && (score->kind == 19 || score->kind == 15) &&
                        sequence_data && success && sequence_score && sequence_data->type==queries[3].object && success->size==1 &&
                        sequence_score->size==4 && (sequence_score->kind==15 || sequence_score->kind==16) &&
                        sequence_kind && sequence_kind->kind==8 && actions && actions->kind==4 && current_action && current_action->kind==3 &&
                        category && category->kind==8 && category->size==4 && action_score && base_score && landing && landing->kind==19) {
                        std::lock_guard lock(s.mutex);
                        if (s.world.load() == wanted) {
                            s.binding = Binding{queries[0].object, queries[1].object, *data, *reason, *score,queries[2].object,queries[3].object,*sequence_data,*sequence_score,*success,*sequence_kind,*actions,*current_action,*category,*action_score,*base_score,*landing}; s.ready.store(true);
                            logging::log(logging::Level::info, logging::Channel::progression,
                                "1-Up: native finalized line scoring bound (LineData +{}, LineEndReason +{}, LineScore +{}, kind {}).",
                                data->offset, reason->offset, score->offset, score->kind);
                            logging::log(logging::Level::info,logging::Channel::progression,"1-Up: native landed trick scoring bound (SequenceData +{}, SequenceScore +{}, WasSuccess +{}).",sequence_data->offset,sequence_score->offset,success->offset);
                        }
                    }
                }
            } catch (...) {}
            s.scanning.store(false);
        }).detach();
    } catch (...) { r.scanning.store(false); }
}
bool native_lines_available() noexcept { return runtime().ready.load(); }
void update_native_scoring_rule(Address base, std::uint64_t world, bool active) noexcept {
    auto& rule=scoring_rule(); auto& r=runtime();
    try {
        const auto ui=read<Address>(base+engine::ui_manager);
        const auto manager=ui?read<Address>(ui+0x140):0;
        if (!manager) { r.multiplier_disabled.store(false); return; }
        const menu_data::Context c(base,manager); game::ModelWriteLock lock(manager);
        if (rule.manager && rule.manager!=manager) rule.originals.clear();
        rule.manager=manager;
        if (rule.world!=world || !active) {
            restore_scoring_rule(c,rule); rule.world=world;
            r.multiplier_disabled.store(false); rule.retry=0;
            if (!active) return;
        }
        // Reassert the native rule if another native owner republishes settings.
        // Do not divide an already-rounded trick score or change line timers.
        unsigned bound{};
        for (const auto& [id,saved] : rule.originals) {
            if (c.type_of(id)!=saved.first.type || !game::native_data().models.value(manager,id,0,1)) continue;
            if (!menu_data::read<bool>(c.address(saved.first))) c.set(saved.first,true);
            ++bound;
        }
        const auto now=GetTickCount64();
        if (!bound || now>=rule.retry) {
            rule.retry=now+2000;
            for (const auto& root:c.roots({active_scoring_type})) {
                menu_data::require(menu_data::size(root.model.type)==active_scoring_size,"1-Up scoring model schema differs.");
                const auto settings=c.field(root.model,settings_field);
                menu_data::require(menu_data::size(settings.type)==settings_size,"1-Up scoring settings schema differs.");
                for(const auto setting:{disable_lines_field,0xce4a0fc8U}) {
                const auto disabled=c.field(settings,setting);
                menu_data::require(menu_data::kind(disabled.type)==10 && menu_data::size(disabled.type)==1,"1-Up multiplier flag schema differs.");
                if (!rule.originals.contains(disabled.handle))
                    rule.originals.emplace(disabled.handle,std::pair{disabled,menu_data::read<bool>(c.address(disabled))});
                if (!menu_data::read<bool>(c.address(disabled))) c.set(disabled,true);
                ++bound;
                }
            }
        }
        const bool was=r.multiplier_disabled.exchange(bound!=0);
        if (!was && bound) logging::log(logging::Level::info,logging::Channel::progression,
            "1-Up: native line and stale multipliers disabled; skating sequence points are used directly.");
        if (!bound && now>=rule.report) { rule.report=now+5000; logging::log(logging::Level::warning,logging::Channel::progression,
            "1-Up: waiting for native multiplier settings; score capture paused."); }
    } catch (const std::exception& e) {
        r.multiplier_disabled.store(false);
        const auto now=GetTickCount64();
        if(now>=rule.report) { rule.report=now+5000; logging::log(logging::Level::warning,logging::Channel::progression,"1-Up scoring rule: {}",e.what()); }
    }
}
void arm_native_lines(Token token, bool starts, Time at) noexcept {
    auto& r = runtime(); std::lock_guard lock(r.mutex); r.armed = {token, starts, at};
    if (!(r.running.token == token)) { r.running = {}; r.line = 0; }
}
void observe_native_line(Address vm) noexcept {
    auto& r = runtime();
    if (!vm || !r.ready.load()) return;
    Address resource{}; std::uint32_t graph{};
    if (!memory::peek(vm + 0x38, resource) || !memory::peek(resource + 0x10, graph) || (graph != start_graph && graph != end_graph && graph != stats_end_graph && graph != sequence_end_graph)) return;
    try {
        if (!client(r.base.load()) || !contract(resource, graph)) return;
        std::lock_guard lock(r.mutex);
        if (!r.binding || read<std::uint32_t>(read<Address>(r.binding->event)) != end_type || read<std::uint32_t>(read<Address>(r.binding->line)) != line_type) return;
        const auto now = GetTickCount64();
        if(graph==sequence_end_graph) {
            const auto instance=read<Address>(vm+0x30);
            if(read<Address>(instance)!=resource)return;
            const auto page=read<Address>(instance+0x470+8);
            const auto payload=read<Address>(page+1080);
            const auto& b=*r.binding;
            if(read<std::uint32_t>(read<Address>(b.sequence_event))!=sequence_end_type ||
               read<std::uint32_t>(read<Address>(b.sequence))!=sequence_type)return;
            const bool landed=read<bool>(payload+b.success.offset);
            const auto sequence=payload+b.sequence_data.offset;
            const auto sequence_kind=read<std::uint32_t>(sequence+b.sequence_kind.offset);
            const auto action_category=[&](Address action) {
                return action?read<std::uint32_t>(action+b.category.offset):UINT32_MAX;
            };
            const auto current_category=action_category(read<Address>(sequence+b.current_action.offset));
            bool real_trick=skating_trick_category(current_category);
            const auto actions=read<Address>(sequence+b.actions.offset);
            const auto count=actions?read<std::uint32_t>(actions-4)&0x7fffffffU:0;
            if(count>64)return;
            for(unsigned i=0;i<count;++i)real_trick|=skating_trick_category(action_category(read<Address>(actions+i*8)));
            const bool eligible=skating_sequence(sequence_kind,real_trick);
            const double score=b.sequence_score.kind==16 ? static_cast<double>(read<std::uint32_t>(payload+b.sequence_data.offset+b.sequence_score.offset)) :
                static_cast<double>(read<std::int32_t>(payload+b.sequence_data.offset+b.sequence_score.offset));
            if(score<0 || score>2147483647.0)return;
            logging::log(logging::Level::debug,logging::Channel::progression,
                "1-Up native trick scored: points {}, action {}, base {}, landing {}, sequence {}, category {}, actions {}, skating {}, landed {}, armed {}.",
                score,read<std::uint32_t>(sequence+b.action_score.offset),read<std::uint32_t>(sequence+b.base_score.offset),read<float>(sequence+b.landing.offset),
                sequence_kind,current_category,count,eligible,landed,r.armed.starts);
            // DisableLineMultiplierRule makes this the landed trick's base
            // points. Preserve prior landed tricks after a later bail and do
            // not wait for the line or multiplier UI to finish.
            if(eligible && landed && score>0 && r.multiplier_disabled.load() && r.armed.token.match && r.armed.starts && now>=r.armed.at && now-r.armed.at<120000 && r.events.size()<128)
                r.events.push_back({r.armed.token,++r.next_line,score,static_cast<std::uint32_t>(now-r.armed.at),true,true,true});
            return;
        }
        if (graph == start_graph) {
            logging::log(logging::Level::debug, logging::Channel::progression, "1-Up native line start: graph {:#x}.", graph);
            if (r.line) return;
            r.running = {}; r.line = 0;
            if (r.armed.token.match && r.armed.starts && now >= r.armed.at && now - r.armed.at <= 120000) {
                r.running = r.armed; r.line = ++r.next_line;
                if (r.events.size() < 128) r.events.push_back({r.running.token, r.line, 0, static_cast<std::uint32_t>(now - r.running.at), false, false});
            }
            return;
        }
        // Interpreter pages: its existing throwdown pump reads page 2 at instance + aligned
        // frame + 16. Page 1 is the adjacent pointer at +8. Value is a borrowed payload
        // reference at offset 40 in telemetry, or offset 16 in the stats listener.
        const auto instance = read<Address>(vm + 0x30);
        if (read<Address>(instance) != resource) return;
        const auto frame = graph == end_graph ? 0x70u : 0x50u;
        const auto input = graph == end_graph ? 40u : 16u;
        const auto page = read<Address>(instance + frame + 8);
        const auto payload = read<Address>(page + input);
        const auto& b = *r.binding;
        const auto reason = read<std::uint32_t>(payload + b.reason.offset);
        const auto score = b.score.kind == 19 ? static_cast<double>(read<float>(payload + b.data.offset + b.score.offset))
                                            : static_cast<double>(read<std::int32_t>(payload + b.data.offset + b.score.offset));
        if (reason > 8 || !std::isfinite(score) || score < 0 || score > 2147483647.0) {
            if (r.end_failures++ < 8) logging::log(logging::Level::debug, logging::Channel::progression,
                "1-Up native end payload rejected: graph {:#x}, reason {}, score {}.", graph, reason, score);
            return;
        }
        // Only natural timer/stall finalization is eligible. Teleports,
        // activity/editor transitions cannot bank a 1-Up line; only the native line timer or
        // stall timeout qualifies. LineScore is copied once, without multiplying it again.
        const bool landed = reason == 1 || reason == 6;
        logging::log(logging::Level::debug, logging::Channel::progression, "1-Up native line finalized: reason {}, score {}, eligible {}.", reason, score, landed);
        if (r.line && r.running.token == r.armed.token && now >= r.running.at && now - r.running.at <= 125000 && r.events.size() < 128)
            r.events.push_back({r.running.token, r.line, score, static_cast<std::uint32_t>(now - r.running.at), true, landed});
        r.running = {}; r.line = 0;
    } catch (...) {
        if (graph != start_graph) {
            std::lock_guard lock(r.mutex);
            if (r.end_failures++ < 8) logging::log(logging::Level::debug, logging::Channel::progression,
                "1-Up native end payload unavailable: graph {:#x}.", graph);
        }
    }
}
std::vector<NativeLine> take_native_lines() { auto& r = runtime(); std::lock_guard lock(r.mutex); return std::exchange(r.events, {}); }
} // namespace dingosdk::multiplayer::one_up
