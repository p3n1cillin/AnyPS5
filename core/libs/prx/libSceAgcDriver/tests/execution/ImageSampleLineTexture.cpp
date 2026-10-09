#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::uint32_t Threads = 32;
alignas(256) std::array<std::uint32_t, Threads * 2> Input{};
alignas(256) std::array<std::uint32_t, Threads * 4> Output{};
std::span<std::uint8_t> Texels;

class TextureBlock {
public:
    TextureBlock() {
#ifdef _WIN32
        storage = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 65536u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        storage = static_cast<std::uint8_t*>(std::aligned_alloc(65536u, 65536u));
#endif
        Require(storage != nullptr && (reinterpret_cast<std::uintptr_t>(storage) & 65535u) == 0u, "cannot allocate aligned line texture memory");
        Texels = {storage, 65536u};
    }

    ~TextureBlock() {
#ifdef _WIN32
        VirtualFree(storage, 0, MEM_RELEASE);
#else
        std::free(storage);
#endif
    }

    TextureBlock(const TextureBlock&) = delete;
    TextureBlock& operator=(const TextureBlock&) = delete;

private:
    std::uint8_t* storage = nullptr;
};
constexpr std::array<std::uint32_t, 11> Sample{
    0x34020083u, 0xe0341000u, 0x80000201u, 0xbf8c3f70u, 0xf09c0f08u, 0x00820402u,
    0x34020084u, 0xbf8c3f70u, 0xe0781000u, 0x80010401u, 0xbf810000u};
constexpr std::array<float, 8> Coordinates{-1.0f, 0.0f, 0.0625f, 0.125f, 0.5f, 0.9375f, 1.0f, 2.0f};

enum class Addressing { Clamp, Repeat, Border, Mirror };

struct Sampler {
    std::array<std::uint32_t, 4> words;
    bool linear;
    Addressing addressing;
};

constexpr std::array<Sampler, 6> Samplers{{
    {{0x7092u, 0x00fff000u, 0x06500000u, 0u}, true, Addressing::Clamp},
    {{0x7092u, 0x00fff000u, 0x04000000u, 0u}, false, Addressing::Clamp},
    {{0x7000u, 0x00fff000u, 0x06500000u, 0u}, true, Addressing::Repeat},
    {{0x71b6u, 0x00fff000u, 0x06500000u, 0u}, true, Addressing::Border},
    {{0x71b6u, 0x00fff000u, 0x04000000u, 0u}, false, Addressing::Border},
    {{0x7049u, 0x00fff000u, 0x06500000u, 0u}, true, Addressing::Mirror},
}};

std::array<std::uint32_t, 4> Buffer(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> Texture(std::uint32_t width, std::uint32_t format, std::uint32_t tile) {
    const auto address = reinterpret_cast<std::uintptr_t>(Texels.data());
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        (width - 1u) >> 2u, 0x80000facu | (tile << 20u), 0u, 0u, 0u, 0u};
}

void Fill(std::uint32_t width, std::uint32_t format, std::uint32_t tile) {
    constexpr std::array<std::uint32_t, 8> r32Offsets{0u, 4u, 8u, 12u, 128u, 132u, 136u, 140u};
    constexpr std::array<std::uint32_t, 8> rgba16Offsets{0u, 8u, 32u, 40u, 64u, 72u, 96u, 104u};
    constexpr std::array<std::uint32_t, 8> rgba32Offsets{0u, 16u, 64u, 80u, 8192u, 8208u, 8256u, 8272u};
    const auto channels = format == 22u ? 1u : 4u;
    const auto bytes = format == 71u ? 2u : 4u;
    std::fill(Texels.begin(), Texels.end(), 0u);
    for (std::uint32_t x = 0; x < width; ++x) {
        const auto offset = tile == 0u ? x * channels * bytes : format == 22u ? r32Offsets[x] : format == 71u ? rgba16Offsets[x] : rgba32Offsets[x];
        for (std::uint32_t component = 0; component < channels; ++component) {
            const auto bits = std::bit_cast<std::uint32_t>(10.25f + static_cast<float>(x + component * 20u));
            if (bytes == 2u) {
                const auto half = static_cast<std::uint16_t>(((bits >> 23u) - 112u) << 10u | ((bits >> 13u) & 0x3ffu));
                std::memcpy(Texels.data() + offset + component * bytes, &half, sizeof(half));
            } else {
                std::memcpy(Texels.data() + offset + component * bytes, &bits, sizeof(bits));
            }
        }
    }
}

float Expected(float coordinate, std::uint32_t width, std::uint32_t component, std::uint32_t channels, const Sampler& sampler) {
    const auto tap = [&](int index) {
        const auto extent = static_cast<int>(width);
        switch (sampler.addressing) {
        case Addressing::Repeat: index = (index % extent + extent) % extent; break;
        case Addressing::Mirror:
            index = (index % (extent * 2) + extent * 2) % (extent * 2);
            if (index >= extent) index = extent * 2 - index - 1;
            break;
        case Addressing::Border:
            if (index < 0 || index >= extent) return 0.0f;
            break;
        case Addressing::Clamp: index = std::clamp(index, 0, extent - 1); break;
        }
        return 10.25f + static_cast<float>(index + static_cast<int>(component % channels) * 20);
    };
    if (!sampler.linear) return tap(static_cast<int>(std::floor(coordinate * width)));
    const auto position = coordinate * width - 0.5f;
    const auto low = static_cast<int>(std::floor(position));
    const auto weight = position - static_cast<float>(low);
    return tap(low) * (1.0f - weight) + tap(low + 1) * weight;
}

