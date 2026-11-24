#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "../src/third_person_gpu_ipc.h"
#include "../src/projected_video_ipc.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {
// Third-person camera export state (existing)
ID3D11Texture2D* g_source = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_export = nullptr;
IDXGIKeyedMutex* g_mutex = nullptr;
HANDLE g_mapping = nullptr;
bigscreen_third_person_gpu::State* g_state = nullptr;
uint64_t g_generation = 0;

void WriteReport(const wchar_t* path, const char* source, const char* destination, const char* device) {
    if (!path) return;
    FILE* f = nullptr; _wfopen_s(&f, path, L"w, ccs=UTF-8"); if (!f) return;
    fwprintf(f, L"Source=%S\nDestination=%S\nUnityDevice=%S\n", source, destination, device);
    fclose(f);
}

void Cleanup() {
    if (g_state) { g_state->valid = 0; UnmapViewOfFile(g_state); }
    if (g_mapping) CloseHandle(g_mapping);
    if (g_mutex) g_mutex->Release(); if (g_export) g_export->Release();
    if (g_context) g_context->Release(); if (g_device) g_device->Release(); if (g_source) g_source->Release();
    g_state = nullptr; g_mapping = nullptr; g_mutex = nullptr; g_export = nullptr; g_context = nullptr; g_device = nullptr; g_source = nullptr; g_generation = 0;
}

// --------------------------------------------------------------------------
// BigSoup Desktop Texture Routing Hook
// --------------------------------------------------------------------------
typedef void (*UnityRenderEvent)(int eventId);
typedef UnityRenderEvent (*pfnBigSoupProcessLocalDesktops)();
typedef ID3D11Texture2D* (*pfnBigSoupGetNativeDesktopTexturePointer)(int index);

pfnBigSoupGetNativeDesktopTexturePointer g_pfnGetNativeDesktop = nullptr;
UnityRenderEvent g_originalProcessLocalDesktops = nullptr;
uint8_t* g_pTrampoline = nullptr;
bool g_hookInstalled = false;

HANDLE g_projectedMapping = nullptr;
const bigscreen_projected_video::State* g_projectedState = nullptr;
ID3D11Texture2D* g_sharedVideoTex = nullptr;
uint64_t g_currentSharedHandle = 0;
ID3D11Texture2D* g_blackTex = nullptr;

void EnsureBlackTexture(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC& deskDesc) {
    if (g_blackTex) {
        D3D11_TEXTURE2D_DESC curDesc{};
        g_blackTex->GetDesc(&curDesc);
        if (curDesc.Width == deskDesc.Width && curDesc.Height == deskDesc.Height && curDesc.Format == deskDesc.Format) {
            return;
        }
        g_blackTex->Release();
        g_blackTex = nullptr;
    }
    D3D11_TEXTURE2D_DESC bDesc = deskDesc;
    bDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bDesc.MiscFlags = 0;
    bDesc.CPUAccessFlags = 0;
    bDesc.Usage = D3D11_USAGE_DEFAULT;
    std::vector<uint32_t> blackData(bDesc.Width * bDesc.Height, 0xFF000000); // opaque black
    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = blackData.data();
    initData.SysMemPitch = bDesc.Width * sizeof(uint32_t);
    pDevice->CreateTexture2D(&bDesc, &initData, &g_blackTex);
}

