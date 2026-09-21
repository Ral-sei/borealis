// Xbox 360 entry point for the vendored NanoVG D3D9 backend.
// The XDK exposes the D3D9 device ABI; window creation and presentation are
// owned by Xbox360VideoContext and must not be pulled in by this header.
#ifndef BOREALIS_NANOVG_D3D9_XBOX360_H
#define BOREALIS_NANOVG_D3D9_XBOX360_H

#if !defined(PLATFORM_XBOX360) && !defined(__XBOX360__) && !defined(_XBOX) && !defined(_XENON)
#error "nanovg_d3d9_xbox360.h requires the Xbox 360/XDK build"
#endif

#if defined(_XBOX) || defined(_XENON)
// XDK d3d9.h expects the normal XDK scalar typedefs to be visible first.
#include <xdk.h>
#include <excpt.h>
#include <windef.h>
#include <winbase.h>
#include <xbox.h>
#endif

#include "nanovg_d3d9.h"

#endif
