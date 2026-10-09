#include "VulkanTestDevice.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Environment(const char* name, const std::string& value) {
#ifdef _WIN32
    Require(_putenv_s(name, value.c_str()) == 0, "cannot set capture environment");
#else
    Require(setenv(name, value.c_str(), 1) == 0, "cannot set capture environment");
#endif
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open(), "driver capture file missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Run(const std::filesystem::path& directory) {
    alignas(256) std::array<std::uint32_t, 1> code{0xbf810000u};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    std::ostringstream addressText;
    addressText << std::hex << address;
    Environment("APS5_DUMP_DISPATCH", addressText.str());
    Environment("APS5_DUMP_DISPATCH_DIR", directory.string());
    Environment("APS5_DUMP_DISPATCH_LIMIT", "3");
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 7> registers{};
        ShaderSpecialRegs specials{};
    } header;
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18;
    header.shader.header_size = sizeof(header);
    header.shader.shader_size = sizeof(code);
    header.shader.code = code.data();
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.specials = &header.specials;
    header.specials.dispatch_modifier = 0x8000;
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)},
        {0x207, 1}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 0}}};
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    for (const auto groups : {1u, 2u, 3u, 4u}) {
        std::vector<std::uint32_t> commands;
        for (const auto reg : header.registers) commands.insert(commands.end(), {0xc0017600u, reg.offset, reg.value});
        commands.insert(commands.end(), {0xc0031500u, groups, 1, 1, 0x8041});
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "capture dispatch submission failed");
        AgcDriverWaitIdle_nid_postfix();
    }
    AgcDriverShutdown_nid_postfix();
    const auto sessions = std::filesystem::directory_iterator(directory);
    Require(sessions != std::filesystem::directory_iterator{}, "driver capture session missing");
    const auto session = sessions->path();
    Require(!std::filesystem::exists(session / "dispatch_4"), "driver capture count limit exceeded");
    std::string spirv;
    for (unsigned index = 1; index <= 3; ++index) {
        const auto capture = session / ("dispatch_" + std::to_string(index));
        const auto decoded = ShaderRecompiler::RequestSerializer{}.Deserialize(Read(capture / "request.req"));
        Require(decoded.request.shader.codeAddress == address && decoded.shaderCode == std::vector<std::uint32_t>{code[0]}, "driver capture used another request");
        const auto metadata = Read(capture / "dispatch.json");
        const auto groups = "\"groups\":[" + std::to_string(index) + ",1,1]";
        Require(metadata.find(groups) != std::string::npos, "driver capture lost current group dimensions");
        const auto cache = index == 1 ? "\"cache_hit\":false" : "\"cache_hit\":true";
        Require(metadata.find(cache) != std::string::npos, "driver capture did not run on cache hits");
        const auto selected = Read(capture / "shader.spv");
        Require(selected.size() >= 20, "driver capture SPIR-V truncated");
        if (index == 1) spirv = selected;
        else Require(selected == spirv, "unchanged cached pipeline captured different SPIR-V");
    }
}

}

int main() {
    std::filesystem::path directory;
    int result = 0;
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        directory = std::filesystem::temp_directory_path() / ("aps5-captured-dispatch-test-" + std::to_string(std::random_device{}()));
        Require(std::filesystem::create_directory(directory), "capture test directory already exists");
        Run(directory);
        std::puts("captured dispatch tests passed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        result = 1;
    }
    std::error_code error;
    if (directory.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(directory, error);
    return result;
}
