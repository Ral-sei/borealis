//
// Copyright (c) 2009-2013 Mikko Mononen memon@inside.org
// Port of _gl.h to _d3d9.h by Patrick Mours
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//
#ifndef NANOVG_D3D9_H
#define NANOVG_D3D9_H

#include <d3d9.h>

#ifdef __cplusplus
extern "C" {
#endif

enum NVGcreateFlags {
	// Flag indicating if geometry based anti-aliasing is used (may not be needed when using MSAA).
	NVG_ANTIALIAS 		= 1<<0,
	// Flag indicating if strokes should be drawn using stencil buffer. The rendering will be a little
	// slower, but path overlaps (i.e. self-intersecting or sharp turns) will be drawn just once.
	NVG_STENCIL_STROKES	= 1<<1,
};

struct NVGcontext* nvgCreateD3D9(IDirect3DDevice9* pDevice, int flags);
void nvgDeleteD3D9(struct NVGcontext* ctx);

// These are additional flags on top of NVGimageFlags.
enum NVGimageFlagsD3D9 {
	NVG_IMAGE_NODELETE			= 1<<16,	// Do not delete texture object.
};

#ifdef __cplusplus
}
#endif

#endif /* NANOVG_D3D9_H */

#ifdef NANOVG_D3D9_IMPLEMENTATION

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "nanovg.h"

#include "D3D9VertexShader.h"
#include "D3D9PixelShaderAA.h"
#include "D3D9PixelShader.h"

#if defined(_XBOX) || defined(_XENON)
#define NANOVG_D3D9_USAGE_DYNAMIC 0
#define NANOVG_D3D9_USAGE_WRITEONLY 0
#define NANOVG_D3D9_USAGE_AUTOGENMIPMAP 0
#define NANOVG_D3D9_LOCK_DISCARD 0
#else
#define NANOVG_D3D9_USAGE_DYNAMIC D3DUSAGE_DYNAMIC
#define NANOVG_D3D9_USAGE_WRITEONLY D3DUSAGE_WRITEONLY
#define NANOVG_D3D9_USAGE_AUTOGENMIPMAP D3DUSAGE_AUTOGENMIPMAP
#define NANOVG_D3D9_LOCK_DISCARD D3DLOCK_DISCARD
#endif

enum D3D9NVGshaderType {
	NSVG_SHADER_FILLGRAD,
	NSVG_SHADER_FILLIMG,
	NSVG_SHADER_SIMPLE,
	NSVG_SHADER_IMG
};

struct D3D9NVGshader {
	IDirect3DPixelShader9* frag;
	IDirect3DVertexShader9* vert;
};

struct D3D9NVGtexture {
	int id;
	IDirect3DTexture9* tex;
	int width, height;
	int type;
	int flags;
};

enum D3D9NVGcallType {
	D3D9NVG_NONE = 0,
	D3D9NVG_FILL,
	D3D9NVG_CONVEXFILL,
	D3D9NVG_STROKE,
	D3D9NVG_TRIANGLES
};

struct D3D9NVGcall {
	int type;
	int image;
	int pathOffset;
	int pathCount;
	int triangleOffset;
	int triangleCount;
	int uniformOffset;
};

struct D3D9NVGpath {
	int fillOffset;
	int fillCount;
	int strokeOffset;
	int strokeCount;
};

struct D3D9NVGvertexbuffer {
	#if defined(_XBOX) || defined(_XENON)
	IDirect3DVertexBuffer9* pBuffers[8];
	unsigned int bufferSizes[8];
	unsigned int currentBuffer;
	#else
	unsigned int MaxBufferEntries;
	unsigned int CurrentBufferEntry;
	IDirect3DVertexBuffer9* pBuffer;
	#endif
};

struct D3D9NVGfragUniforms {
	// note: after modifying layout or size of uniform array,
	// don't forget to also update the fragment shader source!
	#define NANOVG_D3D9_UNIFORMARRAY_SIZE 14
	union {
		struct {
			float scissorMat[16]; // matrices are actually 3 vec4s
			float scissorExt[2];
			float scissorScale[2];
			float paintMat[16];
			struct NVGcolor innerCol;
			struct NVGcolor outerCol;
			float extent[2];
			float radius;
			float feather;
			float strokeMult[4];
			float type;
			float texType;
		};
		float uniformArray[NANOVG_D3D9_UNIFORMARRAY_SIZE][4];
	};
};

struct D3D9NVGcontext {
	
	struct D3D9NVGshader shader;
	struct D3D9NVGtexture* textures;
	float view[4];    
	int ntextures;
	int ctextures;
	int textureId;

	int fragSize;
	int flags;

	// Per frame buffers
	struct D3D9NVGcall* calls;
	int ccalls;
	int ncalls;
	struct D3D9NVGpath* paths;
	int cpaths;
	int npaths;
	struct NVGvertex* verts;
	int cverts;
	int nverts;
	unsigned char* uniforms;
	int cuniforms;
	int nuniforms;

	// D3D9
	// Geometry
	struct D3D9NVGvertexbuffer VertexBuffer;
	IDirect3DVertexDeclaration9* pLayoutRenderTriangles;

	// State
	IDirect3DDevice9* pDevice;
};

static int D3D9nvg__maxi(int a, int b) { return a > b ? a : b; }

static struct D3D9NVGtexture* D3D9nvg__allocTexture(struct D3D9NVGcontext* D3D)
{
	struct D3D9NVGtexture* tex = NULL;
	int i;

	for (i = 0; i < D3D->ntextures; i++) {
		if (D3D->textures[i].id == 0) {
			tex = &D3D->textures[i];
			break;
		}
	}
	if (tex == NULL) {
		if (D3D->ntextures + 1 > D3D->ctextures) {
			struct D3D9NVGtexture* textures;
			int ctextures = D3D9nvg__maxi(D3D->ntextures+1, 4) +  D3D->ctextures/2; // 1.5x Overallocate
			textures = (struct D3D9NVGtexture*)realloc(D3D->textures, sizeof(struct D3D9NVGtexture)*ctextures);
			if (textures == NULL) return NULL;
			D3D->textures = textures;
			D3D->ctextures = ctextures;
		}
		tex = &D3D->textures[D3D->ntextures++];
	}

	memset(tex, 0, sizeof(*tex));
	tex->id = ++D3D->textureId;

	return tex;
}

static struct D3D9NVGtexture* D3D9nvg__findTexture(struct D3D9NVGcontext* D3D, int id)
{
	int i;
	for (i = 0; i < D3D->ntextures; i++)
		if (D3D->textures[i].id == id)
			return &D3D->textures[i];
	return NULL;
}

