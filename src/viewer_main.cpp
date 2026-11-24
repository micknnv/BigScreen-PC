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

#include "live_frame_ipc.h"
#include "third_person_gpu_ipc.h"
#include "controller_control_ipc.h"

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

namespace {

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

// Remote Control & Presentation State (Segment 1)
bool g_showRemote = true;
bool g_movieOnly = false;
bool g_isFullscreen = false;
bool g_alwaysOnTop = false;
WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };

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
D2D1_RECT_F g_btn1stPersonRect{};
D2D1_RECT_F g_btn3rdPersonRect{};
D2D1_RECT_F g_btnWholeTheaterRect{};
D2D1_RECT_F g_btnMovieOnlyRect{};
D2D1_RECT_F g_btnFullscreenRect{};
D2D1_RECT_F g_btnPinOnTopRect{};
D2D1_RECT_F g_btnRecenterRect{};
D2D1_RECT_F g_btnMenuRect{};
D2D1_RECT_F g_btnSystemRect{};
D2D1_RECT_F g_pillRect{};

bool g_closeHovered = false;
bool g_btn1stHovered = false;
bool g_btn3rdHovered = false;
bool g_btnWholeHovered = false;
bool g_btnMovieHovered = false;
bool g_btnFsHovered = false;
bool g_btnPinHovered = false;
bool g_btnRecenterHovered = false;
bool g_btnMenuHovered = false;
bool g_btnSystemHovered = false;
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
                    SetForegroundWindow(g_hwnd);
                    Log("New audio/video feed detected (audio peak=%.3f). Auto-minimized Bigscreen desktop menu.", g_audioPeak);
                }
            }
        }
    }
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

