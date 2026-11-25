#include <Windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "live_frame_ipc.h"
#include "third_person_gpu_ipc.h"
#include "controller_control_ipc.h"
#include "projected_video_ipc.h"
#include "peer_movie_ipc.h"

#include <filesystem>
namespace fs = std::filesystem;

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

void Log(const char* fmt, ...);
void SyncPointerAndLook();
void ReleasePeerMovieResources();

HWND g_hwnd = nullptr;
IDXGIAdapter1* g_adapter = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psFlip = nullptr;
ID3D11PixelShader* g_psMovie = nullptr;
ID3D11PixelShader* g_psMovieFlip = nullptr;
ID3D11PixelShader* g_psSbs = nullptr;
ID3D11PixelShader* g_psSbsFlip = nullptr;
ID3D11PixelShader* g_psOu = nullptr;
ID3D11PixelShader* g_psOuFlip = nullptr;
ID3D11PixelShader* g_psTheaterComposite = nullptr;
ID3D11PixelShader* g_psTheaterCompositeFlip = nullptr;
ID3D11Buffer* g_cbTheater = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Texture2D* g_sharedTexture = nullptr;
IDXGIKeyedMutex* g_sharedMutex = nullptr;
ID3D11Texture2D* g_localTexture = nullptr;
ID3D11ShaderResourceView* g_localSrv = nullptr;
HANDLE g_mapping = nullptr;
bigscreen_live_frame_ipc::LiveFrameState* g_state = nullptr;
uint64_t g_generation = 0;
uint64_t g_handle = 0;
uint64_t g_lastSequence = 0;
uint64_t g_frames = 0;
uint64_t g_copies = 0;
uint64_t g_dropped = 0;
uint64_t g_opens = 0;
ULONGLONG g_started = 0;
ULONGLONG g_lastMetrics = 0;
uint32_t g_sourceWidth = 0;
uint32_t g_sourceHeight = 0;
D3D11_VIEWPORT g_activeViewport{};

enum class ViewSource { FirstPerson, ThirdPerson };
ViewSource g_viewSource = ViewSource::FirstPerson;
HANDLE g_thirdMapping = nullptr;
bigscreen_third_person_gpu::State* g_thirdState = nullptr;
ID3D11Texture2D* g_thirdShared = nullptr;
IDXGIKeyedMutex* g_thirdMutex = nullptr;
ID3D11Texture2D* g_thirdLocal = nullptr;
ID3D11ShaderResourceView* g_thirdSrv = nullptr;
uint64_t g_thirdGeneration = 0;
uint64_t g_thirdHandle = 0;
uint64_t g_thirdSequence = 0;
ULONGLONG g_thirdLastFrame = 0;
bool g_thirdUnavailable = false;

// Isolated Remote Peer / MediaPlayer Movie State
HANDLE g_peerMapping = nullptr;
bigscreen_peer_movie::State* g_peerState = nullptr;
ID3D11Texture2D* g_peerShared = nullptr;
IDXGIKeyedMutex* g_peerMutex = nullptr;
ID3D11Texture2D* g_peerLocal = nullptr;
ID3D11ShaderResourceView* g_peerSrv = nullptr;
uint64_t g_peerGeneration = 0;
uint64_t g_peerHandle = 0;
uint64_t g_peerSequence = 0;
uint32_t g_peerWidth = 0;
uint32_t g_peerHeight = 0;
ULONGLONG g_peerLastActive = 0;

// Remote Control & Presentation State (Segment 1)
enum class StereoSplitMode {
    Normal2D = 0,
    SBS = 1,
    OU = 2
};

struct AspectRatioPreset {
    const wchar_t* label;
    float ratio;
};

static const AspectRatioPreset kAspectPresets[] = {
    { L"Aspect: Auto",                           0.0f },
    { L"2.39:1 / 2.40:1 Ultra-Wide Theatrical",  2.39f },
    { L"1.85:1 Standard Widescreen Theatrical",  1.85f },
    { L"1.78:1 (16:9) HDTV Ratio",               16.0f / 9.0f },
    { L"1.33:1 (4:3)",                           4.0f / 3.0f },
    { L"1.375:1 Classic TV Ratio",               1.375f },
    { L"2.00:1 Univisium",                       2.00f },
    { L"1.66:1 European Widescreen Format",      1.66f },
    { L"1.90:1 Digital IMAX",                    1.90f },
    { L"2.20:1 (70mm / Todd-AO)",                2.20f },
    { L"1.43:1 (70mm IMAX)",                     1.43f },
    { L"2.76:1 Ultra Panavision 70",             2.76f },
};

bool g_showRemote = true;
bool g_movieOnly = false;
StereoSplitMode g_stereoMode = StereoSplitMode::Normal2D;
size_t g_aspectPresetIndex = 0;
bool g_isFullscreen = false;
bool g_alwaysOnTop = false;
bool g_keepForegroundUntilMinimized = false;
bool g_inProjectAppMenu = false;
WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };

// Room Ownership State
bool g_bigscreenRunning = false;
bool g_roomHookReady = false;
bool g_inOwnRoom = false;
bool g_isLoadingRoom = false;
bool g_peerStreamActive = false;

bool CanProjectInCurrentRoom() {
    return g_bigscreenRunning && g_roomHookReady && g_inOwnRoom && !g_isLoadingRoom && !g_peerStreamActive;
}

// Mouse Pointer & Look State
bool g_pointerActive = false;
bool g_pointerTrigger = false;
float g_pointerYaw = 0.0f;
float g_pointerPitch = 0.0f;
float g_lookYaw = 0.0f;
float g_lookPitch = 0.0f;
float g_cursorX = 0.0f;
float g_cursorY = 0.0f;
bool g_isRightDragging = false;
POINT g_lastRightPt{};

// Remote UI Layout Rects & Hover States
D2D1_RECT_F g_remoteRect{};
D2D1_RECT_F g_closeRect{};
D2D1_RECT_F g_powerHeaderRect{};
D2D1_RECT_F g_btnWholeTheaterRect{};
D2D1_RECT_F g_btnMovieOnlyRect{};
D2D1_RECT_F g_btnStereo3dRect{};
D2D1_RECT_F g_btnProjectScreenRect{};
D2D1_RECT_F g_btnProjectAppRect{};
D2D1_RECT_F g_btnFullscreenRect{};
D2D1_RECT_F g_btnRecenterRect{};
D2D1_RECT_F g_btnMenuRect{};
D2D1_RECT_F g_btnSystemRect{};
D2D1_RECT_F g_btnPowerRect{};
D2D1_RECT_F g_pillRect{};
D2D1_RECT_F g_btnAspectRect{};

bool g_closeHovered = false;
bool g_powerHeaderHovered = false;
bool g_btnWholeHovered = false;
bool g_btnMovieHovered = false;
bool g_btnStereo3dHovered = false;
bool g_btnAspectHovered = false;
bool g_btnProjectScreenHovered = false;
bool g_btnProjectAppHovered = false;
bool g_btnFsHovered = false;
bool g_btnRecenterHovered = false;
bool g_btnMenuHovered = false;
bool g_btnSystemHovered = false;
bool g_btnPowerHovered = false;
bool g_pillHovered = false;
bool g_btnSystemDown = false;

// Direct2D & DirectWrite Resources
ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
ID2D1RenderTarget* g_d2dRenderTarget = nullptr;

IDWriteTextFormat* g_textFormatTitle = nullptr;
IDWriteTextFormat* g_textFormatSection = nullptr;
IDWriteTextFormat* g_textFormatBtn = nullptr;
IDWriteTextFormat* g_textFormatPill = nullptr;
IDWriteTextFormat* g_textFormatFooter = nullptr;

ID2D1SolidColorBrush* g_brushPanelBg = nullptr;
ID2D1SolidColorBrush* g_brushPanelBorder = nullptr;
ID2D1SolidColorBrush* g_brushBtnActiveBg = nullptr;
ID2D1SolidColorBrush* g_brushBtnInactiveBg = nullptr;
ID2D1SolidColorBrush* g_brushBtnHoverBg = nullptr;
ID2D1SolidColorBrush* g_brushBtnBorder = nullptr;
ID2D1SolidColorBrush* g_brushBtnActiveBorder = nullptr;
ID2D1SolidColorBrush* g_brushTextWhite = nullptr;
ID2D1SolidColorBrush* g_brushTextMuted = nullptr;
ID2D1SolidColorBrush* g_brushTextSection = nullptr;
ID2D1SolidColorBrush* g_brushAccent = nullptr;
ID2D1SolidColorBrush* g_brushCloseHover = nullptr;
ID2D1SolidColorBrush* g_brushSeparator = nullptr;
ID2D1SolidColorBrush* g_brushPointerOuter = nullptr;
ID2D1SolidColorBrush* g_brushPointerInner = nullptr;
ID2D1SolidColorBrush* g_brushPointerActive = nullptr;
ID2D1SolidColorBrush* g_brushFeedActive = nullptr;
ID2D1SolidColorBrush* g_brushFeedStandby = nullptr;
ID2D1SolidColorBrush* g_brushPowerOn = nullptr;
ID2D1SolidColorBrush* g_brushPowerOff = nullptr;

// Isolated App Projection State
enum class ProjectionMode {
    None,
    Screen,
    App
};
ProjectionMode g_projectionMode = ProjectionMode::None;

HWND g_projectedHwnd = nullptr;
std::wstring g_projectedTitle;
ID3D11Texture2D* g_projectedTexture = nullptr;
ID3D11ShaderResourceView* g_projectedSrv = nullptr;
uint32_t g_projectedWidth = 0;
uint32_t g_projectedHeight = 0;

HDC g_fullWinMemDc = nullptr;
HBITMAP g_fullWinBitmap = nullptr;
void* g_fullWinBitmapBits = nullptr;
uint32_t g_fullWinWidth = 0;
uint32_t g_fullWinHeight = 0;
std::vector<uint32_t> g_croppedBits;

int g_lastKnownWinW = 0;
int g_lastKnownWinH = 0;
int g_lastKnownCropX = 0;
int g_lastKnownCropY = 0;
int g_lastKnownCropW = 0;
int g_lastKnownCropH = 0;
float g_lastAdjustedAspect = 0.0f;

struct alignas(16) CbTheaterLayout {
    float screenBounds[4]; // minU, minV, maxU, maxV
    float videoBounds[4];  // minU, minV, maxU, maxV
};

// Direct3D 11 Shared Desktop Texture & Projected Video IPC
ID3D11Texture2D* g_sharedDesktopTex = nullptr;
ID3D11RenderTargetView* g_sharedDesktopRtv = nullptr;
ID3D11ShaderResourceView* g_sharedDesktopSrv = nullptr;
HANDLE g_sharedDesktopHandle = nullptr;
uint32_t g_sharedDesktopW = 0;
uint32_t g_sharedDesktopH = 0;

HANDLE g_projectedVideoMapping = nullptr;
bigscreen_projected_video::State* g_projectedVideoState = nullptr;
DWORD g_lastInjectedBigscreenPid = 0;
ULONGLONG g_lastInjectionCheck = 0;

void EnsureProjectedVideoIpc() {
    if (!g_projectedVideoMapping) {
        g_projectedVideoMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(bigscreen_projected_video::State),
            bigscreen_projected_video::kMappingName);
    }
    if (g_projectedVideoMapping && !g_projectedVideoState) {
        g_projectedVideoState = static_cast<bigscreen_projected_video::State*>(
            MapViewOfFile(g_projectedVideoMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                          sizeof(bigscreen_projected_video::State)));
        if (g_projectedVideoState && GetLastError() != ERROR_ALREADY_EXISTS) {
            *g_projectedVideoState = {};
            g_projectedVideoState->magic = bigscreen_projected_video::kMagic;
            g_projectedVideoState->version = bigscreen_projected_video::kVersion;
            g_projectedVideoState->privacyMode = 1;
        }
    }
}

float GetSelectedAspectOverride() {
    if (g_aspectPresetIndex >= _countof(kAspectPresets)) return 0.0f;
    return kAspectPresets[g_aspectPresetIndex].ratio;
}

void SyncAspectOverrideToIpc() {
    EnsureProjectedVideoIpc();
    if (g_projectedVideoState) {
        g_projectedVideoState->magic = bigscreen_projected_video::kMagic;
        g_projectedVideoState->version = bigscreen_projected_video::kVersion;
        g_projectedVideoState->aspectOverride = GetSelectedAspectOverride();
    }
}

void PublishProjectedVideoState(bool active, bool privacyMode, uint64_t sharedHandle,
                                uint32_t width, uint32_t height, uint32_t vidW, uint32_t vidH) {
    EnsureProjectedVideoIpc();
    if (g_projectedVideoState) {
        g_projectedVideoState->magic = bigscreen_projected_video::kMagic;
        g_projectedVideoState->version = bigscreen_projected_video::kVersion;
        g_projectedVideoState->active = active ? 1 : 0;
        g_projectedVideoState->privacyMode = privacyMode ? 1 : 0;
        g_projectedVideoState->sharedHandle = sharedHandle;
        g_projectedVideoState->width = width;
        g_projectedVideoState->height = height;
        g_projectedVideoState->videoWidth = vidW;
        g_projectedVideoState->videoHeight = vidH;
        g_projectedVideoState->aspectOverride = GetSelectedAspectOverride();
        ++g_projectedVideoState->frameSequence;
        g_projectedVideoState->lastFrameMicros = GetTickCount64() * 1000ull;
    }
}

float ComputeMovieDisplayAspect(uint32_t rawW, uint32_t rawH) {
    float ov = GetSelectedAspectOverride();
    if (ov > 0.0f) return ov;
    if (rawW == 0 || rawH == 0) return 16.0f / 9.0f;
    float rawAspect = static_cast<float>(rawW) / static_cast<float>(rawH);
    if (!g_movieOnly || g_stereoMode == StereoSplitMode::Normal2D) {
        return rawAspect;
    }
    if (g_stereoMode == StereoSplitMode::SBS) {
        return (rawAspect > 2.6f) ? (rawAspect * 0.5f) : rawAspect;
    }
    return (rawAspect < 1.15f) ? (rawAspect * 2.0f) : rawAspect;
}