static int D3D9nvg__deleteTexture(struct D3D9NVGcontext* D3D, int id)
{
	int i;
	for (i = 0; i < D3D->ntextures; i++) {
		if (D3D->textures[i].id == id) {
			if (D3D->textures[i].tex != NULL && (D3D->textures[i].flags & NVG_IMAGE_NODELETE) == 0)
			{
				IDirect3DTexture9_Release(D3D->textures[i].tex);
			}
			memset(&D3D->textures[i], 0, sizeof(D3D->textures[i]));
			return 1;
		}
	}
	return 0;
}

static int D3D9nvg__createShader(struct D3D9NVGcontext* D3D, struct D3D9NVGshader* shader, const void* vshader, const void* fshader)
{
	IDirect3DVertexShader9* vert = NULL;
	IDirect3DPixelShader9* frag = NULL;
	HRESULT hr;
	
	memset(shader, 0, sizeof(*shader));

	// Shader byte code is created at compile time from the .hlsl files.
	// No need for error checks; shader errors can be fixed in the IDE.
	hr = IDirect3DDevice9_CreateVertexShader(D3D->pDevice, (const DWORD*)vshader, &vert);

	if (FAILED(hr))
		return 0;

	hr = IDirect3DDevice9_CreatePixelShader(D3D->pDevice, (const DWORD*)fshader, &frag);

	if (FAILED(hr))
	{
		IDirect3DVertexShader9_Release(vert);
		return 0;
	}
	
	shader->vert = vert;
	shader->frag = frag;

	return 1;
}

static void D3D9nvg__deleteShader(struct D3D9NVGshader* shader)
{
	if (shader->vert != NULL)
		IDirect3DVertexShader9_Release(shader->vert);
	if (shader->frag != NULL)
		IDirect3DPixelShader9_Release(shader->frag);
}

#if defined(_XBOX) || defined(_XENON)
static IDirect3DVertexBuffer9* D3D9nvg_updateVertexBuffer(struct D3D9NVGcontext* D3D)
{
	struct D3D9NVGvertexbuffer* vb = &D3D->VertexBuffer;
	const unsigned int index = vb->currentBuffer;
	const unsigned int size = (unsigned int)D3D->nverts * sizeof(NVGvertex);
	IDirect3DVertexBuffer9* buffer = vb->pBuffers[index];
	unsigned int lockSize;
	HRESULT hr;
	void* data = NULL;

	// Xenon has no D3DLOCK_DISCARD. Rotate buffers so the GPU can finish reading
	// earlier submissions before the CPU reuses their storage.
	vb->currentBuffer = (index + 1) % 8;
	if (vb->bufferSizes[index] < size)
	{
		IDirect3DVertexBuffer9* replacement = NULL;
		hr = IDirect3DDevice9_CreateVertexBuffer(D3D->pDevice, size,
			NANOVG_D3D9_USAGE_DYNAMIC | NANOVG_D3D9_USAGE_WRITEONLY, 0,
			D3DPOOL_DEFAULT, &replacement, NULL);
		if (FAILED(hr))
			return NULL;

		if (buffer)
			IDirect3DVertexBuffer9_Release(buffer);
		vb->pBuffers[index] = buffer = replacement;
		vb->bufferSizes[index] = size;
	}

	lockSize = vb->bufferSizes[index];
	hr = IDirect3DVertexBuffer9_Lock(buffer, 0, lockSize, &data, 0);
	if (FAILED(hr) || !data)
		return NULL;
	memcpy(data, D3D->verts, size);
	hr = IDirect3DVertexBuffer9_Unlock(buffer);
	if (FAILED(hr))
		return NULL;

	return buffer;
}
#else
static unsigned int D3D9nvg_updateVertexBuffer(struct D3D9NVGcontext* D3D)
{
	void* data;
	unsigned int offset = D3D->VertexBuffer.CurrentBufferEntry * sizeof(NVGvertex), size = D3D->nverts * sizeof(NVGvertex);
	HRESULT hr;

	if (D3D->nverts > D3D->VertexBuffer.MaxBufferEntries)
	{
		IDirect3DVertexBuffer9_Release(D3D->VertexBuffer.pBuffer);

		while (D3D->VertexBuffer.MaxBufferEntries < D3D->nverts)
			D3D->VertexBuffer.MaxBufferEntries *= 2;
		D3D->VertexBuffer.CurrentBufferEntry = 0;
		hr = IDirect3DDevice9_CreateVertexBuffer(D3D->pDevice, sizeof(NVGvertex) * D3D->VertexBuffer.MaxBufferEntries, NANOVG_D3D9_USAGE_DYNAMIC | NANOVG_D3D9_USAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &D3D->VertexBuffer.pBuffer, NULL);

		if (FAILED(hr))
		{
			ZeroMemory(&D3D->VertexBuffer, sizeof(D3D->VertexBuffer));
			return 0;
		}
	}
	if ((D3D->VertexBuffer.CurrentBufferEntry + D3D->nverts) >= D3D->VertexBuffer.MaxBufferEntries || D3D->VertexBuffer.CurrentBufferEntry == 0)
	{
		offset = 0;
		D3D->VertexBuffer.CurrentBufferEntry = 0;
		IDirect3DVertexBuffer9_Lock(D3D->VertexBuffer.pBuffer, 0, size, &data, NANOVG_D3D9_LOCK_DISCARD);
	}
	else
	{
		IDirect3DVertexBuffer9_Lock(D3D->VertexBuffer.pBuffer, offset, size, &data, D3DLOCK_NOOVERWRITE);
	}

	memcpy(data, D3D->verts, size);
	
	IDirect3DVertexBuffer9_Unlock(D3D->VertexBuffer.pBuffer);

	D3D->VertexBuffer.CurrentBufferEntry += D3D->nverts;

	return offset;
}
#endif

