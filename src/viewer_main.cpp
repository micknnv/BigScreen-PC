#include <Windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <shlobj.h>
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
void ReleaseProjectedAppResources();
void ClearSharedDesktopToBlack();
void UnloadVirtualScreen();
void StopAllProjectionAndUnloadVirtual();

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
uint64_t g_reqOwnRoomSeq = 0;
bool g_waitingOwnRoomForProject = false;

bool CanProjectInCurrentRoom() {
    return g_bigscreenRunning && g_roomHookReady && g_inOwnRoom && !g_isLoadingRoom && !g_peerStreamActive;
}

bool CanLeaveToOwnRoomToProject() {
    return g_bigscreenRunning && g_roomHookReady && !g_inOwnRoom && !g_isLoadingRoom;
}

// Mouse Pointer & Look State
bool g_pointerActive = false;
bool g_pointerTrigger = false;
float g_pointerYaw = 0.0f;
float g_pointerPitch = 0.0f;
float g_lookYaw = 0.0f;
float g_lookPitch = 0.0f;
float g_lookZoom = 0.0f;
float g_cursorX = 0.0f;
float g_cursorY = 0.0f;
bool g_isRightDragging = false;
POINT g_lastRightPt{};

// Remote Control State, Layout Hit Targets & LCD Communications
enum class RemoteBtn {
    None = 0,
    Hide,
    Power,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    DpadSelect,
    Menu,
    Settings,
    Fullscreen,
    ProjectScreen,
    Info,
    MovieMode,
    TheaterMode,
    VolUp,
    VolMute,
    VolDown,
    Microphone,
    MuteOthers,
    No3D,
    AspectRatio
};

D2D1_RECT_F g_remoteRect{};
D2D1_RECT_F g_pillRect{};
D2D1_RECT_F g_lcdRect{};
D2D1_RECT_F g_rBtnHide{};
D2D1_RECT_F g_rBtnPower{};
D2D1_POINT_2F g_dpadCenter{};
float g_dpadOuterR = 52.0f;
float g_dpadInnerR = 23.0f;
D2D1_RECT_F g_rBtnMenu{};
D2D1_RECT_F g_rBtnSettings{};
D2D1_RECT_F g_rBtnFullscreen{};
D2D1_RECT_F g_rBtnProject{};
D2D1_RECT_F g_rBtnInfo{};
D2D1_RECT_F g_rBtnMovie{};
D2D1_RECT_F g_rBtnTheater{};
D2D1_RECT_F g_rBtnVolPill{};
D2D1_RECT_F g_rBtnVolUp{};
D2D1_RECT_F g_rBtnVolMute{};
D2D1_RECT_F g_rBtnVolDown{};
D2D1_RECT_F g_rBtnMic{};
D2D1_RECT_F g_rBtnMuteOthers{};
D2D1_RECT_F g_rBtnNo3D{};
D2D1_RECT_F g_rBtnAspect{};

// Keep legacy alias rects for compatibility where referenced
D2D1_RECT_F g_btnProjectScreenRect{};
D2D1_RECT_F g_btnFullscreenRect{};

RemoteBtn g_hoveredRemoteBtn = RemoteBtn::None;
RemoteBtn g_heldDpadBtn = RemoteBtn::None;
ULONGLONG g_lastDpadRepeatMs = 0;
bool g_pillHovered = false;
bool g_btnSystemDown = false;
bool g_zoomMode = false;            // D-Pad mode: false = Panning (default), true = Zoom
bool g_infoPanelOpen = false;       // Astroslide-style LCD Info slide-out state
float g_infoSlideProgress = 0.0f;   // 0.0 (retracted) -> 1.0 (extended to left)
D2D1_RECT_F g_infoPanelRect{};

inline bool PointInRectF(float x, float y, const D2D1_RECT_F& r) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline bool IsOverInfoPanel(float x, float y) {
    return g_showRemote && g_infoSlideProgress > 0.05f && PointInRectF(x, y, g_infoPanelRect);
}

inline bool IsOverRemoteUi(float x, float y) {
    return g_showRemote && (PointInRectF(x, y, g_remoteRect) || IsOverInfoPanel(x, y));
}

bool g_remotePosInit = false;
float g_remotePosX = 0.0f;
float g_remotePosY = 0.0f;
bool g_isDraggingRemote = false;
POINT g_remoteDragStartPt{};
float g_remoteDragStartX = 0.0f;
float g_remoteDragStartY = 0.0f;

std::wstring g_lcdTransientLine1;
std::wstring g_lcdTransientLine2;
ULONGLONG g_lcdTransientUntilMs = 0;

void SetLcdNotification(const wchar_t* line1, const wchar_t* line2 = L"", ULONGLONG durationMs = 3000) {
    g_lcdTransientLine1 = line1 ? line1 : L"";
    g_lcdTransientLine2 = line2 ? line2 : L"";
    g_lcdTransientUntilMs = GetTickCount64() + durationMs;
}

// Bigscreen Microphone / Mute Others / App-Only Audio State
bool g_micMuted = false;            // Default ON (false -> glowing CYAN)
bool g_allMuted = false;            // Default OFF (false -> WHITE when 2+ users)
uint32_t g_remoteUserCount = 0;
bool g_audioHookReady = false;
bool g_bigscreenAudioMuted = false;
float g_bigscreenAudioVol = 1.0f;
bool g_bigscreenMenuToggled = false;
uint64_t g_reqMicToggleSeq = 0;
uint64_t g_reqMuteAllToggleSeq = 0;

// Direct2D & DirectWrite Resources
ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
ID2D1RenderTarget* g_d2dRenderTarget = nullptr;

IDWriteTextFormat* g_textFormatTitle = nullptr;
IDWriteTextFormat* g_textFormatSection = nullptr;
IDWriteTextFormat* g_textFormatBtn = nullptr;
IDWriteTextFormat* g_textFormatPill = nullptr;
IDWriteTextFormat* g_textFormatFooter = nullptr;
IDWriteTextFormat* g_textFormatLcd = nullptr;
IDWriteTextFormat* g_textFormatRemoteLbl = nullptr;
IDWriteTextFormat* g_textFormatBrand = nullptr;
IDWriteTextFormat* g_textFormatFxIcon = nullptr;
IDWriteTextFormat* g_textFormatInfoHdr = nullptr;
IDWriteTextFormat* g_textFormatInfoBody = nullptr;

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

// TV Remote Visual Brushes
ID2D1SolidColorBrush* g_brushRemoteCyan = nullptr;
ID2D1SolidColorBrush* g_brushRemoteGrey = nullptr;
ID2D1SolidColorBrush* g_brushRemoteLabel = nullptr;
ID2D1SolidColorBrush* g_brushRemoteBezel = nullptr;
ID2D1SolidColorBrush* g_brushRemoteWellBg = nullptr;
ID2D1SolidColorBrush* g_brushRemoteBtnFill = nullptr;
ID2D1SolidColorBrush* g_brushRemoteBtnHover = nullptr;
ID2D1SolidColorBrush* g_brushRemoteBtnRing = nullptr;
ID2D1SolidColorBrush* g_brushRemoteShadow = nullptr;
ID2D1SolidColorBrush* g_brushLcdBg = nullptr;
ID2D1SolidColorBrush* g_brushLcdBorder = nullptr;
ID2D1SolidColorBrush* g_brushLcdShine = nullptr;
ID2D1SolidColorBrush* g_brushLcdText = nullptr;
ID2D1LinearGradientBrush* g_brushRemoteBodyGrad = nullptr;
ID2D1LinearGradientBrush* g_brushRainbowBar = nullptr;

// Isolated Screen / Virtual Screen Projection State
enum class ProjectionMode {
    None,
    Screen,
    VirtualScreen,
    App
};
ProjectionMode g_projectionMode = ProjectionMode::None;

struct MonitorEntry {
    std::wstring deviceName;
    std::wstring label;
    RECT rect{};
    bool isPrimary = false;
};
std::wstring g_virtualMonitorDev;
RECT g_virtualMonitorRect{};
bool g_virtualScreenLoaded = false;
int g_vddBaseCountBeforeLoad = 0;
bool g_heldProjectBtn = false;
ULONGLONG g_projectHoldStartMs = 0;
bool g_projectHoldTriggered = false;
std::wstring g_selectedScreenLabel;
RECT g_projectedMonitorRect{};
bool g_controlVirtualScreen = false;

HWND g_projectedHwnd = nullptr;
DWORD g_projectedPid = 0;
std::wstring g_projectedExeName;
std::wstring g_projectedAppDisplayName;
std::wstring g_projectedTitle;
ULONGLONG g_appWindowLostSinceMs = 0;
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
HANDLE g_fallbackMapping = nullptr;
uint8_t* g_fallbackView = nullptr;
ID3D11Texture2D* g_fallbackScaleTex = nullptr;
ID3D11RenderTargetView* g_fallbackScaleRtv = nullptr;
ID3D11Texture2D* g_fallbackReadTex = nullptr;
uint32_t g_fallbackW = 0;
uint32_t g_fallbackH = 0;
uint64_t g_fallbackSeq = 0;
DWORD g_lastInjectedBigscreenPid = 0;
ULONGLONG g_lastInjectionCheck = 0;

void EnsureProjectedFallbackIpc() {
    if (!g_fallbackMapping) {
        const size_t totalBytes = sizeof(bigscreen_projected_video::FallbackHeader) +
            static_cast<size_t>(bigscreen_projected_video::kFallbackMaxWidth) *
            bigscreen_projected_video::kFallbackMaxHeight * 4u;
        g_fallbackMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(totalBytes),
            bigscreen_projected_video::kFallbackMappingName);
    }
    if (g_fallbackMapping && !g_fallbackView) {
        const size_t totalBytes = sizeof(bigscreen_projected_video::FallbackHeader) +
            static_cast<size_t>(bigscreen_projected_video::kFallbackMaxWidth) *
            bigscreen_projected_video::kFallbackMaxHeight * 4u;
        g_fallbackView = static_cast<uint8_t*>(
            MapViewOfFile(g_fallbackMapping, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes));
    }
}

