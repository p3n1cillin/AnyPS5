#include "prx/libSceAgcDriver/Execution/include/DispatchCapture.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace AgcDriver {
namespace {

std::uint64_t Parse(const char* value, int base) {
    std::string_view text(value);
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) text.remove_prefix(2);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text.empty()) {
        throw std::runtime_error("invalid dispatch capture number");
    }
    return result;
}

void WriteFile(const std::filesystem::path& path, const char* data, std::size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) throw std::runtime_error("cannot open dispatch capture file");
    file.write(data, static_cast<std::streamsize>(size));
    if (!file) throw std::runtime_error("cannot write dispatch capture file");
    file.close();
    if (!file) throw std::runtime_error("cannot close dispatch capture file");
}

template <class TValues>
void Array(std::ostream& out, const TValues& values) {
    out << '[';
    bool comma = false;
    for (const auto value : values) {
        if (comma) out << ',';
        out << value;
        comma = true;
    }
    out << ']';
}

std::string Metadata(const ShaderRecompiler::RecompileRequest& request,
    const ShaderRecompiler::RecompileResult& compiled, const DispatchCaptureInfo& info, std::uint32_t sequence) {
    std::ostringstream out;
    out << std::boolalpha;
    out << "{\n\"schema_version\":1,\"sequence\":" << sequence;
    out << ",\"program\":\"0x" << std::hex << request.shader.codeAddress << std::dec << '"';
    out << ",\"variant_id\":\"0x" << std::hex << compiled.variantId << std::dec << '"';
    out << ",\"pipeline_variant_id\":\"0x" << std::hex << compiled.PipelineVariantId() << std::dec << '"';
    out << ",\"cache_key\":\"0x" << std::hex << info.cacheKey << std::dec << '"';
    out << ",\"indirect_arguments\":\"0x" << std::hex << info.indirectArguments << std::dec << '"';
    out << ",\"queue\":" << info.queue << ",\"groups\":";
    Array(out, info.groups);
    out << ",\"groups_resolved\":" << (info.indirectArguments == 0);
    out << ",\"cache_hit\":" << info.cacheHit << ",\"data_hit\":" << info.dataHit;
    out << ",\"host_subgroup_size\":" << compiled.hostSubgroupSize;
    out << ",\"spirv_words\":" << compiled.spirv.size();
    out << ",\"capture_stage\":\"driver_selected_shader\",\"captures_gpu_contents\":false,\"memory_source\":\"validated_capture_regions\"";
    out << ",\"specialization\":[";
    for (std::size_t i = 0; i < compiled.specialization.size(); ++i) {
        if (i != 0) out << ',';
        const auto& constant = compiled.specialization[i];
        out << "{\"id\":" << constant.id << ",\"value\":" << constant.value << '}';
    }
    out << "],\"push_constants\":[";
    for (std::size_t i = 0; i < compiled.pushConstants.size(); ++i) {
        if (i != 0) out << ',';
        out << std::to_integer<unsigned int>(compiled.pushConstants[i]);
    }
    out << "],\"bindings\":[";
    for (std::size_t i = 0; i < compiled.bindings.size(); ++i) {
        if (i != 0) out << ',';
        const auto& binding = compiled.bindings[i];
        out << "{\"kind\":" << static_cast<unsigned int>(binding.kind);
        out << ",\"role\":" << static_cast<unsigned int>(binding.role);
        out << ",\"set\":" << binding.descriptorSet << ",\"binding\":" << binding.binding;
        out << ",\"count\":" << binding.count << ",\"read_only\":" << binding.readOnly;
        out << ",\"image_shape\":";
        if (binding.imageShape) out << static_cast<unsigned int>(*binding.imageShape);
        else out << "null";
        out << ",\"guest_descriptor\":";
        Array(out, binding.guestDescriptor);
        out << ",\"image_written\":";
        Array(out, binding.imageWritten);
        out << ",\"image_atomic\":";
        Array(out, binding.imageAtomic);
        out << ",\"image_atomic64\":";
        Array(out, binding.imageAtomic64);
        out << ",\"image_depth_compare\":";
        Array(out, binding.imageDepthCompare);
        out << ",\"sampler_depth_compare\":";
        Array(out, binding.samplerDepthCompare);
        out << ",\"buffer_written\":";
        Array(out, binding.bufferWritten);
        out << ",\"buffer_atomic\":";
        Array(out, binding.bufferAtomic);
        out << '}';
    }
    out << "]\n}\n";
    return out.str();
}

}