static int D3D9nvg__renderCreate(void* uptr)
{
	HRESULT hr;
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;

	const D3DVERTEXELEMENT9 LayoutRenderTriangles[] = 
	{
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		D3DDECL_END()
	}; 
	
	if (D3D->flags & NVG_ANTIALIAS) {
		if (D3D9nvg__createShader(D3D, &D3D->shader, g_vs30_D3D9VertexShader_Main, g_ps30_D3D9PixelShaderAA_Main) == 0)
			return 0;
	}
	else {
		if (D3D9nvg__createShader(D3D, &D3D->shader, g_vs30_D3D9VertexShader_Main, g_ps30_D3D9PixelShader_Main) == 0)
			return 0;
	}

#if defined(_XBOX) || defined(_XENON)
	D3D->VertexBuffer.currentBuffer = 0;
#else
	D3D->VertexBuffer.MaxBufferEntries = 20000;
	D3D->VertexBuffer.CurrentBufferEntry = 0;

	// Create the vertex buffer.
	hr = IDirect3DDevice9_CreateVertexBuffer(D3D->pDevice, sizeof(NVGvertex) * D3D->VertexBuffer.MaxBufferEntries, NANOVG_D3D9_USAGE_DYNAMIC | NANOVG_D3D9_USAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &D3D->VertexBuffer.pBuffer, NULL);
	if (FAILED(hr))
	{
		D3D9nvg__deleteShader(&D3D->shader);
		return 0;
	}
#endif

	hr = IDirect3DDevice9_CreateVertexDeclaration(D3D->pDevice, LayoutRenderTriangles, &D3D->pLayoutRenderTriangles);
	
	if (FAILED(hr))
	{
		D3D9nvg__deleteShader(&D3D->shader);
#if defined(_XBOX) || defined(_XENON)
		int i;
		for (i = 0; i < 8; i++)
			if (D3D->VertexBuffer.pBuffers[i])
				IDirect3DVertexBuffer9_Release(D3D->VertexBuffer.pBuffers[i]);
#else
		IDirect3DVertexBuffer9_Release(D3D->VertexBuffer.pBuffer);
#endif
		return 0;
	}

	D3D->fragSize = sizeof(struct D3D9NVGfragUniforms) + 16 - sizeof(struct D3D9NVGfragUniforms) % 16;
	
	return 1;
}

#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
#ifdef __cplusplus
extern "C" {
#endif
extern void DbgPrint(const char* fmt, ...);
#ifdef __cplusplus
}
#endif
#include <stdlib.h>
#include <xbox.h> /* XMemCpyStreaming_WriteCombined (xapilib.lib) */
/* P0 #9 探针：把一次建纹理拆成 CreateTexture(分配 51.8MB) 与 逐行填充+字节交换
   两段计时，看 12.4s 到底花在哪一段。 */
static double D3D9nvg__nowMs(void)
{
	static LARGE_INTEGER freq = {0};
	LARGE_INTEGER now;
	if (freq.QuadPart == 0)
	{
		QueryPerformanceFrequency(&freq);
		if (freq.QuadPart == 0) freq.QuadPart = 1;
	}
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#endif

/* Copy pixel data into a LockRect'ed texture row/block.
   On Xbox 360 the pointer returned by LockRect targets write-combined memory.
   A plain memcpy of a row whose length is not a multiple of four, or of a row
   that is not gap-free, makes the Xenon WC store buffers flush partially
   ("Bad Write-Combined Writes"), and reading the destination back (the RGBA
   byte swap used to do that in place) is an "Uncached/Write-Combined Read"
   that the XDK requires to be repaired.  XMemCpyStreaming_WriteCombined is the
   XDK's cached -> write-combined streaming copy: it drives the dcbt/dcbf/
   dcbz128 sequence itself and keeps its L2 footprint at 128 KB
   (Kernel_Write_Combined / XMemCpyStreaming_WriteCombined). */
static void D3D9nvg__copyToTexture(unsigned char* dest, const unsigned char* src, size_t count)
{
	if (count == 0)
		return;
#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
	XMemCpyStreaming_WriteCombined(dest, src, count);
#else
	memcpy(dest, src, count);
#endif
}

static int D3D9nvg__renderCreateTexture(void* uptr, int type, int w, int h, int imageFlags, const unsigned char* data)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGtexture* tex = D3D9nvg__allocTexture(D3D);
	D3DLOCKED_RECT locked;
	HRESULT hr;
	INT levels = 1, usage = NANOVG_D3D9_USAGE_DYNAMIC, pixelWidthBytes;
	D3DFORMAT format;
	int y;
	unsigned char *texData;
	
	if (tex == NULL)
	{
		return 0;
	}

	tex->width = w;
	tex->height = h;
	tex->type = type;
	tex->flags = imageFlags;

	// Mip maps
	if (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS)
	{
		levels = 0;
		usage |= NANOVG_D3D9_USAGE_AUTOGENMIPMAP;
	}

	if (type == NVG_TEXTURE_RGBA)
	{
		#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
		// Xbox 360 32-bit texture formats use 8-in-32 endian swapping.
		format = D3DFMT_LIN_A8B8G8R8;
		#else
		format = D3DFMT_A8R8G8B8;
		#endif
		pixelWidthBytes = 4;
	}
	else
	{
		#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
		format = D3DFMT_LIN_L8;
		#else
		format = D3DFMT_L8;
		#endif
		pixelWidthBytes = 1;
	}

#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
	const double tCreate0 = D3D9nvg__nowMs();
#endif
	hr = IDirect3DDevice9_CreateTexture(D3D->pDevice, w, h, levels, usage, format, D3DPOOL_DEFAULT, &tex->tex, NULL);

	if (FAILED(hr))
	{
		return 0;
	}
#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
	const double tFill0 = D3D9nvg__nowMs();
#endif

	if (data != NULL)
	{
		hr = IDirect3DTexture9_LockRect(tex->tex, 0, &locked, NULL, 0);

		if (SUCCEEDED(hr))
		{
			texData = (unsigned char*)locked.pBits;

			/* 4800x2700 快照图：原先先 memcpy 进纹理、再在纹理显存上逐像素做 8IN32
			   字节交换，等于对 write-combined 内存做 1296 万次随机字节读写
			   （2 读 + 4 写/像素）→ 实测 12429ms（≈957ns/像素），主线程整段冻结。
			   现在改为：字节交换在一块普通缓存内存里完成，再整行顺序写入纹理。 */
			unsigned char* scratch = NULL;
			if (type == NVG_TEXTURE_RGBA)
			{
				scratch = (unsigned char*)malloc((size_t)w * 4);
			}

			// Have to copy by row since pitch can be different
			for (y = 0; y < h; y++) {
				unsigned char* destination = texData + y * locked.Pitch;
				const unsigned char* source = data + y * w * pixelWidthBytes;
				if (type == NVG_TEXTURE_RGBA)
				{
					// NanoVG supplies little-endian RGBA bytes (r,g,b,a).
					// The big-endian GPU reads LIN_A8B8G8R8 as a 32-bit word,
					// so each pixel needs the documented GPUENDIAN_8IN32 swap
					// (full byte reversal "abcdefgh" -> "dcbahgfe"), leaving
					// (a,b,g,r) in memory. 结果与原实现逐字节一致。
					int i;
					if (scratch != NULL)
					{
						for (i = 0; i < w * 4; i += 4)
						{
							unsigned char b0 = source[i];
							unsigned char b1 = source[i + 1];
							scratch[i]     = source[i + 3];
							scratch[i + 1] = source[i + 2];
							scratch[i + 2] = b1;
							scratch[i + 3] = b0;
						}
						D3D9nvg__copyToTexture(destination, scratch, (size_t)w * 4);
					}
					else
					{
						/* 分配失败时退回原实现（在纹理上原地交换，慢但正确） */
						D3D9nvg__copyToTexture(destination, source, (size_t)w * pixelWidthBytes);
						for (i = 0; i < w * 4; i += 4)
						{
							unsigned char b0 = destination[i];
							unsigned char b1 = destination[i + 1];
							destination[i] = destination[i + 3];
							destination[i + 1] = destination[i + 2];
							destination[i + 2] = b1;
							destination[i + 3] = b0;
						}
					}
				}
				else
				{
					D3D9nvg__copyToTexture(destination, source, (size_t)w * pixelWidthBytes);
				}
			}
			if (scratch != NULL)
			{
				free(scratch);
			}

			IDirect3DTexture9_UnlockRect(tex->tex, 0);
		}
	}
#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
	{
		const double tEnd = D3D9nvg__nowMs();
		/* 大纹理（≥400 万像素，即 4800x2700 快照）必打，用于正面确认修复生效 */
#if X360_RUNTIME_PROBES
		if ((long long)w * h >= 4000000 || tEnd - tCreate0 >= 100.0)
			DbgPrint("[tex] %dx%d type=%d create=%.0fms fill=%.0fms total=%.0fms",
			         w, h, type, tFill0 - tCreate0, tEnd - tFill0, tEnd - tCreate0);
#endif
	}
#endif

	return tex->id;
}