void ReleaseFallbackGpuResources() {
    if (g_fallbackReadTex) { g_fallbackReadTex->Release(); g_fallbackReadTex = nullptr; }
    if (g_fallbackScaleRtv) { g_fallbackScaleRtv->Release(); g_fallbackScaleRtv = nullptr; }
    if (g_fallbackScaleTex) { g_fallbackScaleTex->Release(); g_fallbackScaleTex = nullptr; }
    g_fallbackW = 0;
    g_fallbackH = 0;
}

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

    if (g_projectionMode != ProjectionMode::None && g_projectedSrv && g_projectedWidth > 0 && g_projectedHeight > 0) {
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

    // Cross-GPU CPU fallback writer (only runs when Bigscreen DLL requests fallback due to multi-GPU OpenSharedResource failure)
    if (g_projectedVideoState && g_projectedVideoState->needCpuFallback != 0 && g_sharedDesktopSrv) {
        EnsureProjectedFallbackIpc();
        if (g_fallbackView) {
            uint32_t fbW = (std::min)(deskW, bigscreen_projected_video::kFallbackMaxWidth);
            uint32_t fbH = static_cast<uint32_t>((static_cast<uint64_t>(fbW) * deskH) / (std::max)(1u, deskW));
            if (fbH > bigscreen_projected_video::kFallbackMaxHeight) {
                fbH = bigscreen_projected_video::kFallbackMaxHeight;
                fbW = static_cast<uint32_t>((static_cast<uint64_t>(fbH) * deskW) / (std::max)(1u, deskH));
            }
            fbW = (std::max)(64u, fbW & ~1u);
            fbH = (std::max)(64u, fbH & ~1u);

            if (!g_fallbackScaleTex || g_fallbackW != fbW || g_fallbackH != fbH) {
                ReleaseFallbackGpuResources();
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = fbW;
                sd.Height = fbH;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_DEFAULT;
                sd.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &g_fallbackScaleTex)) &&
                    SUCCEEDED(g_device->CreateRenderTargetView(g_fallbackScaleTex, nullptr, &g_fallbackScaleRtv))) {
                    sd.BindFlags = 0;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &g_fallbackReadTex))) {
                        g_fallbackW = fbW;
                        g_fallbackH = fbH;
                    } else {
                        ReleaseFallbackGpuResources();
                    }
                } else {
                    ReleaseFallbackGpuResources();
                }
            }

            if (g_fallbackScaleRtv && g_fallbackReadTex) {
                g_context->OMSetRenderTargets(1, &g_fallbackScaleRtv, nullptr);
                D3D11_VIEWPORT fvp{ 0.0f, 0.0f, static_cast<float>(fbW), static_cast<float>(fbH), 0.0f, 1.0f };
                g_context->RSSetViewports(1, &fvp);
                g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_context->VSSetShader(g_vs, nullptr, 0);
                g_context->PSSetShader(g_ps, nullptr, 0);
                g_context->PSSetSamplers(0, 1, &g_sampler);
                g_context->PSSetShaderResources(0, 1, &g_sharedDesktopSrv);
                g_context->Draw(3, 0);
                ID3D11ShaderResourceView* nullSrv2 = nullptr;
                g_context->PSSetShaderResources(0, 1, &nullSrv2);
                g_context->CopyResource(g_fallbackReadTex, g_fallbackScaleTex);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(g_context->Map(g_fallbackReadTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                    auto* hdr = reinterpret_cast<bigscreen_projected_video::FallbackHeader*>(g_fallbackView);
                    uint8_t* dst = g_fallbackView + sizeof(bigscreen_projected_video::FallbackHeader);
                    const uint32_t dstStride = fbW * 4u;
                    for (uint32_t y = 0; y < fbH; ++y) {
                        std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                    dstStride);
                    }
                    hdr->width = fbW;
                    hdr->height = fbH;
                    hdr->stride = dstStride;
                    hdr->magic = bigscreen_projected_video::kMagic;
                    ++g_fallbackSeq;
                    hdr->sequence = g_fallbackSeq;
                    g_context->Unmap(g_fallbackReadTex, 0);
                }
            }
        }
    }

    g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    if (numVp > 0) g_context->RSSetViewports(1, &prevVp);

    PublishProjectedVideoState(
        g_projectionMode != ProjectionMode::None,
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
                HWND hwndBig = FindBigscreenWindow();
                if (hwndBig && !IsIconic(hwndBig)) {
                    ShowWindow(hwndBig, SW_MINIMIZE);
                    if (g_keepForegroundUntilMinimized && g_hwnd && !IsIconic(g_hwnd)) {
                        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                    }
                    SetForegroundWindow(g_hwnd);
                    Log("New audio/video feed detected (audio peak=%.3f). Auto-minimized Bigscreen menu.", g_audioPeak);
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
    g_projectedPid = 0;
    g_projectedExeName.clear();
    g_projectedAppDisplayName.clear();
    g_projectedTitle.clear();
    g_appWindowLostSinceMs = 0;
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

bool ResolveWindowAppIdentity(HWND hwnd, DWORD& outPid, std::wstring& outExeName);
HWND FindBestWindowForProjectedApp(DWORD targetPid, const std::wstring& targetExeName);

bool CaptureProjectedWindowFrame() {
    bool needsRebind = (!g_projectedHwnd || !IsWindow(g_projectedHwnd) ||
                        (!IsWindowVisible(g_projectedHwnd) && !IsIconic(g_projectedHwnd)));
    if (needsRebind) {
        HWND rebound = FindBestWindowForProjectedApp(g_projectedPid, g_projectedExeName);
        if (rebound) {
            g_projectedHwnd = rebound;
            DWORD newPid = 0;
            std::wstring newExe;
            if (ResolveWindowAppIdentity(rebound, newPid, newExe)) {
                if (newPid) g_projectedPid = newPid;
                if (!newExe.empty()) g_projectedExeName = newExe;
            }
            g_appIsHidden = false;
            g_appWindowLostSinceMs = 0;
            CloakAppWindow(g_projectedHwnd);
            Log("Rebound Project App stream to updated window for %ls", g_projectedExeName.c_str());
        } else {
            const bool processAlive = !g_projectedExeName.empty() && IsProcessRunning(g_projectedExeName.c_str());
            if (processAlive) {
                const ULONGLONG nowMs = GetTickCount64();
                if (g_appWindowLostSinceMs == 0) {
                    g_appWindowLostSinceMs = nowMs;
                }
                // Keep app projection active across playlist/window recreation while process is running
                if (nowMs - g_appWindowLostSinceMs < 5000) {
                    return false;
                }
            }
            ReleaseProjectedAppResources();
            g_projectionMode = ProjectionMode::None;
            return false;
        }
    } else {
        g_appWindowLostSinceMs = 0;
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

    // Pack cropped rows into g_croppedBits with opaque alpha, omitting top title bar and borders
    for (int y = 0; y < cropH; ++y) {
        const uint32_t* srcRow = static_cast<const uint32_t*>(g_fullWinBitmapBits) + (cropY + y) * g_fullWinWidth + cropX;
        uint32_t* dstRow = g_croppedBits.data() + y * cropW;
        for (int x = 0; x < cropW; ++x) {
            dstRow[x] = srcRow[x] | 0xFF000000u;
        }
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
    DWORD pid = 0;
    std::wstring exeName;
    std::wstring appDisplay;
    std::wstring title;
};

struct SupportedVideoApp {
    const wchar_t* exeName;
    const wchar_t* displayName;
};

static const SupportedVideoApp kSupportedVideoApps[] = {
    { L"Microsoft.Media.Player.exe", L"Windows Media Player (Modern)" },
    { L"wmplayer.exe",               L"Windows Media Player Legacy" },
    { L"Video.UI.exe",               L"Movies & TV" },
    { L"vlc.exe",                    L"VLC media player" },
    { L"mpc-hc64.exe",               L"MPC-HC (64-bit)" },
    { L"mpc-hc.exe",                 L"MPC-HC" },
    { L"mpv.exe",                    L"mpv" },
    { L"GOM.exe",                    L"GOM Player" },
    { L"GOM64.exe",                  L"GOM Player" },
    { L"KMPlayer.exe",               L"KMPlayer" },
    { L"KMPlayer64.exe",             L"KMPlayer" },
    { L"smplayer.exe",               L"SMPlayer" },
    { L"DivXPlayer.exe",             L"DivX Player" },
    { L"Screenbox.exe",              L"Screenbox" },
    { L"Plex.exe",                   L"Plex" },
    { L"Plex Media Server.exe",      L"Plex Media Server" },
    { L"JellyfinDesktop.exe",        L"Jellyfin" },
    { L"JellyfinMediaPlayer.exe",    L"Jellyfin" },
    { L"jellyfin.exe",               L"Jellyfin" },
    { L"kodi.exe",                   L"Kodi" },
    { L"MediaMonkey.exe",            L"MediaMonkey" },
    { L"MediaMonkeyEngine.exe",      L"MediaMonkey" },
    { L"iTunes.exe",                 L"iTunes" },
};

bool QueryProcessBaseName(DWORD pid, std::wstring& outBaseName) {
    outBaseName.clear();
    if (!pid) return false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    wchar_t exePath[MAX_PATH]{};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size) != FALSE;
    CloseHandle(hProcess);
    if (!ok || !exePath[0]) return false;
    const wchar_t* slash = wcsrchr(exePath, L'\\');
    outBaseName = slash ? (slash + 1) : exePath;
    return true;
}

bool ResolveWindowAppIdentity(HWND hwnd, DWORD& outPid, std::wstring& outExeName) {
    outPid = 0;
    outExeName.clear();
    if (!hwnd || !IsWindow(hwnd)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring exeName;
    if (!QueryProcessBaseName(pid, exeName)) return false;

    // UWP / Windows Store apps (Modern Media Player, Movies & TV, Screenbox) host their CoreWindow inside ApplicationFrameHost.exe
    if (_wcsicmp(exeName.c_str(), L"ApplicationFrameHost.exe") == 0) {
        struct UwpSearchCtx {
            DWORD framePid = 0;
            DWORD childPid = 0;
            std::wstring childExe;
        } ctx{ pid, 0, L"" };
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<UwpSearchCtx*>(lp);
            DWORD cpid = 0;
            GetWindowThreadProcessId(child, &cpid);
            if (cpid && cpid != c->framePid) {
                if (QueryProcessBaseName(cpid, c->childExe)) {
                    c->childPid = cpid;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.childExe.empty()) {
            pid = ctx.childPid;
            exeName = ctx.childExe;
        }
    }

    outPid = pid;
    outExeName = exeName;
    return !outExeName.empty();
}

const wchar_t* MatchSupportedVideoAppDisplayName(HWND hwnd, DWORD* outPid = nullptr, std::wstring* outExeName = nullptr) {
    DWORD pid = 0;
    std::wstring exeName;
    if (!ResolveWindowAppIdentity(hwnd, pid, exeName)) return nullptr;
    if (outPid) *outPid = pid;
    if (outExeName) *outExeName = exeName;

    for (const auto& app : kSupportedVideoApps) {
        if (_wcsicmp(exeName.c_str(), app.exeName) == 0) {
            return app.displayName;
        }
    }
    return nullptr;
}

HWND FindBestWindowForProjectedApp(DWORD targetPid, const std::wstring& targetExeName) {
    if (targetPid == 0 && targetExeName.empty()) return nullptr;
    struct MatchSearch {
        DWORD targetPid;
        const std::wstring* targetExe;
        HWND bestPidMatch = nullptr;
        int bestPidArea = -1;
        HWND bestExeMatch = nullptr;
        int bestExeArea = -1;
    } ctx{ targetPid, &targetExeName };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<MatchSearch*>(lp);
        if (hwnd == g_hwnd) return TRUE;
        if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;
        LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        DWORD pid = 0;
        std::wstring exe;
        if (!ResolveWindowAppIdentity(hwnd, pid, exe)) return TRUE;

        const bool sameExe = !c->targetExe->empty() && _wcsicmp(exe.c_str(), c->targetExe->c_str()) == 0;
        const bool samePid = (c->targetPid != 0 && pid == c->targetPid);
        if (!sameExe && !samePid) return TRUE;

        RECT r{};
        GetWindowRect(hwnd, &r);
        int w = (std::max)(0L, r.right - r.left);
        int h = (std::max)(0L, r.bottom - r.top);
        int area = (w >= 120 && h >= 120) ? (w * h) : 1;

        if (samePid && area > c->bestPidArea) {
            c->bestPidArea = area;
            c->bestPidMatch = hwnd;
        }
        if (sameExe && area > c->bestExeArea) {
            c->bestExeArea = area;
            c->bestExeMatch = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.bestPidMatch ? ctx.bestPidMatch : ctx.bestExeMatch;
}

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

        DWORD pid = 0;
        std::wstring exeName;
        const wchar_t* appDisplay = MatchSupportedVideoAppDisplayName(hwnd, &pid, &exeName);
        if (!appDisplay) {
            return TRUE;
        }

        std::wstring label = appDisplay;
        if (wcsstr(title, appDisplay) == nullptr) {
            label += L"  -  ";
            label += title;
        } else {
            label = title;
        }
        outList->push_back({ hwnd, pid, exeName, appDisplay, label });
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
            g_lookYaw, g_lookPitch, g_lookZoom);
    }
}

void ResetLook() {
    g_lookYaw = 0.0f;
    g_lookPitch = 0.0f;
    g_lookZoom = 0.0f;
    g_pointerYaw = 0.0f;
    g_pointerPitch = 0.0f;
    EnsureControlMapping();
    if (g_controlState) {
        bigscreen_controller_control_ipc::TriggerReset(g_controlState);
        SyncPointerAndLook();
    }
}

void ToggleInfoSlideOut() {
    g_infoPanelOpen = !g_infoPanelOpen;
    if (g_infoPanelOpen) {
        SetLcdNotification(L"INFO SCREEN", L"SLID OUT LEFT", 2500);
    } else {
        SetLcdNotification(L"INFO SCREEN", L"RETRACTED", 2000);
    }
}

void ExecuteRemoteDpadStep(RemoteBtn btn, bool updateLcd = true) {
    if (g_movieOnly) return;

    if (g_zoomMode) {
        switch (btn) {
            case RemoteBtn::DpadUp:
                g_lookZoom = std::clamp(g_lookZoom + 0.25f, -3.5f, 8.0f);
                SyncPointerAndLook();
                if (updateLcd) SetLcdNotification(L"ZOOM FORWARD", L"", 1200);
                break;
            case RemoteBtn::DpadDown:
                g_lookZoom = std::clamp(g_lookZoom - 0.25f, -3.5f, 8.0f);
                SyncPointerAndLook();
                if (updateLcd) SetLcdNotification(L"ZOOM OUT", L"", 1200);
                break;
            case RemoteBtn::DpadSelect:
                ResetLook();
                if (updateLcd) SetLcdNotification(L"NORMAL VIEW", L"ZOOM RESET", 1800);
                break;
            default:
                break;
        }
        return;
    }

    switch (btn) {
        case RemoteBtn::DpadUp:
            g_lookPitch += 0.04f;
            SyncPointerAndLook();
            if (updateLcd) SetLcdNotification(L"LOOK UP", L"", 1200);
            break;
        case RemoteBtn::DpadDown:
            g_lookPitch -= 0.04f;
            SyncPointerAndLook();
            if (updateLcd) SetLcdNotification(L"LOOK DOWN", L"", 1200);
            break;
        case RemoteBtn::DpadLeft:
            g_lookYaw -= 0.05f;
            SyncPointerAndLook();
            if (updateLcd) SetLcdNotification(L"LOOK LEFT", L"", 1200);
            break;
        case RemoteBtn::DpadRight:
            g_lookYaw += 0.05f;
            SyncPointerAndLook();
            if (updateLcd) SetLcdNotification(L"LOOK RIGHT", L"", 1200);
            break;
        case RemoteBtn::DpadSelect:
            ResetLook();
            if (updateLcd) SetLcdNotification(L"VIEW RECENTERED", L"", 1800);
            break;
        default:
            break;
    }
}

void StartProjectingAppWindow(HWND targetHwnd, const std::wstring& title,
                              DWORD pid = 0, const std::wstring& exeName = L"", const std::wstring& appDisplay = L"") {
    if (!targetHwnd || !IsWindow(targetHwnd)) return;
    UnloadVirtualScreen();
    ReleaseProjectedAppResources();
    g_projectedHwnd = targetHwnd;
    g_projectedTitle = title;
    g_projectedPid = pid;
    g_projectedExeName = exeName;
    if (g_projectedPid == 0 || g_projectedExeName.empty()) {
        ResolveWindowAppIdentity(targetHwnd, g_projectedPid, g_projectedExeName);
    }
    g_projectedAppDisplayName = !appDisplay.empty() ? appDisplay : (!g_projectedExeName.empty() ? g_projectedExeName : title);
    g_appWindowLostSinceMs = 0;
    g_projectionMode = ProjectionMode::App;
    g_lookYaw = 0.0f;
    g_lookPitch = 0.0f;
    g_lookZoom = 0.0f;
    SyncPointerAndLook();
    CaptureProjectedWindowFrame();
    CloakAppWindow(g_projectedHwnd);
    SetLcdNotification(L"PROJECTING APP", L"PRIVATE WINDOW", 3500);
    Log("Started projecting isolated video app: %ls (pid=%u exe=%ls)",
        g_projectedAppDisplayName.c_str(), g_projectedPid, g_projectedExeName.c_str());
}

void ShowProjectAppMenu(HWND parentHwnd, float clickX, float clickY) {
    auto windows = EnumerateSelectableAppWindows();
    HMENU hMenu = CreatePopupMenu();
    if (windows.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"No supported video apps currently running");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"--- Select Video App to Project ---");
        for (size_t i = 0; i < windows.size() && i < 25; ++i) {
            std::wstring itemText = windows[i].title;
            if (itemText.length() > 55) {
                itemText = itemText.substr(0, 52) + L"...";
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
            const auto& sel = windows[cmd - 1000];
            StartProjectingAppWindow(sel.hwnd, sel.title, sel.pid, sel.exeName, sel.appDisplay);
        }
    }
}

HWND g_lastVirtualTargetHwnd = nullptr;
std::wstring g_lastVddError;

static const GUID kGuidDevClassDisplay = {
    0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

bool IsVirtualDisplayDevice(const DISPLAY_DEVICEW& dd) {
    if (!dd.DeviceString[0]) return false;
    if (wcsstr(dd.DeviceString, L"Immersed") != nullptr) return false;
    return _wcsicmp(dd.DeviceString, L"Virtual Display Driver") == 0 ||
           wcsstr(dd.DeviceString, L"MttVDD") != nullptr ||
           wcsstr(dd.DeviceID, L"MttVDD") != nullptr ||
           wcsstr(dd.DeviceID, L"MTTVDD") != nullptr;
}

bool ContainsCaseInsensitive(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = wcslen(needle);
    for (const wchar_t* p = haystack; *p; ++p) {
        if (_wcsnicmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

std::wstring GetExeDirectory() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path p(exePath);
    return p.parent_path().wstring();
}

std::wstring FindBundledVddInfPath() {
    fs::path exeDir(GetExeDirectory());
    fs::path candidates[] = {
        exeDir / L"vdd" / L"mttvdd.inf",
        exeDir.parent_path().parent_path() / L"resources" / L"vdd" / L"mttvdd.inf",
        exeDir.parent_path().parent_path().parent_path() / L"resources" / L"vdd" / L"mttvdd.inf"
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            return fs::weakly_canonical(c, ec).wstring();
        }
    }
    return L"";
}

bool EnsureVddSettingsFile(const std::wstring& infPath, bool* outUpdated = nullptr, int targetMonitorCount = 1) {
    if (outUpdated) *outUpdated = false;
    std::error_code ec;
    fs::path targetDir(L"C:\\VirtualDisplayDriver");
    fs::create_directories(targetDir, ec);
    fs::path targetXml = targetDir / L"vdd_settings.xml";
    if (!infPath.empty()) {
        fs::path srcXml = fs::path(infPath).parent_path() / L"vdd_settings.xml";
        if (fs::exists(srcXml, ec)) {
            std::string desiredXml;
            {
                FILE* f1 = nullptr;
                if (_wfopen_s(&f1, srcXml.c_str(), L"rb") == 0 && f1) {
                    char buf[4096]{};
                    size_t n = fread(buf, 1, sizeof(buf) - 1, f1);
                    fclose(f1);
                    desiredXml.assign(buf, n);
                }
            }
            if (!desiredXml.empty()) {
                if (targetMonitorCount > 1) {
                    size_t cOpen = desiredXml.find("<count>");
                    size_t cClose = (cOpen != std::string::npos) ? desiredXml.find("</count>", cOpen) : std::string::npos;
                    if (cOpen != std::string::npos && cClose != std::string::npos) {
                        desiredXml.replace(cOpen + 7, cClose - (cOpen + 7), std::to_string(targetMonitorCount));
                    }
                }
                std::string currentXml;
                if (fs::exists(targetXml, ec)) {
                    FILE* f2 = nullptr;
                    if (_wfopen_s(&f2, targetXml.c_str(), L"rb") == 0 && f2) {
                        char buf[4096]{};
                        size_t n = fread(buf, 1, sizeof(buf) - 1, f2);
                        fclose(f2);
                        currentXml.assign(buf, n);
                    }
                }
                if (currentXml != desiredXml) {
                    if (fs::exists(targetXml, ec)) {
                        fs::path bakXml = targetDir / L"vdd_settings.xml.pre_bigscreen_bak";
                        if (!fs::exists(bakXml, ec)) {
                            fs::copy_file(targetXml, bakXml, ec);
                        }
                    }
                    FILE* fOut = nullptr;
                    if (_wfopen_s(&fOut, targetXml.c_str(), L"wb") == 0 && fOut) {
                        size_t written = fwrite(desiredXml.data(), 1, desiredXml.size(), fOut);
                        fclose(fOut);
                        if (written == desiredXml.size() && outUpdated) *outUpdated = true;
                    }
                }
            }
        }
    }
    return fs::exists(targetXml, ec);
}

bool SendMttVddCommand(const wchar_t* cmd) {
    if (!cmd || !*cmd) return false;
    HANDLE hPipe = CreateFileW(L"\\\\.\\pipe\\MTTVirtualDisplayPipe",
                               GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytes = static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t));
    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, cmd, bytes, &written, nullptr);
    CloseHandle(hPipe);
    return ok && written == bytes;
}

std::vector<std::wstring> EnumerateAttachedVddDevices() {
    std::vector<std::wstring> devs;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (IsVirtualDisplayDevice(dd) && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0) {
            devs.push_back(dd.DeviceName);
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return devs;
}

bool ContainsDeviceName(const std::vector<std::wstring>& list, const std::wstring& dev) {
    for (const auto& s : list) {
        if (_wcsicmp(s.c_str(), dev.c_str()) == 0) return true;
    }
    return false;
}

std::wstring FindNewVirtualDisplayDeviceName(const std::vector<std::wstring>& excludeDevs, bool* outAttached) {
    if (outAttached) *outAttached = false;
    std::wstring fallbackUnattached;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (IsVirtualDisplayDevice(dd) && !ContainsDeviceName(excludeDevs, dd.DeviceName)) {
            const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            if (attached) {
                if (outAttached) *outAttached = true;
                return dd.DeviceName;
            }
            if (fallbackUnattached.empty()) {
                fallbackUnattached = dd.DeviceName;
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return fallbackUnattached;
}

std::wstring FindVirtualDisplayDeviceName(bool* outAttached) {
    if (outAttached) *outAttached = false;
    std::wstring fallbackName;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (IsVirtualDisplayDevice(dd)) {
            const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            if (attached) {
                if (outAttached) *outAttached = true;
                return dd.DeviceName;
            }
            if (fallbackName.empty()) {
                fallbackName = dd.DeviceName;
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return fallbackName;
}

bool CheckMttVddPnpDeviceState(bool* outExists, bool* outDisabled, bool* outHasProblem) {
    if (outExists) *outExists = false;
    if (outDisabled) *outDisabled = false;
    if (outHasProblem) *outHasProblem = false;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&kGuidDevClassDisplay, nullptr, nullptr, 0);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(devInfoData);
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devInfoData); ++idx) {
        wchar_t hwIdBuf[512]{};
        wchar_t descBuf[256]{};
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_HARDWAREID, nullptr,
                                          reinterpret_cast<PBYTE>(hwIdBuf), sizeof(hwIdBuf) - sizeof(wchar_t), nullptr);
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_DEVICEDESC, nullptr,
                                          reinterpret_cast<PBYTE>(descBuf), sizeof(descBuf) - sizeof(wchar_t), nullptr);

        bool isMtt = false;
        for (const wchar_t* p = hwIdBuf; *p; p += wcslen(p) + 1) {
            if (ContainsCaseInsensitive(p, L"MttVDD")) {
                isMtt = true;
                break;
            }
        }
        if (!isMtt && !ContainsCaseInsensitive(descBuf, L"Immersed") &&
            _wcsicmp(descBuf, L"Virtual Display Driver") == 0) {
            isMtt = true;
        }
        if (isMtt) {
            if (outExists) *outExists = true;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0) == CR_SUCCESS) {
                if ((status & DN_HAS_PROBLEM) != 0) {
                    if (outHasProblem) *outHasProblem = true;
                    if (problem == CM_PROB_DISABLED && outDisabled) *outDisabled = true;
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return true;
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return true;
}

DWORD InstallOrEnableVddElevatedWorker() {
    std::wstring infPath = FindBundledVddInfPath();
    if (infPath.empty()) {
        Log("Bundled vdd\\mttvdd.inf not found next to viewer.");
        return ERROR_FILE_NOT_FOUND;
    }

    EnsureVddSettingsFile(infPath);

    // Stage INF + CAT + DLL into Windows DriverStore from our bundled folder
    if (!SetupCopyOEMInfW(infPath.c_str(), nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
        DWORD copyErr = GetLastError();
        Log("SetupCopyOEMInfW returned %u (continuing to bind/create device)...", copyErr);
    }

    bool anyExisting = false;
    bool anyFixed = false;

    // 1) Enable or repair any existing MttVDD PnP device nodes
    HDEVINFO hExisting = SetupDiGetClassDevsW(&kGuidDevClassDisplay, nullptr, nullptr, 0);
    if (hExisting != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(devData);
        for (DWORD idx = 0; SetupDiEnumDeviceInfo(hExisting, idx, &devData); ++idx) {
            wchar_t hwIdBuf[512]{};
            SetupDiGetDeviceRegistryPropertyW(hExisting, &devData, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwIdBuf), sizeof(hwIdBuf) - sizeof(wchar_t), nullptr);
            bool isMtt = false;
            for (const wchar_t* p = hwIdBuf; *p; p += wcslen(p) + 1) {
                if (ContainsCaseInsensitive(p, L"MttVDD")) {
                    isMtt = true;
                    break;
                }
            }
            if (!isMtt) continue;

            anyExisting = true;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, devData.DevInst, 0) == CR_SUCCESS &&
                (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED) {
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = DICS_ENABLE;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (SetupDiSetClassInstallParamsW(hExisting, &devData, &pcp.ClassInstallHeader, sizeof(pcp))) {
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hExisting, &devData);
                }
                CM_Enable_DevNode(devData.DevInst, 0);
            }
            BOOL rebootReq = FALSE;
            if (DiInstallDevice(nullptr, hExisting, &devData, nullptr, 0, &rebootReq)) {
                anyFixed = true;
            }
        }
        SetupDiDestroyDeviceInfoList(hExisting);
    }

    if (anyExisting) {
        BOOL rebootReq = FALSE;
        UpdateDriverForPlugAndPlayDevicesW(nullptr, L"Root\\MttVDD", infPath.c_str(), INSTALLFLAG_FORCE, &rebootReq);
        if (anyFixed) return 0;
    }

    // 2) Create a brand new Root\MttVDD Virtual Display Driver device node if none exists
    HDEVINFO hDevInfo = SetupDiCreateDeviceInfoList(&kGuidDevClassDisplay, nullptr);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(devInfoData);
    if (!SetupDiCreateDeviceInfoW(hDevInfo, L"DISPLAY", &kGuidDevClassDisplay,
                                  L"Virtual Display Driver", nullptr, DICD_GENERATE_ID, &devInfoData)) {
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return err;
    }

    static const wchar_t kHwIds[] = L"Root\\MttVDD\0";
    if (!SetupDiSetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(kHwIds), sizeof(kHwIds))) {
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return err;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hDevInfo, &devInfoData)) {
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return err;
    }

    BOOL rebootReq = FALSE;
    BOOL updated = UpdateDriverForPlugAndPlayDevicesW(nullptr, L"Root\\MttVDD", infPath.c_str(),
                                                      INSTALLFLAG_FORCE, &rebootReq);
    if (!updated) {
        updated = DiInstallDevice(nullptr, hDevInfo, &devInfoData, nullptr, 0, &rebootReq);
    }
    DWORD finalErr = updated ? 0 : GetLastError();
    if (!updated) {
        SetupDiCallClassInstaller(DIF_REMOVE, hDevInfo, &devInfoData);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return finalErr;
}

bool EnsureVirtualDisplayDriverInstalled(HWND parentHwnd) {
    bool attached = false;
    if (!FindVirtualDisplayDeviceName(&attached).empty()) {
        return true;
    }

    bool devExists = false, devDisabled = false, devProblem = false;
    CheckMttVddPnpDeviceState(&devExists, &devDisabled, &devProblem);

    // If device is installed & enabled and pipe responds, trigger 1 virtual display without elevation
    if (devExists && !devDisabled && !devProblem) {
        if (SendMttVddCommand(L"SETDISPLAYCOUNT 1")) {
            for (int i = 0; i < 15; ++i) {
                Sleep(200);
                if (!FindVirtualDisplayDeviceName(&attached).empty()) return true;
            }
        }
    }

    std::wstring infPath = FindBundledVddInfPath();
    if (infPath.empty()) {
        g_lastVddError = L"Bundled Virtual Display Driver files (vdd\\mttvdd.inf) were not found next to BigscreenDesktopViewer.exe.";
        return false;
    }

    if (IsUserAnAdmin()) {
        DWORD err = InstallOrEnableVddElevatedWorker();
        if (err != 0) {
            wchar_t msg[256]{};
            swprintf_s(msg, L"Virtual Display Driver installation failed (Win32 error %u).", err);
            g_lastVddError = msg;
            return false;
        }
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = parentHwnd;
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.lpParameters = L"--install-vdd";
        sei.lpDirectory = GetExeDirectory().c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                g_lastVddError = L"Administrator permission (UAC) was cancelled. Elevation is required once to install the Virtual Display Driver.";
            } else {
                wchar_t msg[256]{};
                swprintf_s(msg, L"Could not launch Virtual Display Driver installer (error %u).", err);
                g_lastVddError = msg;
            }
            return false;
        }

        if (sei.hProcess) {
            const ULONGLONG startWait = GetTickCount64();
            while (WaitForSingleObject(sei.hProcess, 50) == WAIT_TIMEOUT) {
                MSG msg{};
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                if (GetTickCount64() - startWait > 45000) break;
            }
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            if (exitCode != 0) {
                wchar_t msg[256]{};
                swprintf_s(msg, L"Virtual Display Driver setup exited with error %u.", exitCode);
                g_lastVddError = msg;
                return false;
            }
        }
    }

    // Give UMDF IddCx driver host up to ~4s to initialize the virtual monitor and settle PnP
    for (int i = 0; i < 20; ++i) {
        SendMttVddCommand(L"SETDISPLAYCOUNT 1");
        Sleep(250);
        if (!FindVirtualDisplayDeviceName(&attached).empty()) {
            Sleep(400); // allow Windows Display Manager to finish initial PnP topology
            return true;
        }
    }

    g_lastVddError = L"Virtual Display Driver was installed, but Windows has not exposed the virtual display output yet.";
    return false;
}

bool IsVirtualDisplayClonedOrPrimary(const std::wstring& devName, bool* outIsPrimary = nullptr, bool* outIsCloned = nullptr) {
    if (outIsPrimary) *outIsPrimary = false;
    if (outIsCloned) *outIsCloned = false;
    if (devName.empty()) return false;

    bool isPrimary = false;
    bool isCloned = false;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (_wcsicmp(dd.DeviceName, devName.c_str()) == 0) {
            isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            int monCount = 0;
            for (DWORD m = 0;; ++m) {
                DISPLAY_DEVICEW mon{};
                mon.cb = sizeof(mon);
                if (!EnumDisplayDevicesW(dd.DeviceName, m, &mon, EDD_GET_DEVICE_INTERFACE_NAME)) break;
                ++monCount;
                if (!ContainsCaseInsensitive(mon.DeviceID, L"MTT")) {
                    isCloned = true;
                }
            }
            if (monCount > 1) {
                isCloned = true;
            }
            break;
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    if (outIsPrimary) *outIsPrimary = isPrimary;
    if (outIsCloned) *outIsCloned = isCloned;
    return isPrimary || isCloned;
}

void EnsureExtendedTopologyForVirtualScreen(std::wstring& devName, bool& alreadyAttached) {
    bool isPrimary = false;
    bool isCloned = false;
    IsVirtualDisplayClonedOrPrimary(devName, &isPrimary, &isCloned);

    // Also check CCD directly for any active clone group involving our target
    bool hasClonePath = false;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 a = 0; a < np; ++a) {
                for (UINT32 b = a + 1; b < np; ++b) {
                    if (paths[a].sourceInfo.adapterId.LowPart == paths[b].sourceInfo.adapterId.LowPart &&
                        paths[a].sourceInfo.adapterId.HighPart == paths[b].sourceInfo.adapterId.HighPart &&
                        paths[a].sourceInfo.id == paths[b].sourceInfo.id) {
                        hasClonePath = true;
                    }
                }
            }
        }
    }

    if (isCloned || hasClonePath) {
        Log("Detected Duplicate/Clone display state on Virtual Screen (%ls); switching to Extended mode...",
            devName.c_str());
        SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_SAVE_TO_DATABASE);
        Sleep(600);
        std::wstring refreshed = FindVirtualDisplayDeviceName(&alreadyAttached);
        if (!refreshed.empty()) devName = refreshed;
    }

    // Ensure a real physical display remains Primary at (0,0) if Windows promoted Virtual Screen to Primary
    IsVirtualDisplayClonedOrPrimary(devName, &isPrimary, &isCloned);
}