void RouteDesktopTexture() {
    if (!g_projectedState) {
        if (!g_projectedMapping) {
            g_projectedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_projected_video::kMappingName);
        }
        if (g_projectedMapping) {
            g_projectedState = static_cast<const bigscreen_projected_video::State*>(
                MapViewOfFile(g_projectedMapping, FILE_MAP_READ, 0, 0, sizeof(bigscreen_projected_video::State)));
        }
    }

    if (!g_projectedState || g_projectedState->magic != bigscreen_projected_video::kMagic) {
        return;
    }

    if (!g_pfnGetNativeDesktop) return;
    ID3D11Texture2D* pDesktopTex = g_pfnGetNativeDesktop(0);
    if (!pDesktopTex) return;

    ID3D11Device* pDevice = nullptr;
    pDesktopTex->GetDevice(&pDevice);
    if (!pDevice) return;

    ID3D11DeviceContext* pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext) {
        pDevice->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC deskDesc{};
    pDesktopTex->GetDesc(&deskDesc);

    const auto state = *g_projectedState;

    if (state.active && state.sharedHandle) {
        if (!g_sharedVideoTex || g_currentSharedHandle != state.sharedHandle) {
            if (g_sharedVideoTex) {
                g_sharedVideoTex->Release();
                g_sharedVideoTex = nullptr;
            }
            HRESULT hr = pDevice->OpenSharedResource(reinterpret_cast<HANDLE>(state.sharedHandle), IID_PPV_ARGS(&g_sharedVideoTex));
            if (SUCCEEDED(hr) && g_sharedVideoTex) {
                g_currentSharedHandle = state.sharedHandle;
            }
        }

        if (g_sharedVideoTex) {
            D3D11_TEXTURE2D_DESC vidDesc{};
            g_sharedVideoTex->GetDesc(&vidDesc);

            if (vidDesc.Width == deskDesc.Width && vidDesc.Height == deskDesc.Height && vidDesc.Format == deskDesc.Format) {
                pContext->CopyResource(pDesktopTex, g_sharedVideoTex);
            } else {
                UINT copyW = (std::min)(vidDesc.Width, deskDesc.Width);
                UINT copyH = (std::min)(vidDesc.Height, deskDesc.Height);
                UINT dstX = (deskDesc.Width > copyW) ? (deskDesc.Width - copyW) / 2 : 0;
                UINT dstY = (deskDesc.Height > copyH) ? (deskDesc.Height - copyH) / 2 : 0;
                D3D11_BOX box{ 0, 0, 0, copyW, copyH, 1 };
                pContext->CopySubresourceRegion(pDesktopTex, 0, dstX, dstY, 0, g_sharedVideoTex, 0, &box);
            }
        }
    } else if (state.privacyMode) {
        if (g_sharedVideoTex && !state.active) {
            pContext->CopyResource(pDesktopTex, g_sharedVideoTex);
        } else {
            EnsureBlackTexture(pDevice, deskDesc);
            if (g_blackTex) {
                pContext->CopyResource(pDesktopTex, g_blackTex);
            }
        }
    }

    pContext->Release();
    pDevice->Release();
}

void Hooked_ProcessLocalDesktops(int eventId) {
    if (g_originalProcessLocalDesktops) {
        g_originalProcessLocalDesktops(eventId);
    }
    RouteDesktopTexture();
}

