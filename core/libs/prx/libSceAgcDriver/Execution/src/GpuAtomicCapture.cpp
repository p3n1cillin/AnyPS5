#include "prx/libSceAgcDriver/Execution/include/GpuAtomicCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
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
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::runtime_error("invalid GPU atomic capture number");
    return result;
}

void WriteFile(const std::filesystem::path& path, const char* data, std::size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) throw std::runtime_error("cannot open GPU atomic capture file");
    file.write(data, static_cast<std::streamsize>(size));
    if (!file) throw std::runtime_error("cannot write GPU atomic capture file");
    file.close();
    if (!file) throw std::runtime_error("cannot close GPU atomic capture file");
}

struct ImageCopy {
    Graphics::ShaderResources::AtomicImage image;
    VkBufferImageCopy region{};
    std::size_t bytes;
    std::shared_ptr<Graphics::Buffer> buffer;
};

std::uint32_t AtomicTexelBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R64_UINT:
    case VK_FORMAT_R64_SINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    default: throw std::runtime_error("GPU atomic capture encountered an unsupported storage format");
    }
}

void RecordCopy(const Graphics::Context& context, Graphics::Recorder& recorder, ImageCopy& copy) {
    copy.buffer = std::make_shared<Graphics::Buffer>(context, copy.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    recorder.Keep(copy.buffer, copy.bytes);
    recorder.Keep(copy.image.texture);
    const auto commands = recorder.Commands();
    VkImageMemoryBarrier source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    source.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.image = copy.image.texture->Image();
    source.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, copy.image.mip, 1, copy.region.imageSubresource.baseArrayLayer, copy.region.imageSubresource.layerCount};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &source);
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, source.image, source.newLayout, copy.buffer->Handle(), 1, &copy.region);
    source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = copy.buffer->Handle();
    host.size = VK_WHOLE_SIZE;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 1, &source);
    recorder.MarkCovered(0);
}

}

GpuAtomicCapture::GpuAtomicCapture(GpuAtomicCaptureOptions options) : options(std::move(options)) {}

GpuAtomicCaptureOptions GpuAtomicCapture::ReadOptions() {
    GpuAtomicCaptureOptions result;
    const auto* program = std::getenv("APS5_DUMP_ATOMIC_INPUT");
    if (program == nullptr) return result;
    try {
        result.program = Parse(program, 16);
        if (const auto* limit = std::getenv("APS5_DUMP_ATOMIC_INPUT_LIMIT")) {
            const auto value = Parse(limit, 10);
            if (value == 0 || value > 64) throw std::runtime_error("GPU atomic capture limit must be 1..64");
            result.limit = static_cast<std::uint32_t>(value);
        }
        if (const auto* directory = std::getenv("APS5_DUMP_ATOMIC_INPUT_DIR")) result.directory = directory;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[gpu-atomic-input] disabled: %s\n", error.what());
        result.program = 0;
    }
    return result;
}