std::wstring FindPrimaryPhysicalDisplayDevice() {
    std::wstring firstPhysical;
    std::wstring primaryFallback;
    std::wstring anyAttachedFallback;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (attached && wcsstr(dd.DeviceString, L"Immersed") == nullptr) {
            const bool isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            if (!IsVirtualDisplayDevice(dd)) {
                if (isPrimary) return dd.DeviceName;
                if (firstPhysical.empty()) firstPhysical = dd.DeviceName;
            } else {
                if (isPrimary && primaryFallback.empty()) primaryFallback = dd.DeviceName;
                if (anyAttachedFallback.empty()) anyAttachedFallback = dd.DeviceName;
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    if (!firstPhysical.empty()) return firstPhysical;
    if (!primaryFallback.empty()) return primaryFallback;
    return anyAttachedFallback;
}

bool ForcePrimaryMonitorViaCcd(const std::wstring& primaryPhysDev, const std::wstring& vddDev,
                               LONG vddPosX, uint32_t vddW, uint32_t vddH) {
    if (primaryPhysDev.empty()) return false;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }

    UINT32 physModeIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    UINT32 vddModeIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    for (UINT32 i = 0; i < np; ++i) {
        if (paths[i].sourceInfo.modeInfoIdx >= nm) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = paths[i].sourceInfo.adapterId;
        srcName.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) == ERROR_SUCCESS) {
            if (_wcsicmp(srcName.viewGdiDeviceName, primaryPhysDev.c_str()) == 0) {
                physModeIdx = paths[i].sourceInfo.modeInfoIdx;
            } else if (!vddDev.empty() && _wcsicmp(srcName.viewGdiDeviceName, vddDev.c_str()) == 0) {
                vddModeIdx = paths[i].sourceInfo.modeInfoIdx;
            }
        }
    }
    if (physModeIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
        modes[physModeIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        return false;
    }

    const LONG shiftX = modes[physModeIdx].sourceMode.position.x;
    const LONG shiftY = modes[physModeIdx].sourceMode.position.y;
    for (UINT32 m = 0; m < nm; ++m) {
        if (modes[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            modes[m].sourceMode.position.x -= shiftX;
            modes[m].sourceMode.position.y -= shiftY;
        }
    }
    if (vddModeIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
        vddModeIdx != physModeIdx &&
        modes[vddModeIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        modes[vddModeIdx].sourceMode.position.x = vddPosX;
        modes[vddModeIdx].sourceMode.position.y = 0;
        modes[vddModeIdx].sourceMode.width = vddW;
        modes[vddModeIdx].sourceMode.height = vddH;
    }

    LONG rc = SetDisplayConfig(np, paths.data(), nm, modes.data(),
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    return rc == ERROR_SUCCESS;
}

bool ApplyExtendedLayoutPreservingPrimary(const std::wstring& anchorPrimaryDev,
                                          std::wstring& vddDev,
                                          uint32_t targetW, uint32_t targetH) {
    struct PhysMon {
        std::wstring devName;
        DEVMODEW dm{};
        bool isPrimary = false;
    };
    std::vector<PhysMon> physList;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (attached &&
            wcsstr(dd.DeviceString, L"Immersed") == nullptr &&
            _wcsicmp(dd.DeviceName, vddDev.c_str()) != 0) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0) &&
                dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
                PhysMon pm;
                pm.devName = dd.DeviceName;
                pm.dm = dm;
                pm.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                physList.push_back(pm);
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }

    if (physList.empty()) return false;

    size_t primIdx = 0;
    bool foundAnchor = false;
    if (!anchorPrimaryDev.empty()) {
        for (size_t i = 0; i < physList.size(); ++i) {
            if (_wcsicmp(physList[i].devName.c_str(), anchorPrimaryDev.c_str()) == 0) {
                primIdx = i;
                foundAnchor = true;
                break;
            }
        }
    }
    if (!foundAnchor) {
        for (size_t i = 0; i < physList.size(); ++i) {
            if (physList[i].isPrimary) {
                primIdx = i;
                foundAnchor = true;
                break;
            }
        }
    }

    const LONG origPrimX = physList[primIdx].dm.dmPosition.x;
    const LONG origPrimY = physList[primIdx].dm.dmPosition.y;
    LONG maxRight = 1920;

    for (size_t i = 0; i < physList.size(); ++i) {
        physList[i].dm.dmFields |= DM_POSITION;
        physList[i].dm.dmPosition.x -= origPrimX;
        physList[i].dm.dmPosition.y -= origPrimY;
        LONG rightEdge = physList[i].dm.dmPosition.x + static_cast<LONG>(physList[i].dm.dmPelsWidth);
        if (rightEdge > maxRight) maxRight = rightEdge;
    }

    // Find exact 1920x1080 mode supported by MttVDD (prefer 60Hz)
    DEVMODEW vddDm{};
    bool hasExact = false;
    for (DWORD modeIdx = 0;; ++modeIdx) {
        DEVMODEW cand{};
        cand.dmSize = sizeof(cand);
        if (!EnumDisplaySettingsExW(vddDev.c_str(), modeIdx, &cand, 0)) break;
        if (cand.dmPelsWidth == targetW && cand.dmPelsHeight == targetH && cand.dmBitsPerPel == 32) {
            if (!hasExact || cand.dmDisplayFrequency == 60) {
                vddDm = cand;
                hasExact = true;
                if (cand.dmDisplayFrequency == 60) break;
            }
        }
    }
    if (!hasExact) {
        vddDm.dmSize = sizeof(vddDm);
        vddDm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_POSITION;
        vddDm.dmPelsWidth = targetW;
        vddDm.dmPelsHeight = targetH;
        vddDm.dmBitsPerPel = 32;
        vddDm.dmDisplayFrequency = 60;
    } else {
        vddDm.dmFields |= DM_POSITION;
    }
    vddDm.dmPosition.x = maxRight;
    vddDm.dmPosition.y = 0;

    // Atomic batch: stage ALL monitors before calling commit once so Windows preserves Main Display cleanly
    for (size_t i = 0; i < physList.size(); ++i) {
        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
        if (i == primIdx) flags |= CDS_SET_PRIMARY;
        ChangeDisplaySettingsExW(physList[i].devName.c_str(), &physList[i].dm, nullptr, flags, nullptr);
    }
    LONG vddStageRes = ChangeDisplaySettingsExW(vddDev.c_str(), &vddDm, nullptr,
                                                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (vddStageRes != DISP_CHANGE_SUCCESSFUL) {
        vddDm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_POSITION;
        ChangeDisplaySettingsExW(vddDev.c_str(), &vddDm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    }
    LONG commitRes = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    Sleep(250);

    bool dummyAttached = false;
    std::wstring refreshed = FindVirtualDisplayDeviceName(&dummyAttached);
    if (!refreshed.empty()) vddDev = refreshed;

    bool vddStillPrimary = false, vddCloned = false;
    IsVirtualDisplayClonedOrPrimary(vddDev, &vddStillPrimary, &vddCloned);
    if (commitRes != DISP_CHANGE_SUCCESSFUL || vddStillPrimary) {
        ForcePrimaryMonitorViaCcd(physList[primIdx].devName, vddDev, maxRight, targetW, targetH);
        Sleep(250);
    }
    return true;
}

bool ActivateDisplayPathViaCcd(const std::wstring& devName) {
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes) != ERROR_SUCCESS || numPaths == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> newPaths;
    for (UINT32 i = 0; i < numPaths; ++i) {
        if ((paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
            newPaths.push_back(paths[i]);
        }
    }

    auto isSourceInUse = [&](const LUID& adapterId, UINT32 sourceId) {
        for (const auto& ap : newPaths) {
            if (ap.sourceInfo.adapterId.LowPart == adapterId.LowPart &&
                ap.sourceInfo.adapterId.HighPart == adapterId.HighPart &&
                ap.sourceInfo.id == sourceId) {
                return true;
            }
        }
        return false;
    };

    bool added = false;
    for (UINT32 i = 0; i < numPaths; ++i) {
        const auto& p = paths[i];
        if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) continue;
        if (!p.targetInfo.targetAvailable) continue;
        if (isSourceInUse(p.sourceInfo.adapterId, p.sourceInfo.id)) continue;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = p.sourceInfo.adapterId;
        srcName.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) == ERROR_SUCCESS &&
            _wcsicmp(srcName.viewGdiDeviceName, devName.c_str()) == 0) {
            DISPLAYCONFIG_PATH_INFO addPath = p;
            addPath.flags |= DISPLAYCONFIG_PATH_ACTIVE;
            addPath.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            addPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            newPaths.push_back(addPath);
            added = true;
            break;
        }
    }

    if (!added) return false;
    LONG rc = SetDisplayConfig(static_cast<UINT32>(newPaths.size()), newPaths.data(),
                               numModes, modes.data(),
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    return rc == ERROR_SUCCESS;
}

std::vector<MonitorEntry> EnumerateSelectableScreens(bool* outOnlyVddMonitors = nullptr) {
    if (outOnlyVddMonitors) *outOnlyVddMonitors = false;
    std::vector<MonitorEntry> screens;
    int nonVddCount = 0;
    int vddCount = 0;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    int screenNum = 1;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        const bool isViewerOwnedVdd = !g_virtualMonitorDev.empty() && _wcsicmp(dd.DeviceName, g_virtualMonitorDev.c_str()) == 0;
        if (attached && !isViewerOwnedVdd && wcsstr(dd.DeviceString, L"Immersed") == nullptr) {
            const bool isVdd = IsVirtualDisplayDevice(dd);
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0) &&
                dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
                if (isVdd) ++vddCount;
                else ++nonVddCount;
                MonitorEntry entry;
                entry.deviceName = dd.DeviceName;
                entry.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                entry.rect = {
                    dm.dmPosition.x,
                    dm.dmPosition.y,
                    dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth),
                    dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight)
                };
                wchar_t buf[160]{};
                swprintf_s(buf, L"Monitor %d%ls%ls  (%ux%u)",
                           screenNum++,
                           entry.isPrimary ? L" [Primary]" : L"",
                           isVdd ? L" [Virtual]" : L"",
                           dm.dmPelsWidth, dm.dmPelsHeight);
                entry.label = buf;
                screens.push_back(entry);
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    if (outOnlyVddMonitors) {
        *outOnlyVddMonitors = (nonVddCount == 0 && vddCount >= 1);
    }
    return screens;
}

bool LoadVirtualScreen() {
    g_lastVddError.clear();

    // Snapshot pre-existing attached VDD displays (exclude viewer's own if reloading)
    std::vector<std::wstring> existingVddDevs = EnumerateAttachedVddDevices();
    if (!g_virtualMonitorDev.empty()) {
        existingVddDevs.erase(
            std::remove_if(existingVddDevs.begin(), existingVddDevs.end(),
                           [&](const std::wstring& s) { return _wcsicmp(s.c_str(), g_virtualMonitorDev.c_str()) == 0; }),
            existingVddDevs.end());
    }
    g_vddBaseCountBeforeLoad = static_cast<int>(existingVddDevs.size());
    const int targetVddCount = g_vddBaseCountBeforeLoad + 1;

    // Snapshot user's real Primary monitor (physical or pre-existing primary VDD) BEFORE attaching/reloading
    std::wstring anchorPrimaryDev = FindPrimaryPhysicalDisplayDevice();

    // Sync C:\VirtualDisplayDriver\vdd_settings.xml with desired display count & 1920x1080-only resolution
    bool xmlUpdated = false;
    EnsureVddSettingsFile(FindBundledVddInfPath(), &xmlUpdated, targetVddCount);
    if (xmlUpdated && SendMttVddCommand(L"RELOAD_DRIVER")) {
        Sleep(500);
    }

    if (!EnsureVirtualDisplayDriverInstalled(g_hwnd)) {
        return false;
    }

    bool alreadyAttached = false;
    std::wstring devName = FindNewVirtualDisplayDeviceName(existingVddDevs, &alreadyAttached);
    if (devName.empty() && targetVddCount > 1) {
        wchar_t countCmd[32]{};
        swprintf_s(countCmd, L"SETDISPLAYCOUNT %d", targetVddCount);
        for (int attempt = 0; attempt < 15 && devName.empty(); ++attempt) {
            SendMttVddCommand(countCmd);
            Sleep(250);
            devName = FindNewVirtualDisplayDeviceName(existingVddDevs, &alreadyAttached);
        }
    }
    if (devName.empty()) {
        g_lastVddError = (targetVddCount > 1)
            ? L"Virtual Display Driver could not create a second 1920x1080 display output."
            : L"Virtual Display Driver installed, but no Virtual Display output is active.";
        return false;
    }

    EnsureExtendedTopologyForVirtualScreen(devName, alreadyAttached);

    if (!alreadyAttached) {
        ActivateDisplayPathViaCcd(devName);
        Sleep(300);
        std::wstring refreshed = FindNewVirtualDisplayDeviceName(existingVddDevs, &alreadyAttached);
        if (!refreshed.empty()) devName = refreshed;
    }

    // Apply 1920x1080 on Virtual Screen and guarantee base Primary monitor stays Main Display (0,0)
    ApplyExtendedLayoutPreservingPrimary(anchorPrimaryDev, devName, 1920, 1080);

    std::wstring finalDev = FindNewVirtualDisplayDeviceName(existingVddDevs, &alreadyAttached);
    if (!finalDev.empty()) devName = finalDev;

    bool finalPrimary = false, finalCloned = false;
    if (IsVirtualDisplayClonedOrPrimary(devName, &finalPrimary, &finalCloned)) {
        if (finalCloned) {
            EnsureExtendedTopologyForVirtualScreen(devName, alreadyAttached);
        }
        ApplyExtendedLayoutPreservingPrimary(anchorPrimaryDev, devName, 1920, 1080);
    }

    DEVMODEW verifyDm{};
    verifyDm.dmSize = sizeof(verifyDm);
    if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &verifyDm, 0) ||
        verifyDm.dmPelsWidth != 1920 || verifyDm.dmPelsHeight != 1080) {
        wchar_t msg[256]{};
        swprintf_s(msg, L"Could not attach %ls at 1920x1080 on Windows Desktop (got %ux%u).",
                   devName.c_str(), verifyDm.dmPelsWidth, verifyDm.dmPelsHeight);
        g_lastVddError = msg;
        Log("LoadVirtualScreen failed to activate %ls at 1920x1080 (got %ux%u)",
            devName.c_str(), verifyDm.dmPelsWidth, verifyDm.dmPelsHeight);
        return false;
    }

    g_virtualMonitorDev = devName;
    g_virtualMonitorRect = {
        verifyDm.dmPosition.x,
        verifyDm.dmPosition.y,
        verifyDm.dmPosition.x + static_cast<LONG>(verifyDm.dmPelsWidth),
        verifyDm.dmPosition.y + static_cast<LONG>(verifyDm.dmPelsHeight)
    };
    g_virtualScreenLoaded = true;
    g_projectedMonitorRect = g_virtualMonitorRect;
    g_selectedScreenLabel = (targetVddCount > 1) ? L"Second Virtual Screen (1920x1080)" : L"Virtual Screen (1920x1080)";
    g_controlVirtualScreen = true;
    Log("Loaded Virtual Screen %ls at (%ld,%ld) %ux%u (Anchor primary=%ls, baseVdd=%d)",
        g_virtualMonitorDev.c_str(),
        g_virtualMonitorRect.left, g_virtualMonitorRect.top,
        verifyDm.dmPelsWidth, verifyDm.dmPelsHeight,
        anchorPrimaryDev.c_str(), g_vddBaseCountBeforeLoad);
    return true;
}

void RescueWindowsFromRect(const RECT& vrect) {
    if (vrect.right <= vrect.left || vrect.bottom <= vrect.top) return;
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea = { 0, 0, 1280, 720 };
    }
    struct RescueCtx {
        RECT vrect;
        RECT workArea;
    } ctx{ vrect, workArea };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        const auto* c = reinterpret_cast<const RescueCtx*>(lp);
        if (hwnd == g_hwnd || !IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[64]{};
        if (GetWindowTextW(hwnd, title, 64) <= 0) return TRUE;
        if (wcscmp(title, L"Program Manager") == 0) return TRUE;

        RECT r{};
        if (IsIconic(hwnd)) {
            WINDOWPLACEMENT wp{ sizeof(wp) };
            if (GetWindowPlacement(hwnd, &wp)) r = wp.rcNormalPosition;
        } else {
            GetWindowRect(hwnd, &r);
        }
        LONG cx = (r.left + r.right) / 2;
        LONG cy = (r.top + r.bottom) / 2;
        if (cx >= c->vrect.left && cx < c->vrect.right &&
            cy >= c->vrect.top && cy < c->vrect.bottom) {
            ShowWindow(hwnd, SW_RESTORE);
            int w = (std::min)(1280, static_cast<int>(c->workArea.right - c->workArea.left - 80));
            int h = (std::min)(720, static_cast<int>(c->workArea.bottom - c->workArea.top - 80));
            w = (std::max)(480, w);
            h = (std::max)(360, h);
            SetWindowPos(hwnd, HWND_TOP, c->workArea.left + 60, c->workArea.top + 60, w, h,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void UnloadVirtualScreen() {
    g_controlVirtualScreen = false;
    g_lastVirtualTargetHwnd = nullptr;
    if (!g_virtualScreenLoaded && g_virtualMonitorDev.empty()) return;

    RescueWindowsFromRect(g_virtualMonitorRect);

    std::wstring devName = g_virtualMonitorDev;
    if (!devName.empty()) {
        bool attached = false;
        if (IsVirtualDisplayClonedOrPrimary(devName)) {
            SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_SAVE_TO_DATABASE);
            Sleep(400);
            std::wstring refreshed = FindVirtualDisplayDeviceName(&attached);
            if (!refreshed.empty()) devName = refreshed;
        }
        if (!IsVirtualDisplayClonedOrPrimary(devName)) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
            dm.dmPelsWidth = 0;
            dm.dmPelsHeight = 0;
            LONG res = ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr,
                                                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            if (res != DISP_CHANGE_SUCCESSFUL) {
                ChangeDisplaySettingsExW(devName.c_str(), &dm, nullptr, CDS_NORESET, nullptr);
            }
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        }
        if (g_vddBaseCountBeforeLoad > 0) {
            bool dummyUpdated = false;
            EnsureVddSettingsFile(FindBundledVddInfPath(), &dummyUpdated, g_vddBaseCountBeforeLoad);
            wchar_t restoreCmd[32]{};
            swprintf_s(restoreCmd, L"SETDISPLAYCOUNT %d", g_vddBaseCountBeforeLoad);
            SendMttVddCommand(restoreCmd);
        } else {
            SendMttVddCommand(L"SETDISPLAYCOUNT 0");
        }
        Log("Unloaded Virtual Screen %ls (restored base VDD count=%d)", devName.c_str(), g_vddBaseCountBeforeLoad);
    }

    g_virtualScreenLoaded = false;
    g_virtualMonitorDev.clear();
    g_virtualMonitorRect = {};
    g_vddBaseCountBeforeLoad = 0;
}

void StopAllProjectionAndUnloadVirtual() {
    ReleaseProjectedAppResources();
    g_controlVirtualScreen = false;
    g_selectedScreenLabel.clear();
    g_projectedMonitorRect = {};
    g_projectionMode = ProjectionMode::None;
    UnloadVirtualScreen();
    ClearSharedDesktopToBlack();
    SyncPointerAndLook();
}

bool CaptureProjectedMonitorFrame() {
    const LONG wLong = g_projectedMonitorRect.right - g_projectedMonitorRect.left;
    const LONG hLong = g_projectedMonitorRect.bottom - g_projectedMonitorRect.top;
    if (wLong <= 32 || hLong <= 32) return false;
    const uint32_t w = static_cast<uint32_t>(wLong);
    const uint32_t h = static_cast<uint32_t>(hLong);

    if (!EnsureFullWinBuffers(w, h)) return false;
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    BOOL ok = BitBlt(g_fullWinMemDc, 0, 0, static_cast<int>(w), static_cast<int>(h),
                     hdcScreen, g_projectedMonitorRect.left, g_projectedMonitorRect.top,
                     SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok || !g_fullWinBitmapBits) return false;

    // Draw real OS cursor onto Virtual/Projected monitor frame if currently inside it
    CURSORINFO ci{ sizeof(ci) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0 && ci.hCursor) {
        if (ci.ptScreenPos.x >= g_projectedMonitorRect.left &&
            ci.ptScreenPos.x < g_projectedMonitorRect.right &&
            ci.ptScreenPos.y >= g_projectedMonitorRect.top &&
            ci.ptScreenPos.y < g_projectedMonitorRect.bottom) {
            ICONINFO ii{};
            int hotX = 0, hotY = 0;
            if (GetIconInfo(ci.hCursor, &ii)) {
                hotX = static_cast<int>(ii.xHotspot);
                hotY = static_cast<int>(ii.yHotspot);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
            DrawIconEx(g_fullWinMemDc,
                       ci.ptScreenPos.x - g_projectedMonitorRect.left - hotX,
                       ci.ptScreenPos.y - g_projectedMonitorRect.top - hotY,
                       ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        }
    }

    if (!EnsureProjectedTexture(w, h)) return false;

    const size_t pixelCount = static_cast<size_t>(w) * h;
    if (g_croppedBits.size() < pixelCount) {
        g_croppedBits.resize(pixelCount);
    }
    const uint32_t* srcBits = static_cast<const uint32_t*>(g_fullWinBitmapBits);
    for (size_t i = 0; i < pixelCount; ++i) {
        g_croppedBits[i] = srcBits[i] | 0xFF000000u;
    }

    D3D11_BOX box{ 0, 0, 0, w, h, 1 };
    g_context->UpdateSubresource(g_projectedTexture, 0, &box, g_croppedBits.data(), w * 4, 0);
    g_projectedWidth = w;
    g_projectedHeight = h;

    RenderVideoToSharedDesktop();
    return true;
}

void ShowMoveToVirtualMenu(HWND parentHwnd, float clickX, float clickY);

void ShowProjectScreenMenu(HWND parentHwnd, float clickX, float clickY) {
    bool onlyVddMonitors = false;
    auto screens = EnumerateSelectableScreens(&onlyVddMonitors);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"--- Choose Screen to Project Privately ---");
    if (onlyVddMonitors) {
        AppendMenuW(hMenu, MF_STRING, 2000, L"Install Second 1920x1080 Screen  [Add Virtual Display]");
    } else {
        AppendMenuW(hMenu, MF_STRING, 2000, L"Virtual Screen (1920x1080)  [Isolated Private Display]");
    }
    if (!screens.empty()) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < screens.size() && i < 16; ++i) {
            AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(2001 + i), screens[i].label.c_str());
        }
    }

    POINT pt{ static_cast<LONG>(clickX), static_cast<LONG>(clickY) };
    ClientToScreen(parentHwnd, &pt);
    g_inProjectAppMenu = true;
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, parentHwnd, nullptr);
    g_inProjectAppMenu = false;
    DestroyMenu(hMenu);

    if (!CanProjectInCurrentRoom()) return;

    if (cmd == 2000) {
        if (LoadVirtualScreen()) {
            ReleaseProjectedAppResources();
            g_projectionMode = ProjectionMode::VirtualScreen;
            g_lookYaw = 0.0f;
            g_lookPitch = 0.0f;
            g_lookZoom = 0.0f;
            SyncPointerAndLook();
            CaptureProjectedMonitorFrame();
            SetLcdNotification(L"PROJECTING ON",
                               onlyVddMonitors ? L"VIRTUAL SCREEN #2" : L"VIRTUAL SCREEN", 4000);
            Log("Projection started on Virtual Screen (1920x1080)");
        } else {
            const wchar_t* errText = !g_lastVddError.empty()
                ? g_lastVddError.c_str()
                : L"Could not activate the Virtual Display Driver (1920x1080).";
            SetLcdNotification(L"VIRTUAL SCREEN", L"LOAD FAILED", 3500);
            MessageBoxW(parentHwnd, errText, L"Virtual Screen Unavailable", MB_OK | MB_ICONWARNING);
        }
    } else if (cmd >= 2001 && static_cast<size_t>(cmd - 2001) < screens.size()) {
        UnloadVirtualScreen();
        ReleaseProjectedAppResources();
        const size_t idx = static_cast<size_t>(cmd - 2001);
        const auto& sel = screens[idx];
        g_projectedMonitorRect = sel.rect;
        g_selectedScreenLabel = sel.label;
        g_projectionMode = ProjectionMode::Screen;
        g_lookYaw = 0.0f;
        g_lookPitch = 0.0f;
        g_lookZoom = 0.0f;
        SyncPointerAndLook();
        CaptureProjectedMonitorFrame();
        wchar_t monBuf[64]{};
        swprintf_s(monBuf, L"MONITOR %zu", idx + 1);
        SetLcdNotification(L"PROJECTING ON", monBuf, 4000);
        Log("Projection started on screen: %ls", sel.label.c_str());
    }
}

