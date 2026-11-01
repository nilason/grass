/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: Free Software Foundation, Inc.
 *
 * Compact adaptation of GNU Gnulib's pathname-component algorithms,
 * specifically basename-lgpl.c, dirname-lgpl.c, and filename.h.
 * Original code copyright (C) Free Software Foundation, Inc.
 * This is a modified derivative, not an unmodified Gnulib distribution.
 *
 * Source reference: GNU gettext 0.23 gnulib-lib files.
 */

/* Basic filename support macros.
   Copyright (C) 2001-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/* basename.c -- return the last element in a file name

   Copyright (C) 1990, 1998-2001, 2003-2006, 2009-2024 Free Software
   Foundation, Inc.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* dirname.c -- return all but the last element in a file name

   Copyright (C) 1990, 1998, 2000-2001, 2003-2006, 2009-2024 Free Software
   Foundation, Inc.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#if !defined(_WIN32)
#include <libgen.h>

char *G_dirname(char *path)
{

    return dirname(path);
}

#elif defined(_WIN32)

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// #if defined(_MSC_VER)
#define GL_DOS_PATHS 1
// #else
// #define GL_DOS_PATHS 0
// #endif

static int gl_slash(char c)
{
    return c == '/' || (GL_DOS_PATHS && c == '\\');
}

static int gl_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static size_t gl_prefix_len(const char *s)
{
    return GL_DOS_PATHS && gl_alpha(s[0]) && s[1] == ':' ? 2 : 0;
}

/* Identify the start of the last nonempty pathname component. */
static const char *gl_last_component(const char *s)
{
    const char *p = s + gl_prefix_len(s);
    const char *last = p;
    int in_component = 0;

    for (; *p; ++p) {
        if (gl_slash(*p)) {
            in_component = 0;
        }
        else if (!in_component) {
            last = p;
            in_component = 1;
        }
    }
    return last;
}

/* Length of the directory portion, following Gnulib's dir_len model. */
static size_t gl_dir_len(const char *s)
{
    size_t prefix = gl_prefix_len(s);
    size_t length;

    if (prefix) {
        if (gl_slash(s[prefix]))
            ++prefix;
    }
    else if (gl_slash(s[0])) {
        prefix = 1;
#if !GL_DOS_PATHS
        /* Preserve // as a distinct root when it has exactly two slashes. */
        if (s[1] == '/' && s[2] != '/')
            prefix = 2;
#endif
    }

    length = (size_t)(gl_last_component(s) - s);
    if (length < prefix)
        length = prefix;
    while (length > prefix && gl_slash(s[length - 1]))
        --length;
    return length;
}

/* Gnulib-style allocated dirname. Caller frees the returned pointer. */
static char *mdir_name(const char *path)
{
    size_t length, prefix, size;
    int append_dot;
    char *result;

    if (!path)
        path = "";
    length = gl_dir_len(path);
    prefix = gl_prefix_len(path);
    append_dot =
        length == 0 || (GL_DOS_PATHS && length == prefix &&
                        path[prefix] != '\0' && !gl_slash(path[prefix]));
    size = length + (size_t)append_dot + 1;
    result = malloc(size);
    if (!result)
        return NULL;
    memcpy(result, path, length);
    if (append_dot)
        result[length++] = '.';
    result[length] = '\0';
    return result;
}

/* POSIX-style interface: may modify path, returns path or static dot.
 * Input must be writable and NUL-terminated; do not pass string literals.
 * The implementation is not guaranteed to preserve POSIX-only path syntax
 * on Windows, because backslashes and drive prefixes are recognised.
 */
char *G_dirname(char *path)
{
    static char dot[] = ".";
    char *dir;
    size_t n;

    if (!path || !*path)
        return dot;
    dir = mdir_name(path);
    if (!dir)
        return NULL;
    n = strlen(dir);
    /* The allocated result never exceeds the original input length. */
    if (n > strlen(path)) {
        free(dir);
        return NULL;
    }
    memcpy(path, dir, n + 1);
    free(dir);
    return path;
}
#endif // _WIN32
