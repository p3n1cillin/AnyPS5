#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <cstdint>
#include <cstdio>
#include <map>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::size_t MiB = std::size_t{1} << 20u;
constexpr VkMemoryPropertyFlags HostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

struct MockDevice {
    std::uint64_t next = 1;
    std::map<VkBuffer, VkDeviceSize> sizes;
    std::map<VkBuffer, VkBufferUsageFlags> usages;
    std::map<VkDeviceMemory, std::vector<std::byte>> hostMemory;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t destroyedBuffers = 0;
    VkDeviceSize liveBytes = 0;
    std::map<VkDeviceMemory, VkDeviceSize> memoryBytes;
    VkResult allocationResult = VK_SUCCESS;
    std::uint64_t allocationAttempts = 0;
    VkDeviceSize liveHostBytes = 0;
    VkDeviceSize maxHostBytes = std::numeric_limits<VkDeviceSize>::max();
    VkResult budgetFailure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::uint32_t memoryTypeBits = 3;
    std::vector<std::uint32_t> allocationTypes;
    std::vector<VkMemoryAllocateFlags> allocationFlags;
    std::map<VkDeviceMemory, std::uint32_t> memoryTypes;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(mock.next++));
    mock.sizes[*buffer] = info->size;
    mock.usages[*buffer] = info->usage;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements) {
    *requirements = {mock.sizes.at(buffer), 256, mock.memoryTypeBits};
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    ++mock.allocationAttempts;
    mock.allocationTypes.push_back(info->memoryTypeIndex);
    mock.allocationFlags.push_back(info->pNext == nullptr ? 0 : static_cast<const VkMemoryAllocateFlagsInfo*>(info->pNext)->flags);
    if (mock.allocationResult != VK_SUCCESS) return mock.allocationResult;
    if (info->memoryTypeIndex == 1 && mock.liveHostBytes + info->allocationSize > mock.maxHostBytes) return mock.budgetFailure;
    *memory = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(mock.next++));
    if (info->memoryTypeIndex != 0) {
        mock.hostMemory[*memory].resize(info->allocationSize);
        if (info->memoryTypeIndex == 1) mock.liveHostBytes += info->allocationSize;
    }
    mock.memoryTypes[*memory] = info->memoryTypeIndex;
    mock.memoryBytes[*memory] = info->allocationSize;
    mock.liveBytes += info->allocationSize;
    ++mock.allocations;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** data) {
    *data = mock.hostMemory.at(memory).data();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}

VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {
    ++mock.destroyedBuffers;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    if (mock.memoryTypes.at(memory) == 1) mock.liveHostBytes -= mock.memoryBytes.at(memory);
    mock.memoryTypes.erase(memory);
    mock.liveBytes -= mock.memoryBytes.at(memory);
    mock.memoryBytes.erase(memory);
    mock.hostMemory.erase(memory);
    ++mock.frees;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL mockGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) {
    return 0x100000000000ULL + reinterpret_cast<std::uintptr_t>(info->buffer) * 0x10000;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCreateBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkGetBufferDeviceAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferDeviceAddress)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    context.memory.memoryTypes[1].propertyFlags = HostVisible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    context.limits.maxMemoryAllocationCount = 1u << 20u;
    context.bufferDeviceAddress = true;
    return context;
}