void ShowMoveToVirtualMenu(HWND parentHwnd, float clickX, float clickY) {
    if (!g_virtualScreenLoaded) return;
    auto windows = EnumerateSelectableAppWindows();
    HMENU hMenu = CreatePopupMenu();
    if (windows.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"No open app windows found");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"--- Move App Window to Virtual Screen ---");
        for (size_t i = 0; i < windows.size() && i < 25; ++i) {
            std::wstring itemText = windows[i].title;
            if (itemText.length() > 45) {
                itemText = itemText.substr(0, 42) + L"...";
            }
            AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(3000 + i), itemText.c_str());
        }
    }
    POINT pt{ static_cast<LONG>(clickX), static_cast<LONG>(clickY) };
    ClientToScreen(parentHwnd, &pt);
    g_inProjectAppMenu = true;
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, parentHwnd, nullptr);
    g_inProjectAppMenu = false;
    DestroyMenu(hMenu);

    if (cmd >= 3000 && static_cast<size_t>(cmd - 3000) < windows.size()) {
        HWND target = windows[cmd - 3000].hwnd;
        if (target && IsWindow(target)) {
            if (IsIconic(target)) {
                ShowWindow(target, SW_RESTORE);
            }
            const LONG vw = g_virtualMonitorRect.right - g_virtualMonitorRect.left;
            const LONG vh = g_virtualMonitorRect.bottom - g_virtualMonitorRect.top;
            SetWindowPos(target, HWND_TOP, g_virtualMonitorRect.left, g_virtualMonitorRect.top,
                         vw, vh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            ShowWindow(target, SW_MAXIMIZE);
            g_lastVirtualTargetHwnd = target;
            g_controlVirtualScreen = true;
            SetForegroundWindow(parentHwnd);
            Log("Moved '%ls' onto Virtual Screen (%ldx%ld)", windows[cmd - 3000].title.c_str(), vw, vh);
        }
    }
}

bool MapViewerPointToVirtualScreen(float mx, float my, LONG* outX, LONG* outY) {
    if (g_projectionMode != ProjectionMode::VirtualScreen || !g_virtualScreenLoaded) return false;
    const LONG vwMon = g_projectedMonitorRect.right - g_projectedMonitorRect.left;
    const LONG vhMon = g_projectedMonitorRect.bottom - g_projectedMonitorRect.top;
    if (vwMon <= 0 || vhMon <= 0) return false;

    const float vx0 = g_activeViewport.Width > 0.0f ? g_activeViewport.TopLeftX : 0.0f;
    const float vy0 = g_activeViewport.Height > 0.0f ? g_activeViewport.TopLeftY : 0.0f;
    const float vw = g_activeViewport.Width;
    const float vh = g_activeViewport.Height;
    if (vw <= 1.0f || vh <= 1.0f) return false;

    const float u = (mx - vx0) / vw;
    const float v = (my - vy0) / vh;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;

    LONG tx = g_projectedMonitorRect.left + static_cast<LONG>(std::lround(u * static_cast<float>(vwMon - 1)));
    LONG ty = g_projectedMonitorRect.top + static_cast<LONG>(std::lround(v * static_cast<float>(vhMon - 1)));
    tx = std::clamp(tx, g_projectedMonitorRect.left, g_projectedMonitorRect.right - 1);
    ty = std::clamp(ty, g_projectedMonitorRect.top, g_projectedMonitorRect.bottom - 1);
    if (outX) *outX = tx;
    if (outY) *outY = ty;
    return true;
}

HWND FindTargetWindowOnVirtualScreen(LONG tx, LONG ty) {
    HWND ptWnd = WindowFromPoint({ tx, ty });
    if (ptWnd && IsWindow(ptWnd)) {
        HWND root = GetAncestor(ptWnd, GA_ROOT);
        if (root && root != g_hwnd) return root;
    }
    if (g_lastVirtualTargetHwnd && IsWindow(g_lastVirtualTargetHwnd)) {
        return g_lastVirtualTargetHwnd;
    }
    return nullptr;
}

void ForwardVirtualMouseHover(LONG tx, LONG ty) {
    HWND child = WindowFromPoint({ tx, ty });
    if (!child || !IsWindow(child)) return;
    HWND root = GetAncestor(child, GA_ROOT);
    if (root == g_hwnd) return;
    POINT pt{ tx, ty };
    ScreenToClient(child, &pt);
    PostMessageW(child, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
}

void ForwardVirtualMouseInput(DWORD mouseFlags, LONG tx, LONG ty, DWORD mouseData = 0, bool restoreCursor = true) {
    POINT savedPt{};
    if (restoreCursor) GetCursorPos(&savedPt);

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    LONG absX = static_cast<LONG>(((static_cast<double>(tx - vx) + 0.5) * 65535.0) / vw);
    LONG absY = static_cast<LONG>(((static_cast<double>(ty - vy) + 0.5) * 65535.0) / vh);
    absX = std::clamp<LONG>(absX, 0, 65535);
    absY = std::clamp<LONG>(absY, 0, 65535);

    INPUT inp[2]{};
    inp[0].type = INPUT_MOUSE;
    inp[0].mi.dx = absX;
    inp[0].mi.dy = absY;
    inp[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    int count = 1;
    if (mouseFlags != 0) {
        inp[1].type = INPUT_MOUSE;
        inp[1].mi.dx = absX;
        inp[1].mi.dy = absY;
        inp[1].mi.mouseData = mouseData;
        inp[1].mi.dwFlags = mouseFlags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        count = 2;
    }
    SendInput(static_cast<UINT>(count), inp, sizeof(INPUT));

    HWND hitRoot = FindTargetWindowOnVirtualScreen(tx, ty);
    if (hitRoot) g_lastVirtualTargetHwnd = hitRoot;

    if (restoreCursor) {
        SetCursorPos(savedPt.x, savedPt.y);
    }
}

void ForwardVirtualKey(UINT msg, WPARAM wParam, LPARAM lParam) {
    LONG cx = (g_projectedMonitorRect.left + g_projectedMonitorRect.right) / 2;
    LONG cy = (g_projectedMonitorRect.top + g_projectedMonitorRect.bottom) / 2;
    HWND target = FindTargetWindowOnVirtualScreen(cx, cy);
    if (target && IsWindow(target)) {
        PostMessageW(target, msg, wParam, lParam);
    }
}

bool IsBigscreenAudioPid(DWORD pid) {
    if (!pid) return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    wchar_t exePath[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &sz)) {
        const wchar_t* base = wcsrchr(exePath, L'\\');
        const wchar_t* name = base ? (base + 1) : exePath;
        if (_wcsicmp(name, L"Bigscreen.exe") == 0 ||
            _wcsicmp(name, L"BigscreenDesktopViewer.exe") == 0) {
            match = true;
        }
    }
    CloseHandle(hProc);
    return match;
}

bool ControlBigscreenSessionAudio(int action /* 0=query, 1=up, -1=down, 2=toggleMute */) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (coHr == RPC_E_CHANGED_MODE) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(IMMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) || !enumerator) {
        return false;
    }

    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) || !device) {
        enumerator->Release();
        return false;
    }
    enumerator->Release();

    IAudioSessionManager2* mgr = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&mgr))) || !mgr) {
        device->Release();
        return false;
    }
    device->Release();

    IAudioSessionEnumerator* sessEnum = nullptr;
    if (FAILED(mgr->GetSessionEnumerator(&sessEnum)) || !sessEnum) {
        mgr->Release();
        return false;
    }

    int count = 0;
    sessEnum->GetCount(&count);
    bool foundAny = false;
    float newVol = g_bigscreenAudioVol;
    bool newMute = g_bigscreenAudioMuted;
    bool computedTarget = false;

    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(sessEnum->GetSession(i, &ctrl)) || !ctrl) continue;

        IAudioSessionControl2* ctrl2 = nullptr;
        DWORD pid = 0;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctrl2))) && ctrl2) {
            ctrl2->GetProcessId(&pid);
            ctrl2->Release();
        }
        if (IsBigscreenAudioPid(pid)) {
            ISimpleAudioVolume* volIf = nullptr;
            if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volIf))) && volIf) {
                foundAny = true;
                float curVol = 1.0f;
                BOOL curMute = FALSE;
                volIf->GetMasterVolume(&curVol);
                volIf->GetMute(&curMute);

                if (!computedTarget) {
                    newVol = curVol;
                    newMute = (curMute != FALSE);
                    if (action == 1) {
                        newVol = std::clamp(curVol + 0.08f, 0.0f, 1.0f);
                        newMute = false;
                    } else if (action == -1) {
                        newVol = std::clamp(curVol - 0.08f, 0.0f, 1.0f);
                    } else if (action == 2) {
                        newMute = (curMute == FALSE);
                    }
                    computedTarget = true;
                }
                if (action != 0) {
                    volIf->SetMasterVolume(newVol, nullptr);
                    volIf->SetMute(newMute ? TRUE : FALSE, nullptr);
                }
                volIf->Release();
            }
        }
        ctrl->Release();
    }

    sessEnum->Release();
    mgr->Release();

    if (foundAny) {
        g_bigscreenAudioVol = newVol;
        g_bigscreenAudioMuted = newMute;
    } else if (action != 0) {
        if (action == 1) {
            g_bigscreenAudioVol = std::clamp(g_bigscreenAudioVol + 0.08f, 0.0f, 1.0f);
            g_bigscreenAudioMuted = false;
        } else if (action == -1) {
            g_bigscreenAudioVol = std::clamp(g_bigscreenAudioVol - 0.08f, 0.0f, 1.0f);
        } else if (action == 2) {
            g_bigscreenAudioMuted = !g_bigscreenAudioMuted;
        }
    }
    return foundAny;
}

void TriggerRemoteMicToggle() {
    EnsureProjectedVideoIpc();
    if (g_projectedVideoState) {
        g_projectedVideoState->requestMicToggleSeq = ++g_reqMicToggleSeq;
    }
    g_micMuted = !g_micMuted;
    SetLcdNotification(g_micMuted ? L"MICROPHONE MUTED" : L"MICROPHONE ON",
                       g_micMuted ? L"VOICE OFF" : L"VOICE ACTIVE", 3000);
}

void TriggerRemoteMuteAllToggle() {
    EnsureProjectedVideoIpc();
    if (g_projectedVideoState) {
        g_projectedVideoState->requestMuteAllToggleSeq = ++g_reqMuteAllToggleSeq;
    }
    g_allMuted = !g_allMuted;
    SetLcdNotification(g_allMuted ? L"MUTE OTHERS: ON" : L"MUTE OTHERS: OFF",
                       g_allMuted ? L"ROOM USERS MUTED" : L"ROOM AUDIO ACTIVE", 3000);
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

void RequestOwnRoomTransition() {
    EnsureProjectedVideoIpc();
    if (g_projectedVideoState) {
        g_projectedVideoState->magic = bigscreen_projected_video::kMagic;
        g_projectedVideoState->version = bigscreen_projected_video::kVersion;
        g_projectedVideoState->requestOwnRoomSeq = ++g_reqOwnRoomSeq;
    }
    g_waitingOwnRoomForProject = true;
    SetLcdNotification(L"LOADING OWN ROOM", L"PLEASE WAIT...", 5000);
    Log("Requested Bigscreen leave current room -> own room (seq=%llu)", static_cast<unsigned long long>(g_reqOwnRoomSeq));
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
    if (g_brushRainbowBar) { g_brushRainbowBar->Release(); g_brushRainbowBar = nullptr; }
    if (g_brushRemoteBodyGrad) { g_brushRemoteBodyGrad->Release(); g_brushRemoteBodyGrad = nullptr; }
    if (g_brushLcdText) { g_brushLcdText->Release(); g_brushLcdText = nullptr; }
    if (g_brushLcdShine) { g_brushLcdShine->Release(); g_brushLcdShine = nullptr; }
    if (g_brushLcdBorder) { g_brushLcdBorder->Release(); g_brushLcdBorder = nullptr; }
    if (g_brushLcdBg) { g_brushLcdBg->Release(); g_brushLcdBg = nullptr; }
    if (g_brushRemoteShadow) { g_brushRemoteShadow->Release(); g_brushRemoteShadow = nullptr; }
    if (g_brushRemoteBtnRing) { g_brushRemoteBtnRing->Release(); g_brushRemoteBtnRing = nullptr; }
    if (g_brushRemoteBtnHover) { g_brushRemoteBtnHover->Release(); g_brushRemoteBtnHover = nullptr; }
    if (g_brushRemoteBtnFill) { g_brushRemoteBtnFill->Release(); g_brushRemoteBtnFill = nullptr; }
    if (g_brushRemoteWellBg) { g_brushRemoteWellBg->Release(); g_brushRemoteWellBg = nullptr; }
    if (g_brushRemoteBezel) { g_brushRemoteBezel->Release(); g_brushRemoteBezel = nullptr; }
    if (g_brushRemoteLabel) { g_brushRemoteLabel->Release(); g_brushRemoteLabel = nullptr; }
    if (g_brushRemoteGrey) { g_brushRemoteGrey->Release(); g_brushRemoteGrey = nullptr; }
    if (g_brushRemoteCyan) { g_brushRemoteCyan->Release(); g_brushRemoteCyan = nullptr; }
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
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.960f, 0.975f, 1.000f, 1.00f), &g_brushTextWhite);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.569f, 0.659f, 0.765f, 1.00f), &g_brushTextMuted);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.431f, 0.686f, 1.000f, 0.95f), &g_brushTextSection);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.898f, 1.000f, 1.00f), &g_brushAccent);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.863f, 0.196f, 0.196f, 0.85f), &g_brushCloseHover);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.137f, 0.216f, 0.314f, 0.60f), &g_brushSeparator);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.831f, 1.000f, 0.75f), &g_brushPointerOuter);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.831f, 1.000f, 0.95f), &g_brushPointerInner);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.000f, 1.000f, 1.000f, 1.00f), &g_brushPointerActive);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.900f, 0.450f, 1.00f), &g_brushFeedActive);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.450f, 0.520f, 0.620f, 0.80f), &g_brushFeedStandby);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.180f, 0.750f, 0.380f, 1.00f), &g_brushPowerOn);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.880f, 0.220f, 0.220f, 1.00f), &g_brushPowerOff);

    // Physical TV Remote Palette
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.900f, 1.000f, 1.00f), &g_brushRemoteCyan);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.392f, 0.427f, 0.471f, 0.85f), &g_brushRemoteGrey);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.840f, 0.875f, 0.920f, 1.00f), &g_brushRemoteLabel);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.365f, 0.392f, 0.427f, 0.95f), &g_brushRemoteBezel);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.110f, 0.125f, 0.145f, 0.98f), &g_brushRemoteWellBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.145f, 0.165f, 0.190f, 1.00f), &g_brushRemoteBtnFill);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.205f, 0.235f, 0.275f, 1.00f), &g_brushRemoteBtnHover);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.045f, 0.055f, 0.068f, 1.00f), &g_brushRemoteBtnRing);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.000f, 0.000f, 0.000f, 0.55f), &g_brushRemoteShadow);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.055f, 0.114f, 0.184f, 1.00f), &g_brushLcdBg);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.200f, 0.235f, 0.285f, 1.00f), &g_brushLcdBorder);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.450f, 0.650f, 0.900f, 0.12f), &g_brushLcdShine);
    g_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.910f, 0.960f, 1.000f, 1.00f), &g_brushLcdText);

    D2D1_GRADIENT_STOP bodyStops[3] = {
        { 0.0f, D2D1::ColorF(0.220f, 0.245f, 0.275f, 0.98f) },
        { 0.45f, D2D1::ColorF(0.165f, 0.185f, 0.212f, 0.98f) },
        { 1.0f, D2D1::ColorF(0.125f, 0.140f, 0.165f, 0.98f) }
    };
    ID2D1GradientStopCollection* bodyStopCol = nullptr;
    if (SUCCEEDED(g_d2dRenderTarget->CreateGradientStopCollection(bodyStops, 3, &bodyStopCol))) {
        g_d2dRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(240, 620)),
            bodyStopCol, &g_brushRemoteBodyGrad);
        bodyStopCol->Release();
    }

    D2D1_GRADIENT_STOP rbStops[4] = {
        { 0.0f, D2D1::ColorF(0.000f, 0.880f, 1.000f, 1.00f) },
        { 0.35f, D2D1::ColorF(0.250f, 0.560f, 1.000f, 0.95f) },
        { 0.68f, D2D1::ColorF(0.950f, 0.620f, 0.220f, 0.95f) },
        { 1.0f, D2D1::ColorF(0.920f, 0.260f, 0.420f, 0.95f) }
    };
    ID2D1GradientStopCollection* rbStopCol = nullptr;
    if (SUCCEEDED(g_d2dRenderTarget->CreateGradientStopCollection(rbStops, 4, &rbStopCol))) {
        g_d2dRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(200, 0)),
            rbStopCol, &g_brushRainbowBar);
        rbStopCol->Release();
    }

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

    g_dwriteFactory->CreateTextFormat(
        L"Consolas", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &g_textFormatLcd);
    if (g_textFormatLcd) {
        g_textFormatLcd->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatLcd->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 9.8f, L"en-us", &g_textFormatRemoteLbl);
    if (g_textFormatRemoteLbl) {
        g_textFormatRemoteLbl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatRemoteLbl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_EXPANDED, 15.0f, L"en-us", &g_textFormatBrand);
    if (g_textFormatBrand) {
        g_textFormatBrand->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatBrand->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Georgia", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC,
        DWRITE_FONT_STRETCH_NORMAL, 15.5f, L"en-us", &g_textFormatFxIcon);
    if (g_textFormatFxIcon) {
        g_textFormatFxIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFormatFxIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &g_textFormatInfoHdr);
    if (g_textFormatInfoHdr) {
        g_textFormatInfoHdr->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFormatInfoHdr->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    g_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.8f, L"en-us", &g_textFormatInfoBody);
    if (g_textFormatInfoBody) {
        g_textFormatInfoBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFormatInfoBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        g_textFormatInfoBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    return CreateD2DDeviceResources();
}

void DrawTriggerPill(float windowWidth) {
    const float btnSize = 38.0f;
    g_pillRect = D2D1::RectF(windowWidth - btnSize - 16.0f, 16.0f, windowWidth - 16.0f, 16.0f + btnSize);

    const float cx = (g_pillRect.left + g_pillRect.right) * 0.5f;
    const float cy = (g_pillRect.top + g_pillRect.bottom) * 0.5f;
    const float radius = btnSize * 0.5f;

    // Subtle dark circular backdrop so the Cyan remote icon stays clear over bright scenes
    D2D1_ELLIPSE outerCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
    ID2D1SolidColorBrush* bgBrush = g_pillHovered ? g_brushRemoteBtnHover : g_brushLcdBg;
    g_d2dRenderTarget->FillEllipse(&outerCircle, bgBrush);
    g_d2dRenderTarget->DrawEllipse(&outerCircle, g_brushRemoteCyan, g_pillHovered ? 2.1f : 1.5f);

    const float stroke = g_pillHovered ? 1.55f : 1.35f;

    // Two concentric IR signal arcs above remote top nub (matching reference icon)
    if (g_d2dFactory) {
        const float waveOriginY = cy - 4.8f;
        const float arcRadii[2] = { 4.2f, 7.6f };
        for (float rArc : arcRadii) {
            ID2D1PathGeometry* waveGeo = nullptr;
            if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&waveGeo)) && waveGeo) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(waveGeo->Open(&sink)) && sink) {
                    const float dx = rArc * 0.64f;
                    const float dy = rArc * 0.77f;
                    sink->BeginFigure(D2D1::Point2F(cx - dx, waveOriginY - dy), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(cx + dx, waveOriginY - dy),
                        D2D1::SizeF(rArc, rArc), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    sink->Release();
                    g_d2dRenderTarget->DrawGeometry(waveGeo, g_brushRemoteCyan, stroke);
                }
                waveGeo->Release();
            }
        }

        // Small IR emitter half-dome nub on top of remote casing
        ID2D1PathGeometry* nubGeo = nullptr;
        if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&nubGeo)) && nubGeo) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(nubGeo->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(cx - 2.0f, waveOriginY), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 2.0f, waveOriginY),
                    D2D1::SizeF(2.0f, 2.0f), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                sink->Release();
                g_d2dRenderTarget->DrawGeometry(nubGeo, g_brushRemoteCyan, stroke);
            }
            nubGeo->Release();
        }

        // Minimalist vertical remote body: slight top corner radius, rounded U-shaped bottom
        const float rLeft = cx - 6.0f;
        const float rRight = cx + 6.0f;
        const float rTop = cy - 4.6f;
        const float rBot = cy + 12.2f;
        const float topR = 1.6f;
        const float botR = 6.0f;

        ID2D1PathGeometry* bodyGeo = nullptr;
        if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&bodyGeo)) && bodyGeo) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(bodyGeo->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(rLeft + topR, rTop), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(rRight - topR, rTop));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rRight, rTop + topR),
                    D2D1::SizeF(topR, topR), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(rRight, rBot - botR));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rLeft, rBot - botR),
                    D2D1::SizeF(botR, botR), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(rLeft, rTop + topR));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rLeft + topR, rTop),
                    D2D1::SizeF(topR, topR), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                g_d2dRenderTarget->DrawGeometry(bodyGeo, g_brushRemoteCyan, stroke);
            }
            bodyGeo->Release();
        }
    }

    // 2x3 grid of 6 small circular button outlines inside remote body
    const float colX[2] = { cx - 2.35f, cx + 2.35f };
    const float rowY[3] = { cy - 0.8f, cy + 3.1f, cy + 7.0f };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            D2D1_ELLIPSE btnDot = D2D1::Ellipse(D2D1::Point2F(colX[c], rowY[r]), 1.35f, 1.35f);
            g_d2dRenderTarget->DrawEllipse(&btnDot, g_brushRemoteCyan, 1.15f);
        }
    }
}

void DrawPointerReticle(float x, float y, bool active) {
    const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(x, y), 8.0f, 8.0f);
    const D2D1_ELLIPSE inner = D2D1::Ellipse(D2D1::Point2F(x, y), active ? 4.0f : 2.5f, active ? 4.0f : 2.5f);
    
    g_d2dRenderTarget->DrawEllipse(&outer, g_brushPointerOuter, 1.2f);
    g_d2dRenderTarget->FillEllipse(&inner, active ? g_brushPointerActive : g_brushPointerInner);
}

bool IsRemoteBtnSelectable(RemoteBtn btn) {
    switch (btn) {
        case RemoteBtn::Hide:
        case RemoteBtn::Power:
        case RemoteBtn::Menu:
        case RemoteBtn::Fullscreen:
        case RemoteBtn::Info:
        case RemoteBtn::MovieMode:
        case RemoteBtn::TheaterMode:
        case RemoteBtn::VolUp:
        case RemoteBtn::VolMute:
        case RemoteBtn::VolDown:
        case RemoteBtn::AspectRatio:
            return true;
        case RemoteBtn::Settings:
            return !g_movieOnly; // fx D-Pad mode toggle (Panning <-> Zoom) in Theater Mode
        case RemoteBtn::DpadUp:
        case RemoteBtn::DpadDown:
            return !g_movieOnly; // Theater Mode only (Panning look or Zoom forward/out)
        case RemoteBtn::DpadLeft:
        case RemoteBtn::DpadRight:
            return !g_movieOnly && !g_zoomMode; // Panning Mode only (disabled in Zoom Mode or Movie Mode)
        case RemoteBtn::DpadSelect:
            return !g_movieOnly; // Theater Mode (Panning recenter or Zoom reset)
        case RemoteBtn::ProjectScreen:
            return (g_projectionMode != ProjectionMode::None) || CanProjectInCurrentRoom() || CanLeaveToOwnRoomToProject();
        case RemoteBtn::Microphone:
            return g_bigscreenRunning;
        case RemoteBtn::MuteOthers:
            return g_bigscreenRunning && g_remoteUserCount >= 1; // Requires 2+ total users in room
        case RemoteBtn::No3D:
            return g_movieOnly; // Movie Mode only
        default:
            return false;
    }
}

