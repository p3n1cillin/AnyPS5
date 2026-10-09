#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GpuAtomicCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

alignas(256) std::array<std::uint32_t, 128> Output{};
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

void Environment(const char* name, const std::string& value) {
#ifdef _WIN32
    Require(_putenv_s(name, value.c_str()) == 0, "cannot set GPU capture environment");
#else
    Require(setenv(name, value.c_str(), 1) == 0, "cannot set GPU capture environment");
#endif
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open(), "GPU capture file missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct GuestBlock {
        static constexpr std::size_t Size = 65536;
        void* data = nullptr;
        GuestBlock() {
#ifdef _WIN32
            data = GuestArena::GuestArenaAllocate_nid_postfix(Size, Size);
            Require(data != nullptr, "cannot allocate GPU capture guest block");
            GuestArena::GuestArenaCommit_nid_postfix(data, Size, PAGE_READWRITE, Size);
#else
            void* raw = mmap(nullptr, Size * 2u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map GPU capture guest block");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + Size - 1u) & ~(Size - 1u);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + Size != begin + Size * 2u) munmap(reinterpret_cast<void*>(aligned + Size), begin + Size - aligned);
            data = reinterpret_cast<void*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, Size);
#endif
            Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(data), Size), "GPU capture guest block is not tracked");
            GuestAllocations::Mutation().Add(data, Size, true, true);
        }
        ~GuestBlock() {
            GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
            GuestArena::GuestArenaReset_nid_postfix(data, Size);
            GuestArena::GuestArenaRelease_nid_postfix(data, Size);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, Size);
            munmap(data, Size);
#endif
        }
};

bool Run(AgcDriver::VulkanDevice& device, const std::filesystem::path& directory) {
    GuestBlock block;
    const std::span<std::uint32_t> texels(static_cast<std::uint32_t*>(block.data), GuestBlock::Size / 4u);
    const std::array<std::uint32_t, 4> zero{};
    if (!device.FillBuffer(reinterpret_cast<std::uintptr_t>(block.data), GuestBlock::Size, zero)) return false;
    device.WaitIdle();
    const auto address = reinterpret_cast<std::uintptr_t>(Code);
    std::ostringstream addressText;
    addressText << "0x" << std::hex << address;
    Environment("APS5_DUMP_ATOMIC_INPUT", "invalid");
    Require(AgcDriver::GpuAtomicCapture::ReadOptions().program == 0, "invalid program enabled GPU capture");
    Environment("APS5_DUMP_ATOMIC_INPUT", addressText.str());
    Environment("APS5_DUMP_ATOMIC_INPUT_LIMIT", "65");
    Require(AgcDriver::GpuAtomicCapture::ReadOptions().program == 0, "oversized count enabled GPU capture");
    Environment("APS5_DUMP_ATOMIC_INPUT_LIMIT", "2");
    Environment("APS5_DUMP_ATOMIC_INPUT_DIR", directory.string());
    Output.fill(0xdeadbeefu);
    std::fill(texels.begin(), texels.end(), 0u);
    texels[0] = 17u;
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(Output.data());
    const auto textureAddress = reinterpret_cast<std::uintptr_t>(texels.data());
    std::vector<std::uint32_t> userData(16, 0u);
    const std::array<std::uint32_t, 4> output{
        static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>(outputAddress >> 32u) & 0xffffu,
        static_cast<std::uint32_t>(Output.size() * 4u), 0x31016facu};
    const std::array<std::uint32_t, 8> texture{
        static_cast<std::uint32_t>(textureAddress >> 8u),
        static_cast<std::uint32_t>(textureAddress >> 40u) | (20u << 20u) | (3u << 30u),
        1u | (7u << 14u), 0xfacu | (9u << 28u), 0u, 0u, 0u, 0u};
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{address, std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 2, 1}, 128u, {true, false, false}, false, 2};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, address, code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.ComputeTarget(32), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, address + 256u);
    Require(std::filesystem::is_empty(directory), "unmatched program wrote GPU capture");
    for (unsigned index = 0; index < 3; ++index) device.Dispatch(result, 1, 1, 1, {}, address);
    device.WaitIdle();
    std::vector<std::filesystem::path> sessions;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) sessions.push_back(entry.path());
    Require(sessions.size() == 1, "GPU capture did not create one session");
    Require(!std::filesystem::exists(sessions.front() / "input_3"), "GPU capture count limit exceeded");
    for (unsigned index = 1; index <= 2; ++index) {
        const auto capture = sessions.front() / ("input_" + std::to_string(index));
        const auto data = Read(capture / "image_0.bin");
        Require(data.size() == 8u * 8u * 4u, "GPU snapshot is not a packed image");
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[byte])) << (byte * 8u);
        Require(value == 17u + 2u * index, "GPU snapshot missed preceding queued atomic writes");
        Require(std::all_of(data.begin() + 4, data.end(), [](char byte) { return byte == 0; }), "GPU snapshot changed untouched texels");
        const auto metadata = Read(capture / "input.json");
        Require(metadata.find("\"synchronized\":true") != std::string::npos && metadata.find("\"captures_guest_buffers\":false") != std::string::npos, "GPU snapshot metadata misrepresents capture");
        Require(Read(capture / "shader.spv").size() == result.spirv.size() * 4u, "GPU snapshot missed selected shader");
    }
    for (unsigned lane = 0; lane < 64; ++lane) {
        const auto ticket = 23u + lane / 32u;
        Require(Output[lane * 2u] == ticket && Output[lane * 2u + 1u] == 123u + (ticket & 3u), "GPU capture changed shader execution results");
    }
    std::array<std::uint32_t, 1> counter{};
    AgcDriver::GuestMemory::Read(textureAddress, std::as_writable_bytes(std::span(counter)), 4);
    Require(counter[0] == 25u, "GPU capture altered final atomic counter");
    return true;
}

}

int main() {
    std::filesystem::path directory;
    int result = 0;
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32u) return VulkanTestSkipped;
        directory = std::filesystem::temp_directory_path() / ("aps5-gpu-atomic-input-test-" + std::to_string(std::random_device{}()));
        Require(std::filesystem::create_directory(directory), "GPU capture test directory already exists");
        if (Run(*device, directory)) {
            std::puts("GPU atomic input capture tests passed");
        } else {
            std::puts("skipped, the device does not import guest memory");
            result = VulkanTestSkipped;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code error;
    if (directory.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(directory, error);
    return result;
}
