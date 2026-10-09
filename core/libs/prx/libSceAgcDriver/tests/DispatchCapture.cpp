#include "prx/libSceAgcDriver/Execution/include/DispatchCapture.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open(), "capture file missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Environment(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value == nullptr ? "" : value) == 0, "cannot set capture environment");
#else
    Require((value == nullptr ? unsetenv(name) : setenv(name, value, 1)) == 0, "cannot set capture environment");
#endif
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() / ("aps5-dispatch-capture-test-" + std::to_string(std::random_device{}()));
        Require(std::filesystem::create_directory(path), "capture test directory already exists");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(path, error);
    }
};

void CheckOptions() {
    Environment("APS5_DUMP_DISPATCH", nullptr);
    Environment("APS5_DUMP_DISPATCH_LIMIT", nullptr);
    Environment("APS5_DUMP_DISPATCH_DIR", nullptr);
    Require(AgcDriver::DispatchCapture::ReadOptions().program == 0, "capture enabled by default");
    Environment("APS5_DUMP_DISPATCH", "0x1234");
    Environment("APS5_DUMP_DISPATCH_LIMIT", "2");
    Environment("APS5_DUMP_DISPATCH_DIR", "capture output");
    const auto options = AgcDriver::DispatchCapture::ReadOptions();
    Require(options.program == 0x1234 && options.limit == 2 && options.directory == "capture output", "capture options differ");
    for (const auto* program : {"", "-1", "1234tail", "0x", "10000000000000000"}) {
        Environment("APS5_DUMP_DISPATCH", program);
        Require(AgcDriver::DispatchCapture::ReadOptions().program == 0, "invalid capture program accepted");
    }
    Environment("APS5_DUMP_DISPATCH", "1234");
    for (const auto* limit : {"0", "1025", "-1", "2tail"}) {
        Environment("APS5_DUMP_DISPATCH_LIMIT", limit);
        Require(AgcDriver::DispatchCapture::ReadOptions().program == 0, "invalid capture limit accepted");
    }
    Environment("APS5_DUMP_DISPATCH", nullptr);
    Environment("APS5_DUMP_DISPATCH_LIMIT", nullptr);
    Environment("APS5_DUMP_DISPATCH_DIR", nullptr);
}

