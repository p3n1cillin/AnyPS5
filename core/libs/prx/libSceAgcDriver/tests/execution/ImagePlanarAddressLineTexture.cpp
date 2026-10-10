#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::uint32_t Threads = 32;
alignas(256) std::array<std::uint32_t, Threads * 2> Input{};
alignas(256) std::array<std::uint32_t, Threads * 4> Output{};
alignas(4096) std::array<std::uint32_t, 1024> Texels{};
constexpr std::array<std::uint32_t, 11> Load{
    0x34020083u, 0xe0341000u, 0x80000201u, 0xbf8c3f70u, 0xf0000f08u, 0x00020402u,
    0x34020084u, 0xbf8c3f70u, 0xe0781000u, 0x80010401u, 0xbf810000u};
constexpr std::array<std::uint32_t, 15> Store{
    0x34020083u, 0xe0341000u, 0x80000201u, 0xbf8c3f70u, 0x7e0802ffu, 0x3f800000u,
    0x7e0a02ffu, 0x40000000u, 0x7e0c02ffu, 0x40400000u, 0x7e0e02ffu, 0x40800000u,
    0xf0200f08u, 0x00020402u, 0xbf810000u};

std::array<std::uint32_t, 4> Buffer(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> Texture(std::uint32_t width, std::uint32_t format) {
    const auto address = reinterpret_cast<std::uintptr_t>(Texels.data());
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        (width - 1u) >> 2u, 0x80000facu, 0u, 0u, 0u, 0u};
}

void Fill(std::uint32_t width, std::uint32_t channels) {
    Texels.fill(0u);
    for (std::uint32_t x = 0; x < width; ++x) {
        for (std::uint32_t component = 0; component < channels; ++component) {
            Texels[x * channels + component] = std::bit_cast<std::uint32_t>(10.25f + static_cast<float>(x + component * 20u));
        }
    }
}

auto Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture, std::uint32_t threads) {
    const auto input = Buffer(Input.data(), sizeof(Input));
    const auto output = Buffer(Output.data(), sizeof(Output));
    std::array<std::uint32_t, 20> data{};
    std::copy(input.begin(), input.end(), data.begin());
    std::copy(output.begin(), output.end(), data.begin() + 4);
    std::copy(texture.begin(), texture.end(), data.begin() + 8);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {32u, 0u, data, compute, std::nullopt, std::nullopt, {}}, device.Target(), {0u, 0u, 0u, 128u}};
    return ShaderRecompiler::Recompile(request);
}

void Reject(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture, std::string_view expected = "image descriptor is incompatible with the static runtime image interface") {
    std::string error;
    try {
        static_cast<void>(Compile(device, code, texture, 1u));
    } catch (const std::exception& failure) {
        error = failure.what();
        error = error.substr(0u, error.find('\n'));
    }
    Require(error.find(expected) != std::string::npos, "unmeasured line access was not rejected: " + error);
}

void RunDimension(AgcDriver::VulkanDevice& device, std::uint32_t dimension) {
    auto lineLoad = Load;
    lineLoad[4] &= ~8u;
    auto lineStore = Store;
    lineStore[12] &= ~8u;
    const auto& load = dimension == 1u ? lineLoad : Load;
    const auto& store = dimension == 1u ? lineStore : Store;
    std::uint64_t loadVariant = 0u;
    std::uint64_t storeVariant = 0u;
    for (const auto format : {22u, 77u}) {
        const std::uint32_t channels = format == 22u ? 1u : 4u;
        for (const auto width : {1u, 8u}) {
            for (const auto y : {0u, 1u, 0xffffffffu}) {
                Fill(width, channels);
                Output.fill(0xdeadbeefu);
                const std::array<std::uint32_t, 4> xs{0u, width - 1u, width, 0xffffffffu};
                for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                    Input[lane * 2u] = xs[lane % xs.size()];
                    Input[lane * 2u + 1u] = y;
                }
                const auto shader = Compile(device, load, Texture(width, format), Threads);
                if (loadVariant == 0u) loadVariant = shader.variantId;
                else Require(shader.variantId == loadVariant && shader.cacheHit, "line load descriptors changed the compiled artifact");
                device.Dispatch(shader, 1u, 1u, 1u);
                device.WaitIdle();
                for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                    const auto x = Input[lane * 2u];
                    for (std::uint32_t component = 0; component < 4u; ++component) {
                        const auto expected = x < width ? Texels[x * channels + component % channels] : 0u;
                        Require(Output[lane * 4u + component] == expected, "load of a 1D image differs from native RDNA2: dim=" + std::to_string(dimension) + " format=" + std::to_string(format) + " lane=" + std::to_string(lane) + " component=" + std::to_string(component) + " y=" + std::to_string(y) + " actual=" + std::to_string(Output[lane * 4u + component]) + " expected=" + std::to_string(expected));
                    }
                }
            }
            for (const auto coordinates : {std::array{0u, 0u}, std::array{width - 1u, 0u}, std::array{width, 0u}, std::array{0xffffffffu, 0u}, std::array{0u, 1u}, std::array{0u, 0xffffffffu}}) {
                Fill(width, channels);
                auto expected = Texels;
                Input[0] = coordinates[0];
                Input[1] = coordinates[1];
                if (coordinates[0] < width) {
                    for (std::uint32_t component = 0; component < channels; ++component) {
                        expected[coordinates[0] * channels + component] = std::bit_cast<std::uint32_t>(static_cast<float>(component + 1u));
                    }
                }
                const auto shader = Compile(device, store, Texture(width, format), 1u);
                if (storeVariant == 0u) storeVariant = shader.variantId;
                else Require(shader.variantId == storeVariant && shader.cacheHit, "line store descriptors changed the compiled artifact");
                device.Dispatch(shader, 1u, 1u, 1u);
                device.WaitIdle();
                AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(Texels.data()), width * channels * 4u, nullptr, "test");
                device.WaitIdle();
                Require(Texels == expected, "store of a 1D image differs from native RDNA2 or writeback failed: dim=" + std::to_string(dimension) + " x=" + std::to_string(coordinates[0]) + " y=" + std::to_string(coordinates[1]));
            }
        }
    }
}