void DrawButton(const D2D1_RECT_F& rect, const wchar_t* text, bool active, bool hovered, float radius = 6.0f) {
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
    ID2D1SolidColorBrush* bgBrush = active ? g_brushBtnActiveBg : (hovered ? g_brushBtnHoverBg : g_brushBtnInactiveBg);
    ID2D1SolidColorBrush* borderBrush = active ? g_brushBtnActiveBorder : (hovered ? g_brushAccent : g_brushBtnBorder);
    
    g_d2dRenderTarget->FillRoundedRectangle(&rounded, bgBrush);
    g_d2dRenderTarget->DrawRoundedRectangle(&rounded, borderBrush, active ? 1.5f : 1.0f);
    
    ID2D1SolidColorBrush* textBrush = active ? g_brushTextWhite : (hovered ? g_brushTextWhite : g_brushTextMuted);
    g_d2dRenderTarget->DrawText(
        text, static_cast<UINT32>(wcslen(text)),
        g_textFormatBtn, &rect, textBrush);
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
    const float panelHeight = 475.0f;
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
    D2D1_RECT_F titleRect = D2D1::RectF(left + 32.0f, top, right - 40.0f, top + 42.0f);
    g_d2dRenderTarget->DrawText(
        titleText, static_cast<UINT32>(wcslen(titleText)),
        g_textFormatTitle, &titleRect, g_brushTextWhite);

    // Close button [✕]
    g_closeRect = D2D1::RectF(right - 36.0f, top + 8.0f, right - 10.0f, top + 34.0f);
    DrawCloseButton(g_closeRect, g_closeHovered);

    // SECTION 1: VIEW MODE
    const wchar_t* s1Text = L"VIEW MODE";
    D2D1_RECT_F s1Rect = D2D1::RectF(left + 18.0f, top + 50.0f, right - 18.0f, top + 68.0f);
    g_d2dRenderTarget->DrawText(
        s1Text, static_cast<UINT32>(wcslen(s1Text)),
        g_textFormatSection, &s1Rect, g_brushTextSection);

    g_btn1stPersonRect = D2D1::RectF(left + 18.0f, top + 70.0f, left + 148.0f, top + 104.0f);
    g_btn3rdPersonRect = D2D1::RectF(left + 156.0f, top + 70.0f, right - 18.0f, top + 104.0f);
    DrawButton(g_btn1stPersonRect, L"1st Person", g_viewSource == ViewSource::FirstPerson, g_btn1stHovered);
    DrawButton(g_btn3rdPersonRect, L"3rd Person", g_viewSource == ViewSource::ThirdPerson, g_btn3rdHovered);

    // SECTION 2: THEATER FRAMING
    const wchar_t* s2Text = L"THEATER FRAMING";
    D2D1_RECT_F s2Rect = D2D1::RectF(left + 18.0f, top + 116.0f, right - 18.0f, top + 134.0f);
    g_d2dRenderTarget->DrawText(
        s2Text, static_cast<UINT32>(wcslen(s2Text)),
        g_textFormatSection, &s2Rect, g_brushTextSection);

    g_btnWholeTheaterRect = D2D1::RectF(left + 18.0f, top + 136.0f, left + 148.0f, top + 170.0f);
    g_btnMovieOnlyRect = D2D1::RectF(left + 156.0f, top + 136.0f, right - 18.0f, top + 170.0f);
    DrawButton(g_btnWholeTheaterRect, L"Whole Theater", !g_movieOnly, g_btnWholeHovered);
    DrawButton(g_btnMovieOnlyRect, L"Movie Only", g_movieOnly, g_btnMovieHovered);

    // SECTION 3: WINDOW & DISPLAY
    const wchar_t* s3Text = L"WINDOW & DISPLAY";
    D2D1_RECT_F s3Rect = D2D1::RectF(left + 18.0f, top + 182.0f, right - 18.0f, top + 200.0f);
    g_d2dRenderTarget->DrawText(
        s3Text, static_cast<UINT32>(wcslen(s3Text)),
        g_textFormatSection, &s3Rect, g_brushTextSection);

    g_btnFullscreenRect = D2D1::RectF(left + 18.0f, top + 202.0f, left + 148.0f, top + 236.0f);
    g_btnPinOnTopRect = D2D1::RectF(left + 156.0f, top + 202.0f, right - 18.0f, top + 236.0f);
    DrawButton(g_btnFullscreenRect, g_isFullscreen ? L"Windowed" : L"Fullscreen", g_isFullscreen, g_btnFsHovered);
    DrawButton(g_btnPinOnTopRect, g_alwaysOnTop ? L"Pinned Top" : L"Pin on Top", g_alwaysOnTop, g_btnPinHovered);

    // SECTION 4: QUICK ACTIONS
    const wchar_t* s4Text = L"QUICK ACTIONS";
    D2D1_RECT_F s4Rect = D2D1::RectF(left + 18.0f, top + 248.0f, right - 18.0f, top + 266.0f);
    g_d2dRenderTarget->DrawText(
        s4Text, static_cast<UINT32>(wcslen(s4Text)),
        g_textFormatSection, &s4Rect, g_brushTextSection);

    g_btnRecenterRect = D2D1::RectF(left + 18.0f, top + 268.0f, left + 140.0f, top + 300.0f);
    g_btnMenuRect = D2D1::RectF(left + 148.0f, top + 268.0f, right - 18.0f, top + 300.0f);
    DrawButton(g_btnRecenterRect, L"Recenter", false, g_btnRecenterHovered);
    DrawButton(g_btnMenuRect, L"BigScreen Menu", false, g_btnMenuHovered);

    g_btnSystemRect = D2D1::RectF(left + 18.0f, top + 306.0f, right - 18.0f, top + 338.0f);
    DrawButton(g_btnSystemRect, L"SteamVR Dashboard", false, g_btnSystemHovered);

    // SECTION 5: MEDIA FEED STATUS
    const wchar_t* s5Text = L"MEDIA FEED STATUS";
    D2D1_RECT_F s5Rect = D2D1::RectF(left + 18.0f, top + 346.0f, right - 18.0f, top + 364.0f);
    g_d2dRenderTarget->DrawText(
        s5Text, static_cast<UINT32>(wcslen(s5Text)),
        g_textFormatSection, &s5Rect, g_brushTextSection);

    D2D1_RECT_F feedBoxRect = D2D1::RectF(left + 18.0f, top + 366.0f, right - 18.0f, top + 396.0f);
    D2D1_ROUNDED_RECT feedBox = D2D1::RoundedRect(feedBoxRect, 6.0f, 6.0f);
    g_d2dRenderTarget->FillRoundedRectangle(&feedBox, g_brushBtnInactiveBg);
    g_d2dRenderTarget->DrawRoundedRectangle(&feedBox, g_feedActive ? g_brushFeedActive : g_brushBtnBorder, 1.0f);

    const D2D1_ELLIPSE feedDot = D2D1::Ellipse(
        D2D1::Point2F(feedBoxRect.left + 14.0f, (feedBoxRect.top + feedBoxRect.bottom) * 0.5f),
        4.0f, 4.0f);
    g_d2dRenderTarget->FillEllipse(&feedDot, g_feedActive ? g_brushFeedActive : g_brushFeedStandby);

    wchar_t feedText[64]{};
    if (g_feedActive) {
        swprintf_s(feedText, L"ACTIVE  |  Streaming (Audio: %.0f%%)", g_audioPeak * 100.0f);
    } else {
        wcscpy_s(feedText, L"IDLE  |  No Active Stream");
    }
    D2D1_RECT_F feedTextRect = D2D1::RectF(feedBoxRect.left + 24.0f, feedBoxRect.top, feedBoxRect.right - 8.0f, feedBoxRect.bottom);
    g_d2dRenderTarget->DrawText(
        feedText, static_cast<UINT32>(wcslen(feedText)),
        g_textFormatFooter, &feedTextRect, g_feedActive ? g_brushFeedActive : g_brushTextMuted);

    // Footer separator line
    g_d2dRenderTarget->DrawLine(
        D2D1::Point2F(left + 18.0f, top + 404.0f),
        D2D1::Point2F(right - 18.0f, top + 404.0f),
        g_brushSeparator, 1.0f);

    // Footer Hotkey Hints
    const wchar_t* footerText = L"Mouse Move: Laser Pointer  |  Left-Click: Trigger\nRight-Click Drag: Look  |  Esc: SteamVR Dashboard\nR: Recenter  |  Alt+Drag or Header: Move Window";
    D2D1_RECT_F footerRect = D2D1::RectF(left + 8.0f, top + 408.0f, right - 8.0f, bottom - 6.0f);
    g_d2dRenderTarget->DrawText(
        footerText, static_cast<UINT32>(wcslen(footerText)),
        g_textFormatFooter, &footerRect, g_brushTextMuted);
}