void ReleaseSharedDesktopTexture() {
    if (g_sharedDesktopRtv) { g_sharedDesktopRtv->Release(); g_sharedDesktopRtv = nullptr; }
    if (g_sharedDesktopSrv) { g_sharedDesktopSrv->Release(); g_sharedDesktopSrv = nullptr; }
    if (g_sharedDesktopTex) { g_sharedDesktopTex->Release(); g_sharedDesktopTex = nullptr; }
    g_sharedDesktopHandle = nullptr;
    g_sharedDesktopW = 0;
    g_sharedDesktopH = 0;
}

bool EnsureSharedDesktopTexture(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || !g_device) return false;
    if (g_sharedDesktopTex && g_sharedDesktopW == w && g_sharedDesktopH == h) return true;
    ReleaseSharedDesktopTexture();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_sharedDesktopTex))) return false;
    if (FAILED(g_device->CreateRenderTargetView(g_sharedDesktopTex, nullptr, &g_sharedDesktopRtv))) {
        ReleaseSharedDesktopTexture(); return false;
    }
    if (FAILED(g_device->CreateShaderResourceView(g_sharedDesktopTex, nullptr, &g_sharedDesktopSrv))) {
        ReleaseSharedDesktopTexture(); return false;
    }

    IDXGIResource* res = nullptr;
    if (SUCCEEDED(g_sharedDesktopTex->QueryInterface(IID_PPV_ARGS(&res)))) {
        res->GetSharedHandle(&g_sharedDesktopHandle);
        res->Release();
    }

    g_sharedDesktopW = w;
    g_sharedDesktopH = h;
    return true;
}

void RenderVideoToSharedDesktop() {
    uint32_t deskW = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    uint32_t deskH = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    if (deskW == 0 || deskH == 0 || !g_context) return;
    if (!EnsureSharedDesktopTexture(deskW, deskH)) return;

    ID3D11RenderTargetView* prevRtv = nullptr;
    ID3D11DepthStencilView* prevDsv = nullptr;
    g_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);
    UINT numVp = 1;
    D3D11_VIEWPORT prevVp{};
    g_context->RSGetViewports(&numVp, &prevVp);

    g_context->OMSetRenderTargets(1, &g_sharedDesktopRtv, nullptr);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_context->ClearRenderTargetView(g_sharedDesktopRtv, black);

    if (g_projectionMode == ProjectionMode::App && g_projectedSrv && g_projectedWidth > 0 && g_projectedHeight > 0) {
        float ov = GetSelectedAspectOverride();
        float videoAspect = (ov > 0.0f) ? ov : (static_cast<float>(g_projectedWidth) / static_cast<float>(g_projectedHeight));
        float canvasAspect = static_cast<float>(deskW) / static_cast<float>(deskH);

        float boxW = 0.0f, boxH = 0.0f, boxX = 0.0f, boxY = 0.0f;
        if (canvasAspect > videoAspect) {
            boxH = static_cast<float>(deskH);
            boxW = boxH * videoAspect;
            boxX = (static_cast<float>(deskW) - boxW) * 0.5f;
            boxY = 0.0f;
        } else {
            boxW = static_cast<float>(deskW);
            boxH = boxW / videoAspect;
            boxX = 0.0f;
            boxY = (static_cast<float>(deskH) - boxH) * 0.5f;
        }

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = boxX;
        vp.TopLeftY = boxY;
        vp.Width = boxW;
        vp.Height = boxH;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_context->RSSetViewports(1, &vp);

        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vs, nullptr, 0);
        g_context->PSSetShader(g_ps, nullptr, 0);
        g_context->PSSetSamplers(0, 1, &g_sampler);
        g_context->PSSetShaderResources(0, 1, &g_projectedSrv);
        g_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);
    }

    g_context->Flush();

    g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    if (numVp > 0) g_context->RSSetViewports(1, &prevVp);

    PublishProjectedVideoState(
        g_projectionMode == ProjectionMode::App,
        true,
        reinterpret_cast<uint64_t>(g_sharedDesktopHandle),
        deskW, deskH,
        g_projectedWidth, g_projectedHeight
    );
}

void ClearSharedDesktopToBlack() {
    uint32_t deskW = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    uint32_t deskH = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    if (EnsureSharedDesktopTexture(deskW, deskH) && g_sharedDesktopRtv && g_context) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_context->ClearRenderTargetView(g_sharedDesktopRtv, black);
        g_context->Flush();
    }
    PublishProjectedVideoState(false, true, reinterpret_cast<uint64_t>(g_sharedDesktopHandle),
                              deskW, deskH, 0, 0);
}

bool InjectDllIntoBigscreen(DWORD pid) {
    if (!pid) return false;
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{}; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, L"BigscreenThirdPersonGpuExport.dll") == 0) {
                    CloseHandle(snap);
                    CloseHandle(hProcess);
                    return true;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    wchar_t myPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, myPath, MAX_PATH);
    fs::path dir = fs::path(myPath).parent_path();
    fs::path dllPath = dir / L"BigscreenThirdPersonGpuExport.dll";
    if (!fs::exists(dllPath)) dllPath = dir / L"runtime" / L"BigscreenThirdPersonGpuExport.dll";
    if (!fs::exists(dllPath)) dllPath = dir / L".." / L"runtime" / L"BigscreenThirdPersonGpuExport.dll";
    if (!fs::exists(dllPath)) dllPath = dir / L"build" / L"runtime" / L"BigscreenThirdPersonGpuExport.dll";
    if (!fs::exists(dllPath)) {
        CloseHandle(hProcess);
        return false;
    }

    std::wstring ws = dllPath.wstring();
    size_t bytes = (ws.length() + 1) * sizeof(wchar_t);
    LPVOID pRemoteBuf = VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemoteBuf) { CloseHandle(hProcess); return false; }

    if (!WriteProcessMemory(hProcess, pRemoteBuf, ws.c_str(), bytes, nullptr)) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    auto pfnLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hK32, "LoadLibraryW"));
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pfnLoadLibrary, pRemoteBuf, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    Log("Successfully injected desktop routing hook into Bigscreen.exe (PID %u)", pid);
    return true;
}

void EnsureBigscreenHookInjected() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastInjectionCheck < 1500) return;
    g_lastInjectionCheck = now;

    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"Bigscreen.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (pid && pid != g_lastInjectedBigscreenPid) {
        if (InjectDllIntoBigscreen(pid)) {
            g_lastInjectedBigscreenPid = pid;
        }
    }
}

RECT g_origAppRect{};
LONG g_origAppExStyle = 0;
bool g_appIsHidden = false;

void CloakAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!g_appIsHidden) {
        GetWindowRect(hwnd, &g_origAppRect);
        g_origAppExStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetWindowLongW(hwnd, GWL_EXSTYLE, g_origAppExStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);

    int origW = g_origAppRect.right - g_origAppRect.left;
    int origH = g_origAppRect.bottom - g_origAppRect.top;
    if (origW < 200 || origH < 200) { origW = 1280; origH = 720; }
    SetWindowPos(hwnd, HWND_BOTTOM, g_origAppRect.left, g_origAppRect.top, origW, origH,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_appIsHidden = true;
    Log("Cloaked video app window (invisible, click-through, rendering at full speed in background)");
}

void RestoreAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !g_appIsHidden) return;
    SetWindowLongW(hwnd, GWL_EXSTYLE, g_origAppExStyle);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    int origW = g_origAppRect.right - g_origAppRect.left;
    int origH = g_origAppRect.bottom - g_origAppRect.top;
    if (origW < 200 || origH < 200) { origW = 1280; origH = 720; }
    SetWindowPos(hwnd, HWND_TOP, g_origAppRect.left, g_origAppRect.top, origW, origH,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    g_appIsHidden = false;
    Log("Restored video app window to normal desktop visibility");
}

void ToggleAppWindowVisibility() {
    if (!g_projectedHwnd || !IsWindow(g_projectedHwnd)) return;
    if (g_appIsHidden) {
        RestoreAppWindow(g_projectedHwnd);
    } else {
        CloakAppWindow(g_projectedHwnd);
    }
}

void AdjustWindowToVideoAspect(HWND hwnd, float aspect) {
    if (!hwnd || g_isFullscreen || aspect <= 0.2f || aspect > 4.0f) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    int curH = client.bottom - client.top;
    if (curH < 360) curH = 720;
    int newW = static_cast<int>(std::round(curH * aspect));
    int maxW = GetSystemMetrics(SM_CXSCREEN) - 80;
    if (newW > maxW) {
        newW = maxW;
        curH = static_cast<int>(std::round(newW / aspect));
    }
    RECT wr = { 0, 0, newW, curH };
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    AdjustWindowRect(&wr, style, FALSE);
    
    RECT curWin{};
    GetWindowRect(hwnd, &curWin);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;
    int winX = curWin.left;
    int winY = curWin.top;
    if (winX + winW > GetSystemMetrics(SM_CXSCREEN)) {
        winX = GetSystemMetrics(SM_CXSCREEN) - winW - 20;
    }
    if (winY + winH > GetSystemMetrics(SM_CYSCREEN)) {
        winY = GetSystemMetrics(SM_CYSCREEN) - winH - 40;
    }
    if (winX < 0) winX = 20;
    if (winY < 0) winY = 20;

    SetWindowPos(hwnd, nullptr, winX, winY, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Controller Control IPC
HANDLE g_controlMapping = nullptr;
bigscreen_controller_control_ipc::State* g_controlState = nullptr;

void Log(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    std::printf("%s\n", buffer);
    std::fflush(stdout);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

class AudioFeedDetector {
public:
    AudioFeedDetector() = default;
    ~AudioFeedDetector() { Release(); }

    bool Initialize() {
        if (meter_) return true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) return false;

        IMMDevice* defaultDevice = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice);
        enumerator->Release();
        if (FAILED(hr) || !defaultDevice) return false;

        hr = defaultDevice->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&meter_));
        defaultDevice->Release();
        return SUCCEEDED(hr) && meter_ != nullptr;
    }

    float GetPeak() {
        if (!meter_) {
            if (!Initialize()) return 0.0f;
        }
        float peak = 0.0f;
        HRESULT hr = meter_->GetPeakValue(&peak);
        if (FAILED(hr)) {
            Release();
            return 0.0f;
        }
        return peak;
    }

    void Release() {
        if (meter_) {
            meter_->Release();
            meter_ = nullptr;
        }
    }

private:
    IAudioMeterInformation* meter_ = nullptr;
};

AudioFeedDetector g_audioDetector;
bool g_feedActive = false;
bool g_waitingForNewFeed = false;
bool g_feedActiveWhenMenuOpened = false;
bool g_feedSawDropAfterMenuOpen = false;
ULONGLONG g_menuOpenedTime = 0;
float g_audioPeak = 0.0f;
ULONGLONG g_lastFeedCheck = 0;
int g_feedActiveStreak = 0;

HWND FindBigscreenWindow() {
    HWND hwnd = FindWindowW(L"UnityWndClass", L"Bigscreen");
    if (!hwnd) hwnd = FindWindowW(nullptr, L"Bigscreen");
    if (!hwnd) {
        DWORD targetPid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"Bigscreen.exe") == 0) {
                        targetPid = pe.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (targetPid != 0) {
            struct EnumCtx { DWORD pid; HWND hwnd; } ctx{ targetPid, nullptr };
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == c->pid && IsWindowVisible(h)) {
                    LONG style = GetWindowLongW(h, GWL_STYLE);
                    if ((style & WS_CAPTION) || (style & WS_POPUP)) {
                        c->hwnd = h;
                        return FALSE;
                    }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
            hwnd = ctx.hwnd;
        }
    }
    return hwnd;
}

void ToggleDesktopMenu() {
    HWND hwnd = FindBigscreenWindow();
    if (!hwnd) {
        Log("Bigscreen desktop window not found.");
        return;
    }
    if (IsIconic(hwnd)) {
        // Currently minimized -> Restore and bring to front
        if (g_keepForegroundUntilMinimized && g_hwnd) {
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        g_waitingForNewFeed = true;
        g_menuOpenedTime = GetTickCount64();
        g_feedActiveWhenMenuOpened = g_feedActive;
        g_feedSawDropAfterMenuOpen = false;
        Log("Restored Bigscreen desktop menu. Waiting for feed to auto-minimize...");
    } else {
        // Currently visible/open -> Minimize it!
        ShowWindow(hwnd, SW_MINIMIZE);
        if (g_keepForegroundUntilMinimized && g_hwnd && !IsIconic(g_hwnd)) {
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        SetForegroundWindow(g_hwnd);
        g_waitingForNewFeed = false;
        Log("User minimized Bigscreen desktop menu.");
    }
}

void CheckFeedStatus() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFeedCheck < 100) return;
    g_lastFeedCheck = now;

    g_audioPeak = g_audioDetector.GetPeak();
    if (g_audioPeak > 0.005f) {
        if (g_feedActiveStreak < 10) g_feedActiveStreak++;
    } else {
        if (g_feedActiveStreak > 0) g_feedActiveStreak--;
    }
    g_feedActive = (g_feedActiveStreak >= 3);

    if (g_waitingForNewFeed) {
        // Enforce a minimum 2-second grace period after opening menu before auto-minimizing
        if (now - g_menuOpenedTime >= 2000) {
            bool newFeedDetected = false;
            if (!g_feedActiveWhenMenuOpened) {
                // If there was no stream when menu opened, auto-minimize as soon as stream begins
                if (g_feedActive) {
                    newFeedDetected = true;
                }
            } else {
                // If a stream was already playing when menu opened, wait for stream to stop/switch (silence) and then resume
                if (!g_feedActive) {
                    g_feedSawDropAfterMenuOpen = true;
                } else if (g_feedSawDropAfterMenuOpen && g_feedActive) {
                    newFeedDetected = true;
                }
            }

            if (newFeedDetected) {
                g_waitingForNewFeed = false;
                g_movieOnly = false; // When the user enters a room that is playing a movie, load in Theater Mode!
                g_lookYaw = 0.0f;
                g_lookPitch = 0.0f;
                SyncPointerAndLook();
                HWND hwndBig = FindBigscreenWindow();
                if (hwndBig && !IsIconic(hwndBig)) {
                    ShowWindow(hwndBig, SW_MINIMIZE);
                    if (g_keepForegroundUntilMinimized && g_hwnd && !IsIconic(g_hwnd)) {
                        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                    }
                    SetForegroundWindow(g_hwnd);
                    Log("New audio/video feed detected (audio peak=%.3f). Loaded Theater Mode & auto-minimized Bigscreen menu.", g_audioPeak);
                }
            }
        }
    }
}