bool InstallHookInternal(HMODULE hBigSoup) {
    if (g_hookInstalled || !hBigSoup) return g_hookInstalled;

    auto fnProcessLocal = reinterpret_cast<pfnBigSoupProcessLocalDesktops>(
        GetProcAddress(hBigSoup, "BigSoupProcessLocalDesktops"));
    g_pfnGetNativeDesktop = reinterpret_cast<pfnBigSoupGetNativeDesktopTexturePointer>(
        GetProcAddress(hBigSoup, "BigSoupGetNativeDesktopTexturePointer"));

    if (!fnProcessLocal || !g_pfnGetNativeDesktop) return false;

    UnityRenderEvent targetCallback = fnProcessLocal();
    if (!targetCallback) return false;

    uint8_t* pTarget = reinterpret_cast<uint8_t*>(targetCallback);

    // If already hooked (starts with 0xFF 0x25), skip
    if (pTarget[0] == 0xFF && pTarget[1] == 0x25) {
        g_hookInstalled = true;
        return true;
    }

    // Allocate 64 bytes executable memory for trampoline
    g_pTrampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_pTrampoline) return false;

    // First 14 bytes of target:
    // push rbx (2), sub rsp, 30h (4), xorps xmm0, xmm0 (3), movups [rsp+20h], xmm0 (5) = 14 bytes
    std::memcpy(g_pTrampoline, pTarget, 14);

    // Add 14-byte 64-bit absolute jump back to target + 14:
    // FF 25 00 00 00 00 <8-byte address>
    g_pTrampoline[14] = 0xFF;
    g_pTrampoline[15] = 0x25;
    *reinterpret_cast<uint32_t*>(g_pTrampoline + 16) = 0; // RIP + 0
    *reinterpret_cast<uint64_t*>(g_pTrampoline + 20) = reinterpret_cast<uint64_t>(pTarget + 14);
    FlushInstructionCache(GetCurrentProcess(), g_pTrampoline, 32);

    g_originalProcessLocalDesktops = reinterpret_cast<UnityRenderEvent>(g_pTrampoline);

    // Patch target callback with 14-byte jump to Hooked_ProcessLocalDesktops
    DWORD oldProtect = 0;
    if (VirtualProtect(pTarget, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        pTarget[0] = 0xFF;
        pTarget[1] = 0x25;
        *reinterpret_cast<uint32_t*>(pTarget + 2) = 0; // RIP + 0
        *reinterpret_cast<uint64_t*>(pTarget + 6) = reinterpret_cast<uint64_t>(Hooked_ProcessLocalDesktops);
        DWORD tmp = 0;
        VirtualProtect(pTarget, 14, oldProtect, &tmp);
        FlushInstructionCache(GetCurrentProcess(), pTarget, 14);
        g_hookInstalled = true;
    }

    return g_hookInstalled;
}

DWORD WINAPI HookInitThread(LPVOID) {
    HMODULE hBigSoup = nullptr;
    for (int i = 0; i < 200 && !hBigSoup; ++i) {
        hBigSoup = GetModuleHandleW(L"BigSoup.dll");
        if (!hBigSoup) Sleep(200);
    }
    if (hBigSoup) {
        InstallHookInternal(hBigSoup);
    }
    return 0;
}
} // namespace

// --------------------------------------------------------------------------
// Exported Functions
// --------------------------------------------------------------------------
extern "C" __declspec(dllexport) int __cdecl InstallBigSoupDesktopHook() {
    HMODULE hBigSoup = GetModuleHandleW(L"BigSoup.dll");
    if (!hBigSoup) return 1;
    return InstallHookInternal(hBigSoup) ? 0 : 2;
}

