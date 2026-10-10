#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t DepthAddress = 0x7f0000000000ull;
constexpr VkExtent2D Extent{64, 64};
constexpr std::uint32_t Format32Float = 22;

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) ++family;
            Require(family < count, "no Vulkan graphics queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &robustness;
            function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(context.physical, &features);
            Require(robustness.nullDescriptor == VK_TRUE, "the test device does not support null descriptors");
            robustness.robustBufferAccess2 = VK_FALSE;
            robustness.robustImageAccess2 = VK_FALSE;
            const char* extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
            device.enabledExtensionCount = 1;
            device.ppEnabledExtensionNames = &extension;
            device.pNext = &robustness;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.nullDescriptors = true;
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.imageFormatProperties = function<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) ClearDepthSurfaces(context.device);
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

constexpr std::size_t Block = 65536;

PFN_vkGetDeviceProcAddr uploadResolver = nullptr;
std::uint32_t newDepthUploads = 0;
VkBuffer depthStaging = VK_NULL_HANDLE;
PFN_vkCmdCopyBufferToImage copyDepthImage = nullptr;

VKAPI_ATTR void VKAPI_CALL captureDepthCopy(VkCommandBuffer commands, VkBuffer buffer, VkImage image, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    if (count == 2) depthStaging = buffer;
    copyDepthImage(commands, buffer, image, layout, count, regions);
}