bool IsProcessRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool IsVrRunning() {
    return IsProcessRunning(L"Bigscreen.exe") ||
           IsProcessRunning(L"vrserver.exe") ||
           IsProcessRunning(L"vrmonitor.exe");
}

void TerminateVrProcesses() {
    const wchar_t* targets[] = {
        L"Bigscreen.exe",
        L"vrserver.exe",
        L"vrmonitor.exe",
        L"vrcompositor.exe",
        L"vrdashboard.exe",
        L"BigscreenDesktopBridge.exe"
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            for (const auto* target : targets) {
                if (_wcsicmp(pe.szExeFile, target) == 0) {
                    HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (proc) {
                        TerminateProcess(proc, 0);
                        CloseHandle(proc);
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

void MaintainForegroundWhileLaunching() {
    if (!g_keepForegroundUntilMinimized || !g_hwnd || !IsWindow(g_hwnd) || IsIconic(g_hwnd)) {
        return;
    }
    if (g_waitingForNewFeed || g_inProjectAppMenu) {
        return;
    }

    static ULONGLONG s_lastEnforceMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastEnforceMs < 150) return;
    s_lastEnforceMs = now;

    if ((GetWindowLongW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd) {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid);
        bool vrOrSteamStoleFocus = false;
        if (hProc) {
            wchar_t exePath[MAX_PATH]{};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exePath, &sz)) {
                if (wcsstr(exePath, L"Bigscreen.exe") ||
                    wcsstr(exePath, L"vrmonitor.exe") ||
                    wcsstr(exePath, L"vrserver.exe") ||
                    wcsstr(exePath, L"vrcompositor.exe") ||
                    wcsstr(exePath, L"steam.exe") ||
                    wcsstr(exePath, L"steamwebhelper.exe")) {
                    vrOrSteamStoleFocus = true;
                }
            }
            CloseHandle(hProc);
        }
        if (vrOrSteamStoleFocus) {
            SetForegroundWindow(g_hwnd);
        }
    }
}

void LaunchVrProcesses() {
    g_keepForegroundUntilMinimized = true;
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (IsIconic(g_hwnd)) {
            ShowWindow(g_hwnd, SW_RESTORE);
        }
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_hwnd);
    }
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOW);
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/457550", nullptr, nullptr, SW_SHOW);
}

void ReleaseProjectedAppResources() {
    if (g_projectedHwnd && IsWindow(g_projectedHwnd) && g_appIsHidden) {
        RestoreAppWindow(g_projectedHwnd);
    }
    ClearSharedDesktopToBlack();
    if (g_projectedSrv) { g_projectedSrv->Release(); g_projectedSrv = nullptr; }
    if (g_projectedTexture) { g_projectedTexture->Release(); g_projectedTexture = nullptr; }
    if (g_fullWinBitmap) { DeleteObject(g_fullWinBitmap); g_fullWinBitmap = nullptr; }
    if (g_fullWinMemDc) { DeleteDC(g_fullWinMemDc); g_fullWinMemDc = nullptr; }
    g_fullWinBitmapBits = nullptr;
    g_fullWinWidth = 0;
    g_fullWinHeight = 0;
    g_projectedWidth = 0;
    g_projectedHeight = 0;
    g_projectedHwnd = nullptr;
    g_projectedTitle.clear();
    g_croppedBits.clear();
    g_lastKnownWinW = 0;
    g_lastKnownWinH = 0;
    g_lastKnownCropX = 0;
    g_lastKnownCropY = 0;
    g_lastKnownCropW = 0;
    g_lastKnownCropH = 0;
    g_lastAdjustedAspect = 0.0f;
    g_appIsHidden = false;
    g_stereoMode = StereoSplitMode::Normal2D;
}

bool EnsureFullWinBuffers(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (g_fullWinBitmap && g_fullWinWidth == width && g_fullWinHeight == height) return true;

    if (g_fullWinBitmap) { DeleteObject(g_fullWinBitmap); g_fullWinBitmap = nullptr; }
    if (g_fullWinMemDc) { DeleteDC(g_fullWinMemDc); g_fullWinMemDc = nullptr; }

    HDC screenDc = GetDC(nullptr);
    g_fullWinMemDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!g_fullWinMemDc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_fullWinBitmap = CreateDIBSection(g_fullWinMemDc, &bmi, DIB_RGB_COLORS, &g_fullWinBitmapBits, nullptr, 0);
    if (!g_fullWinBitmap || !g_fullWinBitmapBits) return false;
    SelectObject(g_fullWinMemDc, g_fullWinBitmap);

    g_fullWinWidth = width;
    g_fullWinHeight = height;
    return true;
}

bool EnsureProjectedTexture(uint32_t cropW, uint32_t cropH) {
    if (cropW == 0 || cropH == 0 || !g_device) return false;
    if (g_projectedTexture && g_projectedWidth == cropW && g_projectedHeight == cropH) return true;

    if (g_projectedSrv) { g_projectedSrv->Release(); g_projectedSrv = nullptr; }
    if (g_projectedTexture) { g_projectedTexture->Release(); g_projectedTexture = nullptr; }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = cropW;
    desc.Height = cropH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_projectedTexture))) return false;
    if (FAILED(g_device->CreateShaderResourceView(g_projectedTexture, nullptr, &g_projectedSrv))) return false;

    g_projectedWidth = cropW;
    g_projectedHeight = cropH;
    g_croppedBits.resize(cropW * cropH);
    return true;
}

HWND FindVideoChildWindow(HWND parent) {
    struct SearchData {
        HWND bestHwnd = nullptr;
        int maxScore = 0;
    };
    SearchData data;

    EnumChildWindows(parent, [](HWND child, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<SearchData*>(lParam);
        if (!IsWindowVisible(child)) return TRUE;
        RECT crc{};
        GetWindowRect(child, &crc);
        int w = crc.right - crc.left;
        int h = crc.bottom - crc.top;
        if (w < 80 || h < 80) return TRUE;

        wchar_t cls[128]{};
        GetClassNameW(child, cls, 128);

        bool isVideoClass = (wcsstr(cls, L"VLC") != nullptr) ||
                            (wcsstr(cls, L"Video") != nullptr) ||
                            (wcsstr(cls, L"video") != nullptr) ||
                            (wcsstr(cls, L"Render") != nullptr) ||
                            (wcsstr(cls, L"D3D") != nullptr) ||
                            (wcsstr(cls, L"Direct3D") != nullptr) ||
                            (wcsstr(cls, L"OpenGL") != nullptr) ||
                            (wcsstr(cls, L"Surface") != nullptr) ||
                            (wcsstr(cls, L"SubWindow") != nullptr);

        int score = w * h;
        if (isVideoClass) {
            score *= 3; // strong preference for designated video surfaces
        } else {
            float aspect = static_cast<float>(w) / static_cast<float>(h);
            if (aspect < 0.35f || aspect > 3.2f) {
                return TRUE; // ignore skinny toolbars / docks
            }
        }

        if (score > d->maxScore) {
            d->bestHwnd = child;
            d->maxScore = score;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.bestHwnd;
}

bool CaptureProjectedWindowFrame() {
    if (!g_projectedHwnd || !IsWindow(g_projectedHwnd)) {
        ReleaseProjectedAppResources();
        g_projectionMode = ProjectionMode::None;
        return false;
    }

    const bool isIconic = IsIconic(g_projectedHwnd) != FALSE;
    RECT winRect{};
    if (isIconic) {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(g_projectedHwnd, &wp)) {
            winRect = wp.rcNormalPosition;
        }
    }
    if (winRect.right <= winRect.left) {
        GetWindowRect(g_projectedHwnd, &winRect);
    }
    int winW = winRect.right - winRect.left;
    int winH = winRect.bottom - winRect.top;
    if (winW <= 32 || winH <= 32) {
        if (g_lastKnownWinW > 32 && g_lastKnownWinH > 32) {
            winW = g_lastKnownWinW;
            winH = g_lastKnownWinH;
        } else {
            return false;
        }
    }

    if (!EnsureFullWinBuffers(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH))) return false;

    BOOL ok = PrintWindow(g_projectedHwnd, g_fullWinMemDc, 0x00000002 /* PW_RENDERFULLCONTENT */);
    if (!ok && isIconic) {
        // If DWM discarded the minimized surface, restore without activation to keep rendering active in background
        WINDOWPLACEMENT wp{ sizeof(wp) };
        GetWindowPlacement(g_projectedHwnd, &wp);
        ShowWindow(g_projectedHwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_projectedHwnd, HWND_BOTTOM, wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                     winW, winH, SWP_NOACTIVATE | SWP_NOZORDER);
        ok = PrintWindow(g_projectedHwnd, g_fullWinMemDc, 0x00000002);
    }
    if (!ok) {
        ok = PrintWindow(g_projectedHwnd, g_fullWinMemDc, 0);
    }
    if (!ok || !g_fullWinBitmapBits) return false;

    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;

    if (!isIconic) {
        RECT targetRect{};
        HWND childVid = FindVideoChildWindow(g_projectedHwnd);
        if (childVid && IsWindow(childVid) && IsWindowVisible(childVid)) {
            GetWindowRect(childVid, &targetRect);
        } else {
            POINT pt{ 0, 0 };
            ClientToScreen(g_projectedHwnd, &pt);
            RECT cr{};
            GetClientRect(g_projectedHwnd, &cr);
            targetRect = { pt.x, pt.y, pt.x + cr.right, pt.y + cr.bottom };
        }

        cropX = targetRect.left - winRect.left;
        cropY = targetRect.top - winRect.top;
        cropW = targetRect.right - targetRect.left;
        cropH = targetRect.bottom - targetRect.top;

        if (cropX < 0) { cropW += cropX; cropX = 0; }
        if (cropY < 0) { cropH += cropY; cropY = 0; }
        if (cropX + cropW > winW) cropW = winW - cropX;
        if (cropY + cropH > winH) cropH = winH - cropY;

        if (cropW >= 32 && cropH >= 32) {
            g_lastKnownWinW = winW;
            g_lastKnownWinH = winH;
            g_lastKnownCropX = cropX;
            g_lastKnownCropY = cropY;
            g_lastKnownCropW = cropW;
            g_lastKnownCropH = cropH;
        }
    } else {
        cropX = g_lastKnownCropX;
        cropY = g_lastKnownCropY;
        cropW = g_lastKnownCropW;
        cropH = g_lastKnownCropH;
    }

    if (cropW < 32 || cropH < 32) return false;

    if (!EnsureProjectedTexture(static_cast<uint32_t>(cropW), static_cast<uint32_t>(cropH))) return false;

    // Pack cropped rows into g_croppedBits, completely omitting top title bar and borders
    for (int y = 0; y < cropH; ++y) {
        const uint32_t* srcRow = static_cast<const uint32_t*>(g_fullWinBitmapBits) + (cropY + y) * g_fullWinWidth + cropX;
        uint32_t* dstRow = g_croppedBits.data() + y * cropW;
        memcpy(dstRow, srcRow, cropW * sizeof(uint32_t));
    }

    if (g_context && g_projectedTexture) {
        g_context->UpdateSubresource(g_projectedTexture, 0, nullptr, g_croppedBits.data(), cropW * sizeof(uint32_t), 0);
        RenderVideoToSharedDesktop();

        // In Movie Only mode, automatically adapt the window to match the video's exact aspect ratio
        if (g_movieOnly) {
            float aspect = ComputeMovieDisplayAspect(static_cast<uint32_t>(cropW), static_cast<uint32_t>(cropH));
            if (std::abs(aspect - g_lastAdjustedAspect) > 0.02f) {
                g_lastAdjustedAspect = aspect;
                AdjustWindowToVideoAspect(g_hwnd, aspect);
            }
        }
        return true;
    }
    return false;
}

struct AppWindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
};

std::vector<AppWindowInfo> EnumerateSelectableAppWindows() {
    std::vector<AppWindowInfo> list;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* outList = reinterpret_cast<std::vector<AppWindowInfo>*>(lParam);
        if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
        if (hwnd == g_hwnd) return TRUE;

        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        if (!(style & WS_VISIBLE) && !IsIconic(hwnd)) return TRUE;
        LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        wchar_t title[256]{};
        int len = GetWindowTextW(hwnd, title, 256);
        if (len <= 0) return TRUE;

        if (wcscmp(title, L"Program Manager") == 0 ||
            wcscmp(title, L"Windows Input Experience") == 0 ||
            wcscmp(title, L"Settings") == 0) {
            return TRUE;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess) {
            wchar_t exePath[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
                if (wcsstr(exePath, L"BigscreenDesktopViewer") ||
                    wcsstr(exePath, L"SteamVR") ||
                    wcsstr(exePath, L"vrserver") ||
                    wcsstr(exePath, L"vrmonitor")) {
                    CloseHandle(hProcess);
                    return TRUE;
                }
            }
            CloseHandle(hProcess);
        }

        outList->push_back({ hwnd, title });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));

    return list;
}

void EnsureControlMapping() {
    if (!g_controlMapping) {
        g_controlMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(bigscreen_controller_control_ipc::State),
            bigscreen_controller_control_ipc::kMappingName);
    }
    if (g_controlMapping && !g_controlState) {
        g_controlState = static_cast<bigscreen_controller_control_ipc::State*>(
            MapViewOfFile(g_controlMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                          sizeof(bigscreen_controller_control_ipc::State)));
        if (g_controlState && GetLastError() != ERROR_ALREADY_EXISTS) {
            g_controlState->magic = bigscreen_controller_control_ipc::kMagic;
            g_controlState->version = bigscreen_controller_control_ipc::kVersion;
            g_controlState->virtualControllersEnabled = 1;
            g_controlState->cameraDistance = 2.5f;
            g_controlState->cameraHeight = 0.6f;
            g_controlState->cameraFov = 60.0f;
        }
    }
}