extern "C" __declspec(dllexport) int __cdecl InitThirdPersonGpu(void* nativePtr, const wchar_t* reportPath) {
    Cleanup();
    if (!nativePtr) return 1;
    IUnknown* unknown = reinterpret_cast<IUnknown*>(nativePtr);
    if (FAILED(unknown->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_source)))) { Cleanup(); return 2; }
    g_source->GetDevice(&g_device);
    if (!g_device) { Cleanup(); return 3; }
    g_device->GetImmediateContext(&g_context);
    D3D11_TEXTURE2D_DESC src{}; g_source->GetDesc(&src);
    D3D11_TEXTURE2D_DESC dst = src;
    dst.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    dst.CPUAccessFlags = 0;
    dst.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(g_device->CreateTexture2D(&dst, nullptr, &g_export))) { Cleanup(); return 4; }
    if (FAILED(g_export->QueryInterface(IID_PPV_ARGS(&g_mutex)))) { Cleanup(); return 5; }
    IDXGIResource* resource = nullptr; if (FAILED(g_export->QueryInterface(IID_PPV_ARGS(&resource)))) { Cleanup(); return 6; }
    HANDLE shared = nullptr; HRESULT hrHandle = resource->GetSharedHandle(&shared); resource->Release();
    if (FAILED(hrHandle) || !shared) { Cleanup(); return 7; }
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bigscreen_third_person_gpu::State), bigscreen_third_person_gpu::kMappingName);
    if (!g_mapping) { Cleanup(); return 8; }
    g_state = static_cast<bigscreen_third_person_gpu::State*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_third_person_gpu::State)));
    if (!g_state) { Cleanup(); return 9; }
    DXGI_ADAPTER_DESC ad{}; uint64_t luid = 0; IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) { IDXGIAdapter* adapter = nullptr; if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) { adapter->GetDesc(&ad); std::memcpy(&luid, &ad.AdapterLuid, sizeof(luid)); adapter->Release(); } dxgiDevice->Release(); }
    *g_state = {}; g_state->magic = bigscreen_third_person_gpu::kMagic; g_state->version = bigscreen_third_person_gpu::kVersion; g_state->width = dst.Width; g_state->height = dst.Height; g_state->format = static_cast<uint32_t>(dst.Format); g_state->sharedHandle = reinterpret_cast<uint64_t>(shared); g_state->adapterLuid = luid; g_generation = GetTickCount64(); g_state->generation = g_generation; g_state->valid = 1;
    ID3D11Device1* device1 = nullptr; ID3D11DeviceContext1* context1 = nullptr; IDXGIResource1* resource1 = nullptr;
    const bool hasDevice1 = SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&device1))); const bool hasContext1 = g_context && SUCCEEDED(g_context->QueryInterface(IID_PPV_ARGS(&context1))); const bool hasResource1 = SUCCEEDED(g_export->QueryInterface(IID_PPV_ARGS(&resource1)));
    if (device1) device1->Release(); if (context1) context1->Release(); if (resource1) resource1->Release();
    char s[256]{}, d[256]{}, dev[256]{}; sprintf_s(s, "source=%ux%u format=%u misc=0x%X", src.Width, src.Height, src.Format, src.MiscFlags); sprintf_s(d, "destination=%ux%u format=%u misc=0x%X keyedMutex=yes sharedHandle=%p", dst.Width, dst.Height, dst.Format, dst.MiscFlags, shared); sprintf_s(dev, "device1=%s context1=%s resource1=%s adapterLuid=0x%llX", hasDevice1?"yes":"no", hasContext1?"yes":"no", hasResource1?"yes":"no", (unsigned long long)luid); WriteReport(reportPath, s, d, dev);
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl CopyThirdPersonGpu(void* nativePtr) {
    if (!g_export || !g_mutex || !g_context || !nativePtr) return 1;
    ID3D11Texture2D* source = nullptr; if (FAILED(reinterpret_cast<IUnknown*>(nativePtr)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source)))) return 2;
    HRESULT hr = g_mutex->AcquireSync(0, 0); if (SUCCEEDED(hr)) { g_context->CopyResource(g_export, source); g_context->Flush(); g_mutex->ReleaseSync(1); if (g_state) { ++g_state->frameSequence; g_state->lastCopyMicros = GetTickCount64() * 1000ull; } }
    source->Release(); return SUCCEEDED(hr) ? 0 : 3;
}

extern "C" __declspec(dllexport) int __cdecl ShutdownThirdPersonGpu() { Cleanup(); return 0; }

// DLL Entry Point: automatically start hook thread when loaded into Bigscreen.exe
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(nullptr, 0, HookInitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_projectedState) { UnmapViewOfFile(g_projectedState); g_projectedState = nullptr; }
        if (g_projectedMapping) { CloseHandle(g_projectedMapping); g_projectedMapping = nullptr; }
        if (g_sharedVideoTex) { g_sharedVideoTex->Release(); g_sharedVideoTex = nullptr; }
        if (g_blackTex) { g_blackTex->Release(); g_blackTex = nullptr; }
        if (g_pTrampoline) { VirtualFree(g_pTrampoline, 0, MEM_RELEASE); g_pTrampoline = nullptr; }
    }
    return TRUE;
}