VKAPI_ATTR VkResult VKAPI_CALL refuseDepthUpload(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* callbacks, VkBuffer* buffer) {
    if (info->size == Block && info->usage == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        ++newDepthUploads;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return reinterpret_cast<PFN_vkCreateBuffer>(uploadResolver(device, "vkCreateBuffer"))(device, info, callbacks, buffer);
}

PFN_vkVoidFunction VKAPI_CALL depthUploadProc(VkDevice device, const char* name) {
    if (std::string_view(name) == "vkCreateBuffer") return reinterpret_cast<PFN_vkVoidFunction>(refuseDepthUpload);
    if (std::string_view(name) == "vkCmdCopyBufferToImage") return reinterpret_cast<PFN_vkVoidFunction>(captureDepthCopy);
    return uploadResolver(device, name);
}

void* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

DepthTarget Depth(std::uint64_t address = DepthAddress, VkExtent2D extent = Extent) {
    return {address, 0, extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
}

GuestTextureResource View(std::uint32_t width, std::uint32_t height, std::uint64_t address = DepthAddress) {
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = width;
    resource.height = height;
    resource.mipCount = 1;
    resource.dimension = TextureDimension::k2D;
    resource.format = Format32Float;
    return resource;
}

constexpr VkComponentMapping Identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

std::shared_ptr<Texture> Sample(const Context& context, const GuestTextureResource& resource) {
    const std::array<std::uint32_t, 8> words{0, 0, 0, 0, resource.width, resource.height, 0, 0};
    return DepthSurfaceTexture(context, words, resource, Identity);
}

bool Refused(const Context& context, const GuestTextureResource& resource) {
    try {
        Sample(context, resource);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("is not implemented") != std::string::npos;
    }
    return false;
}

void ExpectStorage(const Context& context, const StorageTexture& storage, float expected) {
    Buffer pixels(context, Extent.width * Extent.height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CommandBatch batch(context);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {Extent.width, Extent.height, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(batch.Handle(), storage.Image(), VK_IMAGE_LAYOUT_GENERAL, pixels.Handle(), 1, &region);
    batch.SubmitAndWait();
    pixels.Invalidate();
    for (std::size_t offset = 0; offset < pixels.Bytes().size(); offset += sizeof(float)) {
        float actual;
        std::memcpy(&actual, pixels.Bytes().data() + offset, sizeof(actual));
        Require(actual == expected, "depth and storage image transfers changed the depth value");
    }
}

void StorageRoundTrip(const Context& context, std::uint64_t address) {
    auto target = Depth(address);
    target.clearDepth = 0.25f;
    DepthSurfaceView(context, target);
    TextureDetiler detiler(context);
    auto storage = std::make_shared<StorageTexture>(context, detiler, View(Extent.width, Extent.height, address), 0);
    SeedStorageFromDepth(context, storage);
    ExpectStorage(context, *storage, 0.25f);
    {
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue clear{{0.75f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(batch.Handle(), storage->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        batch.SubmitAndWait();
    }
    DepthSurfaceView(context, target);
    auto copy = std::make_shared<StorageTexture>(context, detiler, View(Extent.width, Extent.height, address), 0);
    SeedStorageFromDepth(context, copy);
    ExpectStorage(context, *copy, 0.75f);
    Require(DepthSurfaceAt(address), "a genuine depth storage write retired the depth surface");
}

void ColorStorageWriter(const Context& context, std::uint64_t address) {
    TextureDetiler detiler(context);
    auto configured = context;
    configured.detiler = &detiler;
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding{};
    binding.kind = ShaderRecompiler::DescriptorKind::StorageImage;
    binding.role = ShaderRecompiler::DescriptorRole::GuestImages;
    binding.binding = ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding;
    binding.count = 1;
    binding.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    binding.imageWritten.resize(1, true);
    binding.guestDescriptor = {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (36u << 20u) | (((Extent.width - 1u) & 3u) << 30u),
        ((Extent.width - 1u) >> 2u) | ((Extent.height - 1u) << 14u),
        0x90000facu, 0, 0, 0, 0,
    };
    program.bindings.push_back(std::move(binding));
    const CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    try {
        program.bindings[0].guestDescriptor[1] = (program.bindings[0].guestDescriptor[1] & ~(0x1ffu << 20u)) | (Format32Float << 20u);
        {
            ShaderResources resources(configured, shader);
            Require(DepthSurfaceAt(address), "binding a genuine depth storage writer retired the depth surface");
        }
        program.bindings[0].guestDescriptor[1] = (program.bindings[0].guestDescriptor[1] & ~(0x1ffu << 20u)) | (36u << 20u);
        program.bindings[0].imageWritten[0] = false;
        bool refused = false;
        try {
            ShaderResources resources(configured, shader);
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("storage image access to depth surface") != std::string::npos;
        }
        Require(refused && DepthSurfaceAt(address), "an incompatible read-only storage view silently retired the live depth surface");
        program.bindings[0].imageWritten[0] = true;
        ShaderResources resources(configured, shader);
        Require(!DepthSurfaceAt(address), "a color storage writer at the depth plane's own address left the depth surface live");
        DepthSurfaceView(context, Depth(address));
        Require(resources.Revalidate(shader), "a color storage writer could not revalidate after a depth rebind");
        Require(!DepthSurfaceAt(address), "revalidating a color storage writer left the rebound depth surface live");
    } catch (...) {
        ClearCachedTextures(context.device);
        throw;
    }
    ClearCachedTextures(context.device);
}

void DepthArraySharedPool(const Context& context, bool recorded) {
    alignas(256) static std::array<std::uint32_t, 2 * Block / 4> depth;
    depth.fill(0x3e800000);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto configured = context;
    configured.bufferPool = std::make_shared<BufferPool>(context);
    TextureDetiler detiler(configured);
    configured.detiler = &detiler;
    const auto address = reinterpret_cast<std::uintptr_t>(depth.data());
    constexpr VkExtent2D extent{128, 128};
    DepthSurfaceView(configured, Depth(address, extent));
    {
        Buffer unused(configured, Block, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    uploadResolver = context.deviceProc;
    copyDepthImage = context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    newDepthUploads = 0;
    depthStaging = VK_NULL_HANDLE;
    configured.deviceProc = depthUploadProc;
    auto resource = View(extent.width, extent.height, address);
    resource.dimension = TextureDimension::k2DArray;
    resource.depthOrLastArray = 1;
    resource.tileMode = TextureTileMode::kZ64KBX;
    std::unique_ptr<Recorder> recorder;
    if (recorded) {
        recorder = std::make_unique<Recorder>(configured);
        configured.recorder = recorder.get();
        recorder->Activate();
    }
    try {
        const auto sampled = Sample(configured, resource);
        Require(sampled != nullptr && sampled->View() != VK_NULL_HANDLE && sampled->SampledViewRange(false).type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && sampled->SampledViewRange(false).layers == VK_REMAINING_ARRAY_LAYERS && sampled->RefreshedPerUse(), "a mixed resident/guest depth array did not produce its refreshed array view");
        Require(newDepthUploads == 0, "a depth array upload ignored the caller's reusable host buffer");
        if (recorder != nullptr) {
            recorder->Submit();
            Require(recorder->InFlightKeptBytes() == Block, "a recorded depth array omitted its guest upload from the kept-byte budget");
            recorder->Sync();
            Require(recorder->InFlightKeptBytes() == 0, "a completed depth array retained its upload byte count");
        }
        Require(depthStaging != VK_NULL_HANDLE, "the mixed depth array did not copy its linear staging buffer");
        Buffer pixels(context, 2 * Block, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{0, 0, 2 * Block};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(batch.Handle(), depthStaging, pixels.Handle(), 1, &region);
        batch.SubmitAndWait();
        pixels.Invalidate();
        for (std::size_t offset = 0; offset < pixels.Bytes().size(); offset += sizeof(float)) {
            float actual;
            std::memcpy(&actual, pixels.Bytes().data() + offset, sizeof(actual));
            Require(actual == (offset < Block ? 1.0f : 0.25f), "the mixed resident/guest depth array changed a layer's pixels");
        }
    } catch (...) {
        recorder.reset();
        ClearDepthSurfaces(context.device);
        throw;
    }
    recorder.reset();
    ClearDepthSurfaces(context.device);
}

void ResummarizeStorageWriter(const Context& context) {
    alignas(256) static std::array<std::uint32_t, 16384> fallback;
    const auto watched = AgcDriver::GuestMemory::WriteWatched();
    const auto depth = watched ? std::span(static_cast<std::uint32_t*>(AllocateWatched(Block)), fallback.size()) : std::span(fallback);
    alignas(256) static std::array<std::uint32_t, 8192> metadata;
    std::fill(depth.begin(), depth.end(), 0xdeadbeef);
    metadata.fill(0xfffc000f);
    metadata[4] = 0xabcdef0f;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    TextureDetiler detiler(context);
    auto configured = context;
    configured.detiler = &detiler;
    auto target = Depth(reinterpret_cast<std::uintptr_t>(depth.data()));
    target.clearDepth = 0.25f;
    target.htileAddress = reinterpret_cast<std::uintptr_t>(metadata.data());
    DepthSurfaceView(configured, target);
    auto storage = std::make_shared<StorageTexture>(configured, detiler, View(Extent.width, Extent.height, target.address), 0);
    SeedStorageFromDepth(configured, storage);
    {
        CommandBatch batch(configured);
        RecordMemoryBarrier(configured, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue clear{{0.75f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        configured.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(batch.Handle(), storage->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        batch.SubmitAndWait();
    }
    std::array<std::uint8_t, Extent.width * Extent.height> coverage;
    coverage.fill(1);
    ResummarizeDepthSurface(configured, target, coverage);
    for (std::uint32_t y = 0; y < Extent.height; y += 8) {
        for (std::uint32_t x = 0; x < Extent.width; x += 8) Require(metadata[HtileWordOffset(Extent, x, y) / 4] == 0xc003000f, "resummarization did not take the pending storage depth writer");
    }
    float first;
    std::memcpy(&first, depth.data(), sizeof(first));
    Require(first == 0.75f && depth.back() == 0xdeadbeef && metadata[4] == 0xabcdef0f, "storage resummarization changed depth data or padding");
    SeedStorageFromDepth(configured, storage);
    {
        Buffer input(configured, Extent.width * Extent.height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for (std::uint32_t y = 0; y < Extent.height; ++y) {
            for (std::uint32_t x = 0; x < Extent.width; ++x) {
                const float value = ((x ^ y) & 1u) != 0 ? 0.8f : 0.2f;
                std::memcpy(input.Bytes().data() + (y * Extent.width + x) * sizeof(float), &value, sizeof(value));
            }
        }
        CommandBatch batch(configured);
        RecordMemoryBarrier(configured, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {Extent.width, Extent.height, 1};
        configured.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(batch.Handle(), input.Handle(), storage->Image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        batch.SubmitAndWait();
    }
    ResummarizeDepthSurface(configured, target, coverage);
    for (std::uint32_t y = 0; y < Extent.height; y += 8) {
        for (std::uint32_t x = 0; x < Extent.width; x += 8) Require(metadata[HtileWordOffset(Extent, x, y) / 4] == 0xcccccccf, "mixed-depth resummarization differs from the native checkerboard word");
    }
    if (watched) {
        SeedStorageFromDepth(configured, storage);
        depth.front() = 0x3f600000;
        AgcDriver::GuestMemory::MarkWritten(target.address, 4);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        const auto before = metadata;
        bool rejected = false;
        try { ResummarizeDepthSurface(configured, target, coverage); } catch (const std::exception&) { rejected = true; }
        Require(rejected && metadata == before && depth.front() == 0x3f600000, "unresolved CPU depth writes published stale GPU metadata");
    }
}

void Run(const Context& context) {
    const auto wide = View(2 * Extent.width, Extent.height);
    const auto exact = View(Extent.width, Extent.height);
    const auto depthBytes = DepthSliceBytes(Extent, 4);

    DepthSurfaceView(context, Depth());
    Require(DepthSurfaceAt(DepthAddress), "a bound depth surface is not found at its address");
    Require(Refused(context, wide), "depth, then a view of another extent: the live depth surface did not refuse it");
    Require(Sample(context, exact) != nullptr, "depth, then its own view: the depth surface did not serve it");

    RetireDepthSurfaces(context.device, DepthAddress + depthBytes, 0x10000);
    Require(Refused(context, wide), "a write past the depth plane retired the depth surface");

    RetireDepthSurfaces(context.device, DepthAddress + 0x100, 0x1000);
    Require(!DepthSurfaceAt(DepthAddress), "a color target over the depth plane left the depth surface live");
    Require(Sample(context, wide) == nullptr, "depth, then a color target, then a view of another extent: the view was not left to guest memory");
    Require(Sample(context, exact) == nullptr, "depth, then a color target, then its own view: the retired depth surface served it");

    DepthSurfaceView(context, Depth());
    Require(DepthSurfaceAt(DepthAddress), "binding the depth target again did not bring the depth surface back");
    Require(Refused(context, wide), "depth, color target, depth again, then a view of another extent: the live depth surface did not refuse it");

    const auto shared = DepthAddress + 0x1000000;
    const VkExtent2D wideExtent{2 * Extent.width, Extent.height};
    const auto sharedWide = View(wideExtent.width, wideExtent.height, shared);
    const auto sharedExact = View(Extent.width, Extent.height, shared);
    DepthSurfaceView(context, Depth(shared, wideExtent));
    DepthSurfaceView(context, Depth(shared));
    DepthSurfaceView(context, Depth(shared, wideExtent));
    Require(Sample(context, sharedWide) != nullptr, "two depth surfaces at one address, the older one bound last, then its own view: it did not serve it");
    Require(Refused(context, sharedExact), "two depth surfaces at one address, the older one bound last, then the other's view: the live depth surface did not refuse it");
    DepthSurfaceView(context, Depth(shared));
    Require(Sample(context, sharedExact) != nullptr, "two depth surfaces at one address, the newer one bound last, then its own view: it did not serve it");
    Require(Refused(context, sharedWide), "two depth surfaces at one address, the newer one bound last, then the other's view: the live depth surface did not refuse it");

    if (!AgcDriver::GuestMemory::WriteWatched()) {
        Require(Refused(context, wide), "without write watching, a view of another extent was not refused");
        std::puts("guest memory is not write-watched here: the CPU write order is not exercised");
        return;
    }

    auto* memory = static_cast<volatile std::uint8_t*>(AllocateWatched(2 * Block));
    const auto watched = reinterpret_cast<std::uint64_t>(memory);
    Require(depthBytes <= 2 * Block, "the depth plane does not fit the watched block");
    const auto watchedWide = View(2 * Extent.width, Extent.height, watched);
    const auto watchedExact = View(Extent.width, Extent.height, watched);
    AgcDriver::GuestMemory::CollectWrites(watched, 2 * Block);

    DepthSurfaceView(context, Depth(watched));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Refused(context, watchedWide), "depth, then a view of another extent: the live depth surface over watched memory did not refuse it");

    memory[8] = 0x5a;
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Sample(context, watchedExact) != nullptr, "depth, then a CPU write, then its own view: the depth surface did not serve it");
    Require(Sample(context, watchedWide) == nullptr, "depth, then a CPU write, then a view of another extent: the view was not left to guest memory");
    Require(!DepthSurfaceAt(watched), "depth, then a CPU write, then a view of another extent: the depth surface stayed live");

    memory[16] = 0xa5;
    AgcDriver::GuestMemory::BumpCollectEpoch();
    DepthSurfaceView(context, Depth(watched));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Refused(context, watchedWide), "a CPU write, then depth, then a view of another extent: the live depth surface did not refuse it");

    AgcDriver::GuestMemory::MarkWritten(watched + Block, 4);
    Require(Refused(context, watchedWide), "a driver store past the depth plane retired the depth surface");
    AgcDriver::GuestMemory::MarkWritten(watched + 0x100, 4);
    Require(Sample(context, watchedWide) == nullptr, "depth, then a driver store, then a view of another extent: the view was not left to guest memory");

    {
        GuestAllocations::Mutation mutation;
        mutation.Add(const_cast<std::uint8_t*>(memory), 2 * Block, true, true, true);
    }
    StorageRoundTrip(context, watched + Block);
    ColorStorageWriter(context, watched + Block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(const_cast<std::uint8_t*>(memory));
    }
}

}

int main() {
    try {
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>();
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return VulkanTestSkipped;
        }
        Run(device->GetContext());
        ResummarizeStorageWriter(device->GetContext());
        DepthArraySharedPool(device->GetContext(), false);
        DepthArraySharedPool(device->GetContext(), true);
        std::puts("depth surface last writer tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