void SizeClassesAndDirections() {
    mock = MockDevice{};
    auto context = mockContext();
    VkBuffer first = VK_NULL_HANDLE;
    {
        DeviceBuffer upload(context, 10 * MiB + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        first = upload.Handle();
        Expect(mock.sizes.at(first) == 11 * MiB, "a 10 MiB + 4 KiB device buffer was created with " + std::to_string(mock.sizes.at(first)) + " bytes, not its 11 MiB class");
        constexpr VkBufferUsageFlags all = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Expect((mock.usages.at(first) & all) == all, "a device buffer was created without storage and both transfer directions");
        Expect(upload.Size() == 10 * MiB + 4096, "a device buffer reports its class instead of the requested size");
    }
    const auto made = mock.allocations;
    DeviceBuffer writeBack(context, 10 * MiB + 512 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Expect(writeBack.Handle() == first && mock.allocations == made, "a retained 11 MiB device buffer did not serve a write-back of another size and direction in its class");
    Expect(writeBack.Size() == 10 * MiB + 512 * 1024, "a reused device buffer reports the retained size");
}

void LargerClass() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer twelve = VK_NULL_HANDLE;
    {
        DeviceBuffer large(context, 12 * MiB, usage);
        twelve = large.Handle();
    }
    {
        DeviceBuffer smaller(context, 7 * MiB, usage);
        Expect(smaller.Handle() == twelve, "a 7 MiB request did not take the retained 12 MiB buffer");
    }
    const auto made = mock.allocations;
    {
        DeviceBuffer tooSmall(context, 5 * MiB, usage);
        Expect(tooSmall.Handle() != twelve && mock.allocations == made + 1, "a 5 MiB request took a retained buffer of more than twice its size");
        Expect(mock.sizes.at(tooSmall.Handle()) == 5 * MiB, "a 5 MiB device buffer was not created at its own class");
    }
    DeviceBuffer again(context, 12 * MiB, usage);
    Expect(again.Handle() == twelve, "a buffer served to a smaller request did not return to its own 12 MiB class");
    DeviceBuffer best(context, 4 * MiB + 1, usage);
    Expect(best.Handle() != twelve && mock.sizes.at(best.Handle()) == 5 * MiB, "a 4 MiB + 1 request did not take the smallest retained class that fits (5 MiB)");
}

void KeptUntilFence() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    std::vector<std::shared_ptr<DeviceBuffer>> batch;
    for (int i = 0; i < 3; ++i) batch.push_back(std::make_shared<DeviceBuffer>(context, 6 * MiB + static_cast<std::size_t>(i + 1) * 4096, usage));
    for (std::size_t i = 0; i < batch.size(); ++i) {
        for (std::size_t j = i + 1; j < batch.size(); ++j) Expect(batch[i]->Handle() != batch[j]->Handle(), "two buffers of one unfinished batch share a VkBuffer");
    }
    const auto kept = batch.front()->Handle();
    const auto made = mock.allocations;
    {
        DeviceBuffer during(context, 6 * MiB, usage);
        Expect(during.Handle() != kept && mock.allocations == made + 1, "a buffer kept by an unfinished batch was handed out again");
    }
    batch.clear();
    std::vector<std::unique_ptr<DeviceBuffer>> after;
    for (int i = 0; i < 3; ++i) after.push_back(std::make_unique<DeviceBuffer>(context, 6 * MiB + 100, usage));
    after.push_back(std::make_unique<DeviceBuffer>(context, 6 * MiB, usage));
    Expect(mock.allocations == made + 1, "the buffers released after the batch fence were not reused (" + std::to_string(mock.allocations - made - 1) + " new allocations)");
}

void Budget() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    std::vector<std::unique_ptr<DeviceBuffer>> live;
    for (int i = 0; i < 12; ++i) live.push_back(std::make_unique<DeviceBuffer>(context, (48 + static_cast<std::size_t>(i) * 3) * MiB, usage));
    const auto first = live.front()->Handle();
    VkDeviceSize madeBytes = 0;
    for (const auto& buffer : live) madeBytes += mock.sizes.at(buffer->Handle());
    Expect(madeBytes > 512 * MiB, "the budget case does not exceed the budget");
    for (auto& buffer : live) buffer.reset();
    Expect(mock.frees != 0 && mock.liveBytes <= 512 * MiB, "the device tier retains " + std::to_string(mock.liveBytes / MiB) + " MiB after " + std::to_string(mock.frees) + " evictions, over its 512 MiB budget");
    DeviceBuffer probe(context, 48 * MiB, usage);
    Expect(probe.Handle() != first, "the least recently used device buffer survived the eviction");
}