std::uint32_t GpuAtomicCapture::Write(const Graphics::Context& context, Graphics::Recorder& recorder,
    const Graphics::ShaderResources& resources, const ShaderRecompiler::RecompileResult& shader,
    std::uint64_t program, const std::array<std::uint32_t, 3>& groups, std::uint64_t arguments,
    std::span<const std::byte> pushConstants) {
    if (options.program == 0 || program != options.program) return 0;
    std::lock_guard lock(mutex);
    if (stopped || count >= options.limit) return 0;
    const auto images = resources.AtomicImages();
    if (images.empty()) return 0;
    const auto remaining = options.byteBudget - bytes;
    std::uint64_t total = shader.spirv.size() * sizeof(std::uint32_t) + pushConstants.size();
    std::vector<ImageCopy> copies;
    for (const auto& image : images) {
        const auto& descriptor = image.texture->Descriptor();
        const auto geometry = Graphics::DescribeSurface(descriptor);
        const auto& mip = geometry.mips.at(image.mip);
        const auto layers = image.firstLayer ? 1u : geometry.imageLayers - descriptor.baseArray;
        const auto depth = std::max(geometry.imageDepth >> image.mip, 1u);
        const std::uint64_t size = static_cast<std::uint64_t>(mip.width) * mip.height * depth * layers * AtomicTexelBytes(image.texture->StorageFormat());
        if (size > 16u * 1024u * 1024u || total > remaining || size > remaining - total) {
            stopped = true;
            std::fprintf(stderr, "[gpu-atomic-input] stopped: image or session byte budget exhausted\n");
            return 0;
        }
        total += size;
        ImageCopy copy{image, {}, static_cast<std::size_t>(size), {}};
        copy.region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, image.mip, descriptor.baseArray, layers};
        copy.region.imageExtent = {mip.width, mip.height, depth};
        copies.push_back(std::move(copy));
    }
    std::ostringstream out;
    out << "{\n\"schema_version\":1,\"sequence\":" << count + 1u << ",\"program\":\"0x" << std::hex << program << std::dec;
    out << "\",\"variant_id\":\"0x" << std::hex << shader.variantId << "\",\"pipeline_variant_id\":\"0x" << shader.PipelineVariantId() << std::dec;
    out << "\",\"groups\":[" << groups[0] << ',' << groups[1] << ',' << groups[2] << "]";
    out << ",\"groups_resolved\":" << (arguments == 0 ? "true" : "false") << ",\"indirect_arguments\":\"0x" << std::hex << arguments << std::dec;
    out << "\",\"stage\":\"prepared_pre_dispatch\",\"synchronized\":true,\"gpu_atomic_image_contents\":true,\"captures_guest_buffers\":false,\"layout\":\"packed_texels_layer_then_z_then_y_then_x\",\"images\":[";
    for (std::size_t index = 0; index < copies.size(); ++index) {
        if (index != 0) out << ',';
        const auto& copy = copies[index];
        const auto& descriptor = copy.image.texture->Descriptor();
        out << "{\"file\":\"image_" << index << ".bin\",\"storage_element\":" << copy.image.storageElement;
        out << ",\"guest_address\":\"0x" << std::hex << descriptor.baseAddress << std::dec << "\",\"guest_format\":" << descriptor.format;
        out << ",\"storage_format\":" << static_cast<unsigned int>(copy.image.texture->StorageFormat()) << ",\"mip\":" << copy.image.mip;
        out << ",\"base_layer\":" << descriptor.baseArray << ",\"layers\":" << copy.region.imageSubresource.layerCount;
        out << ",\"extent\":[" << copy.region.imageExtent.width << ',' << copy.region.imageExtent.height << ',' << copy.region.imageExtent.depth << "],\"bytes\":" << copy.bytes << '}';
    }
    out << "]\n}\n";
    const auto metadata = out.str();
    if (total > remaining || metadata.size() > remaining - total) {
        stopped = true;
        std::fprintf(stderr, "[gpu-atomic-input] stopped: metadata exceeds session byte budget\n");
        return 0;
    }
    std::filesystem::path directory;
    try {
        std::error_code error;
        if (session.empty()) {
            session = options.directory / ("gpu_atomic_capture_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(options.directory, error);
            if (error || !std::filesystem::create_directory(session, error)) throw std::runtime_error("cannot create GPU atomic capture session");
        }
        directory = session / ("input_" + std::to_string(count + 1u));
        if (!std::filesystem::create_directory(directory, error)) throw std::runtime_error("cannot create GPU atomic capture directory");
    } catch (const std::exception& error) {
        stopped = true;
        std::fprintf(stderr, "[gpu-atomic-input] stopped: %s\n", error.what());
        return 0;
    }
    for (auto& copy : copies) RecordCopy(context, recorder, copy);
    recorder.Sync();
    for (auto& copy : copies) copy.buffer->Invalidate();
    try {
        for (std::size_t index = 0; index < copies.size(); ++index) {
            const auto data = copies[index].buffer->Bytes();
            WriteFile(directory / ("image_" + std::to_string(index) + ".bin"), reinterpret_cast<const char*>(data.data()), data.size());
        }
        WriteFile(directory / "shader.spv", reinterpret_cast<const char*>(shader.spirv.data()), shader.spirv.size() * sizeof(std::uint32_t));
        WriteFile(directory / "push_constants.bin", reinterpret_cast<const char*>(pushConstants.data()), pushConstants.size());
        WriteFile(directory / "input.json", metadata.data(), metadata.size());
        ++count;
        bytes += total + metadata.size();
        std::fprintf(stderr, "[gpu-atomic-input] captured sequence %u program 0x%llx (%zu images): %s\n", count, static_cast<unsigned long long>(program), copies.size(), directory.string().c_str());
        return count;
    } catch (const std::exception& error) {
        stopped = true;
        std::fprintf(stderr, "[gpu-atomic-input] stopped: %s\n", error.what());
        return 0;
    }
}

}