static int D3D9nvg__renderDeleteTexture(void* uptr, int image)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	return D3D9nvg__deleteTexture(D3D, image);
}

static int D3D9nvg__renderUpdateTexture(void* uptr, int image, int x, int y, int w, int h, const unsigned char* data)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGtexture* tex = D3D9nvg__findTexture(D3D, image);
	RECT rect;
	D3DLOCKED_RECT locked;
	HRESULT hr;
	INT pixelWidthBytes;
	unsigned char* texData;
	int row;
	
	if (tex == NULL)
	{
		return 0;
	}

	rect.left = x;
	rect.right = (x + w);
	rect.top = y;
	rect.bottom = (y + h);

	hr = IDirect3DTexture9_LockRect(tex->tex, 0, &locked, &rect, 0);

	if (FAILED(hr))
	{
		return 0;
	}

	if (tex->type == NVG_TEXTURE_RGBA)
	{
		pixelWidthBytes = 4;
	}
	else
	{
		pixelWidthBytes = 1;
	}

	texData = (unsigned char*)locked.pBits;

	// RGBA updates need the same GPUENDIAN_8IN32 byte swap as texture creation,
	// but doing it in place on the locked texture means reading back from
	// write-combined memory (an "Uncached/Write-Combined Read").  Swap into a
	// cached scratch row first, then copy it out with the streaming write
	// (mirrors D3D9nvg__renderCreateTexture).
	unsigned char* scratch = NULL;
	if (tex->type == NVG_TEXTURE_RGBA)
		scratch = (unsigned char*)malloc((size_t)w * 4);

	for (row = 0; row < h; row++)
	{
		unsigned char* destination = texData + row * locked.Pitch;
		const unsigned char* source = data + ((y + row) * tex->width + x) * pixelWidthBytes;
		if (tex->type == NVG_TEXTURE_RGBA)
		{
			int i;
			if (scratch != NULL)
			{
				// Keep partial atlas updates in the same GPUENDIAN_8IN32 byte
				// order (a,b,g,r) as texture creation.
				for (i = 0; i < w * 4; i += 4)
				{
					unsigned char b0 = source[i];
					unsigned char b1 = source[i + 1];
					scratch[i]     = source[i + 3];
					scratch[i + 1] = source[i + 2];
					scratch[i + 2] = b1;
					scratch[i + 3] = b0;
				}
				D3D9nvg__copyToTexture(destination, scratch, (size_t)w * 4);
			}
			else
			{
				/* 分配失败时退回原实现（在纹理上原地交换，慢但正确） */
				D3D9nvg__copyToTexture(destination, source, (size_t)w * pixelWidthBytes);
				for (i = 0; i < w * 4; i += 4)
				{
					unsigned char b0 = destination[i];
					unsigned char b1 = destination[i + 1];
					destination[i] = destination[i + 3];
					destination[i + 1] = destination[i + 2];
					destination[i + 2] = b1;
					destination[i + 3] = b0;
				}
			}
		}
		else
		{
			D3D9nvg__copyToTexture(destination, source, (size_t)w * pixelWidthBytes);
		}

	}
	if (scratch != NULL)
		free(scratch);

	IDirect3DTexture9_UnlockRect(tex->tex, 0);
 
	return 1;
}

static int D3D9nvg__renderGetTextureSize(void* uptr, int image, int* w, int* h)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGtexture* tex = D3D9nvg__findTexture(D3D, image);
	if (tex == NULL)
	{
		return 0;
	}
	*w = tex->width;
	*h = tex->height;
	return 1;
}

static void D3D9nvg__xformToMat3x3(float* m3, float* t)
{
	m3[0] = t[0];
	m3[1] = t[1];
	m3[2] = 0.0f;
	m3[3] = t[2];
	m3[4] = t[3];
	m3[5] = 0.0f;
	m3[6] = t[4];
	m3[7] = t[5];
	m3[8] = 1.0f;
}
static void D3D9nvg_copyMatrix3to4(float* pDest, const float* pSource)
{
	unsigned int i;
	for (i = 0; i < 4; i++)
	{
		memcpy(&pDest[i * 4], &pSource[i * 3], sizeof(float) * 3);
	}
}

static struct NVGcolor D3D9nvg__premulColor(struct NVGcolor c)
{
	c.r *= c.a;
	c.g *= c.a;
	c.b *= c.a;
	return c;
}