void AddressAndHostUnchanged() {
    mock = MockDevice{};
    auto context = mockContext();
    VkBuffer scratch = VK_NULL_HANDLE;
    {
        DeviceBuffer buffer(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        scratch = buffer.Handle();
    }
    Buffer addressed(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Expect(addressed.Handle() != scratch && addressed.DeviceAddress() != 0, "an addressable device-local buffer took a scratch buffer without a device address");
    Buffer shadow(context, 2 * MiB - 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Expect(shadow.Handle() == scratch, "a device-local staging buffer did not share the scratch buffers' class");
    VkBuffer host = VK_NULL_HANDLE;
    {
        Buffer copy(context, 10 * MiB + 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
        host = copy.Handle();
        Expect(mock.sizes.at(host) == 10 * MiB + 4096, "a large host buffer was rounded to a class");
        Expect(mock.usages.at(host) == VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "a host buffer was created with more usages than asked");
    }
    Buffer other(context, 10 * MiB + 8192, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
    Expect(other.Handle() != host, "a large host buffer of another exact size was reused");
    Buffer same(context, 10 * MiB + 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
    Expect(same.Handle() == host, "a large host buffer of the same exact size was not reused");
}

void SmallDeviceClasses() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer mebibyte = VK_NULL_HANDLE;
    VkBuffer half = VK_NULL_HANDLE;
    {
        DeviceBuffer one(context, MiB, usage);
        mebibyte = one.Handle();
    }
    {
        DeviceBuffer smaller(context, 300 * 1024, usage);
        Expect(smaller.Handle() != mebibyte && mock.sizes.at(smaller.Handle()) == 512 * 1024, "a 300 KiB request took a 1 MiB buffer or skipped its power-of-two class");
        half = smaller.Handle();
    }
    DeviceBuffer again(context, 260 * 1024, usage);
    Expect(again.Handle() == half, "a 260 KiB request did not take the retained 512 KiB buffer");
    constexpr auto indirect = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer tiny = VK_NULL_HANDLE;
    {
        DeviceBuffer args(context, 20, indirect);
        tiny = args.Handle();
    }
    const auto made = mock.allocations;
    DeviceBuffer nextArgs(context, 20, indirect);
    Expect(nextArgs.Handle() == tiny && mock.allocations == made, "a 20-byte device request did not reuse the retained 256-byte buffer of its class");
}

VKAPI_ATTR void VKAPI_CALL mockMemoryBudget(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    auto* budget = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(properties->pNext);
    budget->heapBudget[1] = 8 * MiB;
    budget->heapUsage[1] = 7 * MiB;
}

void AllocationFailure() {
    for (const bool reported : {false, true}) {
        mock = MockDevice{};
        mock.allocationResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        auto context = mockContext();
        context.memory.memoryHeapCount = 2;
        context.memory.memoryTypes[1].heapIndex = 1;
        context.memory.memoryHeaps[1].size = 16 * MiB;
        context.physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1});
        if (reported) context.memoryProperties2 = mockMemoryBudget;
        bool rejected = false;
        try {
            Buffer buffer(context, 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        } catch (const std::runtime_error& error) {
            rejected = true;
            const std::string message = error.what();
            Expect(message.find("Vulkan result -2") != std::string::npos, "an allocation failure lost the original Vulkan result");
            Expect(message.find("bytes=20 capacity=256 allocation=256 type=1 heap=1") != std::string::npos, "an allocation failure lost its requested and actual allocation sizes or selected heap");
            const auto budget = reported ? "heap-budget=8388608 heap-usage=7340032" : "heap-budget=unavailable heap-usage=unavailable";
            Expect(message.find(budget) != std::string::npos, "an allocation failure misreported the heap budget");
        }
        Expect(rejected && mock.destroyedBuffers == 1 && mock.allocations == 0 && mock.frees == 0, "an allocation failure leaked its buffer or freed memory that was never allocated");
        Expect(mock.allocationAttempts == 1, "an allocation failure retried without releasing any unused memory");
    }
}

void AllocationPressure() {
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        mock = MockDevice{};
        mock.maxHostBytes = 16 * MiB;
        mock.budgetFailure = failure;
        auto context = mockContext();
        Buffer live(context, 4 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        live.Bytes().front() = std::byte{0x42};
        {
            Buffer unused(context, 8 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }
        VkBuffer deviceHandle = VK_NULL_HANDLE;
        {
            DeviceBuffer unused(context, 20 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            deviceHandle = unused.Handle();
        }
        const auto attempts = mock.allocationAttempts;
        Buffer recovered(context, 9 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Expect(mock.allocationAttempts == attempts + 2 && mock.frees == 1 && mock.liveHostBytes == 13 * MiB, "host allocation pressure did not reclaim unused memory and retry exactly once");
        Expect(live.Bytes().front() == std::byte{0x42} && recovered.Bytes().size() == 9 * MiB, "pressure recovery altered a live buffer or the requested allocation size");
        DeviceBuffer device(context, 20 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Expect(device.Handle() == deviceHandle, "host allocation pressure discarded a retained device-only buffer");
    }
}

void PersistentAllocationFailure() {
    for (const auto failure : {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST}) {
        mock = MockDevice{};
        auto context = mockContext();
        {
            Buffer unused(context, 2 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }
        mock.allocationResult = failure;
        const auto attempts = mock.allocationAttempts;
        bool rejected = false;
        try {
            Buffer buffer(context, 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        } catch (const std::runtime_error& error) {
            rejected = true;
            Expect(std::string(error.what()).find("Vulkan result " + std::to_string(failure)) != std::string::npos, "pressure recovery masked a persistent Vulkan failure");
        }
        const bool pressure = failure == VK_ERROR_OUT_OF_DEVICE_MEMORY;
        Expect(rejected && mock.allocationAttempts == attempts + (pressure ? 2 : 1) && mock.frees == (pressure ? 1 : 0), "persistent exhaustion retried more than once or a non-memory failure reclaimed host buffers");
    }
}

Context alternateHeapContext() {
    auto context = mockContext();
    context.memory.memoryHeapCount = 2;
    context.memory.memoryTypeCount = 3;
    context.memory.memoryTypes[2] = {HostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1};
    return context;
}

void AlternateHostHeap() {
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        for (const bool reclaim : {false, true}) {
            mock = MockDevice{};
            mock.memoryTypeBits = 7;
            mock.budgetFailure = failure;
            auto context = alternateHeapContext();
            Buffer live(context, 4 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            live.Bytes().front() = std::byte{0x42};
            if (reclaim) {
                Buffer unused(context, 2 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            }
            mock.maxHostBytes = 4 * MiB;
            const auto attempts = mock.allocationAttempts;
            const auto expected = reclaim ? 3u : 2u;
            VkBuffer handle = VK_NULL_HANDLE;
            constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            {
                Buffer recovered(context, 9 * MiB, usage);
                handle = recovered.Handle();
                recovered.Bytes().front() = std::byte{0x73};
                recovered.Bytes().back() = std::byte{0x39};
                Expect(mock.allocationAttempts == attempts + expected && mock.allocationTypes.back() == 2, "host pressure did not try one compatible alternate heap after checking unused memory");
                Expect(live.Bytes().front() == std::byte{0x42} && recovered.Bytes().size() == 9 * MiB && recovered.DeviceAddress() != 0 && mock.allocationFlags.back() == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, "alternate-heap recovery altered a live buffer, mapping or device address");
            }
            const auto retainedAttempts = mock.allocationAttempts;
            const auto retainedFrees = mock.frees;
            Buffer different(context, 7 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            Expect(mock.allocationAttempts == retainedAttempts + 2 && mock.frees == retainedFrees, "pressure recovery discarded reusable memory from a different heap");
            Buffer reused(context, 9 * MiB, usage);
            Expect(reused.Handle() == handle && mock.allocationAttempts == retainedAttempts + 2 && reused.Bytes().front() == std::byte{0x73} && reused.Bytes().back() == std::byte{0x39}, "an alternate-heap allocation was not retained and reused intact");
        }
    }
}

void AlternateHostHeapGuards() {
    for (unsigned control = 0; control < 10; ++control) {
        mock = MockDevice{};
        mock.memoryTypeBits = 7;
        mock.maxHostBytes = 0;
        auto context = alternateHeapContext();
        auto properties = HostVisible;
        if (control == 0) mock.memoryTypeBits = 3;
        if (control == 1) context.memory.memoryTypes[2].heapIndex = 0;
        if (control == 2) context.memory.memoryTypes[2].propertyFlags &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (control == 3) context.memory.memoryTypes[2].propertyFlags &= ~VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if (control == 4) properties |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if (control == 5) mock.allocationResult = VK_ERROR_DEVICE_LOST;
        if (control == 6) properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (control == 6 || control == 7) mock.allocationResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (control == 7) {
            context.memory.memoryHeapCount = 3;
            context.memory.memoryTypeCount = 4;
            context.memory.memoryTypes[3] = {HostVisible, 2};
            mock.memoryTypeBits = 15;
        }
        if (control == 8) context.memory.memoryTypes[2].propertyFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
        if (control == 9) context.memory.memoryTypes[2].propertyFlags |= VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
        bool rejected = false;
        try {
            Buffer buffer(context, 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, properties);
        } catch (const std::runtime_error& error) {
            rejected = true;
            const auto result = control == 5 ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            Expect(std::string(error.what()).find("Vulkan result " + std::to_string(result)) != std::string::npos, "alternate-heap failure lost the Vulkan result");
        }
        const auto expected = control == 7 ? 2u : 1u;
        Expect(rejected && mock.allocationAttempts == expected && mock.destroyedBuffers == 1 && mock.allocations == 0, "alternate-heap recovery ignored compatibility, retried a non-memory failure or leaked a failed buffer: control " + std::to_string(control));
    }
}

void MixedHeapPool() {
    mock = MockDevice{};
    mock.memoryTypeBits = 7;
    auto context = alternateHeapContext();
    VkBuffer alternateHandle = VK_NULL_HANDLE;
    {
        Buffer original(context, MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        original.Bytes().front() = std::byte{0x42};
        mock.maxHostBytes = MiB;
        Buffer alternate(context, MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        alternateHandle = alternate.Handle();
        alternate.Bytes().front() = std::byte{0x73};
        Expect(original.Bytes().front() == std::byte{0x42}, "alternate allocation changed the original-heap buffer");
    }
    const auto attempts = mock.allocationAttempts;
    Buffer different(context, 2 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Expect(mock.frees == 1 && mock.liveHostBytes == 0 && mock.allocationAttempts == attempts + 3, "mixed-heap pool recovery did not release only the exhausted heap");
    Buffer reused(context, MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Expect(reused.Handle() == alternateHandle && reused.Bytes().front() == std::byte{0x73} && mock.allocationAttempts == attempts + 3, "mixed-heap pool recovery discarded the retained alternate allocation");
}

}

int main() {
    try {
        SizeClassesAndDirections();
        LargerClass();
        KeptUntilFence();
        Budget();
        AddressAndHostUnchanged();
        SmallDeviceClasses();
        AllocationFailure();
        AllocationPressure();
        PersistentAllocationFailure();
        AlternateHostHeap();
        AlternateHostHeapGuards();
        MixedHeapPool();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("buffer pool tests passed");
    return 0;
}