void SyncViewRequest(bigscreen_controller_control_ipc::ViewRequest req) {
    EnsureControlMapping();
    if (g_controlState) {
        bigscreen_controller_control_ipc::SetViewRequest(g_controlState, req);
    }
}

void SyncPointerAndLook() {
    EnsureControlMapping();
    if (g_controlState) {
        bigscreen_controller_control_ipc::SetPointerAndLook(
            g_controlState, g_pointerYaw, g_pointerPitch, g_pointerTrigger, g_pointerActive,
            g_lookYaw, g_lookPitch);
    }
}

void ResetLook() {
    g_lookYaw = 0.0f;
    g_lookPitch = 0.0f;
    g_pointerYaw = 0.0f;
    g_pointerPitch = 0.0f;
    EnsureControlMapping();
    if (g_controlState) {
        bigscreen_controller_control_ipc::TriggerReset(g_controlState);
        SyncPointerAndLook();
    }
}

void StartProjectingAppWindow(HWND targetHwnd, const std::wstring& title) {
    if (!targetHwnd || !IsWindow(targetHwnd)) return;
    ReleaseProjectedAppResources();
    g_projectedHwnd = targetHwnd;
    g_projectedTitle = title;
    g_projectionMode = ProjectionMode::App;
    g_lookYaw = 0.0f;
    g_lookPitch = 0.0f;
    SyncPointerAndLook();
    CloakAppWindow(g_projectedHwnd);
    Log("Started projecting isolated application window: %ls (Cloaked from desktop)", g_projectedTitle.c_str());
}

void ShowProjectAppMenu(HWND parentHwnd, float clickX, float clickY) {
    auto windows = EnumerateSelectableAppWindows();
    HMENU hMenu = CreatePopupMenu();
    if (windows.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"No eligible video/app windows found");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"--- Select Video App to Project Privately ---");
        for (size_t i = 0; i < windows.size() && i < 25; ++i) {
            std::wstring itemText = windows[i].title;
            if (itemText.length() > 45) {
                itemText = itemText.substr(0, 42) + L"...";
            }
            AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(1000 + i), itemText.c_str());
        }
    }
    POINT pt{ static_cast<LONG>(clickX), static_cast<LONG>(clickY) };
    ClientToScreen(parentHwnd, &pt);
    g_inProjectAppMenu = true;
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, parentHwnd, nullptr);
    g_inProjectAppMenu = false;
    DestroyMenu(hMenu);
    if (cmd >= 1000 && static_cast<size_t>(cmd - 1000) < windows.size()) {
        if (CanProjectInCurrentRoom()) {
            StartProjectingAppWindow(windows[cmd - 1000].hwnd, windows[cmd - 1000].title);
        }
    }
}

void ToggleFullscreen(HWND hwnd) {
    const DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_isFullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &g_wpPrev) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_isFullscreen = true;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_wpPrev);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_isFullscreen = false;
    }
}

void ToggleAlwaysOnTop(HWND hwnd) {
    g_alwaysOnTop = !g_alwaysOnTop;
    SetWindowPos(hwnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void ReleaseSharedResources() {
    if (g_localSrv) g_localSrv->Release();
    if (g_localTexture) g_localTexture->Release();
    if (g_sharedMutex) g_sharedMutex->Release();
    if (g_sharedTexture) g_sharedTexture->Release();
    g_localSrv = nullptr;
    g_localTexture = nullptr;
    g_sharedMutex = nullptr;
    g_sharedTexture = nullptr;
    g_generation = 0;
    g_handle = 0;
    g_lastSequence = 0;
    g_sourceWidth = 0;
    g_sourceHeight = 0;
}

void ReleaseThirdPersonResources() {
    if (g_thirdSrv) g_thirdSrv->Release();
    if (g_thirdLocal) g_thirdLocal->Release();
    if (g_thirdMutex) g_thirdMutex->Release();
    if (g_thirdShared) g_thirdShared->Release();
    if (g_thirdState) UnmapViewOfFile(g_thirdState);
    if (g_thirdMapping) CloseHandle(g_thirdMapping);
    g_thirdSrv = nullptr;
    g_thirdLocal = nullptr;
    g_thirdMutex = nullptr;
    g_thirdShared = nullptr;
    g_thirdState = nullptr;
    g_thirdMapping = nullptr;
    g_thirdGeneration = 0;
    g_thirdHandle = 0;
    g_thirdSequence = 0;
    g_thirdLastFrame = 0;
}

void ReleaseD2DDeviceResources() {
    if (g_brushPowerOff) { g_brushPowerOff->Release(); g_brushPowerOff = nullptr; }
    if (g_brushPowerOn) { g_brushPowerOn->Release(); g_brushPowerOn = nullptr; }
    if (g_brushFeedStandby) { g_brushFeedStandby->Release(); g_brushFeedStandby = nullptr; }
    if (g_brushFeedActive) { g_brushFeedActive->Release(); g_brushFeedActive = nullptr; }
    if (g_brushPointerActive) { g_brushPointerActive->Release(); g_brushPointerActive = nullptr; }
    if (g_brushPointerInner) { g_brushPointerInner->Release(); g_brushPointerInner = nullptr; }
    if (g_brushPointerOuter) { g_brushPointerOuter->Release(); g_brushPointerOuter = nullptr; }
    if (g_brushSeparator) { g_brushSeparator->Release(); g_brushSeparator = nullptr; }
    if (g_brushCloseHover) { g_brushCloseHover->Release(); g_brushCloseHover = nullptr; }
    if (g_brushAccent) { g_brushAccent->Release(); g_brushAccent = nullptr; }
    if (g_brushTextSection) { g_brushTextSection->Release(); g_brushTextSection = nullptr; }
    if (g_brushTextMuted) { g_brushTextMuted->Release(); g_brushTextMuted = nullptr; }
    if (g_brushTextWhite) { g_brushTextWhite->Release(); g_brushTextWhite = nullptr; }
    if (g_brushBtnActiveBorder) { g_brushBtnActiveBorder->Release(); g_brushBtnActiveBorder = nullptr; }
    if (g_brushBtnBorder) { g_brushBtnBorder->Release(); g_brushBtnBorder = nullptr; }
    if (g_brushBtnHoverBg) { g_brushBtnHoverBg->Release(); g_brushBtnHoverBg = nullptr; }
    if (g_brushBtnInactiveBg) { g_brushBtnInactiveBg->Release(); g_brushBtnInactiveBg = nullptr; }
    if (g_brushBtnActiveBg) { g_brushBtnActiveBg->Release(); g_brushBtnActiveBg = nullptr; }
    if (g_brushPanelBorder) { g_brushPanelBorder->Release(); g_brushPanelBorder = nullptr; }
    if (g_brushPanelBg) { g_brushPanelBg->Release(); g_brushPanelBg = nullptr; }
    if (g_d2dRenderTarget) { g_d2dRenderTarget->Release(); g_d2dRenderTarget = nullptr; }
}

bool CreateD2DDeviceResources() {
    if (g_d2dRenderTarget) return true;
    if (!g_swap || !g_d2dFactory) return false;

    IDXGISurface* dxgiSurface = nullptr;
    HRESULT hr = g_swap->GetBuffer(0, IID_PPV_ARGS(&dxgiSurface));
    if (FAILED(hr)) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f
    );
    hr = g_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurface, &props, &g_d2dRenderTarget);
    dxgiSurface->Release();
    if (FAILED(hr)) return false;

    g_d2dRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.047f, 0.086f, 0.141f, 0.94f), &g_brushPanelBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.165f, 0.490f, 1.000f, 0.50f), &g_brushPanelBorder);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.165f, 0.490f, 1.000f, 0.88f), &g_brushBtnActiveBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.086f, 0.133f, 0.204f, 0.82f), &g_brushBtnInactiveBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.125f, 0.204f, 0.306f, 0.92f), &g_brushBtnHoverBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.176f, 0.267f, 0.392f, 0.80f), &g_brushBtnBorder);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.431f, 0.706f, 1.000f, 0.95f), &g_brushBtnActiveBorder);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.941f, 0.961f, 1.000f, 1.00f), &g_brushTextWhite);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.569f, 0.659f, 0.765f, 1.00f), &g_brushTextMuted);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.431f, 0.686f, 1.000f, 0.95f), &g_brushTextSection);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.831f, 1.000f, 1.00f), &g_brushAccent);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.863f, 0.196f, 0.196f, 0.85f), &g_brushCloseHover);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.137f, 0.216f, 0.314f, 0.60f), &g_brushSeparator);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.831f, 1.000f, 0.75f), &g_brushPointerOuter);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.831f, 1.000f, 0.95f), &g_brushPointerInner);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.000f, 1.000f, 1.000f, 1.00f), &g_brushPointerActive);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.900f, 0.450f, 1.00f), &g_brushFeedActive);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.450f, 0.520f, 0.620f, 0.80f), &g_brushFeedStandby);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.180f, 0.750f, 0.380f, 1.00f), &g_brushPowerOn);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.880f, 0.220f, 0.220f, 1.00f), &g_brushPowerOff);

    return true;
}

bool InitD2D() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    if (FAILED(hr)) return false;

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &g_textFormatTitle);
    if (g_textFormatTitle) {
        g_textFormatTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFormatTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &g_textFormatSection);
    if (g_textFormatSection) {
        g_textFormatSection->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFormatSection->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &g_textFormatBtn);
    if (g_textFormatBtn) {
        g_textFormatBtn->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatBtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &g_textFormatPill);
    if (g_textFormatPill) {
        g_textFormatPill->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatPill->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 9.5f, L"en-us", &g_textFormatFooter);
    if (g_textFormatFooter) {
        g_textFormatFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatFooter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    return CreateD2DDeviceResources();
}

void DrawButton(const D2D1_RECT_F& rect, const wchar_t* text, bool active, bool hovered, float radius = 6.0f, bool disabled = false) {
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
    ID2D1SolidColorBrush* bgBrush = disabled ? g_brushBtnInactiveBg : (active ? g_brushBtnActiveBg : (hovered ? g_brushBtnHoverBg : g_brushBtnInactiveBg));
    ID2D1SolidColorBrush* borderBrush = disabled ? g_brushBtnBorder : (active ? g_brushBtnActiveBorder : (hovered ? g_brushAccent : g_brushBtnBorder));
    
    g_d2dRenderTarget->FillRoundedRectangle(&rounded, bgBrush);
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, borderBrush, (!disabled && active) ? 1.5f : 1.0f);
    
    ID2D1SolidColorBrush* textBrush = disabled ? g_brushTextMuted : (active ? g_brushTextWhite : (hovered ? g_brushTextWhite : g_brushTextMuted));
    g_d2dRenderTarget->DrawText(
        text, static_cast<UINT32>(wcslen(text)),
        g_textFormatBtn, &rect, textBrush);
}

void DrawPowerHeaderButton(const D2D1_RECT_F& rect, bool vrRunning, bool hovered) {
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 4.0f, 4.0f);
    if (hovered) {
        g_d2dRenderTarget->FillRoundedRectangle(&rounded, vrRunning ? g_brushCloseHover : g_brushBtnHoverBg);
    }
    ID2D1SolidColorBrush* iconBrush = vrRunning ? g_brushPowerOff : g_brushPowerOn;
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, iconBrush, 1.0f);
    
    const wchar_t* powerIcon = L"\x23FB"; // ⏻
    g_d2dRenderTarget->DrawText(
        powerIcon, 1,
        g_textFormatBtn, &rect, iconBrush);
}

void DrawPowerButton(const D2D1_RECT_F& rect, const wchar_t* text, bool vrRunning, bool hovered) {
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 6.0f, 6.0f);
    ID2D1SolidColorBrush* borderBrush = vrRunning ? g_brushPowerOff : g_brushPowerOn;
    ID2D1SolidColorBrush* bgBrush = hovered ? (vrRunning ? g_brushCloseHover : g_brushBtnHoverBg) : g_brushBtnInactiveBg;
    g_d2dRenderTarget->FillRoundedRectangle(&rounded, bgBrush);
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, borderBrush, 1.4f);
    g_d2dRenderTarget->DrawText(
        text, static_cast<UINT32>(wcslen(text)),
        g_textFormatBtn, &rect, g_brushTextWhite);
}

void DrawCloseButton(const D2D1_RECT_F& rect, bool hovered) {
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 4.0f, 4.0f);
    if (hovered) {
        g_d2dRenderTarget->FillRoundedRectangle(&rounded, g_brushCloseHover);
    }
    const float pad = 7.0f;
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(rect.left + pad, rect.top + pad),
        D2D1::Point2F(rect.right - pad, rect.bottom - pad),
        g_brushTextWhite, 1.8f);
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(rect.right - pad, rect.top + pad),
        D2D1::Point2F(rect.left + pad, rect.bottom - pad),
        g_brushTextWhite, 1.8f);
}

void DrawTriggerPill(float windowWidth) {
    const float pillWidth = 120.0f;
    const float pillHeight = 34.0f;
    g_pillRect = D2D1::RectF(windowWidth - pillWidth - 18.0f, 16.0f, windowWidth - 18.0f, 16.0f + pillHeight);
    
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(g_pillRect, 17.0f, 17.0f);
    ID2D1SolidColorBrush* bgBrush = g_pillHovered ? g_brushBtnHoverBg : g_brushPanelBg;
    ID2D1SolidColorBrush* borderBrush = g_pillHovered ? g_brushAccent : g_brushPanelBorder;
    
    g_d2dRenderTarget->FillRoundedRectangle(&rounded, bgBrush);
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, borderBrush, 1.2f);
    
    const D2D1_ELLIPSE dot = D2D1::Ellipse(
        D2D1::Point2F(g_pillRect.left + 22.0f, (g_pillRect.top + g_pillRect.bottom) * 0.5f),
        4.0f, 4.0f);
    g_d2dRenderTarget->FillEllipse(&dot, g_brushAccent);

    const wchar_t* pillText = L"REMOTE";
    D2D1_RECT_F textRect = D2D1::RectF(g_pillRect.left + 28.0f, g_pillRect.top, g_pillRect.right - 10.0f, g_pillRect.bottom);
    g_d2dRenderTarget->DrawText(
        pillText, static_cast<UINT32>(wcslen(pillText)),
        g_textFormatPill, &textRect, g_brushTextWhite);
}