void GetShortAspectLcdText(size_t idx, wchar_t* outBuf, size_t cap) {
    switch (idx) {
        case 1: swprintf_s(outBuf, cap, L"A/R: 2.39:1 ULTRA"); break;
        case 2: swprintf_s(outBuf, cap, L"A/R: 1.85:1 WIDE"); break;
        case 3: swprintf_s(outBuf, cap, L"A/R: 1.78:1 16:9"); break;
        case 4: swprintf_s(outBuf, cap, L"A/R: 1.33:1 4:3"); break;
        case 5: swprintf_s(outBuf, cap, L"A/R: 1.375:1 TV"); break;
        case 6: swprintf_s(outBuf, cap, L"A/R: 2.00:1 UNIVIS"); break;
        case 7: swprintf_s(outBuf, cap, L"A/R: 1.66:1 EURO"); break;
        case 8: swprintf_s(outBuf, cap, L"A/R: 1.90:1 IMAX"); break;
        case 9: swprintf_s(outBuf, cap, L"A/R: 2.20:1 70MM"); break;
        case 10: swprintf_s(outBuf, cap, L"A/R: 1.43:1 IMAX70"); break;
        case 11: swprintf_s(outBuf, cap, L"A/R: 2.76:1 PAN70"); break;
        default: swprintf_s(outBuf, cap, L"A/R: AUTO"); break;
    }
}

void GetRemoteLcdLines(std::wstring& l1, std::wstring& l2) {
    const bool vrRunning = IsVrRunning();
    const ULONGLONG now = GetTickCount64();

    // 1) Hover always explains the hovered button cleanly on the LCD
    switch (g_hoveredRemoteBtn) {
        case RemoteBtn::Hide:
            l1 = L"HIDE REMOTE";
            l2 = L"(TAB / M TO SHOW)";
            return;
        case RemoteBtn::Power:
            l1 = vrRunning ? L"TURN OFF" : L"TURN ON";
            l2 = L"BIGSCREEN";
            return;
        case RemoteBtn::DpadUp:
            if (g_movieOnly) {
                l1 = L"THEATER MODE ONLY";
                l2 = L"(INACTIVE)";
            } else if (g_zoomMode) {
                l1 = L"ZOOM FORWARD";
                l2 = L"(D-PAD UP)";
            } else {
                l1 = L"LOOK UP";
                l2 = L"(ARROW UP)";
            }
            return;
        case RemoteBtn::DpadDown:
            if (g_movieOnly) {
                l1 = L"THEATER MODE ONLY";
                l2 = L"(INACTIVE)";
            } else if (g_zoomMode) {
                l1 = L"ZOOM OUT";
                l2 = L"(D-PAD DOWN)";
            } else {
                l1 = L"LOOK DOWN";
                l2 = L"(ARROW DOWN)";
            }
            return;
        case RemoteBtn::DpadLeft:
            if (g_movieOnly) {
                l1 = L"THEATER MODE ONLY";
                l2 = L"(INACTIVE)";
            } else if (g_zoomMode) {
                l1 = L"DISABLED";
                l2 = L"(ZOOM MODE)";
            } else {
                l1 = L"LOOK LEFT";
                l2 = L"(ARROW LEFT)";
            }
            return;
        case RemoteBtn::DpadRight:
            if (g_movieOnly) {
                l1 = L"THEATER MODE ONLY";
                l2 = L"(INACTIVE)";
            } else if (g_zoomMode) {
                l1 = L"DISABLED";
                l2 = L"(ZOOM MODE)";
            } else {
                l1 = L"LOOK RIGHT";
                l2 = L"(ARROW RIGHT)";
            }
            return;
        case RemoteBtn::DpadSelect:
            if (g_movieOnly) {
                l1 = L"THEATER MODE ONLY";
                l2 = L"(INACTIVE)";
            } else if (g_zoomMode) {
                l1 = L"RESET TO NORMAL";
                l2 = L"VIEW (ZOOM 0)";
            } else {
                l1 = L"RECENTER VIEW";
                l2 = L"(D-PAD SELECT)";
            }
            return;
        case RemoteBtn::Menu:
            l1 = L"BIGSCREEN MENU";
            l2 = L"SHOW / HIDE MENU";
            return;
        case RemoteBtn::Settings:
            if (g_movieOnly) {
                l1 = L"D-PAD FUNCTION";
                l2 = L"(THEATER ONLY)";
            } else if (g_zoomMode) {
                l1 = L"SELECT PANNING";
                l2 = L"D-PAD MODE";
            } else {
                l1 = L"SELECT ZOOM MODE";
                l2 = L"";
            }
            return;
        case RemoteBtn::Fullscreen:
            l1 = g_isFullscreen ? L"MAKE VIEWER" : L"MAKE VIEWER";
            l2 = g_isFullscreen ? L"WINDOWED" : L"FULLSCREEN";
            return;
        case RemoteBtn::ProjectScreen:
            if (g_projectionMode != ProjectionMode::None) {
                l1 = L"STOP PROJECTING";
                l2 = L"END SCREEN FEED";
            } else if (CanProjectInCurrentRoom()) {
                l1 = L"PICK VIRTUAL SCREEN";
                l2 = L"(1920x1080) OR MON";
            } else if (g_isLoadingRoom || g_waitingOwnRoomForProject) {
                l1 = L"LOADING OWN ROOM";
                l2 = L"PLEASE WAIT...";
            } else if (CanLeaveToOwnRoomToProject()) {
                l1 = L"LOAD OWN ROOM";
                l2 = L"PROJECTION ROOM";
            } else {
                l1 = L"PROJECTION ROOM";
                l2 = g_bigscreenRunning ? L"INITIALIZING..." : L"BIGSCREEN OFF";
            }
            return;
        case RemoteBtn::Info:
            l1 = g_infoPanelOpen ? L"RETRACT INFO" : L"VIEWER CONTROLS";
            l2 = g_infoPanelOpen ? L"LCD SCREEN" : L"SLIDE-OUT INFO";
            return;
        case RemoteBtn::MovieMode:
            l1 = L"SWITCH TO";
            l2 = L"MOVIE MODE";
            return;
        case RemoteBtn::TheaterMode:
            l1 = L"SWITCH TO";
            l2 = L"THEATER MODE";
            return;
        case RemoteBtn::VolUp: {
            wchar_t b[32]{};
            swprintf_s(b, L"CURRENT: %d%%", static_cast<int>(std::round(g_bigscreenAudioVol * 100.0f)));
            l1 = L"BIGSCREEN VOL UP";
            l2 = b;
            return;
        }
        case RemoteBtn::VolMute:
            l1 = g_bigscreenAudioMuted ? L"UNMUTE BIGSCREEN" : L"MUTE BIGSCREEN";
            l2 = L"APP AUDIO ONLY";
            return;
        case RemoteBtn::VolDown: {
            wchar_t b[32]{};
            swprintf_s(b, L"CURRENT: %d%%", static_cast<int>(std::round(g_bigscreenAudioVol * 100.0f)));
            l1 = L"BIGSCREEN VOL DOWN";
            l2 = b;
            return;
        }
        case RemoteBtn::Microphone:
            if (!g_bigscreenRunning) {
                l1 = L"MICROPHONE";
                l2 = L"BIGSCREEN OFF";
            } else {
                l1 = g_micMuted ? L"UNMUTE MICROPHONE" : L"MUTE MICROPHONE";
                l2 = g_micMuted ? L"(MIC OFF)" : L"(MIC ON)";
            }
            return;
        case RemoteBtn::MuteOthers:
            if (!g_bigscreenRunning || g_remoteUserCount == 0) {
                l1 = L"MUTE OTHERS";
                l2 = L"(NEED 2+ USERS)";
            } else {
                l1 = g_allMuted ? L"UNMUTE ALL USERS" : L"MUTE ALL USERS";
                l2 = g_allMuted ? L"(PEERS MUTED)" : L"(PEERS AUDIBLE)";
            }
            return;
        case RemoteBtn::No3D:
            if (!g_movieOnly) {
                l1 = L"NO 3D CONVERSION";
                l2 = L"(MOVIE MODE ONLY)";
            } else if (g_stereoMode == StereoSplitMode::SBS) {
                l1 = L"NO 3D: SBS -> 2D";
                l2 = L"CLICK FOR O/U -> 2D";
            } else if (g_stereoMode == StereoSplitMode::OU) {
                l1 = L"NO 3D: O/U -> 2D";
                l2 = L"CLICK FOR NORMAL 2D";
            } else {
                l1 = L"NO 3D: NORMAL 2D";
                l2 = L"CONVERT 3D TO 2D";
            }
            return;
        case RemoteBtn::AspectRatio: {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l1 = L"CYCLE ASPECT RATIO";
            l2 = arBuf;
            return;
        }
        default:
            break;
    }

    // 2) Recent click status feedback
    if (now < g_lcdTransientUntilMs && !g_lcdTransientLine1.empty()) {
        l1 = g_lcdTransientLine1;
        if (g_aspectPresetIndex > 0 && g_lcdTransientLine2.empty()) {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l2 = arBuf;
        } else {
            l2 = g_lcdTransientLine2;
        }
        return;
    }

    // 3) Steady-state mode / projection / aspect ratio communication
    if (g_projectionMode == ProjectionMode::VirtualScreen) {
        l1 = L"PROJECTING ON";
        if (g_aspectPresetIndex > 0) {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l2 = arBuf;
        } else {
            l2 = L"VIRTUAL SCREEN";
        }
    } else if (g_projectionMode == ProjectionMode::Screen) {
        l1 = L"PROJECTING ON";
        if (g_aspectPresetIndex > 0) {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l2 = arBuf;
        } else {
            // Extract concise "MONITOR N" from g_selectedScreenLabel
            std::wstring shortMon = L"MONITOR";
            if (!g_selectedScreenLabel.empty()) {
                size_t sp = g_selectedScreenLabel.find(L"  ");
                shortMon = (sp != std::wstring::npos) ? g_selectedScreenLabel.substr(0, sp) : g_selectedScreenLabel;
                for (auto& ch : shortMon) ch = static_cast<wchar_t>(towupper(ch));
            }
            l2 = shortMon;
        }
    } else if (g_movieOnly) {
        l1 = L"MOVIE MODE";
        if (g_aspectPresetIndex > 0) {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l2 = arBuf;
        } else if (g_stereoMode == StereoSplitMode::SBS) {
            l2 = L"NO 3D: SBS -> 2D";
        } else if (g_stereoMode == StereoSplitMode::OU) {
            l2 = L"NO 3D: O/U -> 2D";
        } else {
            l2 = L"SELECTED";
        }
    } else {
        l1 = L"THEATER MODE";
        if (g_aspectPresetIndex > 0) {
            wchar_t arBuf[48]{};
            GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
            l2 = arBuf;
        } else {
            l2 = L"SELECTED";
        }
    }
}

RemoteBtn HitTestRemoteButton(float mx, float my) {
    auto inRect = [&](const D2D1_RECT_F& r) {
        return mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom;
    };
    auto inCircle = [&](const D2D1_RECT_F& r) {
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float rad = (r.right - r.left) * 0.5f + 2.0f;
        float dx = mx - cx, dy = my - cy;
        return (dx * dx + dy * dy) <= (rad * rad);
    };

    if (inCircle(g_rBtnHide)) return RemoteBtn::Hide;
    if (inCircle(g_rBtnPower)) return RemoteBtn::Power;
    if (inCircle(g_rBtnMenu)) return RemoteBtn::Menu;
    if (inCircle(g_rBtnSettings)) return RemoteBtn::Settings;

    // D-pad circle + inner Select circle
    {
        float dx = mx - g_dpadCenter.x;
        float dy = my - g_dpadCenter.y;
        float dist2 = dx * dx + dy * dy;
        if (dist2 <= g_dpadInnerR * g_dpadInnerR) {
            return RemoteBtn::DpadSelect;
        }
        if (dist2 <= g_dpadOuterR * g_dpadOuterR) {
            if (std::fabs(dx) > std::fabs(dy)) {
                return (dx < 0.0f) ? RemoteBtn::DpadLeft : RemoteBtn::DpadRight;
            } else {
                return (dy < 0.0f) ? RemoteBtn::DpadUp : RemoteBtn::DpadDown;
            }
        }
    }

    if (inCircle(g_rBtnFullscreen)) return RemoteBtn::Fullscreen;
    if (inCircle(g_rBtnProject)) return RemoteBtn::ProjectScreen;
    if (inCircle(g_rBtnInfo)) return RemoteBtn::Info;

    if (inRect(g_rBtnMovie)) return RemoteBtn::MovieMode;
    if (inRect(g_rBtnTheater)) return RemoteBtn::TheaterMode;

    if (inRect(g_rBtnVolPill)) {
        if (my < g_rBtnVolMute.top) return RemoteBtn::VolUp;
        if (my > g_rBtnVolMute.bottom) return RemoteBtn::VolDown;
        return RemoteBtn::VolMute;
    }

    if (inCircle(g_rBtnMic)) return RemoteBtn::Microphone;
    if (inCircle(g_rBtnMuteOthers)) return RemoteBtn::MuteOthers;
    if (inCircle(g_rBtnNo3D)) return RemoteBtn::No3D;
    if (inCircle(g_rBtnAspect)) return RemoteBtn::AspectRatio;

    return RemoteBtn::None;
}

ID2D1SolidColorBrush* GetRemoteIconBrush(bool activeCyan, bool enabled) {
    if (!enabled) return g_brushRemoteGrey;
    if (activeCyan) return g_brushRemoteCyan;
    return g_brushTextWhite;
}

void DrawRemoteCirclePad(const D2D1_RECT_F& rect, RemoteBtn btnId, bool enabled) {
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float r = (rect.right - rect.left) * 0.5f;
    const bool hovered = (g_hoveredRemoteBtn == btnId) && enabled;

    D2D1_ELLIPSE shadowEl = D2D1::Ellipse(D2D1::Point2F(cx, cy + 2.0f), r + 1.5f, r + 1.5f);
    g_d2dRenderTarget->FillEllipse(&shadowEl, g_brushRemoteShadow);

    D2D1_ELLIPSE ringEl = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 1.0f, r + 1.0f);
    g_d2dRenderTarget->FillEllipse(&ringEl, g_brushRemoteBtnRing);

    D2D1_ELLIPSE faceEl = D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 0.5f, r - 0.5f);
    g_d2dRenderTarget->FillEllipse(&faceEl, hovered ? g_brushRemoteBtnHover : g_brushRemoteBtnFill);
    g_d2dRenderTarget->DrawEllipse(&faceEl, hovered ? g_brushRemoteCyan : g_brushRemoteBezel, hovered ? 1.1f : 0.7f);
}

void GetInfoSlideOutContent(RemoteBtn hovered, std::wstring& outTitle, std::wstring& outBody, bool& outIsDefault) {
    outIsDefault = (hovered == RemoteBtn::None || hovered == RemoteBtn::Info);
    if (outIsDefault) {
        outTitle = L"MOUSE & KEYBOARD CONTROLS";
        outBody =
            L"Mouse Move\n  VR laser pointer / Virtual Screen cursor\n\n"
            L"Left-Click\n  VR trigger / Click Virtual Screen\n\n"
            L"Right-Click Drag\n  Look around in Theater Mode\n\n"
            L"Esc\n  Virtual Screen <-> VR Laser / Menu\n\n"
            L"R or Y\n  Recenter VR view\n\n"
            L"Tab / M / F1\n  Show or hide Remote\n\n"
            L"F11\n  Toggle Fullscreen / Windowed\n\n"
            L"Drag Remote Body\n  Move Remote inside viewer\n\n"
            L"Alt + Drag\n  Move Viewer window";
        return;
    }

    switch (hovered) {
        case RemoteBtn::Hide:
            outTitle = L"HIDE REMOTE";
            outBody = L"Hides the floating remote control.\n\nPress Tab, M, F1, or click the Cyan remote icon in the top-right corner to bring it back.";
            break;
        case RemoteBtn::Power:
            outTitle = L"PWR (POWER)";
            outBody = L"Turns Bigscreen VR and SteamVR on or off.\n\nTurning off also stops active screen projection and unloads any Virtual Screen.";
            break;
        case RemoteBtn::Menu:
            outTitle = L"BIGSCREEN MENU";
            outBody = L"Shows or minimizes the Bigscreen VR desktop window and main menu.";
            break;
        case RemoteBtn::Settings:
            outTitle = L"fx (D-PAD MODE)";
            outBody = L"Switches the D-Pad between Panning Mode (default) and Zoom Mode in Theater Mode.\n\n"
                      L"Glows White in Panning Mode and Cyan when Zoom Mode is active.";
            break;
        case RemoteBtn::DpadUp:
            outTitle = g_zoomMode ? L"D-PAD UP (ZOOM MODE)" : L"D-PAD UP (PANNING)";
            outBody = g_zoomMode
                ? L"Moves your camera forward toward the screen in Theater Mode.\n\nDisabled in Movie Mode."
                : L"Tilts your view upward in Theater Mode (matches Up Arrow key).\n\nDisabled in Movie Mode.";
            break;
        case RemoteBtn::DpadDown:
            outTitle = g_zoomMode ? L"D-PAD DOWN (ZOOM MODE)" : L"D-PAD DOWN (PANNING)";
            outBody = g_zoomMode
                ? L"Moves your camera backward away from the screen in Theater Mode.\n\nDisabled in Movie Mode."
                : L"Tilts your view downward in Theater Mode (matches Down Arrow key).\n\nDisabled in Movie Mode.";
            break;
        case RemoteBtn::DpadLeft:
            outTitle = L"D-PAD LEFT";
            outBody = g_zoomMode
                ? L"Disabled while Zoom Mode is active.\n\nPress fx D-Pad to return to Panning Mode to look left."
                : L"Pans your view to the left in Theater Mode (matches Left Arrow key).\n\nDisabled in Movie Mode.";
            break;
        case RemoteBtn::DpadRight:
            outTitle = L"D-PAD RIGHT";
            outBody = g_zoomMode
                ? L"Disabled while Zoom Mode is active.\n\nPress fx D-Pad to return to Panning Mode to look right."
                : L"Pans your view to the right in Theater Mode (matches Right Arrow key).\n\nDisabled in Movie Mode.";
            break;
        case RemoteBtn::DpadSelect:
            outTitle = L"D-PAD SELECT";
            outBody = g_zoomMode
                ? L"Resets your camera zoom back to normal default view.\n\nDisabled in Movie Mode."
                : L"Recenters your VR view in Theater Mode.\n\nDisabled in Movie Mode.";
            break;
        case RemoteBtn::Fullscreen:
            outTitle = L"FULL SCREEN / WINDOWED";
            outBody = L"Toggles the viewer window between Fullscreen and normal Windowed mode (shortcut: F11).";
            break;
        case RemoteBtn::ProjectScreen:
            outTitle = L"PROJECTION ROOM";
            outBody = L"Click to project a connected monitor or Virtual Screen (1920x1080) onto your Bigscreen theater screen.\n\n"
                      L"Hold for 2 seconds to choose a running video app to project.\n\n"
                      L"In another user's room, click once to load your own room first.";
            break;
        case RemoteBtn::MovieMode:
            outTitle = L"MOVIE MODE";
            outBody = L"Displays only the video full-screen in the viewer window, hiding the surrounding VR theater.";
            break;
        case RemoteBtn::TheaterMode:
            outTitle = L"THEATER MODE";
            outBody = L"Displays the full VR theater environment around the screen (enabled by default).";
            break;
        case RemoteBtn::VolUp:
            outTitle = L"VOLUME UP (+)";
            outBody = L"Increases audio volume for the Bigscreen app only, without changing your other Windows apps.";
            break;
        case RemoteBtn::VolMute:
            outTitle = L"MUTE BIGSCREEN AUDIO";
            outBody = L"Mutes or unmutes audio from the Bigscreen app only.\n\nGlows Cyan while muted.";
            break;
        case RemoteBtn::VolDown:
            outTitle = L"VOLUME DOWN (-)";
            outBody = L"Decreases audio volume for the Bigscreen app only.";
            break;
        case RemoteBtn::Microphone:
            outTitle = L"MICROPHONE";
            outBody = L"Turns your VR room microphone on or off.\n\nOn by default (glowing Cyan). Click to mute your mic.";
            break;
        case RemoteBtn::MuteOthers:
            outTitle = L"MUTE OTHERS";
            outBody = L"Mutes or unmutes all other users in the room when 2 or more users are present.\n\nGlows Cyan while others are muted.";
            break;
        case RemoteBtn::No3D:
            outTitle = L"NO 3D CONVERSION";
            outBody = L"Converts 3D Side-by-Side (SBS) or Over/Under (O/U) video to normal 2D.\n\nAvailable in Movie Mode only.";
            break;
        case RemoteBtn::AspectRatio:
            outTitle = L"A/R (ASPECT RATIO)";
            outBody = L"Cycles through video aspect ratio presets (Auto, 2.39:1, 16:9, 4:3, IMAX, etc.).\n\nAutomatically resets to Auto when changing rooms.";
            break;
        default:
            outTitle = L"MOUSE & KEYBOARD CONTROLS";
            outBody = L"";
            break;
    }
}