static int D3D9nvg__convertPaint(struct D3D9NVGcontext* D3D, struct D3D9NVGfragUniforms* frag,
	struct NVGpaint* paint, NVGcompositeOperationState compositeOperation, struct NVGscissor* scissor,
	float width, float fringe, float strokeThr)
{
	struct D3D9NVGtexture* tex = NULL;
	float invxform[6], paintMat[9], scissorMat[9];

	memset(frag, 0, sizeof(*frag));

	frag->innerCol = D3D9nvg__premulColor(paint->innerColor);
	frag->outerCol = D3D9nvg__premulColor(paint->outerColor);

	//TODO: Use compositeOperation
	
	if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) 
	{
		frag->scissorExt[0] = 1.0f;
		frag->scissorExt[1] = 1.0f;
		frag->scissorScale[0] = 1.0f;
		frag->scissorScale[1] = 1.0f;
	}
	else 
	{
		nvgTransformInverse(invxform, scissor->xform);
		D3D9nvg__xformToMat3x3(scissorMat, invxform);
		D3D9nvg_copyMatrix3to4(frag->scissorMat, scissorMat);
		frag->scissorExt[0] = scissor->extent[0];
		frag->scissorExt[1] = scissor->extent[1];
		frag->scissorScale[0] = sqrtf(scissor->xform[0] * scissor->xform[0] + scissor->xform[2] * scissor->xform[2]) / fringe;
		frag->scissorScale[1] = sqrtf(scissor->xform[1] * scissor->xform[1] + scissor->xform[3] * scissor->xform[3]) / fringe;
	}

	frag->extent[0] = paint->extent[0];
	frag->extent[1] = paint->extent[1];

	frag->strokeMult[0] = (width*0.5f + fringe*0.5f) / fringe;
	frag->strokeMult[1] = strokeThr;

	if (paint->image != 0) 
	{
		tex = D3D9nvg__findTexture(D3D, paint->image);
		if (tex == NULL)
		{
			return 0;
		}
		
		if ((tex->flags & NVG_IMAGE_FLIPY) != 0) 
		{
			float flipped[6];
			nvgTransformScale(flipped, 1.0f, -1.0f);
			nvgTransformMultiply(flipped, paint->xform);
			nvgTransformInverse(invxform, flipped);
		} 
		else
		{
			nvgTransformInverse(invxform, paint->xform);
		}
		frag->type = NSVG_SHADER_FILLIMG;
		
		if (tex->type == NVG_TEXTURE_RGBA)
		{
			frag->texType = (tex->flags & NVG_IMAGE_PREMULTIPLIED) ? 0.0f : 1.0f;
		}
		else
		{
			frag->texType = 2.0f;
		}
	}
	else 
	{
		frag->type = NSVG_SHADER_FILLGRAD;
		frag->radius = paint->radius;
		frag->feather = paint->feather;
		nvgTransformInverse(invxform, paint->xform);
	}

	D3D9nvg__xformToMat3x3(paintMat, invxform);
	D3D9nvg_copyMatrix3to4(frag->paintMat, paintMat);

	return 1;
}

static struct D3D9NVGfragUniforms* nvg__fragUniformPtr(struct D3D9NVGcontext* D3D, int i);

static void D3D9nvg__setUniforms(struct D3D9NVGcontext* D3D, int uniformOffset, int image)
{
	// Pixel shader constants
	struct D3D9NVGfragUniforms* frag = nvg__fragUniformPtr(D3D, uniformOffset);
	IDirect3DDevice9_SetPixelShaderConstantF(D3D->pDevice, 0, &frag->uniformArray[0][0], NANOVG_D3D9_UNIFORMARRAY_SIZE);

	if (image != 0) 
	{
		struct D3D9NVGtexture* tex = D3D9nvg__findTexture(D3D, image);
		if (tex != NULL)
		{
			IDirect3DDevice9_SetTexture(D3D->pDevice, 0, (struct IDirect3DBaseTexture9*)tex->tex);

			IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_ADDRESSU, tex->flags & NVG_IMAGE_REPEATX ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
			IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_ADDRESSV, tex->flags & NVG_IMAGE_REPEATY ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
			IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		}
	}
}

static void D3D9nvg__renderViewport(void* uptr, float width, float height, float devicePixelRatio)
{
	NVG_NOTUSED(devicePixelRatio);

	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	D3D->view[0] = (float)width;
	D3D->view[1] = (float)height;
}

static void D3D9nvg__fill(struct D3D9NVGcontext* D3D, struct D3D9NVGcall* call)
{
	struct D3D9NVGpath* paths = &D3D->paths[call->pathOffset];
	int i, npaths = call->pathCount;

	// set bindpoint for solid loc
	D3D9nvg__setUniforms(D3D, call->uniformOffset, 0);
	
	// Draw shapes
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILENABLE, TRUE);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_COLORWRITEENABLE, 0);
	// Two-sided stenciling is required so the hole subpath (opposite winding)
	// can decrement the cover count and stop the fill from painting the whole
	// outer rect.  Without this, the CCW_STENCILPASS/DECRSAT states below are
	// ignored on Xenon and the entire subpath bounds get filled (the dark
	// overlay seen over every focused widget).
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_TWOSIDEDSTENCILMODE, TRUE);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_DECRSAT);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CULLMODE, D3DCULL_NONE);

	for (i = 0; i < npaths; i++)
	{
		IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLEFAN, paths[i].fillOffset, paths[i].fillCount - 2);
	}
	
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CULLMODE, D3DCULL_CW);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_COLORWRITEENABLE, 0xf);

	D3D9nvg__setUniforms(D3D, call->uniformOffset + D3D->fragSize, call->image);

	// Draw anti-aliased pixels
	if (D3D->flags & NVG_ANTIALIAS) 
	{
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_EQUAL);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
		
		// Draw fringes
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}
	}

	// Draw fill
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_ZERO);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_ZERO);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_NOTEQUAL);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_ZERO);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_ZERO);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_ZERO);

	IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLELIST, call->triangleOffset, call->triangleCount / 3);

	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_TWOSIDEDSTENCILMODE, FALSE);
}

static void D3D9nvg__convexFill(struct D3D9NVGcontext* D3D, struct D3D9NVGcall* call)
{
	struct D3D9NVGpath* paths = &D3D->paths[call->pathOffset];
	int i, npaths = call->pathCount;
	
	D3D9nvg__setUniforms(D3D, call->uniformOffset, call->image);
	
	for (i = 0; i < npaths; i++)
	{
		IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLEFAN, paths[i].fillOffset, paths[i].fillCount - 2);
	}

	if (D3D->flags & NVG_ANTIALIAS) 
	{
		// Draw fringes
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}
	}
}