void DrawPointerReticle(float x, float y, bool active) {
    const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(x, y), 8.0f, 8.0f);
    const D2D1_ELLIPSE inner = D2D1::Ellipse(D2D1::Point2F(x, y), active ? 4.0f : 2.5f, active ? 4.0f : 2.5f);
    
    g_d2dRenderTarget->DrawEllipse(&outer, g_brushPointerOuter, 1.2f);
    g_d2dRenderTarget->FillEllipse(&inner, active ? g_brushPointerActive : g_brushPointerInner);
}

void DrawRemotePanel(float windowWidth, float /*windowHeight*/) {
    const float panelWidth = 310.0f;
    const float panelHeight = 602.0f;
    const float left = windowWidth - panelWidth - 20.0f;
    const float top = 20.0f;
    const float right = left + panelWidth;
    const float bottom = top + panelHeight;
    g_remoteRect = D2D1::RectF(left, top, right, bottom);

    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(g_remoteRect, 10.0f, 10.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&rounded, g_brushPanelBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, g_brushPanelBorder, 1.5f);

    // Header separator line
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(left, top + 42.0f),
        D2D1::Point2F(right, top + 42.0f),
        g_brushSeparator, 1.0f);

    // Header indicator dot
    const D2D1_ELLIPSE headerDot = D2D1::Ellipse(
        D2D1::Point2F(left + 20.0f, top + 21.0f),
        4.0f, 4.0f);
    g_d2dRenderTarget->FillEllipse(&headerDot, g_brushAccent);

    // Header Title
    const wchar_t* titleText = L"REMOTE CONTROL";
    D2D1_RECT_F titleRect = D2D1::RectF(left + 32.0f, top, right - 74.0f, top + 42.0f);
    g_d2dRenderTarget->DrawText(
        titleText, static_cast<UINT32>(wcslen(titleText)),
        g_textFormatTitle, &titleRect, g_brushTextWhite);

    const bool vrRunning = IsVrRunning();

    // Header Power Button [ ⏻ ]
    g_powerHeaderRect = D2D1::RectF(right - 68.0f, top + 8.0f, right - 42.0f, top + 34.0f);
    DrawPowerHeaderButton(g_powerHeaderRect, vrRunning, g_powerHeaderHovered);

    // If mouse hovers over the header power button, display a tooltip badge
    if (g_powerHeaderHovered) {
        const wchar_t* tipText = vrRunning ? L"Turn off BigScreen" : L"Turn on BigScreen";
        D2D1_RECT_F tipRect = D2D1::RectF(right - 200.0f, top + 9.0f, right - 74.0f, top + 33.0f);
        D2D1_ROUNDED_RECT tipRounded = D2D1::RoundedRect(tipRect, 4.0f, 4.0f);
        g_d2dRenderTarget->FillRoundedRectangle(&tipRounded, vrRunning ? g_brushCloseHover : g_brushBtnHoverBg);
        g_d2dRenderTarget->DrawRoundedRectangle(&tipRounded, vrRunning ? g_brushPowerOff : g_brushPowerOn, 1.0f);
        g_d2dRenderTarget->DrawText(tipText, static_cast<UINT32>(wcslen(tipText)), g_textFormatFooter, &tipRect, g_brushTextWhite);
    }

    // Close button [✕]
    g_closeRect = D2D1::RectF(right - 36.0f, top + 8.0f, right - 10.0f, top + 34.0f);
    DrawCloseButton(g_closeRect, g_closeHovered);

    // SECTION 1: VIEW & FRAMING
    const wchar_t* s1Text = L"VIEW & THEATER FRAMING";
    D2D1_RECT_F s1Rect = D2D1::RectF(left + 18.0f, top + 48.0f, right - 18.0f, top + 64.0f);
    g_d2dRenderTarget->DrawText(
        s1Text, static_cast<UINT32>(wcslen(s1Text)),
        g_textFormatSection, &s1Rect, g_brushTextSection);

    g_btnWholeTheaterRect = D2D1::RectF(left + 18.0f, top + 66.0f, left + 148.0f, top + 98.0f);
    g_btnMovieOnlyRect = D2D1::RectF(left + 156.0f, top + 66.0f, right - 18.0f, top + 98.0f);
    DrawButton(g_btnWholeTheaterRect, L"Theater Mode", !g_movieOnly, g_btnWholeHovered);
    DrawButton(g_btnMovieOnlyRect, L"Movie Only", g_movieOnly, g_btnMovieHovered);

    g_btnStereo3dRect = D2D1::RectF(left + 18.0f, top + 104.0f, right - 18.0f, top + 136.0f);
    const wchar_t* stereoLabel = L"3D Off (2D)  |  Movie Mode Only";
    bool stereoActive = false;
    if (g_movieOnly) {
        if (g_stereoMode == StereoSplitMode::SBS) {
            stereoLabel = L"3D Off: SBS (Left Half) \x2192 2D";
            stereoActive = true;
        } else if (g_stereoMode == StereoSplitMode::OU) {
            stereoLabel = L"3D Off: Over/Under (Top) \x2192 2D";
            stereoActive = true;
        } else {
            stereoLabel = L"3D Off: Normal 2D";
        }
    }
    DrawButton(g_btnStereo3dRect, stereoLabel, stereoActive, g_btnStereo3dHovered, 6.0f, !g_movieOnly);

    g_btnAspectRect = D2D1::RectF(left + 18.0f, top + 142.0f, right - 18.0f, top + 180.0f);
    const wchar_t* aspectLabel = (g_aspectPresetIndex < _countof(kAspectPresets))
        ? kAspectPresets[g_aspectPresetIndex].label
        : L"Aspect: Auto";
    DrawButton(g_btnAspectRect, aspectLabel, g_aspectPresetIndex > 0, g_btnAspectHovered, 6.0f, false);

    // SECTION 2: PROJECTION & PRIVACY
    const wchar_t* s2Text = L"PROJECTION & PRIVACY";
    D2D1_RECT_F s2Rect = D2D1::RectF(left + 18.0f, top + 190.0f, right - 18.0f, top + 206.0f);
    g_d2dRenderTarget->DrawText(
        s2Text, static_cast<UINT32>(wcslen(s2Text)),
        g_textFormatSection, &s2Rect, g_brushTextSection);

    g_btnProjectScreenRect = D2D1::RectF(left + 18.0f, top + 208.0f, left + 148.0f, top + 240.0f);
    g_btnProjectAppRect = D2D1::RectF(left + 156.0f, top + 208.0f, right - 18.0f, top + 240.0f);
    const bool isScreenProj = (g_projectionMode == ProjectionMode::Screen);
    const bool isAppProj = (g_projectionMode == ProjectionMode::App);
    const bool canProject = CanProjectInCurrentRoom();

    if (isAppProj) {
        const wchar_t* winToggleText = g_appIsHidden ? L"Show App Window" : L"Hide App Window";
        DrawButton(g_btnProjectScreenRect, winToggleText, false, g_btnProjectScreenHovered);
        DrawButton(g_btnProjectAppRect, L"Stop Projecting", true, g_btnProjectAppHovered);
    } else if (isScreenProj) {
        DrawButton(g_btnProjectScreenRect, L"Stop Projecting", true, g_btnProjectScreenHovered);
        DrawButton(g_btnProjectAppRect, L"Project Video App", false, false, 6.0f, true);
    } else {
        DrawButton(g_btnProjectScreenRect, L"Project Screen", false, canProject && g_btnProjectScreenHovered, 6.0f, !canProject);
        DrawButton(g_btnProjectAppRect, L"Project Video App", false, canProject && g_btnProjectAppHovered, 6.0f, !canProject);
    }

    // SECTION 3: WINDOW & DISPLAY
    const wchar_t* s3Text = L"WINDOW & DISPLAY";
    D2D1_RECT_F s3Rect = D2D1::RectF(left + 18.0f, top + 250.0f, right - 18.0f, top + 266.0f);
    g_d2dRenderTarget->DrawText(
        s3Text, static_cast<UINT32>(wcslen(s3Text)),
        g_textFormatSection, &s3Rect, g_brushTextSection);

    g_btnFullscreenRect = D2D1::RectF(left + 18.0f, top + 268.0f, right - 18.0f, top + 300.0f);
    DrawButton(g_btnFullscreenRect, g_isFullscreen ? L"Exit Fullscreen (Windowed)" : L"Toggle Fullscreen", g_isFullscreen, g_btnFsHovered);

    // SECTION 4: QUICK ACTIONS
    const wchar_t* s4Text = L"QUICK ACTIONS";
    D2D1_RECT_F s4Rect = D2D1::RectF(left + 18.0f, top + 310.0f, right - 18.0f, top + 326.0f);
    g_d2dRenderTarget->DrawText(
        s4Text, static_cast<UINT32>(wcslen(s4Text)),
        g_textFormatSection, &s4Rect, g_brushTextSection);

    g_btnRecenterRect = D2D1::RectF(left + 18.0f, top + 328.0f, left + 148.0f, top + 360.0f);
    g_btnMenuRect = D2D1::RectF(left + 156.0f, top + 328.0f, right - 18.0f, top + 360.0f);
    DrawButton(g_btnRecenterRect, L"Recenter", false, g_btnRecenterHovered);
    DrawButton(g_btnMenuRect, L"BigScreen Menu", false, g_btnMenuHovered);

    g_btnSystemRect = D2D1::RectF(left + 18.0f, top + 366.0f, right - 18.0f, top + 396.0f);
    DrawButton(g_btnSystemRect, L"SteamVR Dashboard", false, g_btnSystemHovered);

    // SECTION 5: VR SYSTEM POWER
    const wchar_t* s5Text = L"VR SYSTEM POWER";
    D2D1_RECT_F s5Rect = D2D1::RectF(left + 18.0f, top + 406.0f, right - 18.0f, top + 422.0f);
    g_d2dRenderTarget->DrawText(
        s5Text, static_cast<UINT32>(wcslen(s5Text)),
        g_textFormatSection, &s5Rect, g_brushTextSection);

    g_btnPowerRect = D2D1::RectF(left + 18.0f, top + 424.0f, right - 18.0f, top + 456.0f);
    const wchar_t* pwrBtnText = nullptr;
    if (g_btnPowerHovered) {
        pwrBtnText = vrRunning ? L"\x23FB Turn off BigScreen" : L"\x23FB Turn on BigScreen";
    } else {
        pwrBtnText = vrRunning ? L"\x23FB BigScreen Running" : L"\x23FB BigScreen Off";
    }
    DrawPowerButton(g_btnPowerRect, pwrBtnText, vrRunning, g_btnPowerHovered);

    // SECTION 6: MEDIA FEED STATUS
    const wchar_t* s6Text = L"MEDIA FEED STATUS";
    D2D1_RECT_F s6Rect = D2D1::RectF(left + 18.0f, top + 466.0f, right - 18.0f, top + 482.0f);
    g_d2dRenderTarget->DrawText(
        s6Text, static_cast<UINT32>(wcslen(s6Text)),
        g_textFormatSection, &s6Rect, g_brushTextSection);

    D2D1_RECT_F feedBoxRect = D2D1::RectF(left + 18.0f, top + 484.0f, right - 18.0f, top + 514.0f);
    D2D1_ROUNDED_RECT feedBox = D2D1::RoundedRect(feedBoxRect, 6.0f, 6.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&feedBox, g_brushBtnInactiveBg);
    
    bool isIndicatorActive = (g_projectionMode != ProjectionMode::None) || g_feedActive;
    g_d2dRenderTarget->DrawRoundedRectangle(&feedBox, isIndicatorActive ? g_brushFeedActive : g_brushBtnBorder, 1.0f);

    const D2D1_ELLIPSE feedDot = D2D1::Ellipse(
        D2D1::Point2F(feedBoxRect.left + 14.0f, (feedBoxRect.top + feedBoxRect.bottom) * 0.5f),
        4.0f, 4.0f);
    g_d2dRenderTarget->FillEllipse(&feedDot, isIndicatorActive ? g_brushFeedActive : g_brushFeedStandby);

    wchar_t feedText[80]{};
    if (g_projectionMode == ProjectionMode::App && !g_projectedTitle.empty()) {
        std::wstring shortTitle = g_projectedTitle;
        if (shortTitle.length() > 20) shortTitle = shortTitle.substr(0, 17) + L"...";
        const wchar_t* hiddenState = g_appIsHidden ? L"(Cloaked)" : L"(Visible)";
        swprintf_s(feedText, L"PROJECTING  |  %ls %ls", shortTitle.c_str(), hiddenState);
    } else if (g_projectionMode == ProjectionMode::Screen) {
        swprintf_s(feedText, L"PROJECTING  |  Desktop Screen Active");
    } else if (g_peerStreamActive || (g_roomHookReady && !g_inOwnRoom)) {
        if (g_feedActive || g_peerStreamActive) {
            swprintf_s(feedText, L"OTHER ROOM STREAM  |  Own Room Only to Project");
        } else {
            swprintf_s(feedText, L"OTHER ROOM  |  Enter Own Room to Project");
        }
    } else {
        if (g_feedActive) {
            swprintf_s(feedText, L"ROOM FEED ACTIVE  |  Movie Playing in Theater");
        } else {
            swprintf_s(feedText, L"THEATER READY  |  Select Movie or Room");
        }
    }
    D2D1_RECT_F feedTextRect = D2D1::RectF(feedBoxRect.left + 24.0f, feedBoxRect.top, feedBoxRect.right - 8.0f, feedBoxRect.bottom);
    g_d2dRenderTarget->DrawText(
        feedText, static_cast<UINT32>(wcslen(feedText)),
        g_textFormatFooter, &feedTextRect, isIndicatorActive ? g_brushFeedActive : g_brushTextMuted);

    // Footer separator line
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(left + 18.0f, top + 524.0f),
        D2D1::Point2F(right - 18.0f, top + 524.0f),
        g_brushSeparator, 1.0f);

    // Footer Hotkey Hints
    const wchar_t* footerText = L"Mouse Move: Laser Pointer  |  Left-Click: Trigger\nRight-Click Drag: Look  |  Esc: SteamVR Dashboard\nR: Recenter  |  Alt+Drag or Header: Move Window";
    D2D1_RECT_F footerRect = D2D1::RectF(left + 8.0f, top + 528.0f, right - 8.0f, bottom - 6.0f);
    g_d2dRenderTarget->DrawText(
        footerText, static_cast<UINT32>(wcslen(footerText)),
        g_textFormatFooter, &footerRect, g_brushTextMuted);
}

