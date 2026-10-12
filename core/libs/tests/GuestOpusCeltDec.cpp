#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceOpusCeltDecInitialize(std::uint32_t*);
int APS5_VABI sceOpusCeltDecTerminate(std::uint32_t*);
int APS5_VABI sceOpusCeltDecGetSize(int);
int APS5_VABI sceOpusCeltDecCreateEx(std::uint32_t*, void*, int, int);
int APS5_VABI sceOpusCeltDecDecode(void*, const std::uint8_t*, int, std::int16_t*, int);
int APS5_VABI sceOpusCeltDecDestroy(void*);
}

namespace {

void Require(bool value) { if (!value) std::abort(); }

template <typename TFunction>
void RequireThrows(TFunction function) {
    try { function(); } catch (const std::runtime_error&) { return; }
    std::abort();
}

const std::uint8_t CELT_STEREO[] = {
    0x3C, 0x00, 0xFC, 0xB5, 0x1F, 0xBC, 0xC8, 0xCA, 0x0B, 0xC2, 0x18, 0x1A, 0xFB, 0xDB, 0x3E, 0x2B, 0x6B, 0x0A, 0x67, 0x71, 0x25, 0x2F, 0x57, 0x2E,
    0x4B, 0xA7, 0x84, 0xEA, 0xB1, 0xA7, 0xD4, 0xF3, 0x0D, 0x24, 0xF7, 0xD2, 0x8F, 0x8D, 0x27, 0x53, 0x7C, 0xDF, 0xC0, 0x22, 0xCC, 0xBA, 0xD1, 0xAB,
    0xFF, 0x70, 0x45, 0x2F, 0xF7, 0x7F, 0x4E, 0x59, 0x00, 0x65, 0x12, 0xD8, 0x2F, 0x6C, 0x3C, 0x00, 0xFC, 0xB2, 0xB3, 0x93, 0x38, 0xF1, 0x28, 0xA7,
    0x55, 0x0D, 0xA2, 0x10, 0xDB, 0xE0, 0xB3, 0x8A, 0x30, 0xD4, 0xC4, 0x5A, 0x42, 0x6C, 0x28, 0x5F, 0xD3, 0xEE, 0x34, 0x90, 0xA6, 0x37, 0x96, 0xD9,
    0xF4, 0x0B, 0x03, 0x58, 0x0D, 0x57, 0x2B, 0xF8, 0x2E, 0x07, 0x18, 0x00, 0xFD, 0xA9, 0xAE, 0xDB, 0x02, 0x11, 0xD5, 0xCC, 0x7E, 0x50, 0x60, 0x07,
    0x80, 0x49, 0xEB, 0xAE, 0x3C, 0x00, 0xFC, 0xB2, 0x46, 0x3B, 0x19, 0x2F, 0xFA, 0xBD, 0xE8, 0x57, 0x7D, 0xC3, 0x69, 0x64, 0x61, 0xBF, 0x3A, 0x3B,
    0xAA, 0x2A, 0xB1, 0x34, 0x86, 0x3A, 0xA4, 0x5D, 0xE7, 0xF6, 0xE4, 0x44, 0x2E, 0xDB, 0x48, 0x96, 0x26, 0xE5, 0x45, 0xDC, 0xB9, 0x67, 0x2A, 0x67,
    0x93, 0x58, 0x09, 0x9E, 0xAD, 0xA0, 0x39, 0x07, 0xBA, 0xDF, 0x33, 0xA9, 0x0D, 0x9E, 0x61, 0x3F, 0x8B, 0xAE,
};

} // namespace