DispatchCapture::DispatchCapture(DispatchCaptureOptions options) : options(std::move(options)) {}

DispatchCaptureOptions DispatchCapture::ReadOptions() {
    DispatchCaptureOptions result;
    const auto* program = std::getenv("APS5_DUMP_DISPATCH");
    if (program == nullptr) return result;
    try {
        result.program = Parse(program, 16);
        if (const auto* limit = std::getenv("APS5_DUMP_DISPATCH_LIMIT")) {
            const auto value = Parse(limit, 10);
            if (value == 0 || value > 1024) throw std::runtime_error("dispatch capture limit must be 1..1024");
            result.limit = static_cast<std::uint32_t>(value);
        }
        if (const auto* directory = std::getenv("APS5_DUMP_DISPATCH_DIR")) result.directory = directory;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[dispatch-capture] disabled: %s\n", error.what());
        result.program = 0;
    }
    return result;
}

std::optional<std::filesystem::path> DispatchCapture::Write(const ShaderRecompiler::RecompileRequest& request,
    const ShaderRecompiler::RecompileResult& compiled, const DispatchCaptureInfo& info) {
    if (options.program == 0 || request.shader.codeAddress != options.program) return std::nullopt;
    std::lock_guard lock(mutex);
    if (stopped || count >= options.limit) return std::nullopt;
    try {
        const auto remaining = options.byteBudget - bytes;
        std::uint64_t inputBytes = compiled.spirv.size() * sizeof(std::uint32_t);
        const auto addBytes = [&](std::size_t size) {
            if (inputBytes > remaining || size > remaining - inputBytes) throw std::runtime_error("dispatch capture byte budget exhausted");
            inputBytes += size;
        };
        addBytes(request.shader.code.size_bytes());
        addBytes(request.shader.header.size_bytes());
        for (const auto& region : request.context.memory) addBytes(region.bytes.size_bytes());
        const auto serialized = ShaderRecompiler::RequestSerializer{}.Serialize(request);
        const auto metadata = Metadata(request, compiled, info, count + 1u);
        const auto spirvBytes = compiled.spirv.size() * sizeof(std::uint32_t);
        const auto totalBytes = serialized.size() + metadata.size() + spirvBytes;
        if (totalBytes > remaining) throw std::runtime_error("dispatch capture byte budget exhausted");
        if (session.empty()) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            session = options.directory / ("dispatch_capture_" + std::to_string(stamp));
            std::error_code error;
            std::filesystem::create_directories(options.directory, error);
            if (error) throw std::runtime_error("cannot create dispatch capture parent: " + error.message());
            if (!std::filesystem::create_directory(session, error)) throw std::runtime_error("cannot create dispatch capture session: " + error.message());
        }
        const auto directory = session / ("dispatch_" + std::to_string(count + 1u));
        std::error_code error;
        if (!std::filesystem::create_directory(directory, error)) throw std::runtime_error("cannot create dispatch capture: " + error.message());
        WriteFile(directory / "request.req", serialized.data(), serialized.size());
        WriteFile(directory / "shader.spv", reinterpret_cast<const char*>(compiled.spirv.data()), spirvBytes);
        WriteFile(directory / "dispatch.json", metadata.data(), metadata.size());
        ++count;
        bytes += totalBytes;
        std::fprintf(stderr, "[dispatch-capture] shader 0x%llx sequence %u variant 0x%llx q0x%x groups %u,%u,%u: %s\n",
            static_cast<unsigned long long>(request.shader.codeAddress), count,
            static_cast<unsigned long long>(compiled.PipelineVariantId()), info.queue, info.groups[0], info.groups[1], info.groups[2], directory.string().c_str());
        return directory;
    } catch (const std::exception& error) {
        stopped = true;
        std::fprintf(stderr, "[dispatch-capture] stopped: %s\n", error.what());
        return std::nullopt;
    }
}

}