void CheckCaptures(const std::filesystem::path& root) {
    const std::array<std::uint32_t, 1> code{0xbf810000};
    std::array<std::byte, 4> capturedBytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{0x2000, capturedBytes}}};
    const std::array<std::uint32_t, 2> userData{17, 23};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Compute, 0x1234, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userData = userData;
    request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{{32, 2, 1}, 128, {true, false, false}, false, 2};
    request.context.memory = memory;
    ShaderRecompiler::RecompileResult compiled{};
    compiled.spirv = std::vector<std::uint32_t>{0x07230203, 0x00010400, 0, 1, 0};
    compiled.variantId = 0x1122334455667788;
    compiled.specializationId = 0x8877665544332211;
    compiled.hostSubgroupSize = 32;
    compiled.specialization = {{1024, 37}, {1025, 0xffffffff}};
    compiled.pushConstants = {std::byte{0}, std::byte{255}};
    ShaderRecompiler::DescriptorBinding binding{};
    binding.descriptorSet = 0;
    binding.binding = 45;
    binding.count = 2;
    binding.guestDescriptor = {11, 22, 33};
    binding.imageAtomic = {true, false};
    binding.imageWritten = {true, true};
    compiled.bindings.push_back(binding);
    AgcDriver::DispatchCaptureInfo info{3, {4096, 1, 1}, 0, 0xabcdef, false, false};

    AgcDriver::DispatchCapture disabled({0, 2, 1024 * 1024, root / "disabled"});
    Require(!disabled.Write(request, compiled, info), "disabled capture wrote files");
    AgcDriver::DispatchCapture unmatched({0x9999, 2, 1024 * 1024, root / "unmatched"});
    Require(!unmatched.Write(request, compiled, info), "unmatched capture wrote files");
    Require(!std::filesystem::exists(root / "disabled") && !std::filesystem::exists(root / "unmatched"), "inactive capture created directory");

    AgcDriver::DispatchCapture capture({0x1234, 2, 1024 * 1024, root / "captures"});
    const auto first = capture.Write(request, compiled, info);
    Require(first.has_value(), "first capture missing");
    const auto serialized = Read(*first / "request.req");
    const auto decoded = ShaderRecompiler::RequestSerializer{}.Deserialize(serialized);
    Require(decoded.request.shader.codeAddress == 0x1234 && decoded.userData == std::vector<std::uint32_t>{17, 23}, "request address or user data differ");
    Require(decoded.memoryBytesStorage[0] == std::vector<std::byte>(capturedBytes.begin(), capturedBytes.end()), "captured request memory differs");
    const auto spirv = Read(*first / "shader.spv");
    Require(spirv == std::string(reinterpret_cast<const char*>(compiled.spirv.data()), compiled.spirv.size() * 4), "selected SPIR-V differs");
    const auto firstMetadata = Read(*first / "dispatch.json");
    Require(firstMetadata.find("\"pipeline_variant_id\":\"0x8877665544332211\"") != std::string::npos, "pipeline ID loses precision");
    Require(firstMetadata.find("\"groups\":[4096,1,1]") != std::string::npos, "dispatch groups differ");
    Require(firstMetadata.find("\"image_atomic\":[true,false]") != std::string::npos, "atomic image flags differ");
    Require(firstMetadata.find("\"captures_gpu_contents\":false") != std::string::npos, "capture limitation missing");
    Require(firstMetadata.find("{\"id\":1025,\"value\":4294967295}") != std::string::npos, "specialization differs");
    Require(firstMetadata.find("\"push_constants\":[0,255]") != std::string::npos, "push constants differ");

    capturedBytes[0] = std::byte{99};
    compiled.spirv[2] = 99;
    compiled.bindings[0].guestDescriptor[0] = 44;
    info.cacheHit = true;
    info.dataHit = true;
    info.indirectArguments = 0x5000;
    const auto second = capture.Write(request, compiled, info);
    Require(second && second != first, "repeated address capture was deduplicated");
    const auto secondDecoded = ShaderRecompiler::RequestSerializer{}.Deserialize(Read(*second / "request.req"));
    Require(secondDecoded.memoryBytesStorage[0][0] == std::byte{99}, "cached capture lost refreshed constants");
    const auto secondMetadata = Read(*second / "dispatch.json");
    Require(secondMetadata.find("\"cache_hit\":true,\"data_hit\":true") != std::string::npos, "cache hit context differs");
    Require(secondMetadata.find("\"groups_resolved\":false") != std::string::npos, "indirect groups misrepresented");
    Require(secondMetadata.find("\"guest_descriptor\":[44,22,33]") != std::string::npos, "live descriptor differs");
    Require(Read(*first / "request.req") == serialized && Read(*first / "dispatch.json") == firstMetadata, "earlier capture overwritten");
    Require(!capture.Write(request, compiled, info), "capture count limit exceeded");

    AgcDriver::DispatchCapture budget({0x1234, 2, 1, root / "budget"});
    Require(!budget.Write(request, compiled, info) && !std::filesystem::exists(root / "budget"), "capture byte budget exceeded");
    const auto firstBytes = serialized.size() + spirv.size() + firstMetadata.size();
    AgcDriver::DispatchCapture cumulative({0x1234, 2, firstBytes + 256, root / "cumulative"});
    Require(cumulative.Write(request, compiled, info).has_value(), "budget should hold one capture");
    Require(!cumulative.Write(request, compiled, info), "cumulative byte budget exceeded");
    const auto notDirectory = root / "file";
    { std::ofstream file(notDirectory); file << "existing file"; }
    AgcDriver::DispatchCapture failed({0x1234, 2, 1024 * 1024, notDirectory});
    Require(!failed.Write(request, compiled, info), "I/O failure reported success");
    Require(!failed.Write(request, compiled, info), "I/O failure retried indefinitely");
    Require(Read(notDirectory) == "existing file", "I/O failure changed unrelated file");
}

}

int main() {
    try {
        CheckOptions();
        const TemporaryDirectory directory;
        CheckCaptures(directory.path);
        std::puts("dispatch capture tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
