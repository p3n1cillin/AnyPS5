#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GPUATOMICCAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GPUATOMICCAPTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver {

struct GpuAtomicCaptureOptions {
    std::uint64_t program = 0;
    std::uint32_t limit = 8;
    std::uint64_t byteBudget = 64u * 1024u * 1024u;
    std::filesystem::path directory = ".";
    std::vector<std::pair<std::uint32_t, std::uint32_t>> buffers;
};

class GpuAtomicCapture {
public:
    explicit GpuAtomicCapture(GpuAtomicCaptureOptions options);
    static GpuAtomicCaptureOptions ReadOptions();
    std::uint32_t Write(const Graphics::Context& context, Graphics::Recorder& recorder,
        const Graphics::ShaderResources& resources, const ShaderRecompiler::RecompileResult& shader,
        std::uint64_t program, const std::array<std::uint32_t, 3>& groups, std::uint64_t arguments,
        std::span<const std::byte> pushConstants);

private:
    GpuAtomicCaptureOptions options;
    std::mutex mutex;
    std::filesystem::path session;
    std::uint32_t count = 0;
    std::uint64_t bytes = 0;
    bool stopped = false;
};

}

#endif
