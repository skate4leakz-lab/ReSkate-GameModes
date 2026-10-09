#include "overlay_internal.h"
#include "input_capture.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Input/playstation_report.h"
#include <array>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <hidsdi.h>

// The game reads DualShock 4 / DualSense pads straight from HID: CreateFileW on the device, then
// overlapped ReadFile completed through GetOverlappedResult[Ex]. Hiding XInput buttons does not
// reach those, so while ReSkate keeps buttons for itself (hide_game_buttons, the D-pad while game
// modes place an area) the D-pad is set released in every report the game receives. Bluetooth
// reports carry a CRC, which is recomputed. ReSkate's own reader opens the pads under
// OverlayInputAccess and is never filtered; every other file read passes straight through.

namespace dingosdk::overlay::detail {
extern std::atomic<std::uint16_t> hidden_game_buttons;
namespace {
constexpr std::uint16_t dpad_bits = 0x000f;
constexpr std::size_t max_pads = 8;

struct PendingRead {
    std::uint8_t *buffer{};
    DWORD size{};
    PlayStationPad kind{};
};
struct Filter {
    Hook create, read, overlapped, overlapped_ex;
    // The game's open pad handles: checked on every ReadFile, so lock-free.
    std::array<std::atomic<HANDLE>, max_pads> handles{};
    std::array<std::atomic<PlayStationPad>, max_pads> kinds{};
    std::mutex mutex; // pending
    std::unordered_map<const OVERLAPPED *, PendingRead> pending;
    // Its own lock: CreateFileW (which forgets reused values) may run while `mutex` is held.
    std::mutex checked_mutex;
    std::unordered_set<HANDLE> checked; // handles identify() already looked at
    std::atomic<bool> announced{};
};
Filter &filter() {
    static auto *value = new Filter;
    return *value;
}

PlayStationPad tracked(HANDLE file) noexcept {
    if (!file || file == INVALID_HANDLE_VALUE) return PlayStationPad::none;
    auto &f = filter();
    for (std::size_t i = 0; i < max_pads; ++i)
        if (f.handles[i].load(std::memory_order_acquire) == file) return f.kinds[i].load(std::memory_order_relaxed);
    return PlayStationPad::none;
}
// USB: "...vid_054c&pid_0ce6..."; Bluetooth: "..._vid&0002054c_pid&0ce6...".
PlayStationPad pad_from_path(const wchar_t *path) noexcept {
    if (!path) return PlayStationPad::none;
    std::wstring lower(path);
    for (auto &c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (lower.find(L"054c") == std::wstring::npos || lower.find(L"hid") == std::wstring::npos) return PlayStationPad::none;
    const auto at = lower.find(L"pid");
    if (at == std::wstring::npos || at + 8 > lower.size()) return PlayStationPad::none;
    unsigned product{};
    for (std::size_t i = at + 4; i < at + 8; ++i) {
        const auto c = lower[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return PlayStationPad::none;
        product = product * 16 + static_cast<unsigned>(digit);
    }
    return playstation_pad(sony_vendor_id, static_cast<std::uint16_t>(product));
}
std::uint32_t crc32(std::uint32_t crc, const std::uint8_t *data, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
// The hat sits in the low nibble of the first button byte; 8 is released. Layouts as in
// parse_playstation_report: DualSense USB 0x01 and Bluetooth 0x31, DualShock 4 USB and the
// Bluetooth simple report 0x01, DualShock 4 Bluetooth 0x11.
void release_dpad(PlayStationPad kind, std::uint8_t *report, DWORD size) noexcept {
    if (!report || size < 10 || !(hidden_game_buttons.load(std::memory_order_relaxed) & dpad_bits)) return;
    const bool bluetooth = playstation_bluetooth(size);
    const auto id = report[0];
    std::size_t at{};
    bool checksum{};
    if (kind == PlayStationPad::dualsense && id == 0x01 && !bluetooth && size >= 11) at = 8;
    else if (kind == PlayStationPad::dualsense && id == 0x31 && size >= 12) { at = 9; checksum = true; }
    else if (id == 0x01) at = 5;
    else if (kind == PlayStationPad::dualshock4 && id == 0x11 && size >= 12) { at = 7; checksum = true; }
    else return;
    if ((report[at] & 0x0f) != 0x08 && !filter().announced.exchange(true))
        logging::write(logging::Level::info, logging::Channel::input, "PlayStation report filter: kept a D-pad press from the game.");
    report[at] = static_cast<std::uint8_t>((report[at] & 0xf0) | 0x08);
    if (checksum && size >= 78) {
        // CRC-32 over the Bluetooth input header byte 0xA1, then the report up to the CRC.
        const std::uint8_t header = 0xa1;
        auto crc = crc32(0xffffffffu, &header, 1);
        crc = ~crc32(crc, report, 74);
        for (int i = 0; i < 4; ++i) report[74 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
}

HANDLE WINAPI captured_create(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                              DWORD flags, HANDLE templ) {
    const auto handle = original<HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
        filter().create)(name, access, share, security, disposition, flags, templ);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    const auto error = GetLastError();
    auto &f = filter();
    // A handle value comes back from the system only after its old handle was closed: whatever was
    // known about it (a pad, or checked and not one) belonged to that old handle. Without this a
    // file opened under a closed pad's value would have its reads "released" like a pad report.
    for (std::size_t i = 0; i < max_pads; ++i) {
        HANDLE expected = handle;
        f.handles[i].compare_exchange_strong(expected, nullptr);
    }
    {
        std::lock_guard lock(f.checked_mutex);
        f.checked.erase(handle);
    }
    if (overlay_input_access) {
        SetLastError(error);
        return handle;
    }
    if (const auto kind = pad_from_path(name); kind != PlayStationPad::none) {
        for (std::size_t i = 0; i < max_pads; ++i) {
            HANDLE expected = nullptr;
            // A slot is reused when its handle value comes back from the system for another pad.
            if (f.handles[i].load() == handle || f.handles[i].compare_exchange_strong(expected, handle)) {
                f.kinds[i].store(kind);
                break;
            }
        }
    }
    SetLastError(error);
    return handle;
}
// The game usually opens its pad before these hooks exist, so a read the size of a PlayStation input
// report (64 bytes over USB, 78 over Bluetooth) from a handle not seen before is checked once.
PlayStationPad identify(HANDLE file, DWORD size) noexcept {
    if (size != 64 && size != 78) return PlayStationPad::none;
    auto &f = filter();
    {
        std::lock_guard lock(f.checked_mutex);
        if (f.checked.contains(file)) return PlayStationPad::none;
        f.checked.insert(file);
    }
    HIDD_ATTRIBUTES attributes{sizeof(attributes)};
    if (!HidD_GetAttributes(file, &attributes)) return PlayStationPad::none;
    const auto kind = playstation_pad(attributes.VendorID, attributes.ProductID);
    if (kind == PlayStationPad::none) return kind;
    for (std::size_t i = 0; i < max_pads; ++i) {
        HANDLE expected = nullptr;
        if (f.handles[i].load() == file || f.handles[i].compare_exchange_strong(expected, file)) {
            f.kinds[i].store(kind);
            logging::printf(logging::Level::info, logging::Channel::input,
                            "PlayStation report filter: the game reads pad %04x (%s).", attributes.ProductID,
                            size > 64 ? "bluetooth" : "usb");
            break;
        }
    }
    return kind;
}

BOOL WINAPI captured_read(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
    const auto result = original<BOOL(WINAPI *)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED)>(filter().read)(file, buffer, size, read,
                                                                                                          overlapped);
    auto kind = tracked(file);
    if (kind == PlayStationPad::none && !overlay_input_access && buffer) {
        const auto error = GetLastError();
        kind = identify(file, size);
        SetLastError(error);
    }
    if (kind == PlayStationPad::none || !buffer) return result;
    const auto error = GetLastError();
    if (result && read && *read) release_dpad(kind, static_cast<std::uint8_t *>(buffer), *read);
    // The game takes most reports without asking Windows again: the read completes into its buffer
    // and it looks at the OVERLAPPED (or an event) by itself. While the D-pad is kept, the read is
    // finished here (the pad reports every few milliseconds; at most 50 ms), its D-pad released,
    // and the game left to find it complete as it would have. Its event is set again in case the
    // wait here took an auto-reset signal.
    if (!result && error == ERROR_IO_PENDING && overlapped && !overlay_input_access &&
        (hidden_game_buttons.load(std::memory_order_relaxed) & dpad_bits)) {
        DWORD transferred{};
        const auto finish = original<BOOL(WINAPI *)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL)>(filter().overlapped_ex);
        if (finish && finish(file, overlapped, &transferred, 50, FALSE)) {
            release_dpad(kind, static_cast<std::uint8_t *>(buffer), std::min(transferred, size));
            if (overlapped->hEvent) SetEvent(overlapped->hEvent);
            SetLastError(ERROR_IO_PENDING);
            return result;
        }
    }
    if (overlapped) {
        auto &f = filter();
        std::lock_guard lock(f.mutex);
        f.pending[overlapped] = {static_cast<std::uint8_t *>(buffer), size, kind};
    }
    SetLastError(error);
    return result;
}
void completed(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred) {
    if (!overlapped || tracked(file) == PlayStationPad::none) return;
    const auto error = GetLastError();
    auto &f = filter();
    {
        std::lock_guard lock(f.mutex);
        if (const auto found = f.pending.find(overlapped); found != f.pending.end()) {
            const auto size = transferred && *transferred ? std::min(*transferred, found->second.size) : found->second.size;
            release_dpad(found->second.kind, found->second.buffer, size);
            f.pending.erase(found);
        }
    }
    SetLastError(error);
}
BOOL WINAPI captured_overlapped(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait) {
    const auto result =
        original<BOOL(WINAPI *)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL)>(filter().overlapped)(file, overlapped, transferred, wait);
    if (result) completed(file, overlapped, transferred);
    return result;
}
BOOL WINAPI captured_overlapped_ex(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, DWORD milliseconds, BOOL alertable) {
    const auto result = original<BOOL(WINAPI *)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL)>(filter().overlapped_ex)(
        file, overlapped, transferred, milliseconds, alertable);
    if (result) completed(file, overlapped, transferred);
    return result;
}
} // namespace

bool install_playstation_filter() {
    auto &f = filter();
    const auto kernel = GetModuleHandleW(L"kernelbase.dll");
    if (!kernel) return false;
    const auto hook = [&](Hook &target, const char *name, void *detour) {
        return install(target, reinterpret_cast<void *>(GetProcAddress(kernel, name)), detour);
    };
    const bool ok = hook(f.create, "CreateFileW", reinterpret_cast<void *>(&captured_create)) &&
                    hook(f.read, "ReadFile", reinterpret_cast<void *>(&captured_read)) &&
                    hook(f.overlapped, "GetOverlappedResult", reinterpret_cast<void *>(&captured_overlapped)) &&
                    hook(f.overlapped_ex, "GetOverlappedResultEx", reinterpret_cast<void *>(&captured_overlapped_ex));
    logging::write(ok ? logging::Level::info : logging::Level::warning, logging::Channel::input,
                   ok ? "PlayStation report filter installed (game modes can keep the D-pad from the game)."
                      : "PlayStation report filter unavailable: the D-pad still reaches the game while placing.");
    return ok;
}
void release_playstation_dpad(PlayStationPad kind, std::uint8_t *report, unsigned long size) noexcept {
    release_dpad(kind, report, static_cast<DWORD>(size));
}
} // namespace dingosdk::overlay::detail
