#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t MaxGroups = 4096;
constexpr std::uint32_t Threads = 64;
alignas(256) std::array<std::uint32_t, MaxGroups * Threads * 2> Output{};
alignas(4096) std::array<std::uint32_t, 4096> Texels{};

alignas(256) constexpr std::uint32_t Code[]{
    0x34040285, 0x4a040500, 0x7e060210, 0x34060686, 0x4a060702, 0x34060683,
    0x34080282, 0x7e0a0280, 0x7e0c0280, 0x7e0e0281, 0x7d840080, 0xbe92246a,
    0xf0442108, 0x00010705, 0xbf8c3f70, 0xd8340000, 0x00000704, 0xbf8cc07f,
    0xbefe04c1, 0xd8d80000, 0x08000004, 0xbf8cc07f, 0x7e1202ff, 123u,
    0x7d841080, 0xbe94246a, 0xbf880002, 0x7e1202ff, 777u, 0xbefe04c1,
    0x36141083, 0x7e160280, 0x7d82150b, 0xbe94246a, 0xbf880004,
    0x4a161681, 0x4a121281, 0xbefe04c1, 0xbf82fff9, 0xbefe04c1,
    0xe0701000, 0x80000803, 0xe0701004, 0x80000903, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor() {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (20u << 20u) | (3u << 30u),
        1u | (7u << 14u), 0xfacu | (9u << 28u), 0u, 0u, 0u, 0u,
    };
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t groups, std::uint32_t initial) {
    Output.fill(0xdeadbeefu);
    Texels.fill(0u);
    Texels[0] = initial;
    std::vector<std::uint32_t> userData(16, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const auto texture = TextureDescriptor();
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 2, 1}, 128u, {true, false, false}, false, 2};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.ComputeTarget(32),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();

    std::vector<std::uint32_t> tickets;
    for (std::uint32_t group = 0; group < groups; ++group) {
        for (std::uint32_t row = 0; row < 2; ++row) {
            const auto ticket = Output[(group * Threads + row * 32u) * 2u];
            tickets.push_back(ticket - initial);
            for (std::uint32_t lane = 0; lane < 32; ++lane) {
                const auto index = (group * Threads + row * 32u + lane) * 2u;
                const auto location = "group " + std::to_string(group) + ", row " + std::to_string(row) + ", lane " + std::to_string(lane);
                Require(Output[index] == ticket, "wave image ticket differs within " + location);
                const auto expected = (ticket == 0u ? 777u : 123u) + (ticket & 3u);
                Require(Output[index + 1u] == expected, "wave image ticket branch or loop differs at " + location);
            }
        }
    }
    std::sort(tickets.begin(), tickets.end());
    for (std::uint32_t index = 0; index < tickets.size(); ++index) {
        Require(tickets[index] == index, "wave image ticket missing or duplicated at " + std::to_string(index));
    }
    for (std::uint32_t index = groups * Threads * 2u; index < Output.size(); ++index) {
        Require(Output[index] == 0xdeadbeefu, "wave image ticket wrote beyond dispatched groups");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32u) {
            std::printf("skipped, subgroup size %u cannot hold a wave64 in two lanes\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        for (const auto groups : {1u, 8u, 64u, MaxGroups}) {
            for (const auto initial : {0u, 1u, 3u, 0xfffffffeu}) {
                std::printf("wave image ticket: groups=%u initial=%u\n", groups, initial);
                std::fflush(stdout);
                Run(*device, groups, initial);
            }
        }
        std::puts("wave image ticket tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
