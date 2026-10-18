#ifndef _BITS_TYPES_MBSTATE_T_H
#define _BITS_TYPES_MBSTATE_T_H 1

/*
 * libc++ probes this glibc header by name even for the freestanding Xenon
 * target.  The XDK CRT uses the same representation, but has no bits/types
 * hierarchy of its own.  Providing the target definition here prevents the
 * host glibc typedef from entering the cross build.
 */
typedef int mbstate_t;

#endif
