#include "overlay_internal.h"
#include "input_capture.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Input/playstation_report.h"
#include <atomic>

// While ReSkate keeps buttons from the game (hide_game_buttons: the D-pad while game modes place an
// area) the D-pad is set released in the DualShock 4 / DualSense reports the game receives through
// input capture. Bluetooth reports carry a CRC, which is recomputed.

namespace dingosdk::overlay::detail {
namespace {
constexpr std::uint16_t dpad_bits = 0x000f;
std::atomic<bool> announced{};

std::uint32_t crc32(std::uint32_t crc, const std::uint8_t *data, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
} // namespace

// The hat sits in the low nibble of the first button byte; 8 is released. Layouts as in
// parse_playstation_report: DualSense USB 0x01 and Bluetooth 0x31, DualShock 4 USB and the
// Bluetooth simple report 0x01, DualShock 4 Bluetooth 0x11.
void release_playstation_dpad(PlayStationPad kind, std::uint8_t *report, unsigned long size) noexcept {
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
    if ((report[at] & 0x0f) != 0x08 && !announced.exchange(true))
        logging::write(logging::Level::info, logging::Channel::input, "PlayStation reports: kept a D-pad press from the game.");
    report[at] = static_cast<std::uint8_t>((report[at] & 0xf0) | 0x08);
    if (checksum && size >= 78) {
        // CRC-32 over the Bluetooth input header byte 0xA1, then the report up to the CRC.
        const std::uint8_t header = 0xa1;
        auto crc = crc32(0xffffffffu, &header, 1);
        crc = ~crc32(crc, report, 74);
        for (int i = 0; i < 4; ++i) report[74 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
}
} // namespace dingosdk::overlay::detail
