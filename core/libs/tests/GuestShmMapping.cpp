#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI shm_open_nid_postfix(const char*, int, int);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
std::int64_t APS5_VABI pread_nid_postfix(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int, const void*, std::size_t, std::int64_t);
int APS5_VABI close_nid_postfix(int);
void* APS5_VABI mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t) noexcept;
int APS5_VABI munmap_nid_postfix(void*, std::size_t) noexcept;
int APS5_VABI mprotect_nid_postfix(void*, std::size_t, int);
int* APS5_VABI __error_nid_postfix();
}

namespace {
constexpr std::size_t Page = 0x4000;
constexpr int Shared = 1, Private = 2, Fixed = 0x10, NoOverwrite = 0x80;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + " (guest errno=" + std::to_string(*__error_nid_postfix()) + ")");
}

char* Map(int descriptor, std::size_t bytes, int protection, int flags, std::int64_t offset = 0, void* address = nullptr) {
    void* mapped = mmap_nid_postfix(address, bytes, protection, flags, descriptor, offset);
    Require(mapped != reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1)), "guest shared-memory mapping failed");
    Require((reinterpret_cast<std::uintptr_t>(mapped) & (Page - 1)) == 0, "mapping is not guest-page aligned");
    return static_cast<char*>(mapped);
}

int ReadOnly(int descriptor) {
#ifdef _WIN32
    HANDLE duplicate = nullptr;
    const auto original = reinterpret_cast<HANDLE>(::_get_osfhandle(descriptor));
    Require(::DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate, GENERIC_READ | GENERIC_EXECUTE, FALSE, 0), "read-only duplicate creation failed");
    const int result = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(duplicate), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (result < 0) ::CloseHandle(duplicate);
    return result;
#else
    return ::open(("/proc/self/fd/" + std::to_string(descriptor)).c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

#ifdef _WIN32
void CheckLargeOffset() {
    const int descriptor = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(descriptor >= 0, "large-offset backing creation failed");
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(descriptor));
    DWORD returned = 0;
    if (::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        constexpr std::int64_t offset = std::int64_t{1} << 32;
        FILE_END_OF_FILE_INFO end{LARGE_INTEGER{}};
        end.EndOfFile.QuadPart = offset + Page;
        Require(::SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof(end)), "large-offset sparse resize failed");
        auto* mapped = Map(descriptor, Page, 3, Shared, offset);
        mapped[Page - 1] = 'l';
        char byte = 0;
        Require(pread_nid_postfix(descriptor, &byte, 1, offset + Page - 1) == 1 && byte == 'l', "mapping offset was truncated to 32 bits");
        Require(close_nid_postfix(descriptor) == 0 && mapped[Page - 1] == 'l', "large-offset mapping did not survive close");
        Require(munmap_nid_postfix(mapped, Page) == 0, "large-offset mapping cleanup failed");
        std::puts("4 GiB sparse guest mapping offset checks passed");
    } else {
        Require(close_nid_postfix(descriptor) == 0, "large-offset backing cleanup failed");
        std::puts("large-offset sparse mapping check unavailable on this filesystem");
    }
}
#endif

void Run() {
    const int descriptor = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(descriptor >= 0 && ftruncate_nid_postfix(descriptor, Page * 4) == 0, "shared-object setup failed");
    auto* first = Map(descriptor, Page * 3, 3, Shared);
    auto* second = Map(descriptor, Page, 3, Shared, Page);
    first[Page] = 'a';
    Require(second[0] == 'a', "shared aliases did not observe each other");
    char byte = 0;
    Require(pread_nid_postfix(descriptor, &byte, 1, Page) == 1 && byte == 'a', "descriptor read did not see mapped write");
    Require(pwrite_nid_disambig1_nid_postfix(descriptor, "b", 1, Page) == 1 && first[Page] == 'b' && second[0] == 'b', "mapped views did not see descriptor write");

    auto* copy = Map(descriptor, Page, 3, Private, Page);
    Require(copy[0] == 'b', "private view did not see initial backing");
    second[1] = 'c';
    Require(copy[1] == 'c', "private view did not see backing before first write");
    copy[0] = 'p';
    Require(second[0] == 'b', "private write escaped into shared backing");
    Require(pread_nid_postfix(descriptor, &byte, 1, Page) == 1 && byte == 'b', "private write escaped into descriptor backing");
    Require(mprotect_nid_postfix(copy, Page, 1) == 0 && mprotect_nid_postfix(copy, Page, 3) == 0, "private protection round trip failed");
    copy[1] = 'q';
    Require(second[1] == 'c', "protection change lost copy-on-write isolation");
    Require(mprotect_nid_postfix(second, Page, 0) == 0 && mprotect_nid_postfix(second, Page, 3) == 0, "shared no-access protection round trip failed");
    second[2] = 'd';
    Require(first[Page + 2] == 'd', "protection change lost shared backing");
    auto* inaccessible = Map(descriptor, Page, 0, Private, Page);
    Require(mprotect_nid_postfix(inaccessible, Page, 3) == 0, "initial no-access private view could not become writable");
    inaccessible[0] = 'i';
    Require(second[0] == 'b', "initial no-access view lost private isolation");
    Require(pread_nid_postfix(descriptor, copy + 128, 1, Page) == 1 && copy[128] == 'b', "host write into private guest memory failed");
    Require(second[128] == 0, "host write into private memory changed shared backing");
#ifdef _WIN32
    void* dirty[4]{};
    std::size_t count = 4;
    Require(!GuestArena::GuestArenaCollectWrites_nid_postfix(reinterpret_cast<std::uintptr_t>(first), Page, dirty, &count, true), "file mapping incorrectly enabled page write tracking");
    bool refusedAlias = false;
    try { GuestArena::GuestArenaMapAlias_nid_postfix(reinterpret_cast<std::uintptr_t>(copy), Page); }
    catch (const std::runtime_error&) { refusedAlias = true; }
    Require(refusedAlias, "private file mapping offered a writable shared alias");
    Require(mmap_nid_postfix(nullptr, Page, 3, Shared, descriptor, Page * 4) == reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1)) && *__error_nid_postfix() == 45, "unsupported beyond-EOF mapping did not report ENOTSUP");