void DrawRemoteInfoSlideOut(float remoteLeft, float remoteTop, float remoteBottom) {
    const float slideW = 264.0f;
    const float overlapW = 24.0f;
    const float visibleW = (slideW - overlapW) * g_infoSlideProgress;
    if (visibleW <= 1.0f) {
        g_infoPanelRect = D2D1::RectF(0, 0, 0, 0);
        return;
    }

    const float pTop = remoteTop + 8.0f;
    const float pBottom = remoteBottom - 8.0f;
    const float pLeft = remoteLeft - visibleW;
    const float pRight = remoteLeft + overlapW;
    g_infoPanelRect = D2D1::RectF(pLeft, pTop, remoteLeft, pBottom);

    // Drop shadow and brushed metallic casing matching the remote shell (connected behind remote left edge)
    D2D1_ROUNDED_RECT shadowRect = D2D1::RoundedRect(
        D2D1::RectF(pLeft + 2.0f, pTop + 4.0f, pRight, pBottom + 5.0f), 22.0f, 22.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&shadowRect, g_brushRemoteShadow);

    D2D1_ROUNDED_RECT shellRect = D2D1::RoundedRect(
        D2D1::RectF(pLeft, pTop, pRight, pBottom), 22.0f, 22.0f);
    g_d2dRenderTarget->FillRoundedRectangle(
        &shellRect,
        g_brushRemoteBodyGrad ? static_cast<ID2D1Brush*>(g_brushRemoteBodyGrad) : g_brushPanelBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&shellRect, g_brushRemoteBezel, 1.6f);

    // Subtle Astroslide hinge/slider rail seam near right edge where panel enters remote casing
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(remoteLeft - 2.0f, pTop + 20.0f),
        D2D1::Point2F(remoteLeft - 2.0f, pBottom - 20.0f),
        g_brushRemoteBtnRing, 2.0f);

    // Large recessed LCD glass screen inside slide-out panel
    D2D1_RECT_F lcdOuterRect = D2D1::RectF(pLeft + 10.0f, pTop + 12.0f, remoteLeft - 8.0f, pBottom - 12.0f);
    if (lcdOuterRect.right - lcdOuterRect.left < 40.0f) return;

    D2D1_ROUNDED_RECT lcdOuter = D2D1::RoundedRect(lcdOuterRect, 12.0f, 12.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&lcdOuter, g_brushLcdBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&lcdOuter, g_brushLcdBorder, 1.5f);

    // Diagonal glass reflection highlight across top-right of slide-out LCD
    if (g_d2dFactory) {
        ID2D1PathGeometry* shineGeo = nullptr;
        if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&shineGeo)) && shineGeo) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(shineGeo->Open(&sink)) && sink) {
                const float lw = lcdOuterRect.right - lcdOuterRect.left;
                sink->BeginFigure(D2D1::Point2F(lcdOuterRect.left + lw * 0.45f, lcdOuterRect.top + 1.5f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(lcdOuterRect.right - 2.0f, lcdOuterRect.top + 1.5f));
                sink->AddLine(D2D1::Point2F(lcdOuterRect.right - 2.0f, lcdOuterRect.top + 150.0f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                g_d2dRenderTarget->FillGeometry(shineGeo, g_brushLcdShine);
            }
            shineGeo->Release();
        }
    }

    // Clip all LCD text to the LCD interior while sliding
    g_d2dRenderTarget->PushAxisAlignedClip(
        D2D1::RectF(lcdOuterRect.left + 4.0f, lcdOuterRect.top + 4.0f, lcdOuterRect.right - 4.0f, lcdOuterRect.bottom - 4.0f),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    std::wstring infoTitle, infoBody;
    bool isDefaultView = true;
    GetInfoSlideOutContent(g_hoveredRemoteBtn, infoTitle, infoBody, isDefaultView);

    const float cLeft = lcdOuterRect.left + 12.0f;
    const float cRight = lcdOuterRect.right - 12.0f;
    float yCursor = lcdOuterRect.top + 12.0f;

    if (isDefaultView) {
        D2D1_RECT_F promptRect = D2D1::RectF(cLeft, yCursor, cRight, yCursor + 36.0f);
        if (g_textFormatInfoBody) {
            g_d2dRenderTarget->DrawText(
                L"Move mouse over remote buttons for information", 47,
                g_textFormatInfoBody, &promptRect, g_brushRemoteCyan);
        }
        yCursor += 40.0f;
        g_d2dRenderTarget->DrawLine(
            D2D1::Point2F(cLeft, yCursor),
            D2D1::Point2F(cRight, yCursor),
            g_brushLcdBorder, 1.0f);
        yCursor += 10.0f;
    }

    D2D1_RECT_F hdrRect = D2D1::RectF(cLeft, yCursor, cRight, yCursor + 24.0f);
    if (g_textFormatInfoHdr) {
        g_d2dRenderTarget->DrawText(
            infoTitle.c_str(), static_cast<UINT32>(infoTitle.length()),
            g_textFormatInfoHdr, &hdrRect, g_brushRemoteCyan);
    }
    yCursor += 28.0f;

    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(cLeft, yCursor),
        D2D1::Point2F(cRight, yCursor),
        g_brushLcdBorder, 1.0f);
    yCursor += 12.0f;

    D2D1_RECT_F bodyRect = D2D1::RectF(cLeft, yCursor, cRight, lcdOuterRect.bottom - 34.0f);
    if (g_textFormatInfoBody && !infoBody.empty()) {
        g_d2dRenderTarget->DrawText(
            infoBody.c_str(), static_cast<UINT32>(infoBody.length()),
            g_textFormatInfoBody, &bodyRect, g_brushLcdText);
    }

    D2D1_RECT_F footRect = D2D1::RectF(cLeft, lcdOuterRect.bottom - 26.0f, cRight, lcdOuterRect.bottom - 8.0f);
    g_d2dRenderTarget->DrawText(
        L"Click Info button or screen to close", 36,
        g_textFormatRemoteLbl, &footRect, g_brushRemoteLabel);

    g_d2dRenderTarget->PopAxisAlignedClip();
}

void DrawRemotePanel(float windowWidth, float windowHeight) {
    const float panelWidth = 236.0f;
    const float panelHeight = 610.0f;

    // Smoothly animate Astroslide Info LCD slide-out
    const float targetSlide = g_infoPanelOpen ? 1.0f : 0.0f;
    if (g_infoSlideProgress < targetSlide) {
        g_infoSlideProgress = (std::min)(targetSlide, g_infoSlideProgress + 0.16f);
    } else if (g_infoSlideProgress > targetSlide) {
        g_infoSlideProgress = (std::max)(targetSlide, g_infoSlideProgress - 0.16f);
    }

    if (!g_remotePosInit) {
        g_remotePosX = windowWidth - panelWidth - 20.0f;
        g_remotePosY = (std::max)(20.0f, (windowHeight - panelHeight) * 0.5f);
        g_remotePosInit = true;
    }
    const float neededLeftClearance = g_infoPanelOpen ? 246.0f : 6.0f;
    if (g_infoPanelOpen && g_remotePosX < neededLeftClearance) {
        g_remotePosX = (std::min)(neededLeftClearance, (std::max)(6.0f, windowWidth - panelWidth - 6.0f));
    }
    g_remotePosX = std::clamp(g_remotePosX, 6.0f, (std::max)(6.0f, windowWidth - panelWidth - 6.0f));
    g_remotePosY = std::clamp(g_remotePosY, 6.0f, (std::max)(6.0f, windowHeight - panelHeight - 6.0f));

    const float left = g_remotePosX;
    const float top = g_remotePosY;
    const float right = left + panelWidth;
    const float bottom = top + panelHeight;
    g_remoteRect = D2D1::RectF(left, top, right, bottom);

    // Draw connected Astroslide LCD panel first so it slides smoothly behind the remote left casing
    DrawRemoteInfoSlideOut(left, top, bottom);

    // Outer drop shadow + brushed metallic remote casing
    D2D1_ROUNDED_RECT shadowRect = D2D1::RoundedRect(
        D2D1::RectF(left + 3.0f, top + 5.0f, right + 4.0f, bottom + 6.0f), 26.0f, 26.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&shadowRect, g_brushRemoteShadow);

    if (g_brushRemoteBodyGrad) {
        g_brushRemoteBodyGrad->SetStartPoint(D2D1::Point2F(left, top));
        g_brushRemoteBodyGrad->SetEndPoint(D2D1::Point2F(right, bottom));
    }
    D2D1_ROUNDED_RECT shellRect = D2D1::RoundedRect(g_remoteRect, 26.0f, 26.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&shellRect, g_brushRemoteBodyGrad ? static_cast<ID2D1Brush*>(g_brushRemoteBodyGrad) : g_brushPanelBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&shellRect, g_brushRemoteBezel, 1.8f);

    // Subtle inner rim
    D2D1_ROUNDED_RECT innerRim = D2D1::RoundedRect(
        D2D1::RectF(left + 2.5f, top + 2.5f, right - 2.5f, bottom - 2.5f), 24.0f, 24.0f);
    g_d2dRenderTarget->DrawRoundedRectangle(&innerRim, g_brushRemoteBtnRing, 1.0f);

    // 1. TOP LCD COMMUNICATIONS DISPLAY
    g_lcdRect = D2D1::RectF(left + 16.0f, top + 16.0f, right - 16.0f, top + 72.0f);
    D2D1_ROUNDED_RECT lcdOuter = D2D1::RoundedRect(g_lcdRect, 5.0f, 5.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&lcdOuter, g_brushLcdBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&lcdOuter, g_brushLcdBorder, 1.5f);

    // Subtle diagonal glass highlight across upper-right LCD corner
    if (g_d2dFactory) {
        ID2D1PathGeometry* shineGeo = nullptr;
        if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&shineGeo)) && shineGeo) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(shineGeo->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(g_lcdRect.left + (g_lcdRect.right - g_lcdRect.left) * 0.42f, g_lcdRect.top + 1.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(g_lcdRect.right - 2.0f, g_lcdRect.top + 1.0f));
                sink->AddLine(D2D1::Point2F(g_lcdRect.right - 2.0f, g_lcdRect.bottom - 2.0f));
                sink->AddLine(D2D1::Point2F(g_lcdRect.left + (g_lcdRect.right - g_lcdRect.left) * 0.68f, g_lcdRect.bottom - 2.0f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                g_d2dRenderTarget->FillGeometry(shineGeo, g_brushLcdShine);
            }
            shineGeo->Release();
        }
    }

    std::wstring lcdL1, lcdL2;
    GetRemoteLcdLines(lcdL1, lcdL2);
    std::wstring lcdFull = lcdL2.empty() ? lcdL1 : (lcdL1 + L"\n" + lcdL2);
    D2D1_RECT_F lcdTextRect = D2D1::RectF(g_lcdRect.left + 6.0f, g_lcdRect.top + 3.0f, g_lcdRect.right - 6.0f, g_lcdRect.bottom - 3.0f);
    g_d2dRenderTarget->DrawText(
        lcdFull.c_str(), static_cast<UINT32>(lcdFull.length()),
        g_textFormatLcd, &lcdTextRect, g_brushLcdText);

    // 2. UPPER RECESSED D-PAD + CORNER BUTTONS WELL
    D2D1_RECT_F wellRect = D2D1::RectF(left + 14.0f, top + 82.0f, right - 14.0f, top + 240.0f);
    D2D1_ROUNDED_RECT wellRound = D2D1::RoundedRect(wellRect, 14.0f, 14.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&wellRound, g_brushRemoteWellBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&wellRound, g_brushRemoteBtnRing, 1.2f);

    const float wellMidX = (wellRect.left + wellRect.right) * 0.5f;
    const float wellMidY = (wellRect.top + wellRect.bottom) * 0.5f;

    // 2a. Hide button (Top-Left of well)
    g_rBtnHide = D2D1::RectF(wellRect.left + 10.0f, wellRect.top + 8.0f, wellRect.left + 44.0f, wellRect.top + 42.0f);
    DrawRemoteCirclePad(g_rBtnHide, RemoteBtn::Hide, true);
    {
        float cx = (g_rBtnHide.left + g_rBtnHide.right) * 0.5f;
        float cy = (g_rBtnHide.top + g_rBtnHide.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_hoveredRemoteBtn == RemoteBtn::Hide, true);
        D2D1_ELLIPSE eyeOuter = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 4.8f);
        D2D1_ELLIPSE eyePupil = D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.3f, 2.3f);
        g_d2dRenderTarget->DrawEllipse(&eyeOuter, br, 1.4f);
        g_d2dRenderTarget->FillEllipse(&eyePupil, br);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 7.5f, cy + 6.0f), D2D1::Point2F(cx + 7.5f, cy - 6.0f), br, 1.5f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnHide.left - 8.0f, g_rBtnHide.bottom + 3.0f, g_rBtnHide.right + 8.0f, g_rBtnHide.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"Hide", 4, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 2b. PWR button (Top-Right of well)
    const bool vrRunning = IsVrRunning();
    g_rBtnPower = D2D1::RectF(wellRect.right - 44.0f, wellRect.top + 8.0f, wellRect.right - 10.0f, wellRect.top + 42.0f);
    DrawRemoteCirclePad(g_rBtnPower, RemoteBtn::Power, true);
    {
        float cx = (g_rBtnPower.left + g_rBtnPower.right) * 0.5f;
        float cy = (g_rBtnPower.top + g_rBtnPower.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(vrRunning, true);
        D2D1_ELLIPSE pwrRing = D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.0f), 7.0f, 7.0f);
        g_d2dRenderTarget->DrawEllipse(&pwrRing, br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx, cy - 8.0f), D2D1::Point2F(cx, cy - 1.0f), br, 2.0f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnPower.left - 8.0f, g_rBtnPower.bottom + 3.0f, g_rBtnPower.right + 8.0f, g_rBtnPower.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"PWR", 3, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 2c. Menu button (Bottom-Left of well)
    g_rBtnMenu = D2D1::RectF(wellRect.left + 10.0f, wellRect.bottom - 52.0f, wellRect.left + 44.0f, wellRect.bottom - 18.0f);
    DrawRemoteCirclePad(g_rBtnMenu, RemoteBtn::Menu, true);
    {
        float cx = (g_rBtnMenu.left + g_rBtnMenu.right) * 0.5f;
        float cy = (g_rBtnMenu.top + g_rBtnMenu.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_bigscreenMenuToggled, true);
        for (int b = -1; b <= 1; ++b) {
            float y = cy + b * 4.5f;
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 7.5f, y), D2D1::Point2F(cx + 7.5f, y), br, 1.7f);
        }
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnMenu.left - 8.0f, g_rBtnMenu.bottom + 3.0f, g_rBtnMenu.right + 8.0f, g_rBtnMenu.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"Menu", 4, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 2d. fx / D-Pad Mode button (Bottom-Right of well: Panning <-> Zoom)
    const bool fxEnabled = !g_movieOnly;
    g_rBtnSettings = D2D1::RectF(wellRect.right - 44.0f, wellRect.bottom - 52.0f, wellRect.right - 10.0f, wellRect.bottom - 18.0f);
    DrawRemoteCirclePad(g_rBtnSettings, RemoteBtn::Settings, fxEnabled);
    {
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_zoomMode && fxEnabled, fxEnabled);
        D2D1_RECT_F fxRect = D2D1::RectF(g_rBtnSettings.left + 1.0f, g_rBtnSettings.top + 1.0f, g_rBtnSettings.right - 1.0f, g_rBtnSettings.bottom - 1.0f);
        if (g_textFormatFxIcon) {
            g_d2dRenderTarget->DrawText(L"fx", 2, g_textFormatFxIcon, &fxRect, br);
        }
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnSettings.left - 12.0f, g_rBtnSettings.bottom + 3.0f, g_rBtnSettings.right + 12.0f, g_rBtnSettings.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"D-Pad", 5, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 2e. Center D-Pad Ring (Arrows + Select center)
    g_dpadCenter = D2D1::Point2F(wellMidX, wellMidY);
    g_dpadOuterR = 52.0f;
    g_dpadInnerR = 23.0f;
    const bool upDnEnabled = !g_movieOnly;
    const bool lrEnabled = !g_movieOnly && !g_zoomMode;
    const bool selEnabled = !g_movieOnly;
    {
        D2D1_ELLIPSE dpadShadow = D2D1::Ellipse(D2D1::Point2F(g_dpadCenter.x, g_dpadCenter.y + 2.5f), g_dpadOuterR + 2.0f, g_dpadOuterR + 2.0f);
        g_d2dRenderTarget->FillEllipse(&dpadShadow, g_brushRemoteShadow);
        D2D1_ELLIPSE dpadRing = D2D1::Ellipse(g_dpadCenter, g_dpadOuterR, g_dpadOuterR);
        g_d2dRenderTarget->FillEllipse(&dpadRing, g_brushRemoteBtnFill);
        g_d2dRenderTarget->DrawEllipse(&dpadRing, g_brushRemoteBezel, 1.0f);

        // Select center button
        bool selHover = selEnabled && (g_hoveredRemoteBtn == RemoteBtn::DpadSelect || g_heldDpadBtn == RemoteBtn::DpadSelect);
        D2D1_ELLIPSE selOuter = D2D1::Ellipse(g_dpadCenter, g_dpadInnerR + 2.0f, g_dpadInnerR + 2.0f);
        g_d2dRenderTarget->FillEllipse(&selOuter, g_brushRemoteBtnRing);
        D2D1_ELLIPSE selFace = D2D1::Ellipse(g_dpadCenter, g_dpadInnerR, g_dpadInnerR);
        g_d2dRenderTarget->FillEllipse(&selFace, selHover ? g_brushRemoteBtnHover : g_brushRemoteBtnFill);
        g_d2dRenderTarget->DrawEllipse(&selFace, selHover ? g_brushRemoteCyan : g_brushRemoteBezel, 1.0f);

        ID2D1SolidColorBrush* selBr = GetRemoteIconBrush(g_heldDpadBtn == RemoteBtn::DpadSelect, selEnabled);
        D2D1_RECT_F selRect = D2D1::RectF(g_dpadCenter.x - g_dpadInnerR, g_dpadCenter.y - 10.0f, g_dpadCenter.x + g_dpadInnerR, g_dpadCenter.y + 10.0f);
        g_d2dRenderTarget->DrawText(L"Select", 6, g_textFormatRemoteLbl, &selRect, selBr);

        // Four direction chevrons (with per-mode selectability: grey when disabled)
        const float rMid = 37.5f;
        const float s = 5.0f;
        auto drawChevron = [&](RemoteBtn btnId, bool btnEnabled, float dx, float dy, float ax1, float ay1, float ax2, float ay2) {
            bool active = (g_heldDpadBtn == btnId) || (btnEnabled && g_hoveredRemoteBtn == btnId);
            ID2D1SolidColorBrush* br = GetRemoteIconBrush(active, btnEnabled);
            float px = g_dpadCenter.x + dx * rMid;
            float py = g_dpadCenter.y + dy * rMid;
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(px + ax1, py + ay1), br, 1.8f);
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(px + ax2, py + ay2), br, 1.8f);
        };
        drawChevron(RemoteBtn::DpadUp,    upDnEnabled,  0.0f, -1.0f, -s, +s, +s, +s);
        drawChevron(RemoteBtn::DpadDown,  upDnEnabled,  0.0f, +1.0f, -s, -s, +s, -s);
        drawChevron(RemoteBtn::DpadLeft,  lrEnabled,   -1.0f,  0.0f, +s, -s, +s, +s);
        drawChevron(RemoteBtn::DpadRight, lrEnabled,   +1.0f,  0.0f, -s, -s, -s, +s);
    }

    // 3. RAINBOW GLOW DIVIDER BAR
    D2D1_RECT_F rbRect = D2D1::RectF(left + 26.0f, top + 247.0f, right - 26.0f, top + 250.5f);
    if (g_brushRainbowBar) {
        g_brushRainbowBar->SetStartPoint(D2D1::Point2F(rbRect.left, rbRect.top));
        g_brushRainbowBar->SetEndPoint(D2D1::Point2F(rbRect.right, rbRect.top));
        D2D1_ROUNDED_RECT rbRound = D2D1::RoundedRect(rbRect, 2.0f, 2.0f);
        g_d2dRenderTarget->FillRoundedRectangle(&rbRound, g_brushRainbowBar);
    }

    // 4. FUNCTION ROW: FULL SCREEN / WINDOWED | PROJECT SCREEN | INFO
    const float rowY = top + 262.0f;
    const float btnSz = 38.0f;
    g_rBtnFullscreen = D2D1::RectF(left + 28.0f, rowY, left + 28.0f + btnSz, rowY + btnSz);
    g_rBtnProject    = D2D1::RectF((left + right - btnSz) * 0.5f, rowY, (left + right + btnSz) * 0.5f, rowY + btnSz);
    g_rBtnInfo       = D2D1::RectF(right - 28.0f - btnSz, rowY, right - 28.0f, rowY + btnSz);
    g_btnFullscreenRect = g_rBtnFullscreen;
    g_btnProjectScreenRect = g_rBtnProject;

    // 4a. Full Screen / Windowed
    DrawRemoteCirclePad(g_rBtnFullscreen, RemoteBtn::Fullscreen, true);
    {
        float cx = (g_rBtnFullscreen.left + g_rBtnFullscreen.right) * 0.5f;
        float cy = (g_rBtnFullscreen.top + g_rBtnFullscreen.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_isFullscreen, true);
        float d = 7.5f, L = 4.0f;
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx - d + L, cy - d), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx - d, cy - d + L), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx + d - L, cy - d), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx + d, cy - d + L), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx - d + L, cy + d), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx - d, cy + d - L), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx + d - L, cy + d), br, 1.7f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx + d, cy + d - L), br, 1.7f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnFullscreen.left - 16.0f, g_rBtnFullscreen.bottom + 3.0f, g_rBtnFullscreen.right + 16.0f, g_rBtnFullscreen.bottom + 28.0f);
        g_d2dRenderTarget->DrawText(L"Full Screen /\nWindowed", 22, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 4b. Projection Room
    const bool projActive = (g_projectionMode != ProjectionMode::None);
    const bool projEnabled = projActive || CanProjectInCurrentRoom() || CanLeaveToOwnRoomToProject();
    DrawRemoteCirclePad(g_rBtnProject, RemoteBtn::ProjectScreen, projEnabled);
    {
        float cx = (g_rBtnProject.left + g_rBtnProject.right) * 0.5f;
        float cy = (g_rBtnProject.top + g_rBtnProject.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(projActive, projEnabled);
        D2D1_ROUNDED_RECT mon = D2D1::RoundedRect(D2D1::RectF(cx - 9.0f, cy - 7.0f, cx + 9.0f, cy + 4.0f), 1.5f, 1.5f);
        g_d2dRenderTarget->DrawRoundedRectangle(&mon, br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx, cy + 4.0f), D2D1::Point2F(cx, cy + 7.5f), br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 5.0f, cy + 7.5f), D2D1::Point2F(cx + 5.0f, cy + 7.5f), br, 1.6f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnProject.left - 16.0f, g_rBtnProject.bottom + 3.0f, g_rBtnProject.right + 16.0f, g_rBtnProject.bottom + 28.0f);
        g_d2dRenderTarget->DrawText(L"Projection\nRoom", 15, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 4c. Info
    DrawRemoteCirclePad(g_rBtnInfo, RemoteBtn::Info, true);
    {
        float cx = (g_rBtnInfo.left + g_rBtnInfo.right) * 0.5f;
        float cy = (g_rBtnInfo.top + g_rBtnInfo.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_infoPanelOpen || g_hoveredRemoteBtn == RemoteBtn::Info, true);
        D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(cx, cy - 5.5f), 1.4f, 1.4f);
        g_d2dRenderTarget->FillEllipse(&dot, br);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx, cy - 1.5f), D2D1::Point2F(cx, cy + 7.0f), br, 2.2f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnInfo.left - 10.0f, g_rBtnInfo.bottom + 3.0f, g_rBtnInfo.right + 10.0f, g_rBtnInfo.bottom + 20.0f);
        g_d2dRenderTarget->DrawText(L"Info", 4, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 5. MOVIE MODE / THEATER MODE WIDE ROCKER PILL
    D2D1_RECT_F rockerRect = D2D1::RectF(left + 16.0f, top + 340.0f, right - 16.0f, top + 394.0f);
    const float rockerMidX = (rockerRect.left + rockerRect.right) * 0.5f;
    g_rBtnMovie   = D2D1::RectF(rockerRect.left, rockerRect.top, rockerMidX, rockerRect.bottom);
    g_rBtnTheater = D2D1::RectF(rockerMidX, rockerRect.top, rockerRect.right, rockerRect.bottom);

    D2D1_ROUNDED_RECT rockerShadow = D2D1::RoundedRect(
        D2D1::RectF(rockerRect.left, rockerRect.top + 2.0f, rockerRect.right, rockerRect.bottom + 2.5f), 10.0f, 10.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&rockerShadow, g_brushRemoteShadow);
    D2D1_ROUNDED_RECT rockerPill = D2D1::RoundedRect(rockerRect, 10.0f, 10.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&rockerPill, g_brushRemoteBtnFill);
    if (g_hoveredRemoteBtn == RemoteBtn::MovieMode) {
        D2D1_ROUNDED_RECT mHi = D2D1::RoundedRect(D2D1::RectF(g_rBtnMovie.left + 1.5f, g_rBtnMovie.top + 1.5f, g_rBtnMovie.right - 0.5f, g_rBtnMovie.bottom - 1.5f), 8.5f, 8.5f);
        g_d2dRenderTarget->FillRoundedRectangle(&mHi, g_brushRemoteBtnHover);
    } else if (g_hoveredRemoteBtn == RemoteBtn::TheaterMode) {
        D2D1_ROUNDED_RECT tHi = D2D1::RoundedRect(D2D1::RectF(g_rBtnTheater.left + 0.5f, g_rBtnTheater.top + 1.5f, g_rBtnTheater.right - 1.5f, g_rBtnTheater.bottom - 1.5f), 8.5f, 8.5f);
        g_d2dRenderTarget->FillRoundedRectangle(&tHi, g_brushRemoteBtnHover);
    }
    g_d2dRenderTarget->DrawRoundedRectangle(&rockerPill, g_brushRemoteBezel, 1.2f);
    g_d2dRenderTarget->DrawLine(D2D1::Point2F(rockerMidX, rockerRect.top + 4.0f), D2D1::Point2F(rockerMidX, rockerRect.bottom - 4.0f), g_brushRemoteBtnRing, 1.4f);

    // 5a. Movie Mode Left Icon & Label
    {
        float cx = (g_rBtnMovie.left + g_rBtnMovie.right) * 0.5f;
        float cy = g_rBtnMovie.top + 18.0f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(g_movieOnly, true);
        D2D1_ROUNDED_RECT scr = D2D1::RoundedRect(D2D1::RectF(cx - 12.0f, cy - 8.5f, cx + 12.0f, cy + 7.5f), 2.0f, 2.0f);
        g_d2dRenderTarget->DrawRoundedRectangle(&scr, br, 1.5f);
        if (g_d2dFactory) {
            ID2D1PathGeometry* tri = nullptr;
            if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&tri)) && tri) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(tri->Open(&sink)) && sink) {
                    sink->BeginFigure(D2D1::Point2F(cx - 3.0f, cy - 4.5f), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(cx + 5.0f, cy - 0.5f));
                    sink->AddLine(D2D1::Point2F(cx - 3.0f, cy + 3.5f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                    g_d2dRenderTarget->FillGeometry(tri, br);
                }
                tri->Release();
            }
        }
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnMovie.left + 2.0f, rockerRect.bottom - 18.0f, g_rBtnMovie.right - 2.0f, rockerRect.bottom - 2.0f);
        g_d2dRenderTarget->DrawText(L"Movie Mode", 10, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 5b. Theater Mode Right Icon + Indicator Dot & Label
    {
        float cx = (g_rBtnTheater.left + g_rBtnTheater.right) * 0.5f;
        float cy = g_rBtnTheater.top + 16.0f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(!g_movieOnly, true);
        D2D1_ROUNDED_RECT scr = D2D1::RoundedRect(D2D1::RectF(cx - 12.0f, cy - 7.5f, cx + 12.0f, cy + 4.5f), 1.5f, 1.5f);
        g_d2dRenderTarget->DrawRoundedRectangle(&scr, br, 1.5f);
        for (int s = -1; s <= 1; ++s) {
            D2D1_ELLIPSE seat = D2D1::Ellipse(D2D1::Point2F(cx + s * 6.5f, cy + 9.0f), 2.1f, 1.8f);
            g_d2dRenderTarget->FillEllipse(&seat, br);
        }
        D2D1_ELLIPSE led = D2D1::Ellipse(D2D1::Point2F(g_rBtnTheater.right - 10.0f, g_rBtnTheater.top + 10.0f), 2.5f, 2.5f);
        g_d2dRenderTarget->FillEllipse(&led, !g_movieOnly ? g_brushRemoteCyan : g_brushRemoteGrey);

        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnTheater.left + 2.0f, rockerRect.bottom - 18.0f, g_rBtnTheater.right - 2.0f, rockerRect.bottom - 2.0f);
        g_d2dRenderTarget->DrawText(L"Theater Mode", 12, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 6. BOTTOM CLUSTER: TALL VOLUME PILL + 2x2 RIGHT CIRCLE BUTTONS
    const float clusterTop = top + 412.0f;
    const float col1X = left + 26.0f;
    const float pillW = 42.0f;
    const float pillH = 122.0f;
    g_rBtnVolPill = D2D1::RectF(col1X, clusterTop, col1X + pillW, clusterTop + pillH);
    g_rBtnVolUp   = D2D1::RectF(g_rBtnVolPill.left, g_rBtnVolPill.top, g_rBtnVolPill.right, g_rBtnVolPill.top + 38.0f);
    g_rBtnVolMute = D2D1::RectF(g_rBtnVolPill.left, g_rBtnVolPill.top + 38.0f, g_rBtnVolPill.right, g_rBtnVolPill.bottom - 38.0f);
    g_rBtnVolDown = D2D1::RectF(g_rBtnVolPill.left, g_rBtnVolPill.bottom - 38.0f, g_rBtnVolPill.right, g_rBtnVolPill.bottom);

    D2D1_ROUNDED_RECT volShadow = D2D1::RoundedRect(
        D2D1::RectF(g_rBtnVolPill.left, g_rBtnVolPill.top + 2.0f, g_rBtnVolPill.right, g_rBtnVolPill.bottom + 2.5f), 21.0f, 21.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&volShadow, g_brushRemoteShadow);
    D2D1_ROUNDED_RECT volRound = D2D1::RoundedRect(g_rBtnVolPill, 21.0f, 21.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&volRound, g_brushRemoteBtnFill);
    g_d2dRenderTarget->DrawRoundedRectangle(&volRound,
        (g_hoveredRemoteBtn == RemoteBtn::VolUp || g_hoveredRemoteBtn == RemoteBtn::VolMute || g_hoveredRemoteBtn == RemoteBtn::VolDown)
            ? g_brushRemoteCyan : g_brushRemoteBezel, 1.1f);

    // Volume + / Speaker / -
    {
        float vcx = (g_rBtnVolPill.left + g_rBtnVolPill.right) * 0.5f;
        float plusY = (g_rBtnVolUp.top + g_rBtnVolUp.bottom) * 0.5f;
        ID2D1SolidColorBrush* plusBr = GetRemoteIconBrush(g_hoveredRemoteBtn == RemoteBtn::VolUp, true);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx - 6.0f, plusY), D2D1::Point2F(vcx + 6.0f, plusY), plusBr, 1.8f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx, plusY - 6.0f), D2D1::Point2F(vcx, plusY + 6.0f), plusBr, 1.8f);

        float spkY = (g_rBtnVolMute.top + g_rBtnVolMute.bottom) * 0.5f;
        ID2D1SolidColorBrush* spkBr = GetRemoteIconBrush(g_bigscreenAudioMuted, true);
        if (g_d2dFactory) {
            ID2D1PathGeometry* cone = nullptr;
            if (SUCCEEDED(g_d2dFactory->CreatePathGeometry(&cone)) && cone) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(cone->Open(&sink)) && sink) {
                    sink->BeginFigure(D2D1::Point2F(vcx - 8.0f, spkY - 3.5f), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(vcx - 3.5f, spkY - 3.5f));
                    sink->AddLine(D2D1::Point2F(vcx + 1.5f, spkY - 7.5f));
                    sink->AddLine(D2D1::Point2F(vcx + 1.5f, spkY + 7.5f));
                    sink->AddLine(D2D1::Point2F(vcx - 3.5f, spkY + 3.5f));
                    sink->AddLine(D2D1::Point2F(vcx - 8.0f, spkY + 3.5f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                    g_d2dRenderTarget->FillGeometry(cone, spkBr);
                }
                cone->Release();
            }
        }
        if (g_bigscreenAudioMuted) {
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx - 9.0f, spkY + 8.0f), D2D1::Point2F(vcx + 9.0f, spkY - 8.0f), g_brushRemoteCyan, 1.9f);
        } else {
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx + 4.5f, spkY - 3.5f), D2D1::Point2F(vcx + 5.5f, spkY), spkBr, 1.5f);
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx + 5.5f, spkY), D2D1::Point2F(vcx + 4.5f, spkY + 3.5f), spkBr, 1.5f);
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx + 7.5f, spkY - 6.0f), D2D1::Point2F(vcx + 9.0f, spkY), spkBr, 1.5f);
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx + 9.0f, spkY), D2D1::Point2F(vcx + 7.5f, spkY + 6.0f), spkBr, 1.5f);
        }

        float minusY = (g_rBtnVolDown.top + g_rBtnVolDown.bottom) * 0.5f;
        ID2D1SolidColorBrush* minusBr = GetRemoteIconBrush(g_hoveredRemoteBtn == RemoteBtn::VolDown, true);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(vcx - 6.0f, minusY), D2D1::Point2F(vcx + 6.0f, minusY), minusBr, 1.8f);
    }

    // 2x2 Right Circles
    const float cBtn = 40.0f;
    const float col2X = left + 96.0f;
    const float col3X = left + 162.0f;
    const float row1Y = clusterTop + 4.0f;
    const float row2Y = clusterTop + 72.0f;

    g_rBtnMic        = D2D1::RectF(col2X, row1Y, col2X + cBtn, row1Y + cBtn);
    g_rBtnMuteOthers = D2D1::RectF(col3X, row1Y, col3X + cBtn, row1Y + cBtn);
    g_rBtnNo3D       = D2D1::RectF(col2X, row2Y, col2X + cBtn, row2Y + cBtn);
    g_rBtnAspect     = D2D1::RectF(col3X, row2Y, col3X + cBtn, row2Y + cBtn);

    // 6a. Microphone (Cyan when ON, White when Muted, Grey if Bigscreen off)
    const bool micEnabled = IsRemoteBtnSelectable(RemoteBtn::Microphone);
    DrawRemoteCirclePad(g_rBtnMic, RemoteBtn::Microphone, micEnabled);
    {
        float cx = (g_rBtnMic.left + g_rBtnMic.right) * 0.5f;
        float cy = (g_rBtnMic.top + g_rBtnMic.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(micEnabled && !g_micMuted, micEnabled);
        D2D1_ROUNDED_RECT cap = D2D1::RoundedRect(D2D1::RectF(cx - 3.6f, cy - 9.0f, cx + 3.6f, cy + 2.0f), 3.6f, 3.6f);
        g_d2dRenderTarget->FillRoundedRectangle(&cap, br);
        D2D1_ELLIPSE cradle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.8f, 6.0f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 6.5f, cy - 1.5f), D2D1::Point2F(cx - 6.5f, cy + 2.5f), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + 6.5f, cy - 1.5f), D2D1::Point2F(cx + 6.5f, cy + 2.5f), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 6.5f, cy + 2.5f), D2D1::Point2F(cx + 6.5f, cy + 2.5f), br, 1.5f);
        (void)cradle;
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx, cy + 8.5f), br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 4.5f, cy + 8.5f), D2D1::Point2F(cx + 4.5f, cy + 8.5f), br, 1.6f);
        if (micEnabled && g_micMuted) {
            g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 8.5f, cy + 7.5f), D2D1::Point2F(cx + 8.5f, cy - 7.5f), br, 1.8f);
        }
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnMic.left - 14.0f, g_rBtnMic.bottom + 3.0f, g_rBtnMic.right + 14.0f, g_rBtnMic.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"Microphone", 10, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 6b. Mute Others (Grey if <2 users, White when off, Cyan when on)
    const bool muteOthersEnabled = IsRemoteBtnSelectable(RemoteBtn::MuteOthers);
    DrawRemoteCirclePad(g_rBtnMuteOthers, RemoteBtn::MuteOthers, muteOthersEnabled);
    {
        float cx = (g_rBtnMuteOthers.left + g_rBtnMuteOthers.right) * 0.5f;
        float cy = (g_rBtnMuteOthers.top + g_rBtnMuteOthers.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(muteOthersEnabled && g_allMuted, muteOthersEnabled);
        D2D1_ELLIPSE head = D2D1::Ellipse(D2D1::Point2F(cx, cy - 4.5f), 4.0f, 4.0f);
        g_d2dRenderTarget->DrawEllipse(&head, br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 7.5f, cy + 7.0f), D2D1::Point2F(cx - 5.5f, cy + 2.0f), br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 5.5f, cy + 2.0f), D2D1::Point2F(cx + 5.5f, cy + 2.0f), br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + 5.5f, cy + 2.0f), D2D1::Point2F(cx + 7.5f, cy + 7.0f), br, 1.6f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 8.5f, cy - 7.5f), D2D1::Point2F(cx + 8.5f, cy + 7.5f), br, 1.7f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnMuteOthers.left - 12.0f, g_rBtnMuteOthers.bottom + 3.0f, g_rBtnMuteOthers.right + 12.0f, g_rBtnMuteOthers.bottom + 26.0f);
        g_d2dRenderTarget->DrawText(L"Mute\nOthers", 11, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 6c. No 3D (Grey in Theater Mode, White in Movie Mode 2D, Cyan when converting SBS or O/U)
    const bool no3dEnabled = IsRemoteBtnSelectable(RemoteBtn::No3D);
    const bool no3dCyan = no3dEnabled && (g_stereoMode != StereoSplitMode::Normal2D);
    DrawRemoteCirclePad(g_rBtnNo3D, RemoteBtn::No3D, no3dEnabled);
    {
        float cx = (g_rBtnNo3D.left + g_rBtnNo3D.right) * 0.5f;
        float cy = (g_rBtnNo3D.top + g_rBtnNo3D.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = GetRemoteIconBrush(no3dCyan, no3dEnabled);
        D2D1_RECT_F iconTxtRect = D2D1::RectF(g_rBtnNo3D.left, g_rBtnNo3D.top + 3.0f, g_rBtnNo3D.right, g_rBtnNo3D.bottom - 3.0f);
        g_d2dRenderTarget->DrawText(L"No\n3D", 5, g_textFormatRemoteLbl, &iconTxtRect, br);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - 8.5f, cy + 8.0f), D2D1::Point2F(cx + 8.5f, cy - 8.0f), br, 1.5f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnNo3D.left - 10.0f, g_rBtnNo3D.bottom + 3.0f, g_rBtnNo3D.right + 10.0f, g_rBtnNo3D.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"No 3D", 5, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 6d. A/R (NEVER Cyan even when non-Auto, per Rule #15; LCD shows preset)
    DrawRemoteCirclePad(g_rBtnAspect, RemoteBtn::AspectRatio, true);
    {
        float cx = (g_rBtnAspect.left + g_rBtnAspect.right) * 0.5f;
        float cy = (g_rBtnAspect.top + g_rBtnAspect.bottom) * 0.5f;
        ID2D1SolidColorBrush* br = g_brushTextWhite; // Always White
        float d = 7.5f, a = 4.0f;
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx + d, cy - d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx - d + a, cy - d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx - d, cy - d + a), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx + d - a, cy - d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx + d, cy - d + a), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx - d + a, cy + d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx - d, cy + d), D2D1::Point2F(cx - d, cy + d - a), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx + d - a, cy + d), br, 1.5f);
        g_d2dRenderTarget->DrawLine(D2D1::Point2F(cx + d, cy + d), D2D1::Point2F(cx + d, cy + d - a), br, 1.5f);
        D2D1_RECT_F lbl = D2D1::RectF(g_rBtnAspect.left - 8.0f, g_rBtnAspect.bottom + 3.0f, g_rBtnAspect.right + 8.0f, g_rBtnAspect.bottom + 18.0f);
        g_d2dRenderTarget->DrawText(L"A/R", 3, g_textFormatRemoteLbl, &lbl, g_brushRemoteLabel);
    }

    // 7. BOTTOM BRANDING BADGE ("Bigscreen")
    D2D1_RECT_F brandRect = D2D1::RectF(left + 20.0f, bottom - 42.0f, right - 20.0f, bottom - 14.0f);
    g_d2dRenderTarget->DrawText(L"Bigscreen", 9, g_textFormatBrand, &brandRect, g_brushRemoteLabel);
}