void ReleaseAll() {
    ReleaseProjectedAppResources();
    ReleaseSharedDesktopTexture();
    ReleaseSharedResources();
    ReleaseThirdPersonResources();
    ReleasePeerMovieResources();
    ReleaseD2DDeviceResources();

    if (g_textFormatFooter) g_textFormatFooter->Release();
    if (g_textFormatPill) g_textFormatPill->Release();
    if (g_textFormatBtn) g_textFormatBtn->Release();
    if (g_textFormatSection) g_textFormatSection->Release();
    if (g_textFormatTitle) g_textFormatTitle->Release();
    if (g_dwriteFactory) g_dwriteFactory->Release();
    if (g_d2dFactory) g_d2dFactory->Release();
    g_textFormatFooter = nullptr;
    g_textFormatPill = nullptr;
    g_textFormatBtn = nullptr;
    g_textFormatSection = nullptr;
    g_textFormatTitle = nullptr;
    g_dwriteFactory = nullptr;
    g_d2dFactory = nullptr;

    if (g_controlState) UnmapViewOfFile(g_controlState);
    if (g_controlMapping) CloseHandle(g_controlMapping);
    g_controlState = nullptr;
    g_controlMapping = nullptr;

    if (g_projectedVideoState) UnmapViewOfFile(g_projectedVideoState);
    if (g_projectedVideoMapping) CloseHandle(g_projectedVideoMapping);
    g_projectedVideoState = nullptr;
    g_projectedVideoMapping = nullptr;

    if (g_peerState) UnmapViewOfFile(g_peerState);
    if (g_peerMapping) CloseHandle(g_peerMapping);
    g_peerState = nullptr;
    g_peerMapping = nullptr;

    if (g_state) UnmapViewOfFile(g_state);
    if (g_mapping) CloseHandle(g_mapping);
    if (g_rtv) g_rtv->Release();
    if (g_sampler) g_sampler->Release();
    if (g_cbTheater) g_cbTheater->Release();
    if (g_psTheaterComposite) g_psTheaterComposite->Release();
    if (g_psTheaterCompositeFlip) g_psTheaterCompositeFlip->Release();
    if (g_ps) g_ps->Release();
    if (g_psFlip) g_psFlip->Release();
    if (g_psMovie) g_psMovie->Release();
    if (g_psMovieFlip) g_psMovieFlip->Release();
    if (g_psSbs) g_psSbs->Release();
    if (g_psSbsFlip) g_psSbsFlip->Release();
    if (g_psOu) g_psOu->Release();
    if (g_psOuFlip) g_psOuFlip->Release();
    if (g_vs) g_vs->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    if (g_adapter) g_adapter->Release();
    g_state = nullptr;
    g_mapping = nullptr;
    g_rtv = nullptr;
    g_sampler = nullptr;
    g_cbTheater = nullptr;
    g_psTheaterComposite = nullptr;
    g_psTheaterCompositeFlip = nullptr;
    g_ps = nullptr;
    g_psFlip = nullptr;
    g_psMovie = nullptr;
    g_psMovieFlip = nullptr;
    g_psSbs = nullptr;
    g_psSbsFlip = nullptr;
    g_psOu = nullptr;
    g_psOuFlip = nullptr;
    g_vs = nullptr;
    g_swap = nullptr;
    g_context = nullptr;
    g_device = nullptr;
    g_adapter = nullptr;
}

bool OpenThirdPersonMapping() {
    if (g_thirdState) return true;
    g_thirdMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_third_person_gpu::kMappingName);
    if (!g_thirdMapping) return false;
    g_thirdState = static_cast<bigscreen_third_person_gpu::State*>(MapViewOfFile(g_thirdMapping, FILE_MAP_READ, 0, 0, sizeof(*g_thirdState)));
    if (!g_thirdState) { CloseHandle(g_thirdMapping); g_thirdMapping = nullptr; return false; }
    Log("Third-person GPU metadata mapping opened");
    return true;
}

void ReleasePeerMovieResources() {
    if (g_peerSrv) { g_peerSrv->Release(); g_peerSrv = nullptr; }
    if (g_peerLocal) { g_peerLocal->Release(); g_peerLocal = nullptr; }
    if (g_peerMutex) { g_peerMutex->Release(); g_peerMutex = nullptr; }
    if (g_peerShared) { g_peerShared->Release(); g_peerShared = nullptr; }
    g_peerGeneration = 0;
    g_peerHandle = 0;
    g_peerSequence = 0;
    g_peerWidth = 0;
    g_peerHeight = 0;
}

bool OpenPeerMovieMapping() {
    if (g_peerState) return true;
    g_peerMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_peer_movie::kMappingName);
    if (!g_peerMapping) return false;
    g_peerState = static_cast<bigscreen_peer_movie::State*>(
        MapViewOfFile(g_peerMapping, FILE_MAP_READ, 0, 0, sizeof(*g_peerState)));
    if (!g_peerState) { CloseHandle(g_peerMapping); g_peerMapping = nullptr; return false; }
    Log("Isolated peer movie GPU metadata mapping opened");
    return true;
}

bool OpenPeerMovieShared(const bigscreen_peer_movie::State& state) {
    ReleasePeerMovieResources();
    if (!g_device || !state.sharedHandle) return false;
    HRESULT hr = g_device->OpenSharedResource(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state.sharedHandle)),
        IID_PPV_ARGS(&g_peerShared));
    if (FAILED(hr) || !g_peerShared) return false;
    if (FAILED(g_peerShared->QueryInterface(IID_PPV_ARGS(&g_peerMutex)))) {
        ReleasePeerMovieResources();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    g_peerShared->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC localDesc = desc;
    localDesc.MiscFlags = 0;
    localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    localDesc.CPUAccessFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&localDesc, nullptr, &g_peerLocal)) ||
        FAILED(g_device->CreateShaderResourceView(g_peerLocal, nullptr, &g_peerSrv))) {
        ReleasePeerMovieResources();
        return false;
    }
    g_peerGeneration = state.generation;
    g_peerHandle = state.sharedHandle;
    g_peerWidth = desc.Width;
    g_peerHeight = desc.Height;
    Log("Isolated peer movie shared texture opened (%ux%u, source=%u)", desc.Width, desc.Height, state.sourceKind);
    return true;
}

bool CopyPeerMovieFrame() {
    if (!OpenPeerMovieMapping() || !g_peerState) return false;
    const auto snapshot = *g_peerState;
    if (snapshot.magic != bigscreen_peer_movie::kMagic ||
        snapshot.version != bigscreen_peer_movie::kVersion ||
        snapshot.active == 0 ||
        snapshot.sharedHandle == 0 ||
        snapshot.frameSequence == 0 ||
        snapshot.width < 64 ||
        snapshot.height < 64) {
        if (g_peerSrv) {
            ReleasePeerMovieResources();
            g_stereoMode = StereoSplitMode::Normal2D;
        }
        return false;
    }
    const ULONGLONG nowUs = GetTickCount64() * 1000ull;
    if (nowUs > snapshot.lastFrameMicros && (nowUs - snapshot.lastFrameMicros) > 4000000ull) {
        return false;
    }
    if (!g_peerLocal || g_peerGeneration != snapshot.generation || g_peerHandle != snapshot.sharedHandle ||
        g_peerWidth != snapshot.width || g_peerHeight != snapshot.height) {
        if (!OpenPeerMovieShared(snapshot)) return false;
    }
    if (snapshot.frameSequence != g_peerSequence) {
        if (g_peerMutex && SUCCEEDED(g_peerMutex->AcquireSync(1, 0))) {
            g_context->CopyResource(g_peerLocal, g_peerShared);
            g_context->Flush();
            g_peerMutex->ReleaseSync(0);
            g_peerSequence = snapshot.frameSequence;
            g_peerLastActive = GetTickCount64();
        }
    } else {
        g_peerLastActive = GetTickCount64();
    }
    return g_peerSrv != nullptr;
}

bool OpenThirdPersonShared(const bigscreen_third_person_gpu::State& state) {
    if (g_thirdSrv) g_thirdSrv->Release();
    if (g_thirdLocal) g_thirdLocal->Release();
    if (g_thirdMutex) g_thirdMutex->Release();
    if (g_thirdShared) g_thirdShared->Release();
    g_thirdSrv = nullptr; g_thirdLocal = nullptr; g_thirdMutex = nullptr; g_thirdShared = nullptr;
    HRESULT hr = g_device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state.sharedHandle)), IID_PPV_ARGS(&g_thirdShared));
    if (FAILED(hr)) return false;
    if (FAILED(g_thirdShared->QueryInterface(IID_PPV_ARGS(&g_thirdMutex)))) { ReleaseThirdPersonResources(); return false; }
    D3D11_TEXTURE2D_DESC desc{}; g_thirdShared->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC localDesc = desc; localDesc.MiscFlags = 0; localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; localDesc.CPUAccessFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&localDesc, nullptr, &g_thirdLocal)) || FAILED(g_device->CreateShaderResourceView(g_thirdLocal, nullptr, &g_thirdSrv))) { ReleaseThirdPersonResources(); return false; }
    g_thirdGeneration = state.generation; g_thirdHandle = state.sharedHandle; g_sourceWidth = desc.Width; g_sourceHeight = desc.Height;
    Log("Third-person shared texture opened");
    return true;
}

bool CopyThirdPersonFrame() {
    if (!OpenThirdPersonMapping() || !g_thirdState) return false;
    const auto snapshot = *g_thirdState;
    if (snapshot.magic != bigscreen_third_person_gpu::kMagic || snapshot.version != bigscreen_third_person_gpu::kVersion || snapshot.valid == 0 || snapshot.sharedHandle == 0 || snapshot.frameSequence == 0) return false;
    if (!g_thirdLocal || g_thirdGeneration != snapshot.generation || g_thirdHandle != snapshot.sharedHandle) if (!OpenThirdPersonShared(snapshot)) return false;
    if (snapshot.frameSequence == static_cast<LONG64>(g_thirdSequence)) return true;
    if (!g_thirdMutex || FAILED(g_thirdMutex->AcquireSync(1, 0))) return false;
    g_context->CopyResource(g_thirdLocal, g_thirdShared); g_context->Flush(); g_thirdMutex->ReleaseSync(0);
    g_thirdSequence = static_cast<uint64_t>(snapshot.frameSequence); g_thirdLastFrame = GetTickCount64(); return true;
}

void SelectViewSource(ViewSource source) {
    g_viewSource = source;
    if (source == ViewSource::ThirdPerson) g_thirdUnavailable = false;
    if (source == ViewSource::FirstPerson) ReleaseThirdPersonResources();
    SyncViewRequest(source == ViewSource::ThirdPerson
                        ? bigscreen_controller_control_ipc::ViewRequest::Third
                        : bigscreen_controller_control_ipc::ViewRequest::First);
}

bool OpenLiveMapping() {
    if (g_state) return true;
    g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_live_frame_ipc::kMappingName);
    if (!g_mapping) return false;
    g_state = static_cast<bigscreen_live_frame_ipc::LiveFrameState*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ, 0, 0, sizeof(bigscreen_live_frame_ipc::LiveFrameState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    Log("Live frame metadata mapping opened");
    return true;
}

bool SelectGraphicsAdapter() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    IDXGIAdapter1* fallback = nullptr;
    DXGI_ADAPTER_DESC1 fallbackDesc{};
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            if (desc.VendorId == 0x10DE) {
                g_adapter = candidate;
                Log("Viewer adapter (NVIDIA): %ls", desc.Description);
                break;
            } else if (!fallback) {
                fallback = candidate;
                fallbackDesc = desc;
                continue;
            }
        }
        if (candidate) candidate->Release();
    }
    if (!g_adapter && fallback) {
        g_adapter = fallback;
        Log("Viewer adapter (Hardware): %ls", fallbackDesc.Description);
        fallback = nullptr;
    } else if (fallback) {
        fallback->Release();
    }
    factory->Release();
    return g_adapter != nullptr;
}

