#include "overlay_internal.h"
#include "input_capture.h"
#include "Engine/Core/Log/logging.h"

// skate. reads a Steam-driven pad (a DualSense with Steam Input on) through Steam's controller
// interfaces, "SteamInput006" and the older "SteamController008": Steam turns the D-pad into the
// game's own actions (return to marker, quick drop, the replay editor...) and the sticks into
// analog actions, in its own process, so nothing in the game's raw controller reads carries them.
// The interfaces are one object each per Steam user, so asking steam_api for them finds the
// game's own, and their action reads are patched in the vtable (slots read off steam_api64's flat
// SteamAPI_ISteamInput_* / SteamAPI_ISteamController_* exports for the supported build):
//  - while game modes keep the D-pad (hide_game_buttons) and a D-pad direction is held, every
//    digital action reads "not pressed";
//  - while game modes keep the sticks too (pause_game_input: placing from the free camera, which
//    flies on them), every digital action reads "not pressed" and every analog action reads zero,
//    so the skater stands still while the camera flies.

namespace dingosdk::overlay::detail {
namespace {
struct DigitalActionData {
    bool state{}, active{};
};
struct AnalogActionData {
    std::int32_t mode{};
    float x{}, y{};
    bool active{};
};
// Members returning a struct: `this`, the hidden return slot, then the arguments.
using GetDigital = DigitalActionData *(*)(void *self, DigitalActionData *out, std::uint64_t controller, std::uint64_t action);
using GetAnalog = AnalogActionData *(*)(void *self, AnalogActionData *out, std::uint64_t controller, std::uint64_t action);

struct Interface {
    const char *name;
    std::size_t digital_slot, analog_slot; // byte offsets in the vtable
    std::atomic<GetDigital> digital{};
    std::atomic<GetAnalog> analog{};
    std::atomic<bool> installed{};
    std::atomic<std::uint64_t> reads{}, blocked{};
};
Interface interfaces[]{{"SteamInput006", 0x88, 0xa8}, {"SteamController008", 0x60, 0x78}};
std::atomic<std::uint16_t> held_buttons{};

bool block_digital() {
    if (overlay_input_access) return false;
    if (game_input_paused.load(std::memory_order_relaxed)) return true;
    const auto hidden = hidden_game_buttons.load(std::memory_order_relaxed) & 0x000f;
    return hidden && (held_buttons.load(std::memory_order_relaxed) & hidden);
}
template <std::size_t I>
DigitalActionData *patched_digital(void *self, DigitalActionData *out, std::uint64_t controller, std::uint64_t action) {
    auto &face = interfaces[I];
    auto *result = face.digital.load(std::memory_order_acquire)(self, out, controller, action);
    face.reads.fetch_add(1, std::memory_order_relaxed);
    if (result && block_digital()) {
        if (result->state) face.blocked.fetch_add(1, std::memory_order_relaxed);
        result->state = false;
    }
    return result;
}
template <std::size_t I>
AnalogActionData *patched_analog(void *self, AnalogActionData *out, std::uint64_t controller, std::uint64_t action) {
    auto *result = interfaces[I].analog.load(std::memory_order_acquire)(self, out, controller, action);
    if (result && !overlay_input_access && game_input_paused.load(std::memory_order_relaxed)) result->x = result->y = 0;
    return result;
}

bool patch_slot(void **slot, void *detour) {
    DWORD protection{};
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    *slot = detour;
    VirtualProtect(slot, sizeof(void *), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}
template <std::size_t I>
void install_interface(void *(*find)(int, const char *), int user) {
    auto &face = interfaces[I];
    if (face.installed.load()) return;
    auto *object = find(user, face.name);
    if (!object) return;
    auto **vtable = *reinterpret_cast<void ***>(object);
    auto **digital = vtable + face.digital_slot / sizeof(void *), **analog = vtable + face.analog_slot / sizeof(void *);
    // Already ours (two interfaces sharing one implementation): nothing to wrap again.
    if (*digital == reinterpret_cast<void *>(&patched_digital<0>) || *digital == reinterpret_cast<void *>(&patched_digital<1>)) return;
    face.digital.store(reinterpret_cast<GetDigital>(*digital), std::memory_order_release);
    face.analog.store(reinterpret_cast<GetAnalog>(*analog), std::memory_order_release);
    if (!patch_slot(digital, reinterpret_cast<void *>(&patched_digital<I>))) return;
    (void)patch_slot(analog, reinterpret_cast<void *>(&patched_analog<I>));
    face.installed.store(true);
    logging::printf(logging::Level::info, logging::Channel::input, "Steam controller block installed on %s: game modes can keep the D-pad and sticks.", face.name);
}

// Steam's overlay (gameoverlayrenderer64) hooks xinput1_4 after ReSkate does and, with Steam Input
// on, writes its virtual pad into the result after ReSkate's mask ran: the game then sees the
// DualSense's D-pad anyway. While game modes keep the D-pad, each XInput export Steam took over
// (a plain `jmp rel32` into Steam) is pointed at a stub near it that jumps here, and here calls on
// into Steam and masks what comes back. Done once per export, so Steam's own trampoline (which
// may hold a copy of our jump) can never loop back here.
struct TopHook {
    std::atomic<void *> next{};
    bool done{};
};
std::array<TopHook, 3> tops; // XInputGetState, #100 (XInputGetStateEx), XInputGetKeystroke
std::uint8_t *top_stubs{};

void mask_state(XINPUT_STATE &state) {
    if (game_input_paused.load(std::memory_order_relaxed)) {
        state.Gamepad = {};
        return;
    }
    state.Gamepad.wButtons &= static_cast<WORD>(~hidden_game_buttons.load(std::memory_order_relaxed));
}
DWORD WINAPI top_state(DWORD user, XINPUT_STATE *output) {
    const auto result = reinterpret_cast<DWORD(WINAPI *)(DWORD, XINPUT_STATE *)>(tops[0].next.load())(user, output);
    if (result == ERROR_SUCCESS && output && !overlay_input_access) mask_state(*output);
    return result;
}
DWORD WINAPI top_state_ex(DWORD user, XINPUT_STATE *output) {
    const auto result = reinterpret_cast<DWORD(WINAPI *)(DWORD, XINPUT_STATE *)>(tops[1].next.load())(user, output);
    if (result == ERROR_SUCCESS && output && !overlay_input_access) mask_state(*output);
    return result;
}
DWORD WINAPI top_keystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE output) {
    const auto result = reinterpret_cast<DWORD(WINAPI *)(DWORD, DWORD, PXINPUT_KEYSTROKE)>(tops[2].next.load())(user, reserved, output);
    if (result == ERROR_SUCCESS && output && !overlay_input_access) {
        const auto hidden = hidden_game_buttons.load(std::memory_order_relaxed);
        const bool dpad = output->VirtualKey >= VK_PAD_DPAD_UP && output->VirtualKey <= VK_PAD_DPAD_RIGHT;
        if (dpad && (hidden & (1u << (output->VirtualKey - VK_PAD_DPAD_UP)))) {
            *output = {};
            return ERROR_EMPTY;
        }
    }
    return result;
}

// A page within a jump's reach (±2 GB) of the target.
std::uint8_t *allocate_near(const void *target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t step = info.dwAllocationGranularity, at = reinterpret_cast<std::uintptr_t>(target) & ~(step - 1);
    for (std::uintptr_t offset = step; offset < 0x7ff00000; offset += step)
        for (const auto candidate : {at - offset, at + offset})
            if (auto *page = VirtualAlloc(reinterpret_cast<void *>(candidate), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<std::uint8_t *>(page);
    return nullptr;
}

void put_on_top(std::size_t index, void *exported, void *detour, const char *name) {
    auto &top = tops[index];
    if (top.done || !exported) return;
    auto *code = static_cast<std::uint8_t *>(exported);
    if (code[0] != 0xE9) return; // not taken over by a jump: ReSkate's own hook is still outermost
    std::int32_t relative{};
    std::memcpy(&relative, code + 1, 4);
    auto *destination = code + 5 + relative;
    if (!top_stubs) top_stubs = allocate_near(exported);
    if (!top_stubs) return;
    auto *stub = top_stubs + index * 16;
    if (destination == stub) return;
    const auto reach = stub - (code + 5);
    if (reach > INT32_MAX || reach < INT32_MIN) return;
    // jmp [rip+0] ; the detour's address
    const std::uint8_t jump[]{0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(stub, jump, sizeof(jump));
    std::memcpy(stub + 6, &detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    top.next.store(destination);
    top.done = true;
    DWORD protection{};
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &protection)) return;
    const auto to_stub = static_cast<std::int32_t>(reach);
    std::memcpy(code + 1, &to_stub, 4);
    VirtualProtect(code, 5, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    logging::printf(logging::Level::info, logging::Channel::input, "XInput: put the D-pad mask back on top of %s (it had been hooked over).", name);
}

void stay_on_top_of_xinput() noexcept {
    try {
        const auto module = GetModuleHandleW(L"xinput1_4.dll");
        if (!module) return;
        put_on_top(0, reinterpret_cast<void *>(GetProcAddress(module, "XInputGetState")), reinterpret_cast<void *>(&top_state), "XInputGetState");
        put_on_top(1, reinterpret_cast<void *>(GetProcAddress(module, MAKEINTRESOURCEA(100))), reinterpret_cast<void *>(&top_state_ex), "XInputGetStateEx");
        put_on_top(2, reinterpret_cast<void *>(GetProcAddress(module, "XInputGetKeystroke")), reinterpret_cast<void *>(&top_keystroke), "XInputGetKeystroke");
    } catch (...) {}
}

// Steam's interfaces exist once the game has started Steam: tried every few seconds until both are in.
void install_steam_blocks(std::uint64_t now) noexcept {
    static std::uint64_t next_try = 0;
    if ((interfaces[0].installed.load() && interfaces[1].installed.load()) || now < next_try) return;
    next_try = now + 3000;
    try {
        const auto api = GetModuleHandleW(L"steam_api64.dll");
        if (!api) return;
        using GetUser = int (*)();
        using FindInterface = void *(*)(int, const char *);
        const auto user = reinterpret_cast<GetUser>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
        const auto find = reinterpret_cast<FindInterface>(GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface"));
        if (!user || !find || !user()) return;
        install_interface<0>(find, user());
        install_interface<1>(find, user());
    } catch (...) {}
}
} // namespace
} // namespace dingosdk::overlay::detail

namespace dingosdk::overlay {
void hold_game_buttons(std::uint16_t held) noexcept {
    using namespace detail;
    held_buttons.store(held, std::memory_order_relaxed);
    const auto now = GetTickCount64();
    install_steam_blocks(now);
    const bool keeping = hidden_game_buttons.load(std::memory_order_relaxed) != 0;
    // Before the first press reaches the game: as soon as game modes keep the D-pad.
    if (keeping) stay_on_top_of_xinput();
    // What the game asked Steam for while game modes kept the pad, for the log.
    static bool was_keeping = false;
    if (keeping && !was_keeping)
        for (auto &face : interfaces) face.reads = face.blocked = 0;
    if (!keeping && was_keeping)
        logging::printf(logging::Level::info, logging::Channel::input,
                        "Steam controller reads while game modes kept the pad: SteamInput006 %llu (%llu presses held back), "
                        "SteamController008 %llu (%llu held back).",
                        static_cast<unsigned long long>(interfaces[0].reads.load()), static_cast<unsigned long long>(interfaces[0].blocked.load()),
                        static_cast<unsigned long long>(interfaces[1].reads.load()), static_cast<unsigned long long>(interfaces[1].blocked.load()));
    was_keeping = keeping;
}
} // namespace dingosdk::overlay
