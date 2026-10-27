/* SPDX-License-Identifier: MIT
 * Standalone MSVC realpath() compatibility implementation.
 * Independently adapted from the approach in libuv src/win/fs.c.
 * Input/output strings are UTF-8 (not libuv's WTF-8).
 * Copyright (c) 2026 OpenAI. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef _WIN32
#error This implementation is Windows-only.
#endif

#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void set_path_errno(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
        errno = EACCES;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
        errno = ENAMETOOLONG;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
}

static wchar_t *to_utf16(const char *input)
{
    int count;
    wchar_t *result;
    count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
    if (!count) {
        errno = EINVAL;
        return NULL;
    }
    result = malloc((size_t)count * sizeof(*result));
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, result,
                             count)) {
        free(result);
        errno = EINVAL;
        return NULL;
    }
    return result;
}

static char *to_utf8(const wchar_t *input)
{
    int count;
    char *result;
    count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, NULL,
                                0, NULL, NULL);
    if (!count) {
        errno = EILSEQ;
        return NULL;
    }
    result = malloc((size_t)count);
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, result,
                             count, NULL, NULL)) {
        free(result);
        errno = EILSEQ;
        return NULL;
    }
    return result;
}

/* For resolved_path != NULL, the caller must supply a sufficiently large
 * buffer, as with the historical POSIX interface. Prefer NULL instead. */
char *realpath(const char *path, char *resolved_path)
{
    wchar_t *wide = NULL, *final = NULL;
    const wchar_t *normal;
    char *utf8 = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD capacity, written, error;
    size_t bytes;

    if (!path || !*path) {
        errno = EINVAL;
        return NULL;
    }
    wide = to_utf16(path);
    if (!wide)
        return NULL;

    handle = CreateFileW(wide, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        wchar_t canonical_path[4096] = {0};
        DWORD result = GetFullPathNameW(wide, 4096, canonical_path, NULL);
        if (result > 0) {
            handle = CreateFileW(
                canonical_path, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (handle == INVALID_HANDLE_VALUE) {
                set_path_errno(GetLastError());
                return NULL;
            }
        }
        else {
            set_path_errno(GetLastError());
            return NULL;
        }
    }
    free(wide);

    capacity = GetFinalPathNameByHandleW(
        handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!capacity) {
        error = GetLastError();
        goto win_error;
    }
    for (;;) {
        if ((size_t)capacity > SIZE_MAX / sizeof(*final)) {
            errno = ENOMEM;
            goto fail;
        }
        final = malloc((size_t)capacity * sizeof(*final));
        if (!final) {
            errno = ENOMEM;
            goto fail;
        }
        written = GetFinalPathNameByHandleW(
            handle, final, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!written) {
            error = GetLastError();
            goto win_error;
        }
        if (written < capacity)
            break;
        free(final);
        final = NULL;
        capacity = written + 1;
        if (!capacity) {
            errno = ENAMETOOLONG;
            goto fail;
        }
    }
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;

    /* Convert \\?\C:\... to C:\... and \\?\UNC\... to \\... */
    if (wcsncmp(final, L"\\\\?\\UNC\\", 8) == 0) {
        final[6] = L'\\';
        final[7] = L'\\';
        normal = final + 6;
    }
    else if (wcsncmp(final, L"\\\\?\\", 4) == 0) {
        normal = final + 4;
    }
    else {
        normal = final;
    }

    utf8 = to_utf8(normal);
    free(final);
    if (!utf8)
        return NULL;
    if (!resolved_path)
        return utf8;
    bytes = strlen(utf8) + 1;
    /* POSIX realpath() does not pass the destination capacity. */
    memcpy(resolved_path, utf8, bytes);
    free(utf8);
    return resolved_path;

win_error:
    set_path_errno(error);
fail:
    free(final);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    return NULL;
}