static void D3D9nvg__stroke(struct D3D9NVGcontext* D3D, struct D3D9NVGcall* call)
{
	struct D3D9NVGpath* paths = &D3D->paths[call->pathOffset];
	int npaths = call->pathCount, i;

	if (D3D->flags & NVG_STENCIL_STROKES) 
	{
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILENABLE, TRUE);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_EQUAL);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_INCR);
	 
		D3D9nvg__setUniforms(D3D, call->uniformOffset + D3D->fragSize, call->image);
		
		// Fill the stroke base without overlap
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}

		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);

		D3D9nvg__setUniforms(D3D, call->uniformOffset, call->image);
		
		// Draw anti-aliased pixels.
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}

		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_COLORWRITEENABLE, 0);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_ZERO);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_ZERO);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_ZERO);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_ZERO);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_ZERO);

		// Clear stencil buffer.		
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}

		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_COLORWRITEENABLE, 0xf);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILENABLE, FALSE);
	} 
	else
	{
		D3D9nvg__setUniforms(D3D, call->uniformOffset, call->image);
		
		for (i = 0; i < npaths; i++)
		{
			IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLESTRIP, paths[i].strokeOffset, paths[i].strokeCount - 2);
		}
	}
}

static void D3D9nvg__triangles(struct D3D9NVGcontext* D3D, struct D3D9NVGcall* call)
{
	D3D9nvg__setUniforms(D3D, call->uniformOffset, call->image);
	
	IDirect3DDevice9_DrawPrimitive(D3D->pDevice, D3DPT_TRIANGLELIST, call->triangleOffset, call->triangleCount / 3);
}

static void D3D9nvg__renderCancel(void* uptr) 
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	D3D->nverts = 0;
	D3D->npaths = 0;
	D3D->ncalls = 0;
	D3D->nuniforms = 0;
}

static void D3D9nvg__renderFlush(void* uptr)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	int i;

	if (D3D->ncalls > 0) 
	{
		IDirect3DVertexBuffer9* vertexBuffer;
		unsigned int buffer0Offset = 0;
		#if defined(_XBOX) || defined(_XENON)
		vertexBuffer = D3D9nvg_updateVertexBuffer(D3D);
		if (vertexBuffer == NULL)
			goto reset_calls;
		#else
		buffer0Offset = D3D9nvg_updateVertexBuffer(D3D);
		vertexBuffer = D3D->VertexBuffer.pBuffer;
		#endif

		// Upload vertex data
		IDirect3DDevice9_SetStreamSource(D3D->pDevice, 0, vertexBuffer, buffer0Offset, sizeof(NVGvertex));
		IDirect3DDevice9_SetVertexDeclaration(D3D->pDevice, D3D->pLayoutRenderTriangles);

		// Ensure valid state
		IDirect3DDevice9_SetVertexShader(D3D->pDevice, D3D->shader.vert);
		IDirect3DDevice9_SetPixelShader(D3D->pDevice, D3D->shader.frag);

		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_SRCBLEND, D3DBLEND_ONE);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CULLMODE, D3DCULL_CW);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_ALPHABLENDENABLE, TRUE);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_ZENABLE, D3DZB_FALSE);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_SCISSORTESTENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_COLORWRITEENABLE, 0xf);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILMASK, 0xffffffff);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILWRITEMASK, 0xffffffff);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_STENCILREF, 0);
		IDirect3DDevice9_SetRenderState(D3D->pDevice, D3DRS_TWOSIDEDSTENCILMODE, FALSE);

		IDirect3DDevice9_SetVertexShaderConstantF(D3D->pDevice, 0, D3D->view, 1);

		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MIPMAPLODBIAS, 0);
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MAXMIPLEVEL, 0);
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_MAXANISOTROPY, 1);
		#if !defined(_XBOX) && !defined(_XENON)
		IDirect3DDevice9_SetSamplerState(D3D->pDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
		#endif

		// Draw shapes	   
		for (i = 0; i < D3D->ncalls; i++) {
			struct D3D9NVGcall* call = &D3D->calls[i];
			
			if (call->type == D3D9NVG_FILL)
				D3D9nvg__fill(D3D, call);
			else if (call->type == D3D9NVG_CONVEXFILL)
				D3D9nvg__convexFill(D3D, call);
			else if (call->type == D3D9NVG_STROKE)
				D3D9nvg__stroke(D3D, call);
			else if (call->type == D3D9NVG_TRIANGLES)
				D3D9nvg__triangles(D3D, call);
		}
	}

	// Reset calls
	reset_calls:
	D3D->nverts = 0;
	D3D->npaths = 0;
	D3D->ncalls = 0;
	D3D->nuniforms = 0;
}

static int D3D9nvg__maxVertCount(const struct NVGpath* paths, int npaths)
{
	int i, count = 0;
	for (i = 0; i < npaths; i++) {
		count += paths[i].nfill;
		count += paths[i].nstroke;
	}
	return count;
}

static struct D3D9NVGcall* D3D9nvg__allocCall(struct D3D9NVGcontext* D3D)
{
	struct D3D9NVGcall* ret = NULL;
	if (D3D->ncalls+1 > D3D->ccalls) {
		struct D3D9NVGcall* calls;
		int ccalls = D3D9nvg__maxi(D3D->ncalls+1, 128) + D3D->ccalls/2; // 1.5x Overallocate
		calls = (struct D3D9NVGcall*)realloc(D3D->calls, sizeof(struct D3D9NVGcall) * ccalls);
		if (calls == NULL) return NULL;
		D3D->calls = calls;
		D3D->ccalls = ccalls;
	}
	ret = &D3D->calls[D3D->ncalls++];
	memset(ret, 0, sizeof(struct D3D9NVGcall));
	return ret;
}

static int D3D9nvg__allocPaths(struct D3D9NVGcontext* D3D, int n)
{
	int ret = 0;
	if (D3D->npaths+n > D3D->cpaths) {
		struct D3D9NVGpath* paths;
		int cpaths = D3D9nvg__maxi(D3D->npaths + n, 128) + D3D->cpaths/2; // 1.5x Overallocate
		paths = (struct D3D9NVGpath*)realloc(D3D->paths, sizeof(struct D3D9NVGpath) * cpaths);
		if (paths == NULL) return -1;
		D3D->paths = paths;
		D3D->cpaths = cpaths;
	}
	ret = D3D->npaths;
	D3D->npaths += n;
	return ret;
}