void ReleaseAll() {
    StopAllProjectionAndUnloadVirtual();
    ReleaseFallbackGpuResources();
    ReleaseProjectedAppResources();
    ReleaseSharedDesktopTexture();
    ReleaseSharedResources();
    ReleaseThirdPersonResources();
    ReleasePeerMovieResources();
    ReleaseD2DDeviceResources();

    if (g_textFormatInfoBody) g_textFormatInfoBody->Release();
    if (g_textFormatInfoHdr) g_textFormatInfoHdr->Release();
    if (g_textFormatFxIcon) g_textFormatFxIcon->Release();
    if (g_textFormatBrand) g_textFormatBrand->Release();
    if (g_textFormatRemoteLbl) g_textFormatRemoteLbl->Release();
    if (g_textFormatLcd) g_textFormatLcd->Release();
    if (g_textFormatFooter) g_textFormatFooter->Release();
    if (g_textFormatPill) g_textFormatPill->Release();
    if (g_textFormatBtn) g_textFormatBtn->Release();
    if (g_textFormatSection) g_textFormatSection->Release();
    if (g_textFormatTitle) g_textFormatTitle->Release();
    if (g_dwriteFactory) g_dwriteFactory->Release();
    if (g_d2dFactory) g_d2dFactory->Release();
    g_textFormatInfoBody = nullptr;
    g_textFormatInfoHdr = nullptr;
    g_textFormatFxIcon = nullptr;
    g_textFormatBrand = nullptr;
    g_textFormatRemoteLbl = nullptr;
    g_textFormatLcd = nullptr;
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

    if (g_fallbackView) UnmapViewOfFile(g_fallbackView);
    if (g_fallbackMapping) CloseHandle(g_fallbackMapping);
    g_fallbackView = nullptr;
    g_fallbackMapping = nullptr;

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
    IDXGIAdapter1* primaryAdapter = nullptr;
    DXGI_ADAPTER_DESC1 primaryDesc{};
    IDXGIAdapter1* nvidiaFallback = nullptr;
    DXGI_ADAPTER_DESC1 nvidiaDesc{};
    IDXGIAdapter1* fallback = nullptr;
    DXGI_ADAPTER_DESC1 fallbackDesc{};

    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            bool ownsPrimaryMonitor = false;
            for (UINT outIdx = 0; ; ++outIdx) {
                IDXGIOutput* output = nullptr;
                if (candidate->EnumOutputs(outIdx, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC outDesc{};
                if (output && SUCCEEDED(output->GetDesc(&outDesc))) {
                    if (outDesc.DesktopCoordinates.left == 0 && outDesc.DesktopCoordinates.top == 0) {
                        ownsPrimaryMonitor = true;
                    }
                }
                if (output) output->Release();
                if (ownsPrimaryMonitor) break;
            }
            if (ownsPrimaryMonitor && !primaryAdapter) {
                primaryAdapter = candidate;
                primaryDesc = desc;
                continue;
            }
            if (desc.VendorId == 0x10DE && !nvidiaFallback) {
                nvidiaFallback = candidate;
                nvidiaDesc = desc;
                continue;
            }
            if (!fallback) {
                fallback = candidate;
                fallbackDesc = desc;
                continue;
            }
        }
        if (candidate) candidate->Release();
    }

    if (primaryAdapter) {
        g_adapter = primaryAdapter;
        Log("Viewer adapter (Primary Display): %ls", primaryDesc.Description);
        primaryAdapter = nullptr;
    } else if (nvidiaFallback) {
        g_adapter = nvidiaFallback;
        Log("Viewer adapter (NVIDIA): %ls", nvidiaDesc.Description);
        nvidiaFallback = nullptr;
    } else if (fallback) {
        g_adapter = fallback;
        Log("Viewer adapter (Hardware): %ls", fallbackDesc.Description);
        fallback = nullptr;
    }
    if (primaryAdapter) primaryAdapter->Release();
    if (nvidiaFallback) nvidiaFallback->Release();
    if (fallback) fallback->Release();
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
    if (g_projectionMode == ProjectionMode::VirtualScreen ||
        g_projectionMode == ProjectionMode::Screen ||
        g_projectionMode == ProjectionMode::App) {
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
        if (g_pointerActive && !IsOverRemoteUi(g_cursorX, g_cursorY)) {
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
    if (g_projectedVideoState) {
        Log("Route/Cap/Aspect diag: hookLocal=%u desktops=%u openHr=0x%08X deskFmt=%u vidFmt=%u copied=%u capHooks=%u capOpenHr=0x%08X capCopied=%u aspMask=0x%X aspApplied=%u",
            g_projectedVideoState->diagHookLocal,
            g_projectedVideoState->diagDesktopCount,
            static_cast<unsigned>(g_projectedVideoState->diagOpenHr),
            g_projectedVideoState->diagDeskFmt,
            g_projectedVideoState->diagVidFmt,
            g_projectedVideoState->diagCopiedCount,
            g_projectedVideoState->diagCapHookCalls,
            static_cast<unsigned>(g_projectedVideoState->diagCapOpenHr),
            g_projectedVideoState->diagCapCopiedCount,
            g_projectedVideoState->diagAspectMask,
            g_projectedVideoState->diagAspectAppliedCount);
    }
    g_lastMetrics = now;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool s_virtualLeftDown = false;
    static bool s_virtualRightDown = false;

    if (message == WM_KEYDOWN) {
        if (wParam == VK_TAB || wParam == 'M' || wParam == VK_F1) {
            g_showRemote = !g_showRemote;
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
                g_controlVirtualScreen = false;
                s_virtualLeftDown = false;
                s_virtualRightDown = false;
                SyncPointerAndLook();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            EnsureControlMapping();
            if (g_controlState) {
                bigscreen_controller_control_ipc::PulseSystemClick(g_controlState);
            }
            return 0;
        }
        if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
            ForwardVirtualKey(message, wParam, lParam);
            return 0;
        }
        // D-pad / Arrow keys mirror Theater Mode navigation (no function in Movie Mode)
        if (!g_movieOnly) {
            if (wParam == VK_LEFT) {
                ExecuteRemoteDpadStep(RemoteBtn::DpadLeft);
                return 0;
            }
            if (wParam == VK_RIGHT) {
                ExecuteRemoteDpadStep(RemoteBtn::DpadRight);
                return 0;
            }
            if (wParam == VK_UP) {
                ExecuteRemoteDpadStep(RemoteBtn::DpadUp);
                return 0;
            }
            if (wParam == VK_DOWN) {
                ExecuteRemoteDpadStep(RemoteBtn::DpadDown);
                return 0;
            }
            if (wParam == VK_RETURN) {
                EnsureControlMapping();
                if (g_controlState) {
                    g_pointerTrigger = true;
                    SyncPointerAndLook();
                }
                return 0;
            }
        }
        if (wParam == 'R' || wParam == 'Y') {
            ResetLook();
            return 0;
        }
    }

    if (message == WM_KEYUP || message == WM_CHAR) {
        if (message == WM_KEYUP && wParam == VK_ESCAPE) {
            EnsureControlMapping();
            if (g_controlState) {
                bigscreen_controller_control_ipc::ClearSystemClick(g_controlState);
            }
            return 0;
        }
        if (message == WM_KEYUP && wParam == VK_RETURN && !g_movieOnly &&
            !(g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen)) {
            g_pointerTrigger = false;
            SyncPointerAndLook();
            return 0;
        }
        if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen &&
            wParam != VK_TAB && wParam != VK_F1 && wParam != VK_F11 && wParam != VK_ESCAPE) {
            ForwardVirtualKey(message, wParam, lParam);
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

        if (g_isDraggingRemote) {
            g_remotePosX = g_remoteDragStartX + (mx - static_cast<float>(g_remoteDragStartPt.x));
            g_remotePosY = g_remoteDragStartY + (my - static_cast<float>(g_remoteDragStartPt.y));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const float winW = static_cast<float>(std::max<LONG>(1, clientRect.right));
        const float winH = static_cast<float>(std::max<LONG>(1, clientRect.bottom));

        if (IsOverRemoteUi(mx, my)) {
            g_pointerActive = false;
            g_pillHovered = false;
            g_hoveredRemoteBtn = PointInRectF(mx, my, g_remoteRect) ? HitTestRemoteButton(mx, my) : RemoteBtn::None;
        } else if (!g_showRemote && inRect(mx, my, g_pillRect)) {
            g_pointerActive = false;
            g_hoveredRemoteBtn = RemoteBtn::None;
            g_pillHovered = true;
        } else {
            // Interactive VR Client Area: mouse controls pointer aim & look (or Virtual Screen when active)
            g_pillHovered = false;
            g_hoveredRemoteBtn = RemoteBtn::None;

            g_pointerActive = true;
            g_cursorX = mx;
            g_cursorY = my;

            if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
                LONG tx = 0, ty = 0;
                if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                    ForwardVirtualMouseHover(tx, ty);
                    if (s_virtualLeftDown) {
                        ForwardVirtualMouseInput(0, tx, ty, 0, true);
                    }
                }
                return 0;
            }

            if (g_movieOnly) {
                g_lookYaw = 0.0f;
                g_lookPitch = 0.0f;
                g_lookZoom = 0.0f;
            } else if (g_isRightDragging) {
                const float dx = mx - static_cast<float>(g_lastRightPt.x);
                const float dy = my - static_cast<float>(g_lastRightPt.y);
                g_lookYaw += dx * 0.003f;
                g_lookPitch -= dy * 0.003f;
                g_lastRightPt = { static_cast<LONG>(mx), static_cast<LONG>(my) };
            }

            const float vw = g_activeViewport.Width > 0.0f ? g_activeViewport.Width : winW;
            const float vh = g_activeViewport.Height > 0.0f ? g_activeViewport.Height : winH;
            const float vx0 = g_activeViewport.Width > 0.0f ? g_activeViewport.TopLeftX : 0.0f;
            const float vy0 = g_activeViewport.Height > 0.0f ? g_activeViewport.TopLeftY : 0.0f;

            const float u = std::clamp(((mx - vx0) / vw - 0.5f) * 2.0f, -1.0f, 1.0f);
            const float v = std::clamp((0.5f - (my - vy0) / vh) * 2.0f, -1.0f, 1.0f);

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
        g_hoveredRemoteBtn = RemoteBtn::None;
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

        if (IsOverInfoPanel(mx, my) && !inRect(mx, my, g_remoteRect)) {
            ToggleInfoSlideOut();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_showRemote && inRect(mx, my, g_remoteRect)) {
            RemoteBtn hit = HitTestRemoteButton(mx, my);
            if (hit == RemoteBtn::None) {
                // Drag remote inside the viewer when clicking non-button areas (LCD, casing, Bigscreen logo)
                g_isDraggingRemote = true;
                g_remoteDragStartPt = { static_cast<LONG>(mx), static_cast<LONG>(my) };
                g_remoteDragStartX = g_remotePosX;
                g_remoteDragStartY = g_remotePosY;
                SetCapture(hwnd);
                return 0;
            }
            if (!IsRemoteBtnSelectable(hit)) {
                return 0;
            }

            switch (hit) {
                case RemoteBtn::Hide:
                    g_showRemote = false;
                    g_hoveredRemoteBtn = RemoteBtn::None;
                    break;
                case RemoteBtn::Power:
                    if (IsVrRunning()) {
                        StopAllProjectionAndUnloadVirtual();
                        TerminateVrProcesses();
                        SetLcdNotification(L"BIGSCREEN OFF", L"POWERED DOWN", 2500);
                    } else {
                        LaunchVrProcesses();
                        SetLcdNotification(L"TURNING ON", L"BIGSCREEN", 3500);
                    }
                    break;
                case RemoteBtn::DpadUp:
                    g_heldDpadBtn = RemoteBtn::DpadUp;
                    g_lastDpadRepeatMs = GetTickCount64();
                    SetCapture(hwnd);
                    ExecuteRemoteDpadStep(RemoteBtn::DpadUp);
                    break;
                case RemoteBtn::DpadDown:
                    g_heldDpadBtn = RemoteBtn::DpadDown;
                    g_lastDpadRepeatMs = GetTickCount64();
                    SetCapture(hwnd);
                    ExecuteRemoteDpadStep(RemoteBtn::DpadDown);
                    break;
                case RemoteBtn::DpadLeft:
                    g_heldDpadBtn = RemoteBtn::DpadLeft;
                    g_lastDpadRepeatMs = GetTickCount64();
                    SetCapture(hwnd);
                    ExecuteRemoteDpadStep(RemoteBtn::DpadLeft);
                    break;
                case RemoteBtn::DpadRight:
                    g_heldDpadBtn = RemoteBtn::DpadRight;
                    g_lastDpadRepeatMs = GetTickCount64();
                    SetCapture(hwnd);
                    ExecuteRemoteDpadStep(RemoteBtn::DpadRight);
                    break;
                case RemoteBtn::DpadSelect:
                    g_heldDpadBtn = RemoteBtn::DpadSelect;
                    ExecuteRemoteDpadStep(RemoteBtn::DpadSelect);
                    g_heldDpadBtn = RemoteBtn::None;
                    break;
                case RemoteBtn::Menu:
                    g_bigscreenMenuToggled = !g_bigscreenMenuToggled;
                    ToggleDesktopMenu();
                    SetLcdNotification(g_bigscreenMenuToggled ? L"MENU OPENED" : L"MENU HIDDEN", L"BIGSCREEN", 2500);
                    break;
                case RemoteBtn::Settings:
                    g_zoomMode = !g_zoomMode;
                    SetLcdNotification(g_zoomMode ? L"ZOOM MODE" : L"PANNING MODE",
                                       g_zoomMode ? L"D-PAD: UP/DN/SEL" : L"D-PAD: LOOK/CTR", 2500);
                    break;
                case RemoteBtn::Fullscreen:
                    ToggleFullscreen(hwnd);
                    SetLcdNotification(g_isFullscreen ? L"VIEWER FULLSCREEN" : L"VIEWER WINDOWED", L"", 2500);
                    break;
                case RemoteBtn::ProjectScreen:
                    if (g_projectionMode != ProjectionMode::None) {
                        StopAllProjectionAndUnloadVirtual();
                        SetLcdNotification(L"PROJECTION", L"STOPPED", 3000);
                    } else {
                        g_heldProjectBtn = true;
                        g_projectHoldTriggered = false;
                        g_projectHoldStartMs = GetTickCount64();
                        SetCapture(hwnd);
                    }
                    break;
                case RemoteBtn::Info:
                    ToggleInfoSlideOut();
                    break;
                case RemoteBtn::MovieMode:
                    g_movieOnly = true;
                    g_waitingForNewFeed = false;
                    g_heldDpadBtn = RemoteBtn::None;
                    g_lookYaw = 0.0f;
                    g_lookPitch = 0.0f;
                    g_lookZoom = 0.0f;
                    SyncPointerAndLook();
                    if (g_projectionMode != ProjectionMode::None && g_projectedWidth && g_projectedHeight) {
                        AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(g_projectedWidth, g_projectedHeight));
                    }
                    SetLcdNotification(L"MOVIE MODE", L"SELECTED", 3000);
                    break;
                case RemoteBtn::TheaterMode:
                    g_movieOnly = false;
                    g_waitingForNewFeed = false;
                    SetLcdNotification(L"THEATER MODE", L"SELECTED", 3000);
                    break;
                case RemoteBtn::VolUp: {
                    ControlBigscreenSessionAudio(1);
                    wchar_t vb[32]{};
                    swprintf_s(vb, L"VOLUME %d%%", static_cast<int>(std::round(g_bigscreenAudioVol * 100.0f)));
                    SetLcdNotification(L"BIGSCREEN", vb, 2500);
                    break;
                }
                case RemoteBtn::VolDown: {
                    ControlBigscreenSessionAudio(-1);
                    wchar_t vb[32]{};
                    swprintf_s(vb, L"VOLUME %d%%", static_cast<int>(std::round(g_bigscreenAudioVol * 100.0f)));
                    SetLcdNotification(L"BIGSCREEN", vb, 2500);
                    break;
                }
                case RemoteBtn::VolMute:
                    ControlBigscreenSessionAudio(2);
                    SetLcdNotification(L"BIGSCREEN", g_bigscreenAudioMuted ? L"AUDIO MUTED" : L"AUDIO UNMUTED", 2500);
                    break;
                case RemoteBtn::Microphone:
                    TriggerRemoteMicToggle();
                    break;
                case RemoteBtn::MuteOthers:
                    TriggerRemoteMuteAllToggle();
                    break;
                case RemoteBtn::No3D:
                    if (g_stereoMode == StereoSplitMode::Normal2D) {
                        g_stereoMode = StereoSplitMode::SBS;
                        SetLcdNotification(L"NO 3D: SBS -> 2D", L"ACTIVE", 3000);
                    } else if (g_stereoMode == StereoSplitMode::SBS) {
                        g_stereoMode = StereoSplitMode::OU;
                        SetLcdNotification(L"NO 3D: O/U -> 2D", L"ACTIVE", 3000);
                    } else {
                        g_stereoMode = StereoSplitMode::Normal2D;
                        SetLcdNotification(L"NO 3D: OFF", L"NORMAL 2D", 3000);
                    }
                    g_lastAdjustedAspect = 0.0f;
                    if (g_projectionMode != ProjectionMode::None && g_projectedWidth && g_projectedHeight) {
                        AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(g_projectedWidth, g_projectedHeight));
                    }
                    break;
                case RemoteBtn::AspectRatio: {
                    g_aspectPresetIndex = (g_aspectPresetIndex + 1) % _countof(kAspectPresets);
                    g_lastAdjustedAspect = 0.0f;
                    SyncAspectOverrideToIpc();
                    if (g_movieOnly) {
                        uint32_t w = (g_projectionMode != ProjectionMode::None) ? g_projectedWidth : g_peerWidth;
                        uint32_t h = (g_projectionMode != ProjectionMode::None) ? g_projectedHeight : g_peerHeight;
                        if (w == 0 || h == 0) { w = g_sourceWidth; h = g_sourceHeight; }
                        AdjustWindowToVideoAspect(hwnd, ComputeMovieDisplayAspect(w, h));
                    }
                    wchar_t arBuf[48]{};
                    GetShortAspectLcdText(g_aspectPresetIndex, arBuf, _countof(arBuf));
                    SetLcdNotification(L"ASPECT RATIO", arBuf, 3000);
                    break;
                }
                default:
                    break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        } else if (!g_showRemote && inRect(mx, my, g_pillRect)) {
            g_showRemote = true;
            return 0;
        }

        if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
            LONG tx = 0, ty = 0;
            if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                s_virtualLeftDown = true;
                SetCapture(hwnd);
                ForwardVirtualMouseInput(MOUSEEVENTF_LEFTDOWN, tx, ty, 0, true);
                return 0;
            }
        }

        // Click on VR screen: Pull controller trigger for laser pointer selection!
        g_pointerTrigger = true;
        SyncPointerAndLook();
        return 0;
    }

    if (message == WM_CAPTURECHANGED) {
        g_isDraggingRemote = false;
        g_heldDpadBtn = RemoteBtn::None;
        g_heldProjectBtn = false;
        g_projectHoldTriggered = false;
        return 0;
    }

    if (message == WM_LBUTTONUP) {
        if (g_heldProjectBtn) {
            const bool holdTriggered = g_projectHoldTriggered;
            g_heldProjectBtn = false;
            g_projectHoldTriggered = false;
            ReleaseCapture();
            if (!holdTriggered) {
                if (CanProjectInCurrentRoom()) {
                    ShowProjectScreenMenu(hwnd, g_rBtnProject.left, g_rBtnProject.bottom);
                } else if (CanLeaveToOwnRoomToProject()) {
                    RequestOwnRoomTransition();
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_isDraggingRemote) {
            g_isDraggingRemote = false;
            ReleaseCapture();
            return 0;
        }
        if (g_heldDpadBtn != RemoteBtn::None) {
            g_heldDpadBtn = RemoteBtn::None;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s_virtualLeftDown) {
            s_virtualLeftDown = false;
            ReleaseCapture();
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            LONG tx = 0, ty = 0;
            if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                ForwardVirtualMouseInput(MOUSEEVENTF_LEFTUP, tx, ty, 0, true);
            }
            return 0;
        }
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

        if (!IsOverRemoteUi(mx, my)) {
            if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
                LONG tx = 0, ty = 0;
                if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                    s_virtualRightDown = true;
                    SetCapture(hwnd);
                    ForwardVirtualMouseInput(MOUSEEVENTF_RIGHTDOWN, tx, ty, 0, true);
                    return 0;
                }
            }
            g_isRightDragging = true;
            g_lastRightPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;
    }

    if (message == WM_RBUTTONUP) {
        if (s_virtualRightDown) {
            s_virtualRightDown = false;
            ReleaseCapture();
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            LONG tx = 0, ty = 0;
            if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                ForwardVirtualMouseInput(MOUSEEVENTF_RIGHTUP, tx, ty, 0, true);
            }
            return 0;
        }
        if (g_isRightDragging) {
            g_isRightDragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    if (message == WM_LBUTTONDBLCLK) {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));

        if (!IsOverRemoteUi(mx, my)) {
            if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
                LONG tx = 0, ty = 0;
                if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                    s_virtualLeftDown = true;
                    SetCapture(hwnd);
                    ForwardVirtualMouseInput(MOUSEEVENTF_LEFTDOWN, tx, ty, 0, true);
                }
                return 0;
            }
            ToggleFullscreen(hwnd);
            return 0;
        }
    }

    if (message == WM_MOUSEWHEEL) {
        if (g_projectionMode == ProjectionMode::VirtualScreen && g_controlVirtualScreen) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            const float mx = static_cast<float>(pt.x);
            const float my = static_cast<float>(pt.y);
            if (!IsOverRemoteUi(mx, my)) {
                LONG tx = 0, ty = 0;
                if (MapViewerPointToVirtualScreen(mx, my, &tx, &ty)) {
                    ForwardVirtualMouseInput(MOUSEEVENTF_WHEEL, tx, ty,
                                             static_cast<DWORD>(GET_WHEEL_DELTA_WPARAM(wParam)), true);
                    return 0;
                }
            }
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

        // Holding Alt lets user drag the viewer OS window from anywhere
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            return HTCAPTION;
        }

        // All remote interactions (buttons, Astroslide Info panel, dragging remote) run in client coordinates
        if (IsOverRemoteUi(cx, cy)) {
            return HTCLIENT;
        }

        // Floating trigger pill interaction
        if (!g_showRemote && inRect(cx, cy, g_pillRect)) {
            return HTCLIENT;
        }

        return HTCLIENT;
    }

    if (message == WM_CLOSE) {
        StopAllProjectionAndUnloadVirtual();
        DestroyWindow(hwnd);
        return 0;
    }

    if (message == WM_DESTROY) {
        UnloadVirtualScreen();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

} // namespace

