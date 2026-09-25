// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// Volume.cpp
#pragma once
#include "stdafx.h"
#include "Volume.h"
#include "Utils.h"
#include "Logging.h"

namespace HidHide
{
    typedef std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(&::FindVolumeClose)> FindVolumeClosePtr;
    typedef std::function<__declspec(noreturn) bool(_In_ std::wstring const& volumeName, _In_ std::filesystem::path const& volumeMountPoint)> IterateAllVolumeMountPointsForFileStorageCallback;
    typedef std::pair<std::filesystem::path, std::filesystem::path> VolumeMountPointAndDosDeviceName;

    // Get the DosDeviceName associated with a volume name
    std::filesystem::path DosDeviceNameForVolumeName(_In_ std::wstring const& volumeName)
    {
        TRACE_ALWAYS(L"");
        std::vector<WCHAR> buffer(UNICODE_STRING_MAX_CHARS);
        // Strip the leading '\\?\' and trailing '\' and isolate the Volume{} part in the volume name
        if (0 == ::QueryDosDeviceW(volumeName.substr(4, volumeName.size() - 5).c_str(), buffer.data(), static_cast<DWORD>(buffer.size()))) THROW_WIN32_LAST_ERROR;
        return (buffer.data());
    }

    // Get the mount points associated with a volume
    std::set<std::filesystem::path> VolumeMountPoints(_In_ std::wstring const& volumeName)
    {
        TRACE_ALWAYS(L"");
        std::set<std::filesystem::path> result;

        std::vector<WCHAR> buffer(UNICODE_STRING_MAX_CHARS);
        DWORD needed{};
        if (FALSE == ::GetVolumePathNamesForVolumeNameW(volumeName.data(), buffer.data(), static_cast<DWORD>(buffer.size()), &needed))
        {
            if (ERROR_MORE_DATA != ::GetLastError()) THROW_WIN32_LAST_ERROR;
        }
        else
        {
            // Iterate all logical disks associated with this volume
            auto const list{ MultiStringToStringList(buffer) };
            for (auto it{ std::begin(list) }; (std::end(list) != it); it++) result.emplace(*it);
        }

        return (result);
    }

    // Iterates all volume mount points and calls the callback method for each entry found
    void IterateAllVolumeMountPointsForFileStorage(_In_ IterateAllVolumeMountPointsForFileStorageCallback iterateAllVolumeMountPointsForFileStorageCallback)
    {
        TRACE_ALWAYS(L"");

        // Prepare the volume iterator
        std::vector<WCHAR> volumeName(UNICODE_STRING_MAX_CHARS);
        auto const findVolumeClosePtr{ FindVolumeClosePtr(::FindFirstVolumeW(volumeName.data(), static_cast<DWORD>(volumeName.size())), &::FindVolumeClose) };
        if (INVALID_HANDLE_VALUE == findVolumeClosePtr.get()) THROW_WIN32_LAST_ERROR;

        // Iterate all volume names with syntax '\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}'
        while (true)
        {
            // Iterate all mount points for this volume
            for (auto const& volumeMountPoint : VolumeMountPoints(volumeName.data()))
            {
                // Bail out when the callback signals no more processing is needed (not continue)
                if (!iterateAllVolumeMountPointsForFileStorageCallback(volumeName.data(), volumeMountPoint)) return;
            }

            // Move to the next volume
            if (FALSE == ::FindNextVolumeW(findVolumeClosePtr.get(), volumeName.data(), static_cast<DWORD>(volumeName.size())))
            {
                if (ERROR_NO_MORE_FILES != ::GetLastError()) THROW_WIN32_LAST_ERROR;

                // No more volumes to iterate
                break;
            }
        }
    }

    // Find the volume mount point that could store the file specified
    // Returns an empty path when no suitable volume was found
    std::filesystem::path FindVolumeMountPointForFullyQualifiedFileName(_In_ std::filesystem::path const& fullyQualifiedFileName)
    {
        TRACE_ALWAYS(L"");

        auto const fullyQualifiedFileNameNative{ fullyQualifiedFileName.native() };
        std::wstring result;
        IterateAllVolumeMountPointsForFileStorage([&fullyQualifiedFileNameNative, &result](_In_ std::wstring const& volumeName, _In_ std::filesystem::path const& volumeMountPoint)
        {
            UNREFERENCED_PARAMETER(volumeName);
            auto const volumeMountPointNative{ volumeMountPoint.native() };
            // Paths are case-insensitive; callers may pass normalized lower-case paths
            auto const length{ static_cast<int>(volumeMountPointNative.size()) };
            if ((volumeMountPointNative.size() <= fullyQualifiedFileNameNative.size()) &&
                (CSTR_EQUAL == ::CompareStringOrdinal(volumeMountPointNative.c_str(), length, fullyQualifiedFileNameNative.c_str(), length, TRUE)))
            {
                // Keep track of the best, most specialized (hence longest) path while iterating
                if (volumeMountPointNative.size() > result.size()) result = volumeMountPoint;
            }

            // Continue iterating
            return (true);
        });

        return (result);
    }

