#ifndef BOREALIS_XBOX360_STDLIB_H
#define BOREALIS_XBOX360_STDLIB_H

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
long long __cdecl strtoll(const char*, char**, int);
unsigned long long __cdecl strtoull(const char*, char**, int);

/* The Xbox 360 XDK CRT exposes strtod but not the C99 strtof entry point. */
static __inline float strtof(const char* text, char** end)
{
    return (float)strtod(text, end);
}
#ifdef __cplusplus
}
#endif

#endif