void UpdateRoomOwnershipState() {
    static ULONGLONG s_lastProcPoll = 0;
    static ULONGLONG s_lastAudioPoll = 0;
    static bool s_hasRoomSeq = false;
    static uint32_t s_lastRoomChangeSeq = 0;
    static bool s_prevLoadingRoom = false;
    static bool s_prevInOwnRoom = false;
    static bool s_prevRoomHookReady = false;
    static bool s_prevBigscreenRunning = false;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastProcPoll >= 400) {
        s_lastProcPoll = now;
        g_bigscreenRunning = IsProcessRunning(L"Bigscreen.exe");
    }
    if (g_bigscreenRunning && now - s_lastAudioPoll >= 1000) {
        s_lastAudioPoll = now;
        ControlBigscreenSessionAudio(0);
    }

    bool roomChanged = false;
    OpenPeerMovieMapping();
    if (g_bigscreenRunning && g_peerState &&
        g_peerState->magic == bigscreen_peer_movie::kMagic) {
        g_peerStreamActive = (g_peerState->active != 0);
        if (g_peerState->roomHookReady != 0) {
            g_roomHookReady = true;
            g_inOwnRoom = (g_peerState->inOwnRoom != 0);
            g_isLoadingRoom = (g_peerState->isLoadingRoom != 0);
            if (!s_hasRoomSeq) {
                s_hasRoomSeq = true;
                s_lastRoomChangeSeq = g_peerState->roomChangeSeq;
            } else if (g_peerState->roomChangeSeq != s_lastRoomChangeSeq) {
                s_lastRoomChangeSeq = g_peerState->roomChangeSeq;
                roomChanged = true;
            }
        } else {
            g_roomHookReady = false;
            g_inOwnRoom = false;
            g_isLoadingRoom = false;
        }
        g_remoteUserCount = g_peerState->remoteUserCount;
        g_audioHookReady = (g_peerState->audioHookReady != 0);
        if (g_peerState->audioHookReady != 0) {
            if (g_peerState->handledMicToggleSeq >= g_reqMicToggleSeq) {
                g_micMuted = (g_peerState->micMuted != 0);
            }
            if (g_peerState->handledMuteAllToggleSeq >= g_reqMuteAllToggleSeq) {
                g_allMuted = (g_peerState->allMuted != 0);
            }
        }
    } else {
        g_peerStreamActive = false;
        g_roomHookReady = false;
        g_inOwnRoom = false;
        g_isLoadingRoom = false;
        g_remoteUserCount = 0;
        g_audioHookReady = false;
        if (!g_bigscreenRunning) {
            g_micMuted = false;
            g_allMuted = false;
            g_bigscreenMenuToggled = false;
            g_waitingOwnRoomForProject = false;
            s_hasRoomSeq = false;
            if (s_prevBigscreenRunning) {
                roomChanged = true;
            }
        }
    }

    s_prevLoadingRoom = g_isLoadingRoom;
    s_prevInOwnRoom = g_inOwnRoom;
    s_prevRoomHookReady = g_roomHookReady;
    s_prevBigscreenRunning = g_bigscreenRunning;

    if (roomChanged) {
        const bool wasMovieMode = g_movieOnly;
        const bool hadCustomAspect = (g_aspectPresetIndex != 0);
        if (wasMovieMode) {
            g_movieOnly = false;
            g_heldDpadBtn = RemoteBtn::None;
            g_lastAdjustedAspect = 0.0f;
            ResetLook();
            Log("Switched from Movie Mode to Theater Mode after room change");
        }
        if (hadCustomAspect) {
            g_aspectPresetIndex = 0; // Reset Aspect Ratio to Auto when changing rooms
            g_lastAdjustedAspect = 0.0f;
            SyncAspectOverrideToIpc();
            Log("Reset Aspect Ratio override to Auto after room change");
        }
        if (wasMovieMode && hadCustomAspect) {
            SetLcdNotification(L"THEATER MODE", L"ASPECT: AUTO", 2500);
        } else if (wasMovieMode) {
            SetLcdNotification(L"THEATER MODE", L"NEW ROOM", 2500);
        } else if (hadCustomAspect) {
            SetLcdNotification(L"ASPECT RATIO", L"RESET TO AUTO", 2500);
        }
    }

    if (g_waitingOwnRoomForProject && CanProjectInCurrentRoom()) {
        g_waitingOwnRoomForProject = false;
        SetLcdNotification(L"OWN ROOM READY", L"CLICK PROJECT", 3500);
        Log("Own room finished loading; Project Screen is ready to select monitor or Virtual Screen");
    }

    // Projection is only valid in the user's own room; automatically stop if user leaves or disconnects
    if (g_projectionMode != ProjectionMode::None &&
        (!g_bigscreenRunning || !g_roomHookReady || (!g_inOwnRoom && !g_isLoadingRoom))) {
        StopAllProjectionAndUnloadVirtual();
        Log("Stopped projection and unloaded Virtual Screen because user is no longer in their own room");
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--install-vdd") != nullptr) {
        return static_cast<int>(InstallOrEnableVddElevatedWorker());
    }
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
        if (g_heldDpadBtn != RemoteBtn::None && IsRemoteBtnSelectable(g_heldDpadBtn)) {
            const ULONGLONG nowTick = GetTickCount64();
            if (nowTick - g_lastDpadRepeatMs >= 45) {
                g_lastDpadRepeatMs = nowTick;
                ExecuteRemoteDpadStep(g_heldDpadBtn, true);
            }
        }
        if (g_heldProjectBtn && !g_projectHoldTriggered) {
            const ULONGLONG heldMs = GetTickCount64() - g_projectHoldStartMs;
            if (heldMs >= 2000) {
                g_projectHoldTriggered = true;
                g_heldProjectBtn = false;
                ReleaseCapture();
                if (CanProjectInCurrentRoom()) {
                    SetLcdNotification(L"PROJECT APP", L"SELECT WINDOW", 2500);
                    ShowProjectAppMenu(g_hwnd, g_rBtnProject.left, g_rBtnProject.bottom);
                } else if (CanLeaveToOwnRoomToProject()) {
                    RequestOwnRoomTransition();
                }
            } else if (heldMs >= 300 && CanProjectInCurrentRoom()) {
                SetLcdNotification(L"HOLD 2s FOR", L"PROJECT APP MENU", 250);
            }
        }
        EnsureBigscreenHookInjected();
        MaintainForegroundWhileLaunching();
        UpdateRoomOwnershipState();
        CheckFeedStatus();
        ID3D11ShaderResourceView* source = nullptr;
        bool flipVertical = false;

        if (g_projectionMode == ProjectionMode::VirtualScreen ||
            g_projectionMode == ProjectionMode::Screen) {
            CaptureProjectedMonitorFrame();
            source = g_movieOnly ? g_projectedSrv : (g_localSrv ? g_localSrv : g_projectedSrv);
            flipVertical = false;
            if (!g_movieOnly) {
                OpenLiveMapping();
                if (g_state) CopyNewFrame();
            }
        } else if (g_projectionMode == ProjectionMode::App) {
            CaptureProjectedWindowFrame();
            if (g_projectionMode == ProjectionMode::App) {
                source = g_movieOnly ? g_projectedSrv : (g_localSrv ? g_localSrv : g_projectedSrv);
                flipVertical = false;
                if (!g_movieOnly) {
                    OpenLiveMapping();
                    if (g_state) CopyNewFrame();
                }
            } else {
                source = g_localSrv;
            }
        } else {
            // Normal Bigscreen Room / Movie theater viewing
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
        if (g_projectionMode == ProjectionMode::VirtualScreen) {
            sprintf_s(title, "Bigscreen Desktop Live Viewer | PROJECTING VIRTUAL SCREEN (1920x1080)%s | Tab/M=REMOTE", mode);
        } else if (g_projectionMode == ProjectionMode::Screen) {
            sprintf_s(title, "Bigscreen Desktop Live Viewer | PROJECTING SCREEN%s | Tab/M=REMOTE", mode);
        } else if (g_projectionMode == ProjectionMode::App && (!g_projectedAppDisplayName.empty() || !g_projectedTitle.empty())) {
            std::wstring shortTitle = !g_projectedAppDisplayName.empty() ? g_projectedAppDisplayName : g_projectedTitle;
            if (shortTitle.length() > 28) shortTitle = shortTitle.substr(0, 25) + L"...";
            sprintf_s(title, "Bigscreen Desktop Live Viewer | PROJECTING APP: %ls%s | Tab/M=REMOTE", shortTitle.c_str(), mode);
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