#endif

    const int readOnly = ReadOnly(descriptor);
    Require(readOnly >= 0, "read-only descriptor creation failed");
    Require(mmap_nid_postfix(nullptr, Page, 3, Shared, readOnly, 0) == reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1)) && *__error_nid_postfix() == 13, "read-only descriptor accepted a writable shared mapping");
    auto* read = Map(readOnly, Page, 1, Shared, Page);
    Require(read[0] == 'b', "read-only shared view has wrong backing");
    Require(mprotect_nid_postfix(read, Page, 3) == -1 && *__error_nid_postfix() == 13, "read-only shared view accepted write protection");
    auto* readCopy = Map(readOnly, Page, 1, Private, Page);
    Require(mprotect_nid_postfix(readCopy, Page, 3) == 0, "private view of read-only descriptor could not become writable");
    readCopy[0] = 'r';
    Require(second[0] == 'b', "private read-only descriptor view changed shared backing");

    Require(munmap_nid_postfix(first + Page, Page) == 0, "partial shared unmap failed");
    first[0] = 'u';
    first[Page * 2] = 'v';
    auto* replacement = Map(descriptor, Page, 3, Shared | Fixed, Page, first + Page);
    Require(replacement == first + Page && replacement[0] == 'b', "fixed mapping did not replace unmapped page");
    Require(mmap_nid_postfix(replacement, Page, 3, Shared | Fixed | NoOverwrite, descriptor, Page) == reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1)), "no-overwrite mapping replaced an existing view");
    Require(replacement[0] == 'b', "rejected replacement damaged the existing view");
    auto* fixedCopy = Map(descriptor, Page, 3, Private | Fixed, Page, replacement);
    fixedCopy[0] = 'f';
    Require(second[0] == 'b', "fixed private replacement changed shared backing");
    Require(mprotect_nid_postfix(first, Page * 3, 1) == 0 && mprotect_nid_postfix(first, Page * 3, 3) == 0, "mixed shared/private protection change failed");
    fixedCopy[1] = 'h';
    first[0] = 'u';
    first[Page * 2] = 'v';
    Require(second[1] == 'c', "mixed protection change lost private isolation");

    const unsigned char code[]{0xb8, 42, 0, 0, 0, 0xc3};
    Require(pwrite_nid_disambig1_nid_postfix(descriptor, code, sizeof(code), Page * 3) == static_cast<std::int64_t>(sizeof(code)), "executable backing initialization failed");
    auto* executable = Map(descriptor, Page, 5, Private, Page * 3);
    Require(reinterpret_cast<int(*)()>(executable)() == 42, "file-backed guest execution returned the wrong value");
    Require(mprotect_nid_postfix(executable, Page, 7) == 0, "private executable mapping could not become writable");
    executable[1] = 43;
    Require(reinterpret_cast<int(*)()>(executable)() == 43, "private executable write was not visible");
    Require(pread_nid_postfix(descriptor, &byte, 1, Page * 3 + 1) == 1 && byte == 42, "private executable write changed the file");
    auto* sharedExecutable = Map(descriptor, Page, 5, Shared, Page * 3);
    Require(reinterpret_cast<int(*)()>(sharedExecutable)() == 42, "shared executable view has wrong contents");
    Require(mprotect_nid_postfix(sharedExecutable, Page, 7) == 0, "shared executable protection upgrade failed");
    sharedExecutable[1] = 44;
    Require(reinterpret_cast<int(*)()>(sharedExecutable)() == 44, "shared executable write was not visible");
    Require(pread_nid_postfix(descriptor, &byte, 1, Page * 3 + 1) == 1 && byte == 44, "shared executable write did not change the file");
    Require(reinterpret_cast<int(*)()>(executable)() == 43, "shared executable update changed the private copy");

    Require(close_nid_postfix(readOnly) == 0 && close_nid_postfix(descriptor) == 0, "mapped descriptor close failed");
    Require(second[0] == 'b' && copy[0] == 'p' && read[0] == 'b' && readCopy[0] == 'r', "views did not survive descriptor close");
    second[3] = 'w';
    Require(munmap_nid_postfix(first, Page * 3) == 0, "mixed shared/private range unmap failed");
    Require(munmap_nid_postfix(second, Page) == 0 && munmap_nid_postfix(copy, Page) == 0, "shared/private alias cleanup failed");
    Require(munmap_nid_postfix(read, Page) == 0 && munmap_nid_postfix(readCopy, Page) == 0 && munmap_nid_postfix(executable, Page) == 0, "protected-view cleanup failed");
    Require(munmap_nid_postfix(inaccessible, Page) == 0 && munmap_nid_postfix(sharedExecutable, Page) == 0, "additional protected-view cleanup failed");
#ifdef _WIN32
    CheckLargeOffset();
#endif
    std::puts("shared and private guest file-mapping checks passed");
}
}

int main() {
    try { Run(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