static int D3D9nvg__allocVerts(struct D3D9NVGcontext* D3D, int n)
{
	int ret = 0;
	if (D3D->nverts+n > D3D->cverts) {
		struct NVGvertex* verts;
		int cverts = D3D9nvg__maxi(D3D->nverts + n, 4096) + D3D->cverts/2; // 1.5x Overallocate
		verts = (struct NVGvertex*)realloc(D3D->verts, sizeof(struct NVGvertex) * cverts);
		if (verts == NULL) return -1;
		D3D->verts = verts;
		D3D->cverts = cverts;
	}
	ret = D3D->nverts;
	D3D->nverts += n;
	return ret;
}

static int D3D9nvg__allocFragUniforms(struct D3D9NVGcontext* D3D, int n)
{
	int ret = 0, structSize = D3D->fragSize;
	if (D3D->nuniforms+n > D3D->cuniforms) {
		unsigned char* uniforms;
		int cuniforms = D3D9nvg__maxi(D3D->nuniforms+n, 128) + D3D->cuniforms/2; // 1.5x Overallocate
		uniforms = (unsigned char*)realloc(D3D->uniforms, structSize * cuniforms);
		if (uniforms == NULL) return -1;
		D3D->uniforms = uniforms;
		D3D->cuniforms = cuniforms;
	}
	ret = D3D->nuniforms * structSize;
	D3D->nuniforms += n;
	return ret;
}

static struct D3D9NVGfragUniforms* nvg__fragUniformPtr(struct D3D9NVGcontext* D3D, int i)
{
	return (struct D3D9NVGfragUniforms*)&D3D->uniforms[i];
}

static void D3D9nvg__vset(struct NVGvertex* vtx, float x, float y, float u, float v)
{
	vtx->x = x;
	vtx->y = y;
	vtx->u = u;
	vtx->v = v;
}

static void D3D9nvg__renderFill(void* uptr, struct NVGpaint* paint, NVGcompositeOperationState compositeOperation, struct NVGscissor* scissor, float fringe,
							  const float* bounds, const struct NVGpath* paths, int npaths)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGcall* call = D3D9nvg__allocCall(D3D);
	struct NVGvertex* quad;
	struct D3D9NVGfragUniforms* frag;
	int i, maxverts, offset;

	if (call == NULL) return;

	call->type = D3D9NVG_FILL;
	call->pathOffset = D3D9nvg__allocPaths(D3D, npaths);
	if (call->pathOffset == -1) goto error;
	call->pathCount = npaths;
	call->image = paint->image;

	if (npaths == 1 && paths[0].convex)
		call->type = D3D9NVG_CONVEXFILL;

	// Allocate vertices for all the paths.
	maxverts = D3D9nvg__maxVertCount(paths, npaths) + 6;
	offset = D3D9nvg__allocVerts(D3D, maxverts);
	if (offset == -1) goto error;

	for (i = 0; i < npaths; i++) {
		struct D3D9NVGpath* copy = &D3D->paths[call->pathOffset + i];
		const struct NVGpath* path = &paths[i];
		memset(copy, 0, sizeof(struct D3D9NVGpath));
		if (path->nfill > 0) {
			copy->fillOffset = offset;
			copy->fillCount = path->nfill;
			memcpy(&D3D->verts[offset], path->fill, sizeof(struct NVGvertex) * path->nfill);
			offset += path->nfill;
		}
		if (path->nstroke > 0) {
			copy->strokeOffset = offset;
			copy->strokeCount = path->nstroke;
			memcpy(&D3D->verts[offset], path->stroke, sizeof(struct NVGvertex) * path->nstroke);
			offset += path->nstroke;
		}
	}

	// Quad
	call->triangleOffset = offset;
	call->triangleCount = 6;
	quad = &D3D->verts[call->triangleOffset];
	D3D9nvg__vset(&quad[0], bounds[0], bounds[3], 0.5f, 1.0f);
	D3D9nvg__vset(&quad[1], bounds[2], bounds[3], 0.5f, 1.0f);
	D3D9nvg__vset(&quad[2], bounds[2], bounds[1], 0.5f, 1.0f);

	D3D9nvg__vset(&quad[3], bounds[0], bounds[3], 0.5f, 1.0f);
	D3D9nvg__vset(&quad[4], bounds[2], bounds[1], 0.5f, 1.0f);
	D3D9nvg__vset(&quad[5], bounds[0], bounds[1], 0.5f, 1.0f);

	// Setup uniforms for draw calls
	if (call->type == D3D9NVG_FILL) {
		call->uniformOffset = D3D9nvg__allocFragUniforms(D3D, 2);
		if (call->uniformOffset == -1) goto error;
		// Simple shader for stencil
		frag = nvg__fragUniformPtr(D3D, call->uniformOffset);
		memset(frag, 0, sizeof(*frag));
		frag->strokeMult[1] = -1.0f;
		frag->type = NSVG_SHADER_SIMPLE;
		// Fill shader
		D3D9nvg__convertPaint(D3D, nvg__fragUniformPtr(D3D, call->uniformOffset + D3D->fragSize), paint, compositeOperation, scissor, fringe, fringe, -1.0f);
	} else {
		call->uniformOffset = D3D9nvg__allocFragUniforms(D3D, 1);
		if (call->uniformOffset == -1) goto error;
		// Fill shader
		D3D9nvg__convertPaint(D3D, nvg__fragUniformPtr(D3D, call->uniformOffset), paint, compositeOperation, scissor, fringe, fringe, -1.0f);
	}

	return;

error:
	// We get here if call alloc was ok, but something else is not.
	// Roll back the last call to prevent drawing it.
	if (D3D->ncalls > 0) D3D->ncalls--;
}

