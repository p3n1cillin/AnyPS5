#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCAPTURE_HPP

#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>

namespace AgcDriver {

struct DispatchCaptureOptions {
    std::uint64_t program = 0;
    std::uint32_t limit = 64;
    std::uint64_t byteBudget = 256u * 1024u * 1024u;
    std::filesystem::path directory = ".";
};

struct DispatchCaptureInfo {
    std::uint32_t queue;
    std::array<std::uint32_t, 3> groups;
    std::uint64_t indirectArguments;
    std::uint64_t cacheKey;
    bool cacheHit;
    bool dataHit;
};

class DispatchCapture {
public:
    explicit DispatchCapture(DispatchCaptureOptions options);
    static DispatchCaptureOptions ReadOptions();
    std::optional<std::filesystem::path> Write(const ShaderRecompiler::RecompileRequest& request,
        const ShaderRecompiler::RecompileResult& compiled, const DispatchCaptureInfo& info);

private:
    DispatchCaptureOptions options;
    std::mutex mutex;
    std::filesystem::path session;
    std::uint32_t count = 0;
    std::uint64_t bytes = 0;
    bool stopped = false;
};

}

#endif
