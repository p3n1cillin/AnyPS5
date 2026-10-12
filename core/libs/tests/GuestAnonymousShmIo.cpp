#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

struct GuestIovec {
    void* base;
    std::size_t length;
};

extern "C" {
int APS5_VABI shm_open_nid_postfix(const char*, int, int);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
std::int64_t APS5_VABI pread_nid_postfix(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int, const void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
std::int64_t APS5_VABI lseek_nid_postfix(int, std::int64_t, int);
std::int64_t APS5_VABI sceKernelWritev(int, const GuestIovec*, int);
std::int64_t APS5_VABI sceKernelPwritev(int, const GuestIovec*, int, std::int64_t);
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + " (guest errno=" + std::to_string(*__error_nid_postfix()) + ")");
}

#ifdef _WIN32
static void CheckLargeObject() {
    const int descriptor = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(descriptor >= 0, "large-object creation failed");
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(descriptor));
    DWORD returned = 0;
    if (::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        constexpr std::int64_t largeSize = (std::int64_t{1} << 32) + 8;
        FILE_END_OF_FILE_INFO end{LARGE_INTEGER{}};
        end.EndOfFile.QuadPart = largeSize;
        Require(::SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof(end)), "sparse-object resize failed");
        Require(lseek_nid_postfix(descriptor, 0, SEEK_END) == largeSize, "shared EOF was truncated to 32 bits");
        Require(pwrite_nid_disambig1_nid_postfix(descriptor, "uv", 2, largeSize - 1) == 1, "large positioned write past EOF was not short");
        Require(write_nid_postfix(descriptor, "z", 1) == 0, "write at large EOF grew the object");
        Require(lseek_nid_postfix(descriptor, 0, SEEK_END) == largeSize, "large write changed the object size");
        char back = 0;
        Require(pread_nid_postfix(descriptor, &back, 1, largeSize - 1) == 1 && back == 'u', "large-object contents are wrong");
        std::puts("large sparse shared-object boundary checks passed");
    } else {
        std::puts("large sparse shared-object check unavailable on this filesystem");
    }
    Require(close_nid_postfix(descriptor) == 0, "large-object cleanup failed");
}
#endif

int main() {
    const int shared = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(shared >= 0, "anonymous object creation failed");
    Require(pwrite_nid_disambig1_nid_postfix(shared, "z", 1, 0) == 0, "write grew a zero-sized shared object");
    Require(ftruncate_nid_postfix(shared, 3) == 0, "shared resize failed");
    Require(pwrite_nid_disambig1_nid_postfix(shared, "xy", 2, 2) == 1, "positioned write past EOF was not short");
    Require(lseek_nid_postfix(shared, 0, SEEK_END) == 3, "positioned write changed the size");
    Require(write_nid_postfix(shared, "z", 1) == 0, "ordinary write at EOF grew the shared object");
    Require(lseek_nid_postfix(shared, 2, SEEK_SET) == 2, "seek near EOF failed");
    Require(write_nid_postfix(shared, "yz", 2) == 1, "ordinary write past EOF was not short");
    Require(lseek_nid_postfix(shared, 4, SEEK_SET) == -1 && *__error_nid_postfix() == 22, "seek beyond EOF did not fail with EINVAL");
    Require(lseek_nid_postfix(shared, -1, SEEK_SET) == -1 && *__error_nid_postfix() == 22, "negative seek did not fail with EINVAL");
    char bytes[] = "ab";
    GuestIovec vectors[2]{{bytes, 1}, {bytes + 1, 1}};
    Require(sceKernelPwritev(shared, vectors, 2, 2) == 1, "positioned vector write past EOF was not short");
    Require(lseek_nid_postfix(shared, 2, SEEK_SET) == 2, "seek before vector write failed");
    Require(sceKernelWritev(shared, vectors, 2) == 1, "vector write past EOF was not short");
    Require(lseek_nid_postfix(shared, 0, SEEK_END) == 3, "vector write grew the object");
    char back[3]{};
    Require(pread_nid_postfix(shared, back, sizeof(back), 0) == 3 && back[0] == 0 && back[1] == 0 && back[2] == 'a', "shared contents are wrong");
    Require(pwrite_nid_disambig1_nid_postfix(shared, "q", 1, 1) == 1, "in-bounds positioned write failed");
    Require(lseek_nid_postfix(shared, 0, SEEK_CUR) == 3, "positioned write changed the shared offset");
    Require(ftruncate_nid_postfix(shared, 1) == 0, "shared shrink failed");
    Require(write_nid_postfix(shared, "z", 1) == 0, "write beyond a shrunken object's EOF grew it");
    Require(lseek_nid_postfix(shared, 0, SEEK_END) == 1, "shrinking did not change EOF");

    const int duplicate = dup_nid_postfix(shared);
    Require(duplicate >= 0 && duplicate != shared, "shared duplicate failed");
    Require(pwrite_nid_disambig1_nid_postfix(duplicate, "z", 1, 1) == 0, "duplicate lost shared-object bounds");
    Require(close_nid_postfix(shared) == 0, "closing original shared descriptor failed");
    Require(ftruncate_nid_postfix(duplicate, 3) == 0, "duplicate resize failed after original close");
    Require(pwrite_nid_disambig1_nid_postfix(duplicate, "uv", 2, 2) == 1, "duplicate write was not bounded after original close");

    FILE* ordinary = std::tmpfile();
    Require(ordinary != nullptr, "ordinary temporary file creation failed");
#ifdef _WIN32
    const int ordinaryDescriptor = ::_fileno(ordinary);
#else
    const int ordinaryDescriptor = ::fileno(ordinary);
#endif
    Require(dup2_nid_postfix(ordinaryDescriptor, duplicate) == duplicate, "ordinary descriptor replacement failed");
    Require(lseek_nid_postfix(duplicate, 9, SEEK_SET) == 9, "ordinary file retained shared seek bounds");
    Require(write_nid_postfix(duplicate, "regular", 7) == 7, "ordinary file retained shared write bounds");
    Require(lseek_nid_postfix(duplicate, 0, SEEK_END) == 16, "ordinary file did not grow normally");
    const int replacement = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(replacement >= 0 && replacement != duplicate, "replacement shared creation failed");
    Require(ftruncate_nid_postfix(replacement, 2) == 0, "replacement shared resize failed");
    Require(dup2_nid_postfix(replacement, duplicate) == duplicate, "shared descriptor replacement failed");
    Require(pwrite_nid_disambig1_nid_postfix(duplicate, "uv", 2, 1) == 1, "reused descriptor slot retained ordinary-file behavior");
    std::atomic<bool> started{false};
    std::thread resize([&] {
        started.store(true, std::memory_order_release);
        for (int iteration = 0; iteration < 256; ++iteration) {
            Require(ftruncate_nid_postfix(replacement, iteration % 2 == 0 ? 1 : 3) == 0, "concurrent resize failed");
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int iteration = 0; iteration < 256; ++iteration) {
        const auto result = pwrite_nid_disambig1_nid_postfix(duplicate, "uv", 2, 2);
        Require(result == 0 || result == 1, "concurrent resize allowed an unbounded write");
    }
    resize.join();
    Require(ftruncate_nid_postfix(replacement, 1) == 0, "final resize failed");
    Require(lseek_nid_postfix(duplicate, 0, SEEK_END) == 1, "concurrent writes left an extended object");
    Require(close_nid_postfix(replacement) == 0 && close_nid_postfix(duplicate) == 0, "shared descriptor cleanup failed");
    Require(std::fclose(ordinary) == 0, "ordinary file cleanup failed");
#ifdef _WIN32
    CheckLargeObject();
#endif
}
