#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <bit>
#include <iostream>

namespace {

using namespace DepthFastClearHarness;
using AgcDriver::Graphics::HtileWordOffset;

alignas(256) std::array<std::uint32_t, 16384> Depth{};
alignas(256) std::array<std::uint32_t, HtileBytes / 4> Htile{};

std::size_t Word(std::uint32_t x, std::uint32_t y) {
    return HtileWordOffset({Width, Height}, x, y) / 4;
}

void Check(std::uint32_t left, std::uint32_t right) {
    for (std::uint32_t y = 0; y < Height; y += 8) {
        for (std::uint32_t x = 0; x < Width; x += 8) Require(Htile[Word(x, y)] == (x < Width / 2 ? left : right), "resummarized metadata does not match the native depth words or preserved coverage");
    }
    Require(Htile[4] == 0xabcdef0f, "resummarization changed metadata padding");
    Require(Depth.back() == 0xdeadbeef, "resummarization changed depth padding");
}

void Rasterize(AgcDriver::VulkanDevice& device, const Shaders& compiled, const AgcDriver::Graphics::DepthTarget& target, VkRect2D scissor, std::uint32_t instances = 1, bool depthWrite = false) {
    alignas(256) static constexpr std::array<std::uint32_t, 1> code{0xbf810000};
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = true;
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, {}, std::nullopt, pixel, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto fragment = ShaderRecompiler::Recompile(request);
    const auto rectangle = ShaderRecompiler::BuildRectListShaders(compiled.vertex, fragment, device.Target());
    const std::array<AgcDriver::Graphics::CompiledShader, 4> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0}, {ShaderStage::TessellationControl, &rectangle.control, 0},
        {ShaderStage::TessellationEvaluation, &rectangle.evaluation, 0}, {ShaderStage::Fragment, &fragment, 0}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 32, 32, std::nullopt, std::nullopt};
    state.depth = target;
    state.depthResummarize = !depthWrite;
    state.depthTest = depthWrite;
    state.depthWrite = depthWrite;
    state.rectList = true;
    state.renderExtent = target.extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = scissor;
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    device.Draw(state, {0, 3, 0, instances, 0, false}, shaders);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = Compile(*device);
        Depth.fill(0xdeadbeef);
        Htile.fill(0xfffc000f);
        Htile[4] = 0xabcdef0f;
        const Surface surface{reinterpret_cast<std::uintptr_t>(Depth.data()), 0, reinterpret_cast<std::uintptr_t>(Htile.data()), false, VK_FORMAT_D32_SFLOAT};
        Require(Draw(*device, shaders, surface, {.depthWrite = true}) == Covered, "depth write setup failed");
        const AgcDriver::Graphics::DepthTarget target{surface.depth, 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, 1.0f, 0, surface.htile, false};
        std::array<std::uint8_t, Width * Height> coverage{};
        for (std::uint32_t y = 0; y < Height; ++y) std::fill_n(coverage.begin() + y * Width, Width / 2, 1);
        device->ResummarizeDepth(target, coverage);
        Check(0x8002000f, 0xfffc000f);
        Require(Depth.front() == std::bit_cast<std::uint32_t>(0.5f), "resummarization did not publish the coherent GPU depth");
        Require(std::count(Depth.begin(), Depth.end(), std::bit_cast<std::uint32_t>(0.5f)) == Width / 2 * Height, "resummarization published uncovered depth texels");
        const auto previous = Htile;
        coverage[0] = 0;
        bool partialRejected = false;
        try { device->ResummarizeDepth(target, coverage); } catch (const std::exception&) { partialRejected = true; }
        Require(partialRejected && Htile == previous, "partial-tile coverage changed metadata instead of rejecting the unmeasured operation");
        coverage.fill(1);
        Htile.fill(0);
        Htile[4] = 0xabcdef0f;
        auto clear = target;
        clear.clearDepth = 0.1234567f;
        device->ResummarizeDepth(clear, coverage);
        Check(0x1f987e6f, 0x1f987e6f);
        Require(Depth.front() == std::bit_cast<std::uint32_t>(clear.clearDepth), "pending fast clear was not materialized before resummarization");
        coverage.fill(0);
        const auto untouched = Htile;
        device->ResummarizeDepth(clear, coverage);
        Require(Htile == untouched, "empty coverage changed metadata");
        Htile.fill(0xfffc000f);
        Htile[4] = 0xabcdef0f;
        Rasterize(*device, shaders, clear, {{0, 0}, {Width / 2, Height}});
        Check(0x1f987e6f, 0xfffc000f);
        const auto scissored = Htile;
        Rasterize(*device, shaders, clear, {{0, 0}, {Width, Height}}, 0);
        Require(Htile == scissored, "zero instances produced resummarization coverage");
        bool rasterPartialRejected = false;
        try { Rasterize(*device, shaders, clear, {{0, 0}, {7, Height}}); } catch (const std::exception&) { rasterPartialRejected = true; }
        Require(rasterPartialRejected && Htile == scissored, "partial raster coverage changed metadata instead of rejecting");
        Rasterize(*device, shaders, clear, {{32, 0}, {Width / 2, Height}});
        Check(0x1f987e6f, 0x1f987e6f);
        Rasterize(*device, shaders, clear, {{0, 0}, {Width, Height}}, 1, true);
        coverage.fill(1);
        device->ResummarizeDepth(clear, coverage);
        Check(0x8002000f, 0x8002000f);
        std::puts("depth resummarization tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