int main() {
    std::uint32_t context = 0, otherContext = 0;
    Require(sceOpusCeltDecInitialize(&context) == 0);
    Require(sceOpusCeltDecInitialize(&otherContext) == 0 && context != otherContext);
    RequireThrows([&] { sceOpusCeltDecInitialize(&context); });
    Require(sceOpusCeltDecGetSize(1) == 640 && sceOpusCeltDecGetSize(2) == 640);
    RequireThrows([&] { sceOpusCeltDecGetSize(3); });
    std::array<std::uint8_t, 640> state{}, otherState{}, monoState{}, unsupported{};
    Require(sceOpusCeltDecCreateEx(&context, state.data(), 48000, 2) == 0);
    Require(sceOpusCeltDecCreateEx(&otherContext, otherState.data(), 48000, 2) == 0);
    Require(sceOpusCeltDecCreateEx(&context, monoState.data(), 48000, 1) == 0);
    RequireThrows([&] { sceOpusCeltDecCreateEx(&context, state.data(), 48000, 2); });
    for (int rate : {8000, 12000, 16000, 24000})
        RequireThrows([&] { sceOpusCeltDecCreateEx(&context, unsupported.data(), rate, 2); });
    RequireThrows([&] { sceOpusCeltDecTerminate(&context); });
    std::array<std::int16_t, 1922> pcm{}, reference{};
    pcm.fill(12345);
    const std::uint8_t silk[] = {0x0C, 0x00};
    const std::uint8_t hybrid[] = {0x7C, 0x00};
    RequireThrows([&] { sceOpusCeltDecDecode(state.data(), silk, sizeof(silk), pcm.data() + 1, 3840); });
    RequireThrows([&] { sceOpusCeltDecDecode(state.data(), hybrid, sizeof(hybrid), pcm.data() + 1, 3840); });
    RequireThrows([&] { sceOpusCeltDecDecode(state.data(), CELT_STEREO + 2, 60, pcm.data() + 1, 3839); });
    RequireThrows([&] { sceOpusCeltDecDecode(state.data(), nullptr, 0, pcm.data() + 1, 3840); });
    Require(std::all_of(pcm.begin(), pcm.end(), [](auto value) { return value == 12345; }));
    std::size_t offset = 0;
    for (int frame = 0; frame < 3; ++frame) {
        const int bytes = CELT_STEREO[offset] | (CELT_STEREO[offset + 1] << 8);
        const auto* packet = CELT_STEREO + offset + 2;
        Require(packet[0] >> 3 == 31);
        Require(sceOpusCeltDecDecode(state.data(), packet, bytes, pcm.data() + 1, 3841) == 3840);
        Require(pcm.front() == 12345 && pcm.back() == 12345);
        Require(sceOpusCeltDecDecode(otherState.data(), packet, bytes, reference.data() + 1, 3840) == 3840);
        Require(std::equal(pcm.begin() + 1, pcm.end() - 1, reference.begin() + 1));
        if (frame > 0) {
            int peak = 0;
            for (auto it = pcm.begin() + 1; it != pcm.end() - 1; ++it) peak = std::max(peak, std::abs(static_cast<int>(*it)));
            Require(peak > 60 && peak < 130);
        }
        std::array<std::int16_t, 962> mono{};
        mono.front() = mono.back() = 12345;
        Require(sceOpusCeltDecDecode(monoState.data(), packet, bytes, mono.data() + 1, 1921) == 1920);
        Require(mono.front() == 12345 && mono.back() == 12345);
        Require(std::any_of(mono.begin() + 1, mono.end() - 1, [](auto value) { return value != 0; }));
        offset += bytes + 2;
    }
    Require(offset == sizeof(CELT_STEREO));
    Require(sceOpusCeltDecDestroy(state.data()) == 0);
    RequireThrows([&] { sceOpusCeltDecDestroy(state.data()); });
    RequireThrows([&] { sceOpusCeltDecDecode(state.data(), CELT_STEREO + 2, 60, pcm.data(), 3840); });
    Require(sceOpusCeltDecDestroy(monoState.data()) == 0);
    Require(sceOpusCeltDecTerminate(&context) == 0);
    RequireThrows([&] { sceOpusCeltDecTerminate(&context); });
    RequireThrows([&] { sceOpusCeltDecCreateEx(&context, state.data(), 48000, 2); });
    Require(sceOpusCeltDecDestroy(otherState.data()) == 0);
    Require(sceOpusCeltDecTerminate(&otherContext) == 0);
}