auto Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture, const Sampler& sampler) {
    const auto input = Buffer(Input.data(), sizeof(Input));
    const auto output = Buffer(Output.data(), sizeof(Output));
    std::array<std::uint32_t, 20> data{};
    std::copy(input.begin(), input.end(), data.begin());
    std::copy(output.begin(), output.end(), data.begin() + 4);
    std::copy(texture.begin(), texture.end(), data.begin() + 8);
    std::copy(sampler.words.begin(), sampler.words.end(), data.begin() + 16);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {32u, 0u, data, compute, std::nullopt, std::nullopt, {}}, device.Target(), {0u, 0u, 0u, 128u}};
    return ShaderRecompiler::Recompile(request);
}

void Run(AgcDriver::VulkanDevice& device) {
    std::vector<std::unique_ptr<TextureBlock>> pixels;
    auto lineSample = Sample;
    lineSample[4] &= ~8u;
    for (const auto dimension : {1u, 2u}) {
        const auto& code = dimension == 1u ? lineSample : Sample;
        std::uint64_t variant = 0u;
        for (const auto format : {71u, 77u, 22u}) {
            const auto channels = format == 22u ? 1u : 4u;
            for (const auto width : {1u, 8u}) {
                for (const auto tile : {0u, 27u}) {
                    pixels.push_back(std::make_unique<TextureBlock>());
                    Fill(width, format, tile);
                    for (const auto& sampler : Samplers) {
                        for (const auto y : {0x00000000u, 0x3f000000u, 0xbf800000u, 0x40000000u, 0x7fc00000u}) {
                            for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                                Input[lane * 2u] = std::bit_cast<std::uint32_t>(Coordinates[lane % Coordinates.size()]);
                                Input[lane * 2u + 1u] = y;
                            }
                            Output.fill(0xdeadbeefu);
                            const auto result = Compile(device, code, Texture(width, format, tile), sampler);
                            if (variant == 0u) variant = result.variantId;
                            else Require(result.variantId == variant && result.cacheHit, "line sample descriptors changed the compiled artifact");
                            device.Dispatch(result, 1u, 1u, 1u);
                            device.WaitIdle();
                            for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                                for (std::uint32_t component = 0; component < 4u; ++component) {
                                    const auto expected = std::bit_cast<std::uint32_t>(Expected(Coordinates[lane % Coordinates.size()], width, component, channels, sampler));
                                    const auto actual = Output[lane * 4u + component];
                                    Require(actual == expected, "line sample differs from native RDNA2: dim=" + std::to_string(dimension) + " format=" + std::to_string(format) + " width=" + std::to_string(width) + " tile=" + std::to_string(tile) + " sampler=" + std::to_string(sampler.words[0]) + " lane=" + std::to_string(lane) + " component=" + std::to_string(component) + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    for (const auto dimension : {1u, 2u}) {
        const auto& code = dimension == 1u ? lineSample : Sample;
        pixels.push_back(std::make_unique<TextureBlock>());
        Fill(1u, 71u, 0u);
        const auto texture = Texture(1u, 71u, 0u);
        for (std::uint32_t start = 0u; start <= 512u; start += Threads) {
            for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                Input[lane * 2u] = std::bit_cast<std::uint32_t>(static_cast<float>(std::min(start + lane, 512u)) / 1024.0f);
                Input[lane * 2u + 1u] = 0x7fc00000u;
            }
            const auto result = Compile(device, code, texture, Samplers[3]);
            device.Dispatch(result, 1u, 1u, 1u);
            device.WaitIdle();
            for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                const auto x = std::bit_cast<float>(Input[lane * 2u]);
                const auto weight = std::nearbyint((x + 0.5f) * 256.0f) / 256.0f;
                for (std::uint32_t component = 0; component < 4u; ++component) {
                    const auto expected = std::bit_cast<std::uint32_t>((10.25f + static_cast<float>(component * 20u)) * weight);
                    Require(Output[lane * 4u + component] == expected, "half-float line interpolation differs from the native eight-bit round-to-even weight");
                }
            }
        }
    }
    for (std::uint32_t invalid = 0u; invalid < 5u; ++invalid) {
        auto sampler = Samplers[0];
        if (invalid == 0u) sampler.words[0] |= 1u << 15u;
        if (invalid == 1u) sampler.words[0] = (sampler.words[0] & ~7u) | 3u;
        if (invalid == 2u) sampler.words[1] |= 1u;
        if (invalid == 3u) sampler.words[2] ^= 1u << 20u;
        if (invalid == 4u) sampler.words[3] = 1u << 30u;
        std::string error;
        try {
            static_cast<void>(Compile(device, Sample, Texture(1u, 71u, 0u), sampler));
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        Require(error.find("unmeasured sampler for a 2D level-zero sample of a 1D image") != std::string::npos, "unmeasured line sampler was not rejected: " + error.substr(0u, error.find('\n')));
    }
    for (const auto opcode : {0xf0800f08u, 0xf0900f08u, 0xf0a00f08u, 0xf11c0108u}) {
        auto unmeasured = Sample;
        unmeasured[4] = opcode;
        std::string error;
        try {
            static_cast<void>(Compile(device, unmeasured, Texture(8u, 71u, 0u), Samplers[0]));
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        Require(error.find("image descriptor is incompatible with the static runtime image interface") != std::string::npos, "unmeasured line sampling was not rejected: " + error.substr(0u, error.find('\n')));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "level-zero sampling of measured 1D image interfaces passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
