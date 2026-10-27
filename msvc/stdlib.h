#ifndef GRASS_MSVC_STDLIB_H
#define GRASS_MSVC_STDLIB_H

#include <../ucrt/stdlib.h>

#define random rand
char *realpath(const char *path, char *resolved_path);

#endif