void RunPartialLoads(AgcDriver::VulkanDevice& device) {
    for (const auto dimension : {1u, 2u}) {
        for (std::uint32_t mask = 1u; mask <= 15u; ++mask) {
            std::vector<std::uint32_t> code(Load.begin(), Load.end());
            code.insert(code.begin() + 4, {0x7e0802ffu, 0xcdcdcdcdu, 0x7e0a02ffu, 0xcdcdcdcdu, 0x7e0c02ffu, 0xcdcdcdcdu, 0x7e0e02ffu, 0xcdcdcdcdu});
            code[12] = 0xf0000000u | (mask << 8u) | (dimension == 1u ? 0u : 8u);
            for (const auto format : {22u, 77u}) {
                const auto channels = format == 22u ? 1u : 4u;
                for (const auto width : {1u, 8u}) {
                    Fill(width, channels);
                    for (const auto y : {0u, 1u, 0xffffffffu}) {
                        const std::array<std::uint32_t, 4> xs{0u, width - 1u, width, 0xffffffffu};
                        for (std::uint32_t lane = 0u; lane < Threads; ++lane) {
                            Input[lane * 2u] = xs[lane % xs.size()];
                            Input[lane * 2u + 1u] = y;
                        }
                        Output.fill(0xdeadbeefu);
                        const auto shader = Compile(device, code, Texture(width, format), Threads);
                        device.Dispatch(shader, 1u, 1u, 1u);
                        device.WaitIdle();
                        for (std::uint32_t lane = 0u; lane < Threads; ++lane) {
                            std::array<std::uint32_t, 4> expected{0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu};
                            std::uint32_t destination = 0u;
                            const auto x = Input[lane * 2u];
                            for (std::uint32_t component = 0u; component < 4u; ++component) {
                                if ((mask & (1u << component)) != 0u) expected[destination++] = x < width ? Texels[x * channels + component % channels] : 0u;
                            }
                            for (std::uint32_t component = 0u; component < 4u; ++component) Require(Output[lane * 4u + component] == expected[component], "partial line load differs from native RDNA2: dim=" + std::to_string(dimension) + " mask=" + std::to_string(mask) + " component=" + std::to_string(component));
                        }
                    }
                }
            }
        }
    }
}

void Run(AgcDriver::VulkanDevice& device) {
    RunDimension(device, 1u);
    RunDimension(device, 2u);
    RunPartialLoads(device);
    auto mipLoad = Load;
    mipLoad[4] += 0x00040000u;
    Reject(device, mipLoad, Texture(8u, 77u));
    auto narrowLoad = Load;
    narrowLoad[5] |= 0x80000000u;
    Reject(device, narrowLoad, Texture(8u, 77u));
    auto narrowAddress = Load;
    narrowAddress[5] |= 0x40000000u;
    Reject(device, narrowAddress, Texture(8u, 77u));
    auto partialStore = Store;
    partialStore[12] = 0xf0200108u;
    Fill(8u, 4u);
    auto expected = Texels;
    Input[0] = 2u;
    Input[1] = 0xffffffffu;
    expected[8] = std::bit_cast<std::uint32_t>(1.0f);
    expected[9] = expected[10] = expected[11] = 0u;
    const auto shader = Compile(device, partialStore, Texture(8u, 77u), 1u);
    device.Dispatch(shader, 1u, 1u, 1u);
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(Texels.data()), 8u * 4u * 4u, nullptr, "test");
    device.WaitIdle();
    Require(Texels == expected, "a partial 2D store to a 1D image did not ignore y and zero omitted components");
    auto mipStore = Store;
    mipStore[12] += 0x00040000u;
    Reject(device, mipStore, Texture(8u, 77u));
    Reject(device, Load, Texture(8u, 20u));
    Reject(device, Store, Texture(8u, 20u));
    auto array = Texture(8u, 77u);
    array[3] = 0xc0000facu;
    Reject(device, Load, array);
    auto swizzled = Texture(8u, 77u);
    swizzled[3] ^= 1u;
    Reject(device, Load, swizzled);
    auto mipView = Texture(8u, 77u);
    mipView[3] |= 0x00011000u;
    Reject(device, Load, mipView);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestAllocations::Mutation().Add(Texels.data(), sizeof(Texels), true, true, true);
        Run(*device);
        GuestAllocations::Mutation().Remove(Texels.data());
        std::cout << "2D accesses to measured 1D image interfaces passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
