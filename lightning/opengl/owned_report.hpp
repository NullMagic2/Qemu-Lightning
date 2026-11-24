/*
Module: qemu_lightning.opengl.owned_report
Purpose: Publish owned diagnostic bytes through guest filesystem caching.
Created: 2026-10-22
Architecture: Shared test boundary; never opens or inherits a device handle.
*/
#pragma once
#include <windows.h>
#include <cstdio>
#include <io.h>

namespace qemu_lightning::opengl {
inline void flush_report(FILE* report) noexcept {
    const DWORD previous_error = GetLastError();
    std::fflush(report);
    const auto descriptor = _get_osfhandle(_fileno(report));
    if (descriptor != -1) {
        const HANDLE output = reinterpret_cast<HANDLE>(descriptor);
        if (GetFileType(output) == FILE_TYPE_DISK) FlushFileBuffers(output);
    }
    SetLastError(previous_error);
}
}