bool InitD3D() {
    if (!SelectGraphicsAdapter()) return false;
    RECT rect{};
    GetClientRect(g_hwnd, &rect);
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = g_hwnd;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapDesc.BufferDesc.Width = static_cast<UINT>(std::max<LONG>(1, rect.right));
    swapDesc.BufferDesc.Height = static_cast<UINT>(std::max<LONG>(1, rect.bottom));
    D3D_FEATURE_LEVEL level{};
    const UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        g_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, createDeviceFlags, nullptr, 0, D3D11_SDK_VERSION,
        &swapDesc, &g_swap, &g_device, &level, &g_context);
    if (FAILED(hr)) {
        Log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv))) {
        if (backBuffer) backBuffer->Release();
        return false;
    }
    backBuffer->Release();
    const char* vertexCode =
        "struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
        "O main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};"
        "float2 u[3]={float2(0,1),float2(0,-1),float2(2,1)};O o;o.p=float4(p[id],0,1);o.uv=u[id];return o;}";
    const char* pixelCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,uv);}";
    const char* pixelFlipCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,float2(uv.x,1.0-uv.y));}";
    const char* pixelMovieCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "float2 muv=float2(lerp(0.265,0.735,uv.x), lerp(0.365,0.630,uv.y));"
        "return t.Sample(s,muv);}";
    const char* pixelMovieFlipCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "float2 muv=float2(lerp(0.265,0.735,uv.x), lerp(0.365,0.630,1.0-uv.y));"
        "return t.Sample(s,muv);}";
    const char* pixelSbsCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "return t.Sample(s,float2(uv.x*0.5,uv.y));}";
    const char* pixelSbsFlipCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "return t.Sample(s,float2(uv.x*0.5,1.0-uv.y));}";
    const char* pixelOuCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "return t.Sample(s,float2(uv.x,uv.y*0.5));}";
    const char* pixelOuFlipCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "return t.Sample(s,float2(uv.x,(1.0-uv.y)*0.5));}";
    const char* theaterCompositeCode =
        "Texture2D tLocal:register(t0);Texture2D tProj:register(t1);SamplerState s:register(s0);"
        "cbuffer CbTheater:register(b0){float4 screenBounds;float4 videoBounds;};"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "if(uv.x>=videoBounds.x && uv.x<=videoBounds.z && uv.y>=videoBounds.y && uv.y<=videoBounds.w){"
        "float2 vuv=float2((uv.x-videoBounds.x)/(videoBounds.z-videoBounds.x),(uv.y-videoBounds.y)/(videoBounds.w-videoBounds.y));"
        "return tProj.Sample(s,vuv);}"
        "if(uv.x>=screenBounds.x && uv.x<=screenBounds.z && uv.y>=screenBounds.y && uv.y<=screenBounds.w){"
        "return float4(0.0,0.0,0.0,1.0);}"
        "return tLocal.Sample(s,uv);}";
    const char* theaterCompositeFlipCode =
        "Texture2D tLocal:register(t0);Texture2D tProj:register(t1);SamplerState s:register(s0);"
        "cbuffer CbTheater:register(b0){float4 screenBounds;float4 videoBounds;};"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
        "float2 luv=float2(uv.x,1.0-uv.y);"
        "if(luv.x>=videoBounds.x && luv.x<=videoBounds.z && luv.y>=videoBounds.y && luv.y<=videoBounds.w){"
        "float2 vuv=float2((luv.x-videoBounds.x)/(videoBounds.z-videoBounds.x),(luv.y-videoBounds.y)/(videoBounds.w-videoBounds.y));"
        "return tProj.Sample(s,vuv);}"
        "if(luv.x>=screenBounds.x && luv.x<=screenBounds.z && luv.y>=screenBounds.y && luv.y<=screenBounds.w){"
        "return float4(0.0,0.0,0.0,1.0);}"
        "return tLocal.Sample(s,luv);}";
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(vertexCode, std::strlen(vertexCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_vs);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelCode, std::strlen(pixelCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_ps);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelFlipCode, std::strlen(pixelFlipCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psFlip);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelMovieCode, std::strlen(pixelMovieCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psMovie);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelMovieFlipCode, std::strlen(pixelMovieFlipCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psMovieFlip);
    blob->Release();
    if (FAILED(hr)) return false;

    auto compilePs = [&](const char* code, ID3D11PixelShader** outPs) -> bool {
        ID3DBlob* b = nullptr;
        ID3DBlob* e = nullptr;
        if (FAILED(D3DCompile(code, std::strlen(code), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e))) {
            if (e) e->Release();
            return false;
        }
        HRESULT cr = g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, outPs);
        b->Release();
        return SUCCEEDED(cr);
    };
    if (!compilePs(pixelSbsCode, &g_psSbs) ||
        !compilePs(pixelSbsFlipCode, &g_psSbsFlip) ||
        !compilePs(pixelOuCode, &g_psOu) ||
        !compilePs(pixelOuFlipCode, &g_psOuFlip)) {
        return false;
    }
    hr = D3DCompile(theaterCompositeCode, std::strlen(theaterCompositeCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psTheaterComposite);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(theaterCompositeFlipCode, std::strlen(theaterCompositeFlipCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psTheaterCompositeFlip);
    blob->Release();
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(CbTheaterLayout);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_device->CreateBuffer(&cbDesc, nullptr, &g_cbTheater))) return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(g_device->CreateSamplerState(&samplerDesc, &g_sampler));
}

void ResizeSwapChain(UINT width, UINT height) {
    if (!g_swap || width == 0 || height == 0) return;
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
    ReleaseD2DDeviceResources();
    g_swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
    CreateD2DDeviceResources();
}

bool OpenSharedFrame(const bigscreen_live_frame_ipc::LiveFrameState& state) {
    ReleaseSharedResources();
    HRESULT hr = g_device->OpenSharedResource(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state.sharedHandle)),
        IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) {
        Log("OpenSharedResource failed: 0x%08X", static_cast<unsigned>(hr));
        ReleaseSharedResources();
        return false;
    }
    if (FAILED(g_sharedTexture->QueryInterface(IID_PPV_ARGS(&g_sharedMutex)))) {
        Log("Shared output has no keyed mutex");
        ReleaseSharedResources();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    g_sharedTexture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC localDesc = desc;
    localDesc.Usage = D3D11_USAGE_DEFAULT;
    localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    localDesc.CPUAccessFlags = 0;
    localDesc.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&localDesc, nullptr, &g_localTexture)) ||
        FAILED(g_device->CreateShaderResourceView(g_localTexture, nullptr, &g_localSrv))) {
        Log("Viewer local texture creation failed");
        ReleaseSharedResources();
        return false;
    }
    g_generation = state.generation;
    g_handle = state.sharedHandle;
    g_sourceWidth = desc.Width;
    g_sourceHeight = desc.Height;
    ++g_opens;
    Log("Opened live texture generation=%llu size=%ux%u format=%u",
        static_cast<unsigned long long>(g_generation), g_sourceWidth, g_sourceHeight, desc.Format);
    return true;
}

bool CopyNewFrame() {
    if (!g_state) return false;
    const auto snapshot = *g_state;
    if (snapshot.magic != bigscreen_live_frame_ipc::kMagic || snapshot.version != bigscreen_live_frame_ipc::kVersion ||
        snapshot.valid == 0 || snapshot.sharedHandle == 0 || snapshot.frameSequence == 0) return false;
    if (!g_localTexture || g_generation != snapshot.generation || g_handle != snapshot.sharedHandle) {
        if (!OpenSharedFrame(snapshot)) return false;
    }
    if (snapshot.frameSequence == static_cast<LONG64>(g_lastSequence)) return false;
    if (!g_sharedMutex || FAILED(g_sharedMutex->AcquireSync(1, 0))) return false;
    g_context->CopyResource(g_localTexture, g_sharedTexture);
    g_context->Flush();
    g_sharedMutex->ReleaseSync(0);
    const uint64_t sequence = static_cast<uint64_t>(snapshot.frameSequence);
    if (g_lastSequence != 0 && sequence > g_lastSequence + 1) g_dropped += sequence - g_lastSequence - 1;
    g_lastSequence = sequence;
    ++g_copies;
    return true;
}

void Render(ID3D11ShaderResourceView* source, bool flipVertical) {
    RECT rect{};
    GetClientRect(g_hwnd, &rect);
    const float windowWidth = static_cast<float>(std::max<LONG>(1, rect.right));
    const float windowHeight = static_cast<float>(std::max<LONG>(1, rect.bottom));
    const float windowAspect = windowWidth / windowHeight;

    ID3D11ShaderResourceView* activeSource = source;
    if (g_projectionMode == ProjectionMode::App) {
        if (g_movieOnly) {
            activeSource = g_projectedSrv;
        } else {
            activeSource = g_localSrv ? g_localSrv : g_projectedSrv;
        }
    }

    float sourceAspect = (g_sourceHeight ? static_cast<float>(g_sourceWidth) / g_sourceHeight : 16.0f / 9.0f);
    if (activeSource == g_projectedSrv && g_projectedWidth && g_projectedHeight) {
        sourceAspect = ComputeMovieDisplayAspect(g_projectedWidth, g_projectedHeight);
    } else if (activeSource == g_peerSrv && g_peerWidth && g_peerHeight) {
        sourceAspect = ComputeMovieDisplayAspect(g_peerWidth, g_peerHeight);
    } else if (g_movieOnly) {
        float ov = GetSelectedAspectOverride();
        if (ov > 0.0f) sourceAspect = ov;
    }

    D3D11_VIEWPORT viewport{};
    if (windowAspect > sourceAspect) {
        viewport.Height = windowHeight;
        viewport.Width = windowHeight * sourceAspect;
        viewport.TopLeftX = (windowWidth - viewport.Width) * 0.5f;
        viewport.TopLeftY = 0.0f;
    } else {
        viewport.Width = windowWidth;
        viewport.Height = windowWidth / sourceAspect;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = (windowHeight - viewport.Height) * 0.5f;
    }
    g_activeViewport = viewport;

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);

    if (activeSource) {
        g_context->RSSetViewports(1, &viewport);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vs, nullptr, 0);
        const bool isIsolatedVideoFeed = (activeSource == g_projectedSrv) || (activeSource == g_peerSrv);
        ID3D11PixelShader* selectedPs = nullptr;
        if (g_movieOnly && isIsolatedVideoFeed) {
            if (g_stereoMode == StereoSplitMode::SBS) {
                selectedPs = flipVertical ? g_psSbsFlip : g_psSbs;
            } else if (g_stereoMode == StereoSplitMode::OU) {
                selectedPs = flipVertical ? g_psOuFlip : g_psOu;
            } else {
                selectedPs = flipVertical ? g_psFlip : g_ps;
            }
        } else if (g_movieOnly) {
            selectedPs = flipVertical ? g_psMovieFlip : g_psMovie;
        } else {
            selectedPs = flipVertical ? g_psFlip : g_ps;
        }
        g_context->PSSetShader(selectedPs, nullptr, 0);
        g_context->PSSetSamplers(0, 1, &g_sampler);
        g_context->PSSetShaderResources(0, 1, &activeSource);
        g_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);
    }

    // Direct2D Overlay & Pointer Rendering
    if (g_d2dRenderTarget || CreateD2DDeviceResources()) {
        g_d2dRenderTarget->BeginDraw();
        
        // Draw Laser Pointer Reticle over VR client view when active
        if (g_pointerActive && (!g_showRemote || g_cursorX < g_remoteRect.left || g_cursorY > g_remoteRect.bottom)) {
            DrawPointerReticle(g_cursorX, g_cursorY, g_pointerTrigger);
        }

        if (g_showRemote) {
            DrawRemotePanel(windowWidth, windowHeight);
        } else {
            DrawTriggerPill(windowWidth);
        }
        g_d2dRenderTarget->EndDraw();
    }

    g_swap->Present(1, 0);
    ++g_frames;
}