    // Find the mount point and dos device name where the full image name can be stored
    // Returns an empty mount point when no suitable storage location was found
    VolumeMountPointAndDosDeviceName FindVolumeMountPointAndDosDeviceNameForFullImageName(_In_ HidHide::FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");

        auto const fullImageNameNative{ fullImageName.native() };
        VolumeMountPointAndDosDeviceName result;
        IterateAllVolumeMountPointsForFileStorage([&fullImageNameNative, &result](_In_ std::wstring const& volumeName, _In_ std::filesystem::path const& volumeMountPoint)
        {
            auto const dosDeviceName{ DosDeviceNameForVolumeName(volumeName) };
            auto const dosDeviceNameNative{ dosDeviceName.native() };
            if (0 == dosDeviceNameNative.compare(0, std::wstring::npos, fullImageNameNative, 0, dosDeviceNameNative.size()))
            {
                result = std::make_pair(volumeMountPoint, dosDeviceName);
                return (false);
            }

            // Continue iterating
            return (true);
        });

        return (result);
    }

    // Get the volume name associated with a volume mount point
    std::wstring VolumeNameForVolumeMountPoint(_In_ std::filesystem::path const& volumeMountPoint)
    {
        TRACE_ALWAYS(L"");
        std::vector<WCHAR> buffer(UNICODE_STRING_MAX_CHARS);
        if (FALSE == ::GetVolumeNameForVolumeMountPointW(volumeMountPoint.native().c_str(), buffer.data(), static_cast<DWORD>(buffer.size()))) THROW_WIN32_LAST_ERROR;
        return (buffer.data());
    }
}

namespace HidHide
{
    _Use_decl_annotations_
    FullImageName FileNameToFullImageName(std::filesystem::path const& fullyQualifiedFileName)
    {
        TRACE_ALWAYS(L"");
        auto const volumeMountPoint{ FindVolumeMountPointForFullyQualifiedFileName(fullyQualifiedFileName) };
        if (volumeMountPoint.empty()) return (FullImageName{});
        auto const dosDeviceName{ DosDeviceNameForVolumeName(VolumeNameForVolumeMountPoint(volumeMountPoint)) };
        auto const fileNameWithoutMountPoint{ std::filesystem::path(fullyQualifiedFileName.native().substr(volumeMountPoint.native().size())) };
        return (dosDeviceName / fileNameWithoutMountPoint);
    }

    _Use_decl_annotations_
    std::filesystem::path FullImageNameToFileName(_In_ FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");
        auto const volumeMountPointAndDosDeviceName{ FindVolumeMountPointAndDosDeviceNameForFullImageName(fullImageName) };
        auto const& [volumeMountPoint, dosDeviceName] { volumeMountPointAndDosDeviceName };
        if (volumeMountPoint.empty()) return (FullImageName{});
        auto const fullImageNameWithoutDosDeviceName{ std::filesystem::path(fullImageName.native().substr(dosDeviceName.native().size() + 1)) };
        return (volumeMountPoint / fullImageNameWithoutDosDeviceName);
    }

    _Use_decl_annotations_
    FullImageNames MissingWhitelistEntries(FullImageNames const& whitelist)
    {
        TRACE_ALWAYS(L"");
        FullImageNames result;
        for (auto const& fullImageName : whitelist)
        {
            // Only an entry on a present volume whose file is absent is provably gone. An entry on a
            // volume that is not mounted (e.g. an unplugged USB drive) or one that can't be checked is kept
            std::filesystem::path fileName;
            try { fileName = FullImageNameToFileName(fullImageName); } catch (...) { continue; }
            if (fileName.empty()) continue;
            std::error_code ec;
            auto const status{ std::filesystem::status(fileName, ec) };
            if (status.type() == std::filesystem::file_type::not_found) result.insert(fullImageName);
        }
        return (result);
    }
}
