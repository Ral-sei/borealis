//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
// D3D9 Demo port by Crosire.
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

#include <stdio.h>
#include "nanovg.h"
#define NANOVG_D3D9_IMPLEMENTATION
#define COBJMACROS // This example is in c, so we use the COM macros
#define INITGUID
#include "nanovg_d3d9.h"
#include "demo.h"
#include "perf.h"
#include <Windows.h>
#include <windowsx.h>

int blowup = 0;
int screenshot = 0;
int premult = 0;
int xm = 0;
int ym = 0;
int xWin = 0;
int yWin = 0;

//#define DEMO_MSAA
struct NVGcontext* vg = NULL;
struct GPUtimer gpuTimer;
struct PerfGraph fps, cpuGraph, gpuGraph;
double prevt = 0, cpuTime = 0;
struct DemoData data;

// Timer
double cpuTimerResolution;
unsigned __int64  cpuTimerBase;

D3DPRESENT_PARAMETERS pp;
IDirect3DDevice9* pDevice = NULL;
IDirect3DSurface9* pRenderTarget = NULL;
IDirect3DSurface9* pDepthStencil = NULL;

// Global Variables:
HINSTANCE hInst;
HWND hWndMain = 0;

// Forward declarations of functions included in this code module:
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
BOOL InitializeDX(unsigned int x, unsigned int y);
void UnInitializeDX();
HRESULT ResizeWindow(unsigned int x, unsigned int y);

void initCPUTimer(void);
double getCPUTime(void);

const char* pszWindowClass = "WindowClass";


// Do the drawing
void Draw()
{
    int n;
    int i;
    float gpuTimes[3];
    double dt;
    double t;
    DWORD clearColor;
    float pxRatio;

    t = getCPUTime();
    dt = t - prevt;
	prevt = t;
    
    if (premult)
    {
        clearColor = 0;
    }
	else
    {
                    
        clearColor = D3DCOLOR_ARGB(255, 77, 77, 78);
    }

	IDirect3DDevice9_BeginScene(pDevice);
	
    IDirect3DDevice9_SetRenderTarget(pDevice, 0, pRenderTarget);
	IDirect3DDevice9_SetDepthStencilSurface(pDevice, pDepthStencil);
	IDirect3DDevice9_Clear(pDevice, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_STENCIL, clearColor, 0.0f, (DWORD)0);

    // Ration 1/1 for now.  Not sure how to support retina on Windows.
    pxRatio = 1.0f;

    nvgBeginFrame(vg, xWin, yWin, pxRatio);

    renderDemo(vg, (float)xm, (float)ym, (float)xWin, (float)yWin, (float)t, blowup, &data);

    renderGraph(vg, 5,5, &fps);
	renderGraph(vg, 5+200+5,5, &cpuGraph);
	if (gpuTimer.supported)
	    renderGraph(vg, 5+200+5+200+5,5, &gpuGraph);

	nvgEndFrame(vg);
	
	IDirect3DDevice9_EndScene(pDevice);

    // Measure the CPU time taken excluding swap buffers (as the swap may wait for GPU)
	cpuTime = getCPUTime() - t;

	updateGraph(&fps, (float)dt);
	updateGraph(&cpuGraph, (float)cpuTime);

	// We may get multiple results.
	n = stopGPUTimer(&gpuTimer, gpuTimes, 3);
	for (i = 0; i < n; i++)
        updateGraph(&gpuGraph, gpuTimes[i]);

    IDirect3DDevice9_Present(pDevice, NULL, NULL, NULL, NULL);
}

// Window message loop
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
        // Keyboard handling
        case WM_KEYDOWN:
        {
            if (GetKeyState(VK_ESCAPE))
            {
                CloseWindow(hWnd);
            }
            else if (GetKeyState(VK_SPACE))
            {
                blowup = !blowup;
            }
            else if (wParam == 'P')
            {
                premult = !premult;
            }
            else if (wParam == 'S')
            {
                screenshot = 1;
            }
        }
        break;
	
        // Mouse pos
        case WM_MOUSEMOVE:
        {     
            xm = GET_X_LPARAM(lParam);
            ym = GET_Y_LPARAM(lParam);
        }
        break;
    
        // Painting
	    case WM_PAINT:
        { 
            Draw();
            ValidateRect(hWnd, NULL);
        }
	    break;

        // Sizing
        case WM_SIZE:
        {
            ResizeWindow(LOWORD(lParam), HIWORD(lParam));
        }
	    break;

	    case WM_ERASEBKGND:
        {
            // No need to erase background
		    return 1;
        }
	    break;

	    case WM_DESTROY:
        {
            UnInitializeDX();

            // Quit the app
            PostQuitMessage(0);
        }
	    break;

	    default:
        {
		    return DefWindowProc(hWnd, message, wParam, lParam);
	    }

    }
	return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);
   
	wcex.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wcex.lpfnWndProc	= WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= NULL;
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= "";
	wcex.lpszClassName	= pszWindowClass;
	wcex.hIconSm		= NULL;

	return RegisterClassEx(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    RECT rcWin;
    
    hInst = hInstance; // Store instance handle in our global variable

    rcWin.left = 0;
    rcWin.right = 1000;
    rcWin.top = 0;
    rcWin.bottom = 600;
  
    AdjustWindowRectEx(&rcWin, WS_OVERLAPPEDWINDOW, FALSE, 0);
    
    rcWin.right += -rcWin.left;
    rcWin.bottom += -rcWin.top;
  
    hWndMain = CreateWindowEx(0, pszWindowClass, L"Nanovg", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, (int)rcWin.right, (int)rcWin.bottom, NULL, NULL, hInstance, NULL);

    if (!hWndMain)
    {
        return FALSE;
    }

    if (FAILED(InitializeDX(rcWin.right, rcWin.bottom)))
    {
        printf("Could not init DX\n");
        return FALSE;
    }

	initGPUTimer(&gpuTimer);
    initCPUTimer();

    initGraph(&fps, GRAPH_RENDER_FPS, "Frame Time");
	initGraph(&cpuGraph, GRAPH_RENDER_MS, "CPU Time");
	initGraph(&gpuGraph, GRAPH_RENDER_MS, "GPU Time");

    InvalidateRect(hWndMain, NULL, TRUE);
    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    startGPUTimer(&gpuTimer);

    return TRUE;
}