static void D3D9nvg__renderStroke(void* uptr, struct NVGpaint* paint, NVGcompositeOperationState compositeOperation, struct NVGscissor* scissor, float fringe,
								float strokeWidth, const struct NVGpath* paths, int npaths)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGcall* call = D3D9nvg__allocCall(D3D);
	int i, maxverts, offset;

	if (call == NULL) return;

	call->type = D3D9NVG_STROKE;
	call->pathOffset = D3D9nvg__allocPaths(D3D, npaths);
	if (call->pathOffset == -1) goto error;
	call->pathCount = npaths;
	call->image = paint->image;

	// Allocate vertices for all the paths.
	maxverts = D3D9nvg__maxVertCount(paths, npaths);
	offset = D3D9nvg__allocVerts(D3D, maxverts);
	if (offset == -1) goto error;

	for (i = 0; i < npaths; i++) {
		struct D3D9NVGpath* copy = &D3D->paths[call->pathOffset + i];
		const struct NVGpath* path = &paths[i];
		memset(copy, 0, sizeof(struct D3D9NVGpath));
		if (path->nstroke) {
			copy->strokeOffset = offset;
			copy->strokeCount = path->nstroke;
			memcpy(&D3D->verts[offset], path->stroke, sizeof(struct NVGvertex) * path->nstroke);
			offset += path->nstroke;
		}
	}

	if (D3D->flags & NVG_STENCIL_STROKES) {
		// Fill shader
		call->uniformOffset = D3D9nvg__allocFragUniforms(D3D, 2);
		if (call->uniformOffset == -1) goto error;

		D3D9nvg__convertPaint(D3D, nvg__fragUniformPtr(D3D, call->uniformOffset), paint, compositeOperation, scissor, strokeWidth, fringe, -1.0f);
		D3D9nvg__convertPaint(D3D, nvg__fragUniformPtr(D3D, call->uniformOffset + D3D->fragSize), paint, compositeOperation, scissor, strokeWidth, fringe, 1.0f - 0.5f/255.0f);

	} else {
		// Fill shader
		call->uniformOffset = D3D9nvg__allocFragUniforms(D3D, 1);
		if (call->uniformOffset == -1) goto error;
		D3D9nvg__convertPaint(D3D, nvg__fragUniformPtr(D3D, call->uniformOffset), paint, compositeOperation, scissor, strokeWidth, fringe, -1.0f);
	}

	return;

error:
	// We get here if call alloc was ok, but something else is not.
	// Roll back the last call to prevent drawing it.
	if (D3D->ncalls > 0) D3D->ncalls--;
}

static void D3D9nvg__renderTriangles(void* uptr, struct NVGpaint* paint, NVGcompositeOperationState compositeOperation, struct NVGscissor* scissor,
								   const struct NVGvertex* verts, int nverts, float fringe)
{
	(void)fringe;
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	struct D3D9NVGcall* call;
	struct D3D9NVGfragUniforms* frag;

	if (nverts == 0)
	{
		return;
	}

	call = D3D9nvg__allocCall(D3D);

	if (call == NULL) return;

	call->type = D3D9NVG_TRIANGLES;
	call->image = paint->image;

	// Allocate vertices for all the paths.
	call->triangleOffset = D3D9nvg__allocVerts(D3D, nverts);
	if (call->triangleOffset == -1) goto error;
	call->triangleCount = nverts;

	memcpy(&D3D->verts[call->triangleOffset], verts, sizeof(struct NVGvertex) * nverts);

	// Fill shader
	call->uniformOffset = D3D9nvg__allocFragUniforms(D3D, 1);
	if (call->uniformOffset == -1) goto error;
	frag = nvg__fragUniformPtr(D3D, call->uniformOffset);
	D3D9nvg__convertPaint(D3D, frag, paint, compositeOperation, scissor, 1.0f, 1.0f, -1.0f);
	frag->type = NSVG_SHADER_IMG;

	return;

error:
	// We get here if call alloc was ok, but something else is not.
	// Roll back the last call to prevent drawing it.
	if (D3D->ncalls > 0) D3D->ncalls--;
}

static void D3D9nvg__renderDelete(void* uptr)
{
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)uptr;
	int i;
	if (D3D == NULL)
	{
		return;
	}

	D3D9nvg__deleteShader(&D3D->shader);

	for (i = 0; i < D3D->ntextures; i++) 
	{
		if (D3D->textures[i].tex != NULL && (D3D->textures[i].flags & NVG_IMAGE_NODELETE) == 0)
		{
			IDirect3DTexture9_Release(D3D->textures[i].tex);
		}
	}

#if defined(_XBOX) || defined(_XENON)
	for (i = 0; i < 8; i++)
		if (D3D->VertexBuffer.pBuffers[i])
			IDirect3DVertexBuffer9_Release(D3D->VertexBuffer.pBuffers[i]);
#else
	IDirect3DVertexBuffer9_Release(D3D->VertexBuffer.pBuffer);
#endif

	IDirect3DVertexDeclaration9_Release(D3D->pLayoutRenderTriangles);
	
	free(D3D->textures);

	free(D3D->paths);
	free(D3D->verts);
	free(D3D->uniforms);
	free(D3D->calls);
	
	free(D3D);
}


struct NVGcontext* nvgCreateD3D9(IDirect3DDevice9* pDevice, int flags)
{
	struct NVGparams params;
	struct NVGcontext* ctx = NULL;
	struct D3D9NVGcontext* D3D = (struct D3D9NVGcontext*)malloc(sizeof(struct D3D9NVGcontext));
	if (D3D == NULL) 
	{
		goto error;
	}
	memset(D3D, 0, sizeof(struct D3D9NVGcontext));
	D3D->pDevice = pDevice;

	memset(&params, 0, sizeof(params));
	params.renderCreate = D3D9nvg__renderCreate;
	params.renderCreateTexture = D3D9nvg__renderCreateTexture;
	params.renderDeleteTexture = D3D9nvg__renderDeleteTexture;
	params.renderUpdateTexture = D3D9nvg__renderUpdateTexture;
	params.renderGetTextureSize = D3D9nvg__renderGetTextureSize;
	params.renderViewport = D3D9nvg__renderViewport;
	params.renderCancel = D3D9nvg__renderCancel;
	params.renderFlush = D3D9nvg__renderFlush;
	params.renderFill = D3D9nvg__renderFill;
	params.renderStroke = D3D9nvg__renderStroke;
	params.renderTriangles = D3D9nvg__renderTriangles;
	params.renderDelete = D3D9nvg__renderDelete;
	params.userPtr = D3D;
	params.edgeAntiAlias = flags & NVG_ANTIALIAS ? 1 : 0;

	D3D->flags = flags;

	ctx = nvgCreateInternal(&params);
	if (ctx == NULL) goto error;

	return ctx;

error:
	// 'D3D9' is freed by nvgDeleteInternal.
	if (ctx != NULL) nvgDeleteInternal(ctx);
	return NULL;
}

void nvgDeleteD3D9(struct NVGcontext* ctx)
{
	nvgDeleteInternal(ctx);
}

#endif /* NANOVG_D3D9_IMPLEMENTATION */