void ReleaseAll() {
    ReleaseSharedResources();
    ReleaseThirdPersonResources();
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

    if (g_state) UnmapViewOfFile(g_state);
    if (g_mapping) CloseHandle(g_mapping);
    if (g_rtv) g_rtv->Release();
    if (g_sampler) g_sampler->Release();
    if (g_ps) g_ps->Release();
    if (g_psFlip) g_psFlip->Release();
    if (g_psMovie) g_psMovie->Release();
    if (g_psMovieFlip) g_psMovieFlip->Release();
    if (g_vs) g_vs->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    if (g_adapter) g_adapter->Release();
    g_state = nullptr;
    g_mapping = nullptr;
    g_rtv = nullptr;
    g_sampler = nullptr;
    g_ps = nullptr;
    g_psFlip = nullptr;
    g_psMovie = nullptr;
    g_psMovieFlip = nullptr;
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
    const float sourceAspect = g_sourceHeight ? static_cast<float>(g_sourceWidth) / g_sourceHeight : 16.0f / 9.0f;
    const float windowAspect = windowWidth / windowHeight;
    D3D11_VIEWPORT viewport{};
    if (g_movieOnly) {
        // Dedicated Movie Player mode: clean 16:9 presentation, centered, black letterbox/pillarbox borders
        constexpr float movieAspect = 16.0f / 9.0f;
        if (windowAspect > movieAspect) {
            viewport.Height = windowHeight;
            viewport.Width = windowHeight * movieAspect;
            viewport.TopLeftX = (windowWidth - viewport.Width) * 0.5f;
            viewport.TopLeftY = 0.0f;
        } else {
            viewport.Width = windowWidth;
            viewport.Height = windowWidth / movieAspect;
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = (windowHeight - viewport.Height) * 0.5f;
        }
    } else {
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
    }
    g_activeViewport = viewport;

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    if (source) {
        g_context->RSSetViewports(1, &viewport);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vs, nullptr, 0);
        ID3D11PixelShader* selectedPs = nullptr;
        if (g_movieOnly) {
            selectedPs = flipVertical ? g_psMovieFlip : g_psMovie;
        } else {
            selectedPs = flipVertical ? g_psFlip : g_ps;
        }
        g_context->PSSetShader(selectedPs, nullptr, 0);
        g_context->PSSetSamplers(0, 1, &g_sampler);
        g_context->PSSetShaderResources(0, 1, &source);
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
        if (wParam == '1' || wParam == '3') {
            SelectViewSource(wParam == '3' ? ViewSource::ThirdPerson : ViewSource::FirstPerson);
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
            g_btn1stHovered = inRect(mx, my, g_btn1stPersonRect);
            g_btn3rdHovered = inRect(mx, my, g_btn3rdPersonRect);
            g_btnWholeHovered = inRect(mx, my, g_btnWholeTheaterRect);
            g_btnMovieHovered = inRect(mx, my, g_btnMovieOnlyRect);
            g_btnFsHovered = inRect(mx, my, g_btnFullscreenRect);
            g_btnPinHovered = inRect(mx, my, g_btnPinOnTopRect);
            g_btnRecenterHovered = inRect(mx, my, g_btnRecenterRect);
            g_btnMenuHovered = inRect(mx, my, g_btnMenuRect);
            g_btnSystemHovered = inRect(mx, my, g_btnSystemRect);
        } else if (!g_showRemote && inRect(mx, my, g_pillRect)) {
            g_pointerActive = false;
            g_pillHovered = true;
        } else {
            // Interactive VR Client Area: mouse controls pointer aim & look
            g_pillHovered = false;
            g_closeHovered = false;
            g_btn1stHovered = false;
            g_btn3rdHovered = false;
            g_btnWholeHovered = false;
            g_btnMovieHovered = false;
            g_btnFsHovered = false;
            g_btnPinHovered = false;
            g_btnRecenterHovered = false;
            g_btnMenuHovered = false;
            g_btnSystemHovered = false;

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
        g_btn1stHovered = false;
        g_btn3rdHovered = false;
        g_btnWholeHovered = false;
        g_btnMovieHovered = false;
        g_btnFsHovered = false;
        g_btnPinHovered = false;
        g_btnRecenterHovered = false;
        g_btnMenuHovered = false;
        g_btnSystemHovered = false;
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
            if (inRect(mx, my, g_btn1stPersonRect)) {
                SelectViewSource(ViewSource::FirstPerson);
                return 0;
            }
            if (inRect(mx, my, g_btn3rdPersonRect)) {
                SelectViewSource(ViewSource::ThirdPerson);
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
                return 0;
            }
            if (inRect(mx, my, g_btnFullscreenRect)) {
                ToggleFullscreen(hwnd);
                return 0;
            }
            if (inRect(mx, my, g_btnPinOnTopRect)) {
                ToggleAlwaysOnTop(hwnd);
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
            if (cy <= g_remoteRect.top + 42.0f && !inRect(cx, cy, g_closeRect)) {
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
        CheckFeedStatus();
        ID3D11ShaderResourceView* source = nullptr;
        bool flipVertical = false;
        if (g_viewSource == ViewSource::ThirdPerson) {
            const bool copied = CopyThirdPersonFrame();
            if (copied && g_thirdSrv && (!g_thirdLastFrame || GetTickCount64() - g_thirdLastFrame <= 1500)) {
                source = g_thirdSrv;
                flipVertical = true;
            } else {
                // Native driver-level 3rd person camera (OpenVR Compositor eye texture with HMD back/up offset)
                OpenLiveMapping();
                if (g_state) CopyNewFrame();
                source = g_localSrv;
                flipVertical = false;
            }
        }
        if (g_viewSource == ViewSource::FirstPerson) {
            OpenLiveMapping();
            if (g_state) CopyNewFrame();
            source = g_localSrv;
            flipVertical = false;
        }
        char title[256]{};
        const char* view = g_viewSource == ViewSource::ThirdPerson ? "3RD PERSON" : "1ST PERSON";
        const char* mode = g_movieOnly ? " | Mode: MOVIE ONLY" : " | Mode: WHOLE THEATER";
        sprintf_s(title, "Bigscreen Desktop Live Viewer | View: %s%s | Tab/M=REMOTE  Click=Trigger  RMB=Look", view, mode);
        SetWindowTextA(g_hwnd, title);
        Render(source, flipVertical);
        UpdateMetrics();
    }
    ReleaseAll();
    return 0;
}
