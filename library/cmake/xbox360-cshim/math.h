#ifndef BOREALIS_XBOX360_MATH_H
#define BOREALIS_XBOX360_MATH_H

#include_next <math.h>

/* The Xbox 360 XDK CRT predates C99 roundf. */
static __inline float roundf(float value)
{
    return value >= 0.0f ? floorf(value + 0.5f) : ceilf(value - 0.5f);
}

#endif
