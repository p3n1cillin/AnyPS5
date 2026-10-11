#ifndef CORE_LIBS_PRX_LIBKERNEL_SYS_WINDOWSREADLINK_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYS_WINDOWSREADLINK_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <windows.h>
#include <winioctl.h>
#include "prx/libc/include/FilesystemError.hpp"

namespace KernelSys {

inline int DecodeLinkTarget_nid_no_patch(std::span<const std::byte> data, std::string& target) {
    if (data.size() < 8) return 5;
    const auto word = [&](std::size_t offset) {
        std::uint16_t value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    std::uint32_t tag;
    std::memcpy(&tag, data.data(), sizeof(tag));
    const std::size_t length = word(4);
    if (length > data.size() - 8) return 5;
    const std::size_t fixed = tag == IO_REPARSE_TAG_SYMLINK ? 12 : tag == IO_REPARSE_TAG_MOUNT_POINT ? 8 : 0;
    if (fixed == 0) return 22;
    if (length < fixed) return 5;
    const std::size_t offset = word(8);
    const std::size_t bytes = word(10);
    if (bytes == 0 || (offset & 1) != 0 || (bytes & 1) != 0 || offset > length - fixed || bytes > length - fixed - offset) return 5;
    std::uint32_t flags = 0;
    if (tag == IO_REPARSE_TAG_SYMLINK) {
        std::memcpy(&flags, data.data() + 16, sizeof(flags));
        if ((flags & ~1u) != 0) return 5;
    }
    static_assert(sizeof(wchar_t) == 2);
    std::wstring contents(bytes / 2, L'\0');
    if (bytes != 0) std::memcpy(contents.data(), data.data() + 8 + fixed + offset, bytes);
    if ((flags & 1u) == 0) {
        if (contents.starts_with(L"\\??\\UNC\\") || contents.starts_with(L"\\\\?\\UNC\\")) contents = L"\\\\" + contents.substr(8);
        else if (contents.starts_with(L"\\??\\") || contents.starts_with(L"\\\\?\\")) contents.erase(0, 4);
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()), nullptr, 0, nullptr, nullptr);
    if (count == 0) return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ? 86 : 5;
    std::string utf8(count, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()), utf8.data(), count, nullptr, nullptr) != count) return 5;
    for (char& character : utf8) {
        if (character == '\\') character = '/';
    }
    target = std::move(utf8);
    return 0;
}

inline int ReadLinkTarget_nid_no_patch(const std::filesystem::path& path, std::string& target) {
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return FilesystemError(std::error_code(GetLastError(), std::system_category()));
    std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> data{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        data.data(), static_cast<DWORD>(data.size()), &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        if (error == ERROR_NOT_A_REPARSE_POINT) return 22;
        return FilesystemError(std::error_code(error, std::system_category()));
    }
    if (returned > data.size()) return 5;
    return DecodeLinkTarget_nid_no_patch(std::span<const std::byte>(data.data(), returned), target);
}

}

#endif
