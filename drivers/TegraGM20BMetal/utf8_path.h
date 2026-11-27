// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far

namespace gm20b {

inline std::wstring utf8ToWide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), int(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(size_t(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), int(utf8.size()), wide.data(), needed) <= 0)
        return {};
    return wide;
}

inline std::string wideToUtf8(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(size_t(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), needed, nullptr, nullptr) <= 0)
        return {};
    return out;
}

inline std::wstring winLongPath(const std::wstring &raw)
{
    if (raw.empty())
        return {};
    std::wstring norm = raw;
    for (wchar_t &ch : norm) {
        if (ch == L'/')
            ch = L'\\';
    }
    if (norm.rfind(L"\\\\?\\", 0) == 0 || norm.rfind(L"\\\\.\\", 0) == 0)
        return norm;
    if (norm.size() >= 240) {
        if (norm.rfind(L"\\\\", 0) == 0)
            return L"\\\\?\\UNC\\" + norm.substr(2);
        if (norm.size() >= 3 && norm[1] == L':' && norm[2] == L'\\')
            return L"\\\\?\\" + norm;
    }
    return norm;
}

inline std::filesystem::path fsPathUtf8(std::string_view utf8)
{
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty() && !utf8.empty())
        return {};
    return std::filesystem::path(winLongPath(wide));
}

inline std::string pathToUtf8(const std::filesystem::path &path)
{
    return wideToUtf8(path.wstring());
}

inline FILE *fopenUtf8(std::string_view utf8, const wchar_t *wmode, const char *)
{
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty())
        return nullptr;
    return _wfopen(winLongPath(wide).c_str(), wmode);
}

inline HANDLE createFileReadUtf8(std::string_view utf8)
{
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty())
        return INVALID_HANDLE_VALUE;
    return CreateFileW(winLongPath(wide).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline bool moveFileReplaceUtf8(std::string_view srcUtf8, std::string_view dstUtf8)
{
    const std::wstring src = utf8ToWide(srcUtf8);
    const std::wstring dst = utf8ToWide(dstUtf8);
    if (src.empty() || dst.empty())
        return false;
    return MoveFileExW(winLongPath(src).c_str(), winLongPath(dst).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace gm20b

#else

namespace gm20b {

inline std::filesystem::path fsPathUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::string(utf8));
}

inline std::string pathToUtf8(const std::filesystem::path &path)
{
    return path.string();
}

inline FILE *fopenUtf8(std::string_view utf8, const wchar_t *, const char *mode)
{
    return std::fopen(std::string(utf8).c_str(), mode);
}

}  // namespace gm20b

#endif