int main()
{
    MSG msg;
    
    hInst = GetModuleHandle(NULL);
	MyRegisterClass(hInst);

	// Perform application initialization:
	if (!InitInstance (hInst, SW_SHOW))
	{
		return FALSE;
	}

    ZeroMemory(&msg, sizeof(msg));
	
    // Main message loop:
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            Draw();
        }
    }
    
    freeDemoData(vg, &data);

	nvgDeleteD3D9(vg);

	printf("Average Frame Time: %.2f ms\n", getGraphAverage(&fps) * 1000.0f);
	printf("          CPU Time: %.2f ms\n", getGraphAverage(&cpuGraph) * 1000.0f);
	printf("          GPU Time: %.2f ms\n", getGraphAverage(&gpuGraph) * 1000.0f);

	return (int) msg.wParam;
}


// Frees everything
void UnInitializeDX()
{ 
	if (pRenderTarget) IDirect3DSurface9_Release(pRenderTarget); pRenderTarget = NULL;
	if (pDepthStencil) IDirect3DSurface9_Release(pDepthStencil); pDepthStencil = NULL;
    if (pDevice) IDirect3DDevice9_Release(pDevice); pDevice = NULL;
}

// Setup the device and the rendering targets
BOOL InitializeDX(unsigned int x, unsigned int y)
{
    HRESULT hr = S_OK;
	IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
	
	if (!pD3D)
	{
		return FALSE;
	}
	
	ZeroMemory(&pp, sizeof(pp));
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = hWndMain;
	pp.Windowed = TRUE;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	
	hr = IDirect3D9_CreateDevice(pD3D, 0, D3DDEVTYPE_HAL, hWndMain, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &pDevice);

	IDirect3D9_Release(pD3D);
	pD3D = NULL;
	
    if (SUCCEEDED(hr))
    {
        hr = ResizeWindow(pp.BackBufferWidth, pp.BackBufferHeight);
        if (FAILED(hr))
        {
            return FALSE;
        }
    }
    else
    {
        // Fail
        UnInitializeDX();
        return FALSE;
    }

    return TRUE;
}

HRESULT ResizeWindow(unsigned int x, unsigned int y)
{
	HRESULT hr = S_OK;
	D3DSURFACE_DESC bbDesc;

    xWin = x;
    yWin = y;
   
    if (!pDevice)
        return E_FAIL;

    // Ensure that nobody is holding onto one of the old resources
    if (pRenderTarget) IDirect3DSurface9_Release(pRenderTarget); pRenderTarget = NULL;
    if (pDepthStencil) IDirect3DSurface9_Release(pDepthStencil); pDepthStencil = NULL;

    freeDemoData(vg, &data);

	nvgDeleteD3D9(vg);

    // Resize render target buffers
	pp.BackBufferWidth = x;
	pp.BackBufferHeight = y;
    hr = IDirect3DDevice9_Reset(pDevice, &pp);
    if (FAILED(hr))
    {
        return hr;
    }

#ifdef DEMO_MSAA
	vg = nvgCreateD3D9(pDevice, NVG_STENCIL_STROKES);
#else
	vg = nvgCreateD3D9(pDevice, NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#endif
	if (vg == NULL) 
    {
		printf("Could not init nanovg.\n");
		return E_FAIL;
	}

	if (loadDemoData(vg, &data) == -1)
    {
		return E_FAIL;
    }
   
    // Create the render target view and set it on the device
    hr = IDirect3DDevice9_GetBackBuffer(pDevice,
        0,
        0,
		D3DBACKBUFFER_TYPE_MONO,
        &pRenderTarget
        );
    if (FAILED(hr))
    {
        return hr;
    }
	
	hr = IDirect3DSurface9_GetDesc(pRenderTarget, &bbDesc);

    hr = IDirect3DDevice9_CreateDepthStencilSurface(pDevice, (UINT)x, (UINT)y, D3DFMT_D24S8, bbDesc.MultiSampleType, bbDesc.MultiSampleQuality, FALSE, &pDepthStencil, NULL);
    return hr;
}

    
// High perf counter.
static unsigned __int64 getRawTime(void)
{
    unsigned __int64 time;
    QueryPerformanceCounter((LARGE_INTEGER*)&time);
    return time;
}

// Initialise timer
void initCPUTimer(void)
{
    unsigned __int64 frequency;

    if (QueryPerformanceFrequency((LARGE_INTEGER*)&frequency))
    {
        cpuTimerResolution = 1.0 / (double)frequency;
    }

    cpuTimerBase = getRawTime();
}

// GetTime
double getCPUTime(void)
{
    return (double)(getRawTime() - cpuTimerBase) *
        cpuTimerResolution;
}
	