void UpdateMetrics() {
    const ULONGLONG now = GetTickCount64();
    if (!g_lastMetrics) g_lastMetrics = now;
    if (now - g_lastMetrics < 1000) return;
    const double elapsed = static_cast<double>(now - g_started) / 1000.0;
    const double fps = elapsed > 0.0 ? static_cast<double>(g_frames) / elapsed : 0.0;
    Log("Viewer metrics: fps=%.1f sourceSeq=%llu copies=%llu dropped=%llu opens=%llu cpuCopy=GPU-only",
        fps, static_cast<unsigned long long>(g_lastSequence), static_cast<unsigned long long>(g_copies),
        static_cast<unsigned long long>(g_dropped), static_cast<unsigned long long>(g_opens));
    g_lastMetrics = now;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN) {
        if (wParam == VK_TAB || wParam == 'M' || wParam == VK_F1) {
            g_showRemote = !g_showRemote;
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        if (wParam == VK_LEFT) {
            g_lookYaw -= 0.05f;
            SyncPointerAndLook();
            return 0;
        }
        if (wParam == VK_RIGHT) {
            g_lookYaw += 0.05f;
            SyncPointerAndLook();
            return 0;
        }
        if (wParam == VK_UP) {
            g_lookPitch += 0.04f;
            SyncPointerAndLook();
            return 0;
        }
        if (wParam == VK_DOWN) {
            g_lookPitch -= 0.04f;
            SyncPointerAndLook();
            return 0;
        }
        if (wParam == 'R' || wParam == 'Y') {
            ResetLook();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            EnsureControlMapping();
            if (g_controlState) {
                bigscreen_controller_control_ipc::PulseSystemClick(g_controlState);
            }
            return 0;
        }
    }

    if (message == WM_KEYUP) {
        if (wParam == VK_ESCAPE) {
            EnsureControlMapping();
            if (g_controlState) {
                bigscreen_controller_control_ipc::ClearSystemClick(g_controlState);
            }
            return 0;
        }
    }

    if (message == WM_SIZE) {
        if (wParam == SIZE_MINIMIZED) {
            if (g_keepForegroundUntilMinimized) {
                g_keepForegroundUntilMinimized = false;
                if (!g_alwaysOnTop) {
                    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                Log("User minimized viewer: cleared persistent foreground lock.");
            }
        }
        ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }

    if (message == WM_MOUSEMOVE) {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const float winW = static_cast<float>(std::max<LONG>(1, clientRect.right));
        const float winH = static_cast<float>(std::max<LONG>(1, clientRect.bottom));

        if (g_showRemote && inRect(mx, my, g_remoteRect)) {
            g_pointerActive = false;
            g_closeHovered = inRect(mx, my, g_closeRect);
            g_powerHeaderHovered = inRect(mx, my, g_powerHeaderRect);
            g_btnWholeHovered = inRect(mx, my, g_btnWholeTheaterRect);
            g_btnMovieHovered = inRect(mx, my, g_btnMovieOnlyRect);
            g_btnStereo3dHovered = inRect(mx, my, g_btnStereo3dRect);
            g_btnAspectHovered = inRect(mx, my, g_btnAspectRect);
            g_btnProjectScreenHovered = inRect(mx, my, g_btnProjectScreenRect);
            g_btnProjectAppHovered = inRect(mx, my, g_btnProjectAppRect);
            g_btnFsHovered = inRect(mx, my, g_btnFullscreenRect);
            g_btnRecenterHovered = inRect(mx, my, g_btnRecenterRect);
            g_btnMenuHovered = inRect(mx, my, g_btnMenuRect);
            g_btnSystemHovered = inRect(mx, my, g_btnSystemRect);
            g_btnPowerHovered = inRect(mx, my, g_btnPowerRect);
        } else if (!g_showRemote && inRect(mx, my, g_pillRect)) {
            g_pointerActive = false;
            g_pillHovered = true;
        } else {
            // Interactive VR Client Area: mouse controls pointer aim & look
            g_pillHovered = false;
            g_closeHovered = false;
            g_powerHeaderHovered = false;
            g_btnWholeHovered = false;
            g_btnMovieHovered = false;
            g_btnStereo3dHovered = false;
            g_btnAspectHovered = false;
            g_btnProjectScreenHovered = false;
            g_btnProjectAppHovered = false;
            g_btnFsHovered = false;
            g_btnRecenterHovered = false;
            g_btnMenuHovered = false;
            g_btnSystemHovered = false;
            g_btnPowerHovered = false;

            g_pointerActive = true;
            g_cursorX = mx;
            g_cursorY = my;

            if (g_movieOnly) {
                // Movie Only mode locks look squarely at the screen
                g_lookYaw = 0.0f;
                g_lookPitch = 0.0f;
            } else if (g_isRightDragging) {
                const float dx = mx - static_cast<float>(g_lastRightPt.x);
                const float dy = my - static_cast<float>(g_lastRightPt.y);
                g_lookYaw += dx * 0.003f;
                g_lookPitch -= dy * 0.003f;
                g_lastRightPt = { static_cast<LONG>(mx), static_cast<LONG>(my) };
            }

            // Map screen coordinates accurately to ray direction within active VR viewport
            const float vw = g_activeViewport.Width > 0.0f ? g_activeViewport.Width : winW;
            const float vh = g_activeViewport.Height > 0.0f ? g_activeViewport.Height : winH;
            const float vx0 = g_activeViewport.Width > 0.0f ? g_activeViewport.TopLeftX : 0.0f;
            const float vy0 = g_activeViewport.Height > 0.0f ? g_activeViewport.TopLeftY : 0.0f;

            // Normalized screen coordinates within active VR image frustum [-1.0, +1.0]
            const float u = std::clamp(((mx - vx0) / vw - 0.5f) * 2.0f, -1.0f, 1.0f);
            const float v = std::clamp((0.5f - (my - vy0) / vh) * 2.0f, -1.0f, 1.0f);

            // OpenVR driver uses GetProjectionRaw tangents = 1.3f horizontally and vertically
            // In Movie Only mode, the active screen covers 0.470 x 0.265 of the projection space
            const float fovScaleX = g_movieOnly ? (1.3f * 0.470f) : 1.3f;
            const float fovScaleY = g_movieOnly ? (1.3f * 0.265f) : 1.3f;
            g_pointerYaw = -std::atan2(u * fovScaleX, 1.0f);
            g_pointerPitch = std::atan2(v * fovScaleY * std::cos(g_pointerYaw), 1.0f);
        }

        SyncPointerAndLook();
        return 0;
    }

    if (message == WM_MOUSELEAVE) {
        g_pillHovered = false;
        g_closeHovered = false;
        g_powerHeaderHovered = false;
        g_btnWholeHovered = false;
        g_btnMovieHovered = false;
        g_btnStereo3dHovered = false;
        g_btnAspectHovered = false;
        g_btnProjectScreenHovered = false;
        g_btnProjectAppHovered = false;
        g_btnFsHovered = false;
        g_btnRecenterHovered = false;
        g_btnMenuHovered = false;
        g_btnSystemHovered = false;
        g_btnPowerHovered = false;
        g_pointerActive = false;
        g_pointerTrigger = false;
        SyncPointerAndLook();
        return 0;
    }

    if (message == WM_LBUTTONDOWN) {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));

        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        if (g_showRemote) {
            if (inRect(mx, my, g_closeRect)) {
                g_showRemote = false;
                return 0;
            }
            if (inRect(mx, my, g_powerHeaderRect) || inRect(mx, my, g_btnPowerRect)) {
                if (IsVrRunning()) {
                    TerminateVrProcesses();
                } else {
                    LaunchVrProcesses();
                }
                return 0;
            }
            if (inRect(mx, my, g_btnWholeTheaterRect)) {
                g_movieOnly = false;
                return 0;
            }
            if (inRect(mx, my, g_btnMovieOnlyRect)) {
                g_movieOnly = true;
                g_lookYaw = 0.0f;
                g_lookPitch = 0.0f;
                SyncPointerAndLook();
                if (g_projectionMode == ProjectionMode::App && g_projectedWidth && g_projectedHeight) {
                    AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(g_projectedWidth, g_projectedHeight));
                }
                return 0;
            }
            if (inRect(mx, my, g_btnStereo3dRect)) {
                if (g_movieOnly) {
                    if (g_stereoMode == StereoSplitMode::Normal2D) {
                        g_stereoMode = StereoSplitMode::SBS;
                    } else if (g_stereoMode == StereoSplitMode::SBS) {
                        g_stereoMode = StereoSplitMode::OU;
                    } else {
                        g_stereoMode = StereoSplitMode::Normal2D;
                    }
                    g_lastAdjustedAspect = 0.0f;
                    if (g_projectionMode == ProjectionMode::App && g_projectedWidth && g_projectedHeight) {
                        AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(g_projectedWidth, g_projectedHeight));
                    }
                }
                return 0;
            }
            if (inRect(mx, my, g_btnAspectRect)) {
                g_aspectPresetIndex = (g_aspectPresetIndex + 1) % _countof(kAspectPresets);
                g_lastAdjustedAspect = 0.0f;
                SyncAspectOverrideToIpc();
                if (g_movieOnly) {
                    uint32_t w = (g_projectionMode == ProjectionMode::App) ? g_projectedWidth : g_peerWidth;
                    uint32_t h = (g_projectionMode == ProjectionMode::App) ? g_projectedHeight : g_peerHeight;
                    if (w == 0 || h == 0) { w = g_sourceWidth; h = g_sourceHeight; }
                    AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(w, h));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (inRect(mx, my, g_btnProjectScreenRect)) {
                if (g_projectionMode == ProjectionMode::App) {
                    ToggleAppWindowVisibility();
                } else if (g_projectionMode == ProjectionMode::Screen) {
                    g_projectionMode = ProjectionMode::None;
                    ClearSharedDesktopToBlack();
                } else if (CanProjectInCurrentRoom()) {
                    g_projectionMode = ProjectionMode::Screen;
                    PublishProjectedVideoState(false, false, 0, 0, 0, 0, 0);
                    ToggleDesktopMenu();
                }
                return 0;
            }
            if (inRect(mx, my, g_btnProjectAppRect)) {
                if (g_projectionMode == ProjectionMode::App) {
                    ReleaseProjectedAppResources();
                    g_projectionMode = ProjectionMode::None;
                } else if (g_projectionMode == ProjectionMode::None && CanProjectInCurrentRoom()) {
                    ShowProjectAppMenu(hwnd, g_btnProjectAppRect.left, g_btnProjectAppRect.bottom);
                }
                return 0;
            }
            if (inRect(mx, my, g_btnFullscreenRect)) {
                ToggleFullscreen(hwnd);
                return 0;
            }
            if (inRect(mx, my, g_btnRecenterRect)) {
                ResetLook();
                return 0;
            }
            if (inRect(mx, my, g_btnMenuRect)) {
                ToggleDesktopMenu();
                return 0;
            }
            if (inRect(mx, my, g_btnSystemRect)) {
                g_btnSystemDown = true;
                EnsureControlMapping();
                if (g_controlState) {
                    bigscreen_controller_control_ipc::PulseSystemClick(g_controlState);
                }
                return 0;
            }
        } else {
            if (inRect(mx, my, g_pillRect)) {
                g_showRemote = true;
                return 0;
            }
        }

        // Click on VR screen: Pull controller trigger for laser pointer selection!
        g_pointerTrigger = true;
        SyncPointerAndLook();
        return 0;
    }

    if (message == WM_LBUTTONUP) {
        if (g_btnSystemDown) {
            g_btnSystemDown = false;
            EnsureControlMapping();
            if (g_controlState) {
                bigscreen_controller_control_ipc::ClearSystemClick(g_controlState);
            }
        }
        if (g_pointerTrigger) {
            g_pointerTrigger = false;
            SyncPointerAndLook();
        }
        return 0;
    }

    if (message == WM_RBUTTONDOWN) {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));
        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        if (!g_showRemote || !inRect(mx, my, g_remoteRect)) {
            g_isRightDragging = true;
            g_lastRightPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;
    }

    if (message == WM_RBUTTONUP) {
        if (g_isRightDragging) {
            g_isRightDragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    if (message == WM_LBUTTONDBLCLK) {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));

        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        if (!g_showRemote || !inRect(mx, my, g_remoteRect)) {
            ToggleFullscreen(hwnd);
            return 0;
        }
    }

    if (message == WM_NCHITTEST) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        const float cx = static_cast<float>(pt.x);
        const float cy = static_cast<float>(pt.y);

        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        // Holding Alt lets user drag the window from anywhere
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            return HTCAPTION;
        }

        // Remote panel interaction: dragging the header bar moves the window
        if (g_showRemote && inRect(cx, cy, g_remoteRect)) {
            if (cy <= g_remoteRect.top + 42.0f && !inRect(cx, cy, g_closeRect) && !inRect(cx, cy, g_powerHeaderRect)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        // Floating trigger pill interaction
        if (!g_showRemote && inRect(cx, cy, g_pillRect)) {
            return HTCLIENT;
        }

        // Client area: Interactive VR display surface (laser pointer + trigger)
        return HTCLIENT;
    }

    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

} // namespace

void UpdateRoomOwnershipState() {
    static ULONGLONG s_lastProcPoll = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastProcPoll >= 400) {
        s_lastProcPoll = now;
        g_bigscreenRunning = IsProcessRunning(L"Bigscreen.exe");
    }

    OpenPeerMovieMapping();
    if (g_bigscreenRunning && g_peerState &&
        g_peerState->magic == bigscreen_peer_movie::kMagic) {
        g_peerStreamActive = (g_peerState->active != 0);
        if (g_peerState->roomHookReady != 0) {
            g_roomHookReady = true;
            g_inOwnRoom = (g_peerState->inOwnRoom != 0);
            g_isLoadingRoom = (g_peerState->isLoadingRoom != 0);
        } else {
            g_roomHookReady = false;
            g_inOwnRoom = false;
            g_isLoadingRoom = false;
        }
    } else {
        g_peerStreamActive = false;
        g_roomHookReady = false;
        g_inOwnRoom = false;
        g_isLoadingRoom = false;
    }

    // Projection is only valid in the user's own room; automatically stop if user leaves or disconnects
    if (g_projectionMode != ProjectionMode::None &&
        (!g_bigscreenRunning || !g_roomHookReady || (!g_inOwnRoom && !g_isLoadingRoom))) {
        if (g_projectionMode == ProjectionMode::App) {
            ReleaseProjectedAppResources();
        } else {
            ClearSharedDesktopToBlack();
        }
        g_projectionMode = ProjectionMode::None;
        Log("Stopped projection because user is no longer in their own room");
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WndProc;
    windowClass.lpszClassName = "BigscreenDesktopViewer";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.style = CS_DBLCLKS;
    RegisterClassA(&windowClass);
    g_hwnd = CreateWindowA(windowClass.lpszClassName, "Bigscreen Desktop Live Viewer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720,
                           nullptr, nullptr, instance, nullptr);
    if (!g_hwnd || !InitD3D() || !InitD2D()) {
        ReleaseAll();
        return 2;
    }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetWindowPos(g_hwnd, nullptr, 100, 100, 1280, 720,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_started = GetTickCount64();
    Log("BigscreenDesktopViewer GPU live core starting");
    MSG message{};
    while (message.message != WM_QUIT) {
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        EnsureBigscreenHookInjected();
        MaintainForegroundWhileLaunching();
        UpdateRoomOwnershipState();
        CheckFeedStatus();
        ID3D11ShaderResourceView* source = nullptr;
        bool flipVertical = false;

        if (g_projectionMode == ProjectionMode::App) {
            if (!g_projectedHwnd || !IsWindow(g_projectedHwnd)) {
                ReleaseProjectedAppResources();
                g_projectionMode = ProjectionMode::None;
                source = g_localSrv;
            } else {
                CaptureProjectedWindowFrame();
                source = g_movieOnly ? g_projectedSrv : (g_localSrv ? g_localSrv : g_projectedSrv);
                flipVertical = false;
                if (!g_movieOnly) {
                    OpenLiveMapping();
                    if (g_state) CopyNewFrame();
                }
            }
        } else {
            // Normal Bigscreen Room / Movie theater viewing (or Project Screen mode)
            OpenLiveMapping();
            if (g_state) CopyNewFrame();
            if (g_movieOnly && CopyPeerMovieFrame()) {
                source = g_peerSrv;
            } else {
                source = g_localSrv;
            }
            flipVertical = false;
        }

        char title[256]{};
        const char* mode = g_movieOnly ? " | Mode: MOVIE ONLY" : " | Mode: THEATER MODE";
        if (g_projectionMode == ProjectionMode::App && !g_projectedTitle.empty()) {
            std::wstring shortTitle = g_projectedTitle;
            if (shortTitle.length() > 28) shortTitle = shortTitle.substr(0, 25) + L"...";
            sprintf_s(title, "Bigscreen Desktop Live Viewer | PROJECTING APP: %ls%s | Tab/M=REMOTE", shortTitle.c_str(), mode);
        } else if (g_projectionMode == ProjectionMode::Screen) {
            sprintf_s(title, "Bigscreen Desktop Live Viewer | PROJECTING SCREEN%s | Tab/M=REMOTE", mode);
        } else if (g_feedActive) {
            sprintf_s(title, "Bigscreen Desktop Live Viewer | ROOM MOVIE ACTIVE%s | Tab/M=REMOTE", mode);
        } else {
            sprintf_s(title, "Bigscreen Desktop Live Viewer | BIGSCREEN THEATER%s | Tab/M=REMOTE", mode);
        }
        SetWindowTextA(g_hwnd, title);
        Render(source, flipVertical);
        UpdateMetrics();
    }
    ReleaseAll();
    return 0;
}
