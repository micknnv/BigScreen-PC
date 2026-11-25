#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include "../src/third_person_gpu_ipc.h"
#include "../src/projected_video_ipc.h"
#include "../src/peer_movie_ipc.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace {
// --------------------------------------------------------------------------
// Third-person camera export state (existing)
// --------------------------------------------------------------------------
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
// Shared Hook Types
// --------------------------------------------------------------------------
typedef void (*UnityRenderEvent)(int eventId);
typedef UnityRenderEvent (*pfnBigSoupProcessLocalDesktops)();
typedef UnityRenderEvent (*pfnBigSoupProcessPeerDesktops)();
typedef ID3D11Texture2D* (*pfnBigSoupGetNativeDesktopTexturePointer)(int index);
typedef int (*pfnBigSoupGetNumberOfIncomingVideoStreams)();

typedef UnityRenderEvent (*pfnBigMediaRenderNextFrameFunc)();
typedef void* (*pfnBigMediaGetVideoTextureId)(int playerId, int eyeIndex);
typedef int (*pfnBigMediaGetVideoTextureWidth)(int playerId);
typedef int (*pfnBigMediaGetVideoTextureHeight)(int playerId);

pfnBigSoupGetNativeDesktopTexturePointer g_pfnGetNativeDesktop = nullptr;
pfnBigSoupGetNumberOfIncomingVideoStreams g_pfnGetIncomingStreamCount = nullptr;
UnityRenderEvent g_originalProcessLocalDesktops = nullptr;
UnityRenderEvent g_originalProcessPeerDesktops = nullptr;
uint8_t* g_pTrampolineLocal = nullptr;
uint8_t* g_pTrampolinePeer = nullptr;
bool g_localHookInstalled = false;
bool g_peerHookInstalled = false;

pfnBigMediaGetVideoTextureId g_pfnMediaGetTexId = nullptr;
pfnBigMediaGetVideoTextureWidth g_pfnMediaGetWidth = nullptr;
pfnBigMediaGetVideoTextureHeight g_pfnMediaGetHeight = nullptr;
UnityRenderEvent g_originalMediaRenderNextFrame = nullptr;
uint8_t* g_pTrampolineMedia = nullptr;
bool g_mediaHookInstalled = false;

// --------------------------------------------------------------------------
// Local Desktop Projection Routing State
// --------------------------------------------------------------------------
HANDLE g_projectedMapping = nullptr;
bigscreen_projected_video::State* g_projectedState = nullptr;
ID3D11Texture2D* g_sharedVideoTex = nullptr;
ID3D11ShaderResourceView* g_sharedVideoSrv = nullptr;
uint64_t g_currentSharedHandle = 0;
ID3D11Texture2D* g_blackTex = nullptr;
ID3D11RenderTargetView* g_blackRtv = nullptr;
ID3D11ShaderResourceView* g_blackSrv = nullptr;
uint32_t g_blackMips = 0;
ID3D11Texture2D* g_routeStagingTex = nullptr;
ID3D11RenderTargetView* g_routeStagingRtv = nullptr;
ID3D11ShaderResourceView* g_routeStagingSrv = nullptr;
uint32_t g_routeStagingW = 0;
uint32_t g_routeStagingH = 0;
uint32_t g_routeStagingMips = 0;
DXGI_FORMAT g_routeStagingFmt = DXGI_FORMAT_UNKNOWN;

// --------------------------------------------------------------------------
// Remote Peer / MediaPlayer Movie Isolation State
// --------------------------------------------------------------------------
CRITICAL_SECTION g_peerCs;
bool g_peerCsInit = false;

uintptr_t* g_ppDesktopManager = nullptr;
uint32_t g_peerVecBeginOff = 0;
uint32_t g_peerVecEndOff = 0;

HANDLE g_peerMapping = nullptr;
bigscreen_peer_movie::State* g_peerState = nullptr;

// --------------------------------------------------------------------------
// Screen Aspect Ratio Override Hooks (MonitorSizer, MirroredScreen, BigMediaRenderer)
// --------------------------------------------------------------------------
struct Il2CppVec3 {
    float x;
    float y;
    float z;
};

typedef const void* (*pfnIl2CppClassGetType)(const void* klass);
typedef void* (*pfnIl2CppTypeGetObject)(const void* type);
typedef void* (*pfnIl2CppClassGetFieldFromName)(const void* klass, const char* name);
typedef size_t (*pfnIl2CppFieldGetOffset)(const void* field);
typedef void (*pfnIl2CppFieldGetValue)(void* obj, const void* field, void* value);
typedef void (*pfnIl2CppFieldSetValue)(void* obj, const void* field, void* value);
typedef uint32_t (*pfnIl2CppArrayLength)(const void* arr);
typedef const void* (*pfnIl2CppClassGetMethods)(const void* klass, void** iter);
typedef const char* (*pfnIl2CppMethodGetName)(const void* method);
typedef uint32_t (*pfnIl2CppMethodGetParamCount)(const void* method);
typedef uint8_t (*pfnIl2CppMethodIsGeneric)(const void* method);

typedef void (*pfnMonitorSetAspectRatio)(void* self, int width, int height, const void* mi);
typedef void (*pfnMirroredUpdateAspect)(void* self, void* texture, const void* mi);

pfnIl2CppClassGetType g_fnClassGetType = nullptr;
pfnIl2CppTypeGetObject g_fnTypeGetObject = nullptr;
pfnIl2CppClassGetFieldFromName g_fnClassGetField = nullptr;
pfnIl2CppFieldGetOffset g_fnFieldGetOffset = nullptr;
pfnIl2CppFieldGetValue g_fnFieldGetValue = nullptr;
pfnIl2CppFieldSetValue g_fnFieldSetValue = nullptr;
pfnIl2CppArrayLength g_fnArrayLength = nullptr;

void* g_klassMonitorSizer = nullptr;
void* g_klassMirroredScreen = nullptr;
void* g_klassBigMediaRenderer = nullptr;
void* g_klassPresentationSectionSizer = nullptr;
void* g_typeMonitorSizer = nullptr;
void* g_typeMirroredScreen = nullptr;
void* g_typeBigMediaRenderer = nullptr;
void* g_typePresentationSectionSizer = nullptr;

const void* g_mFindObjectsOfType = nullptr;
const void* g_mCompGetTransform = nullptr;
const void* g_mGoGetTransform = nullptr;
const void* g_mTransformGetLocalScale = nullptr;
const void* g_mTransformSetLocalScale = nullptr;
const void* g_mMonitorSetAspect = nullptr;
const void* g_mMirroredUpdateAspect = nullptr;

const void* g_fldMediaOrigScale = nullptr;
const void* g_fldMediaScreensGo = nullptr;
const void* g_fldMediaRegularRenderer = nullptr;
const void* g_fldMediaResizeRequired = nullptr;
const void* g_fldMediaCurrentSource = nullptr;
const void* g_fldSizerAspectRatio = nullptr;
const void* g_fldMirroredOrigScale = nullptr;
const void* g_fldMirroredMainTexture = nullptr;

pfnMonitorSetAspectRatio g_origMonitorSetAspect = nullptr;
pfnMonitorSetAspectRatio g_origMonitorLerpAspect = nullptr;
pfnMirroredUpdateAspect g_origMirroredUpdateAspect = nullptr;
uint8_t* g_pTrampMonitorSet = nullptr;
uint8_t* g_pTrampMonitorLerp = nullptr;
uint8_t* g_pTrampMirroredUpdate = nullptr;
bool g_screenAspectHooksAttempted = false;

struct TrackedSizer {
    void* ptr;
    int origW;
    int origH;
};
struct TrackedMirrored {
    void* ptr;
    void* lastTex;
};
struct TrackedPresSizer {
    void* ptr;
    Il2CppVec3 origScale;
};
TrackedSizer g_trackedSizers[32]{};
TrackedMirrored g_trackedMirrored[32]{};
TrackedPresSizer g_trackedPresSizers[32]{};
bool g_inAspectReapply = false;
float g_lastAppliedAspectOverride = -1.0f;
ULONGLONG g_lastAspectSweepMs = 0;

ID3D11Device* g_peerDev = nullptr;
ID3D11Texture2D* g_peerExportTex = nullptr;
ID3D11RenderTargetView* g_peerExportRtv = nullptr;
IDXGIKeyedMutex* g_peerExportMutex = nullptr;
HANDLE g_peerSharedHandle = nullptr;
uint32_t g_peerExportW = 0;
uint32_t g_peerExportH = 0;
uint64_t g_peerGeneration = 0;
uint64_t g_peerSequence = 0;

ID3D11VertexShader* g_peerVs = nullptr;
ID3D11PixelShader* g_peerPsNv12 = nullptr;
ID3D11PixelShader* g_peerPsRgba = nullptr;
ID3D11Buffer* g_peerCb = nullptr;
ID3D11SamplerState* g_peerSampler = nullptr;
ID3D11BlendState* g_peerBlend = nullptr;
ID3D11DepthStencilState* g_peerDepth = nullptr;
ID3D11RasterizerState* g_peerRaster = nullptr;
bool g_peerGpuFailed = false;

ULONGLONG g_lastBigSoupPeerMs = 0;
ULONGLONG g_lastMediaPlayerMs = 0;

// --------------------------------------------------------------------------
// IL2CPP Room Ownership & Own-Room Switch State (Bigscreen.App)
// --------------------------------------------------------------------------
typedef void (*pfnAppTick)(void* appThis, const void* methodInfo);
typedef void* (*pfnIl2CppDomainGet)();
typedef const void** (*pfnIl2CppDomainGetAssemblies)(void* domain, size_t* size);
typedef const void* (*pfnIl2CppAssemblyGetImage)(const void* assembly);
typedef void* (*pfnIl2CppClassFromName)(const void* image, const char* ns, const char* name);
typedef const void* (*pfnIl2CppClassGetMethodFromName)(void* klass, const char* name, int argsCount);
typedef void* (*pfnIl2CppRuntimeInvoke)(const void* method, void* obj, void** params, void** exc);
typedef void* (*pfnIl2CppObjectUnbox)(void* obj);
typedef void* (*pfnIl2CppThreadAttach)(void* domain);
typedef void (*pfnIl2CppThreadDetach)(void* thread);

pfnIl2CppRuntimeInvoke g_il2cppInvoke = nullptr;
pfnIl2CppObjectUnbox g_il2cppUnbox = nullptr;

const void* g_mGetCurrentRoom = nullptr;
const void* g_mGetIsLoadingScene = nullptr;
const void* g_mLeaveRoom = nullptr;
const void* g_mGetIsLocalUserOwner = nullptr;
const void* g_mGetIsSinglePlayer = nullptr;
const void* g_mInLobby = nullptr;

pfnAppTick g_originalAppTick = nullptr;
uint8_t* g_pTrampolineAppTick = nullptr;
bool g_appTickHookInstalled = false;
uint64_t g_handledOwnRoomSeq = 0;
ULONGLONG g_lastRoomPollMs = 0;
ULONGLONG g_lastLeaveRoomMs = 0;

struct alignas(16) PeerNv12Cb {
    float yUseAlpha;
    float uvUseAlpha;
    float pad0;
    float pad1;
};

struct ActivePeerStream {
    ID3D11ShaderResourceView* ySrv;
    ID3D11ShaderResourceView* uvSrv;
    uint32_t width;
    uint32_t height;
};

void ReleasePeerPipeline() {
    if (g_peerRaster) { g_peerRaster->Release(); g_peerRaster = nullptr; }
    if (g_peerDepth) { g_peerDepth->Release(); g_peerDepth = nullptr; }
    if (g_peerBlend) { g_peerBlend->Release(); g_peerBlend = nullptr; }
    if (g_peerSampler) { g_peerSampler->Release(); g_peerSampler = nullptr; }
    if (g_peerCb) { g_peerCb->Release(); g_peerCb = nullptr; }
    if (g_peerPsRgba) { g_peerPsRgba->Release(); g_peerPsRgba = nullptr; }
    if (g_peerPsNv12) { g_peerPsNv12->Release(); g_peerPsNv12 = nullptr; }
    if (g_peerVs) { g_peerVs->Release(); g_peerVs = nullptr; }
}

void ReleasePeerExportTarget() {
    if (g_peerExportMutex) { g_peerExportMutex->Release(); g_peerExportMutex = nullptr; }
    if (g_peerExportRtv) { g_peerExportRtv->Release(); g_peerExportRtv = nullptr; }
    if (g_peerExportTex) { g_peerExportTex->Release(); g_peerExportTex = nullptr; }
    g_peerSharedHandle = nullptr;
    g_peerExportW = 0;
    g_peerExportH = 0;
}

void EnsurePeerMovieIpc() {
    if (!g_peerMapping) {
        g_peerMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(bigscreen_peer_movie::State), bigscreen_peer_movie::kMappingName);
    }
    if (g_peerMapping && !g_peerState) {
        g_peerState = static_cast<bigscreen_peer_movie::State*>(
            MapViewOfFile(g_peerMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_peer_movie::State)));
        if (g_peerState) {
            *g_peerState = {};
            g_peerState->magic = bigscreen_peer_movie::kMagic;
            g_peerState->version = bigscreen_peer_movie::kVersion;
        }
    }
}

void ClearPeerMovieActive() {
    EnsurePeerMovieIpc();
    if (g_peerState) {
        g_peerState->active = 0;
        g_peerState->sourceKind = static_cast<uint32_t>(bigscreen_peer_movie::SourceKind::None);
    }
}

bool EnsurePeerPipeline(ID3D11Device* pDevice) {
    if (g_peerGpuFailed) return false;
    if (g_peerDev != pDevice) {
        ReleasePeerExportTarget();
        ReleasePeerPipeline();
        if (g_peerDev) { g_peerDev->Release(); g_peerDev = nullptr; }
        g_peerDev = pDevice;
        g_peerDev->AddRef();
    }
    if (g_peerVs && g_peerPsNv12 && g_peerPsRgba && g_peerCb) return true;

    HMODULE hComp = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!hComp) hComp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hComp) { g_peerGpuFailed = true; return false; }

    auto pD3DCompile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(hComp, "D3DCompile"));
    if (!pD3DCompile) { g_peerGpuFailed = true; return false; }

    const char* vsCode =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "VSOut main(uint id : SV_VertexID) {"
        "  VSOut o;"
        "  float2 uv = float2((id << 1) & 2, id & 2);"
        "  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);"
        "  o.uv = uv;"
        "  return o;"
        "}";

    const char* psNv12Code =
        "Texture2D<float4> texY : register(t0);"
        "Texture2D<float4> texUV : register(t1);"
        "cbuffer Params : register(b0) { float yUseAlpha; float uvUseAlpha; float pad0; float pad1; };"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
        "  uint yw = 0, yh = 0, uw = 0, uh = 0;"
        "  texY.GetDimensions(yw, yh);"
        "  texUV.GetDimensions(uw, uh);"
        "  int px = clamp(int(uv.x * yw), 0, max(0, int(yw) - 1));"
        "  int py = clamp(int(uv.y * yh), 0, max(0, int(yh) - 1));"
        "  float4 ys = texY.Load(int3(px, py, 0));"
        "  float y = (yUseAlpha > 0.5) ? ys.a : ys.r;"
        "  int uy = clamp(py >> 1, 0, max(0, int(uh) - 1));"
        "  float u = 0.5, v = 0.5;"
        "  if (uw * 2 <= yw + 1 && uvUseAlpha < 0.5) {"
        "    int hx = clamp(px >> 1, 0, max(0, int(uw) - 1));"
        "    float4 rg = texUV.Load(int3(hx, uy, 0));"
        "    u = rg.r; v = rg.g;"
        "  } else {"
        "    int ux = clamp(px & ~1, 0, max(0, int(uw) - 2));"
        "    float4 uvs0 = texUV.Load(int3(ux, uy, 0));"
        "    float4 uvs1 = texUV.Load(int3(ux + 1, uy, 0));"
        "    u = (uvUseAlpha > 0.5) ? uvs0.a : uvs0.r;"
        "    v = (uvUseAlpha > 0.5) ? uvs1.a : uvs1.r;"
        "  }"
        "  float yv = (y - 16.0 / 255.0) * (255.0 / 219.0);"
        "  float uu = (u - 128.0 / 255.0) * (255.0 / 224.0);"
        "  float vv = (v - 128.0 / 255.0) * (255.0 / 224.0);"
        "  float3 rgb = float3("
        "    yv + 1.5748 * vv,"
        "    yv - 0.187324 * uu - 0.468124 * vv,"
        "    yv + 1.8556 * uu);"
        "  return float4(saturate(rgb), 1.0);"
        "}";

    const char* psRgbaCode =
        "Texture2D<float4> texSrc : register(t0);"
        "SamplerState samp : register(s0);"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
        "  float4 c = texSrc.Sample(samp, uv);"
        "  return float4(c.rgb, 1.0);"
        "}";

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;

    if (FAILED(pD3DCompile(vsCode, std::strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &err))) {
        if (err) err->Release();
        g_peerGpuFailed = true;
        return false;
    }
    HRESULT hr = pDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_peerVs);
    blob->Release();
    if (FAILED(hr)) { g_peerGpuFailed = true; return false; }

    if (FAILED(pD3DCompile(psNv12Code, std::strlen(psNv12Code), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err))) {
        if (err) err->Release();
        g_peerGpuFailed = true;
        return false;
    }
    hr = pDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_peerPsNv12);
    blob->Release();
    if (FAILED(hr)) { g_peerGpuFailed = true; return false; }

    if (FAILED(pD3DCompile(psRgbaCode, std::strlen(psRgbaCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err))) {
        if (err) err->Release();
        g_peerGpuFailed = true;
        return false;
    }
    hr = pDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_peerPsRgba);
    blob->Release();
    if (FAILED(hr)) { g_peerGpuFailed = true; return false; }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(PeerNv12Cb);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(pDevice->CreateBuffer(&cbDesc, nullptr, &g_peerCb))) { g_peerGpuFailed = true; return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(pDevice->CreateSamplerState(&sd, &g_peerSampler))) { g_peerGpuFailed = true; return false; }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    pDevice->CreateBlendState(&bd, &g_peerBlend);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    pDevice->CreateDepthStencilState(&dsd, &g_peerDepth);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    pDevice->CreateRasterizerState(&rd, &g_peerRaster);

    return true;
}

bool EnsurePeerExportTarget(ID3D11Device* pDevice, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (!EnsurePeerPipeline(pDevice)) return false;
    if (g_peerExportTex && g_peerExportW == w && g_peerExportH == h) return true;

    ReleasePeerExportTarget();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &g_peerExportTex))) return false;
    if (FAILED(pDevice->CreateRenderTargetView(g_peerExportTex, nullptr, &g_peerExportRtv))) {
        ReleasePeerExportTarget(); return false;
    }
    if (FAILED(g_peerExportTex->QueryInterface(IID_PPV_ARGS(&g_peerExportMutex)))) {
        ReleasePeerExportTarget(); return false;
    }
    IDXGIResource* res = nullptr;
    if (FAILED(g_peerExportTex->QueryInterface(IID_PPV_ARGS(&res)))) {
        ReleasePeerExportTarget(); return false;
    }
    HRESULT hrHandle = res->GetSharedHandle(&g_peerSharedHandle);
    res->Release();
    if (FAILED(hrHandle) || !g_peerSharedHandle) {
        ReleasePeerExportTarget(); return false;
    }

    g_peerExportW = w;
    g_peerExportH = h;
    g_peerGeneration = GetTickCount64();
    return true;
}

bool AcquirePeerMutexForWrite() {
    if (!g_peerExportMutex) return false;
    if (SUCCEEDED(g_peerExportMutex->AcquireSync(0, 0))) return true;
    // If consumer wasn't reading (e.g. Theater Mode or restarted), key 1 is sitting idle; overwrite cleanly.
    if (SUCCEEDED(g_peerExportMutex->AcquireSync(1, 0))) return true;
    return false;
}

struct SavedD3D11State {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    UINT numVp = 1;
    D3D11_VIEWPORT vp{};
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* il = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* srvs[2]{};
    ID3D11SamplerState* samp = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;

    void Save(ID3D11DeviceContext* ctx) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->RSGetViewports(&numVp, &vp);
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->IAGetInputLayout(&il);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->PSGetShaderResources(0, 2, srvs);
        ctx->PSGetSamplers(0, 1, &samp);
        ctx->PSGetConstantBuffers(0, 1, &cb);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&depth, &stencilRef);
        ctx->RSGetState(&raster);
    }

    void Restore(ID3D11DeviceContext* ctx) {
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        if (numVp > 0) ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetInputLayout(il);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(depth, stencilRef);
        ctx->RSSetState(raster);

        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (il) il->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (srvs[0]) srvs[0]->Release();
        if (srvs[1]) srvs[1]->Release();
        if (samp) samp->Release();
        if (cb) cb->Release();
        if (blend) blend->Release();
        if (depth) depth->Release();
        if (raster) raster->Release();
    }
};

void PublishPeerFrameMetadata(bigscreen_peer_movie::SourceKind kind, uint32_t w, uint32_t h) {
    EnsurePeerMovieIpc();
    if (g_peerState) {
        g_peerState->magic = bigscreen_peer_movie::kMagic;
        g_peerState->version = bigscreen_peer_movie::kVersion;
        g_peerState->sourceKind = static_cast<uint32_t>(kind);
        g_peerState->sharedHandle = reinterpret_cast<uint64_t>(g_peerSharedHandle);
        g_peerState->width = w;
        g_peerState->height = h;
        g_peerState->format = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        g_peerState->generation = g_peerGeneration;
        g_peerState->frameSequence = ++g_peerSequence;
        g_peerState->lastFrameMicros = GetTickCount64() * 1000ull;
        g_peerState->active = 1;
    }
}

bool TryFindActivePeerDesktop(ActivePeerStream* out) {
    if (!out || !g_ppDesktopManager || !g_peerVecBeginOff || !g_peerVecEndOff) return false;
    *out = {};
    __try {
        uint8_t* mgr = reinterpret_cast<uint8_t*>(*g_ppDesktopManager);
        if (!mgr) return false;
        uintptr_t vecBegin = *reinterpret_cast<uintptr_t*>(mgr + g_peerVecBeginOff);
        uintptr_t vecEnd = *reinterpret_cast<uintptr_t*>(mgr + g_peerVecEndOff);
        if (!vecBegin || vecEnd <= vecBegin) return false;
        size_t count = (vecEnd - vecBegin) / sizeof(void*);
        if (count > 64) count = 64;

        uint8_t** peers = reinterpret_cast<uint8_t**>(vecBegin);
        uint64_t bestArea = 0;
        for (size_t i = 0; i < count; ++i) {
            uint8_t* pPeer = peers[i];
            if (!pPeer) continue;
            uint8_t active = *pPeer;
            if (!active) continue;
            uint32_t w = *reinterpret_cast<uint32_t*>(pPeer + 0xE4);
            uint32_t h = *reinterpret_cast<uint32_t*>(pPeer + 0xE8);
            if (w < 64 || h < 64 || w > 8192 || h > 8192) continue;
            ID3D11ShaderResourceView* ySrv = *reinterpret_cast<ID3D11ShaderResourceView**>(pPeer + 0x140);
            ID3D11ShaderResourceView* uvSrv = *reinterpret_cast<ID3D11ShaderResourceView**>(pPeer + 0x148);
            if (!ySrv || !uvSrv) continue;
            uint64_t area = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
            if (area > bestArea) {
                bestArea = area;
                out->ySrv = ySrv;
                out->uvSrv = uvSrv;
                out->width = w;
                out->height = h;
            }
        }
        return bestArea > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = {};
        return false;
    }
}

void ExportBigSoupPeerMovie() {
    ActivePeerStream stream{};
    if (!TryFindActivePeerDesktop(&stream)) {
        if (GetTickCount64() - g_lastMediaPlayerMs > 500) {
            ClearPeerMovieActive();
        }
        return;
    }

    ID3D11Resource* yRes = nullptr;
    ID3D11Resource* uvRes = nullptr;
    ID3D11Texture2D* yTex = nullptr;
    ID3D11Texture2D* uvTex = nullptr;
    stream.ySrv->GetResource(&yRes);
    stream.uvSrv->GetResource(&uvRes);
    if (yRes) { yRes->QueryInterface(IID_PPV_ARGS(&yTex)); yRes->Release(); }
    if (uvRes) { uvRes->QueryInterface(IID_PPV_ARGS(&uvTex)); uvRes->Release(); }
    if (!yTex || !uvTex) {
        if (yTex) yTex->Release();
        if (uvTex) uvTex->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC yDesc{};
    D3D11_TEXTURE2D_DESC uvDesc{};
    yTex->GetDesc(&yDesc);
    uvTex->GetDesc(&uvDesc);

    ID3D11Device* pDevice = nullptr;
    yTex->GetDevice(&pDevice);
    if (!pDevice) { yTex->Release(); uvTex->Release(); return; }

    ID3D11DeviceContext* pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext) { pDevice->Release(); yTex->Release(); uvTex->Release(); return; }

    if (EnsurePeerExportTarget(pDevice, yDesc.Width, yDesc.Height) && AcquirePeerMutexForWrite()) {
        SavedD3D11State saved;
        saved.Save(pContext);

        PeerNv12Cb cbData{};
        cbData.yUseAlpha = (yDesc.Format == DXGI_FORMAT_A8_UNORM) ? 1.0f : 0.0f;
        cbData.uvUseAlpha = (uvDesc.Format == DXGI_FORMAT_A8_UNORM) ? 1.0f : 0.0f;
        pContext->UpdateSubresource(g_peerCb, 0, nullptr, &cbData, 0, 0);

        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(yDesc.Width), static_cast<float>(yDesc.Height), 0.0f, 1.0f };
        pContext->OMSetRenderTargets(1, &g_peerExportRtv, nullptr);
        pContext->RSSetViewports(1, &vp);
        pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        pContext->IASetInputLayout(nullptr);
        pContext->VSSetShader(g_peerVs, nullptr, 0);
        pContext->PSSetShader(g_peerPsNv12, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { stream.ySrv, stream.uvSrv };
        pContext->PSSetShaderResources(0, 2, srvs);
        pContext->PSSetConstantBuffers(0, 1, &g_peerCb);
        pContext->OMSetBlendState(g_peerBlend, nullptr, 0xFFFFFFFF);
        pContext->OMSetDepthStencilState(g_peerDepth, 0);
        pContext->RSSetState(g_peerRaster);

        pContext->Draw(3, 0);
        pContext->Flush();

        saved.Restore(pContext);
        g_peerExportMutex->ReleaseSync(1);

        g_lastBigSoupPeerMs = GetTickCount64();
        PublishPeerFrameMetadata(bigscreen_peer_movie::SourceKind::BigSoupPeer, yDesc.Width, yDesc.Height);
    }

    pContext->Release();
    pDevice->Release();
    yTex->Release();
    uvTex->Release();
}

void ExportBigMediaPlayerMovie() {
    if (!g_pfnMediaGetTexId || !g_pfnMediaGetWidth || !g_pfnMediaGetHeight) return;
    if (GetTickCount64() - g_lastBigSoupPeerMs < 350) return; // Prioritize active BigSoup peer broadcast

    void* nativePtr = nullptr;
    int bestW = 0, bestH = 0;
    for (int id = 0; id < 16; ++id) {
        int w = g_pfnMediaGetWidth(id);
        int h = g_pfnMediaGetHeight(id);
        if (w >= 64 && h >= 64 && w <= 8192 && h <= 8192) {
            void* p = g_pfnMediaGetTexId(id, 0);
            if (p && static_cast<int64_t>(w) * h > static_cast<int64_t>(bestW) * bestH) {
                bestW = w; bestH = h; nativePtr = p;
            }
        }
    }
    if (!nativePtr) {
        if (GetTickCount64() - g_lastBigSoupPeerMs > 500) {
            ClearPeerMovieActive();
        }
        return;
    }

    ID3D11Texture2D* srcTex = nullptr;
    ID3D11ShaderResourceView* srcSrv = nullptr;
    IUnknown* unk = reinterpret_cast<IUnknown*>(nativePtr);
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&srcTex))) && srcTex) {
        D3D11_TEXTURE2D_DESC d{}; srcTex->GetDesc(&d);
        if (d.MiscFlags & D3D11_RESOURCE_MISC_HW_PROTECTED) {
            srcTex->Release();
            ClearPeerMovieActive();
            return;
        }
    } else if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&srcSrv))) && srcSrv) {
        ID3D11Resource* res = nullptr;
        srcSrv->GetResource(&res);
        if (res) {
            res->QueryInterface(IID_PPV_ARGS(&srcTex));
            res->Release();
        }
    }
    if (!srcTex) {
        if (srcSrv) srcSrv->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if ((srcDesc.MiscFlags & D3D11_RESOURCE_MISC_HW_PROTECTED) || srcDesc.Width < 64 || srcDesc.Height < 64) {
        srcTex->Release();
        if (srcSrv) srcSrv->Release();
        return;
    }

    ID3D11Device* pDevice = nullptr;
    srcTex->GetDevice(&pDevice);
    if (!pDevice) { srcTex->Release(); if (srcSrv) srcSrv->Release(); return; }

    ID3D11DeviceContext* pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext) { pDevice->Release(); srcTex->Release(); if (srcSrv) srcSrv->Release(); return; }

    if (EnsurePeerExportTarget(pDevice, srcDesc.Width, srcDesc.Height) && AcquirePeerMutexForWrite()) {
        bool copied = false;
        if (srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || srcDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM) {
            pContext->CopySubresourceRegion(g_peerExportTex, 0, 0, 0, 0, srcTex, 0, nullptr);
            pContext->Flush();
            copied = true;
        } else if (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            if (!srcSrv) {
                pDevice->CreateShaderResourceView(srcTex, nullptr, &srcSrv);
            }
            if (srcSrv) {
                SavedD3D11State saved;
                saved.Save(pContext);

                D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(srcDesc.Width), static_cast<float>(srcDesc.Height), 0.0f, 1.0f };
                pContext->OMSetRenderTargets(1, &g_peerExportRtv, nullptr);
                pContext->RSSetViewports(1, &vp);
                pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                pContext->IASetInputLayout(nullptr);
                pContext->VSSetShader(g_peerVs, nullptr, 0);
                pContext->PSSetShader(g_peerPsRgba, nullptr, 0);
                pContext->PSSetShaderResources(0, 1, &srcSrv);
                pContext->PSSetSamplers(0, 1, &g_peerSampler);
                pContext->OMSetBlendState(g_peerBlend, nullptr, 0xFFFFFFFF);
                pContext->OMSetDepthStencilState(g_peerDepth, 0);
                pContext->RSSetState(g_peerRaster);
                pContext->Draw(3, 0);
                pContext->Flush();

                saved.Restore(pContext);
                copied = true;
            }
        }
        g_peerExportMutex->ReleaseSync(1);
        if (copied) {
            g_lastMediaPlayerMs = GetTickCount64();
            PublishPeerFrameMetadata(bigscreen_peer_movie::SourceKind::BigMediaPlayer, srcDesc.Width, srcDesc.Height);
        }
    }

    pContext->Release();
    pDevice->Release();
    if (srcSrv) srcSrv->Release();
    srcTex->Release();
}

// --------------------------------------------------------------------------
// Local Desktop Projection Routing
// --------------------------------------------------------------------------
void ReleaseBlackTexture() {
    if (g_blackSrv) { g_blackSrv->Release(); g_blackSrv = nullptr; }
    if (g_blackRtv) { g_blackRtv->Release(); g_blackRtv = nullptr; }
    if (g_blackTex) { g_blackTex->Release(); g_blackTex = nullptr; }
    g_blackMips = 0;
}

bool EnsureBlackTexture(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, const D3D11_TEXTURE2D_DESC& deskDesc) {
    UINT targetMips = (std::max)(1u, deskDesc.MipLevels);
    if (g_blackTex && g_blackRtv) {
        D3D11_TEXTURE2D_DESC curDesc{};
        g_blackTex->GetDesc(&curDesc);
        if (curDesc.Width == deskDesc.Width && curDesc.Height == deskDesc.Height &&
            curDesc.Format == deskDesc.Format && curDesc.MipLevels == g_blackMips) {
            return true;
        }
        ReleaseBlackTexture();
    }

    D3D11_TEXTURE2D_DESC bDesc{};
    bDesc.Width = deskDesc.Width;
    bDesc.Height = deskDesc.Height;
    bDesc.MipLevels = targetMips;
    bDesc.ArraySize = 1;
    bDesc.Format = deskDesc.Format;
    bDesc.SampleDesc.Count = 1;
    bDesc.Usage = D3D11_USAGE_DEFAULT;
    bDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bDesc.MiscFlags = (targetMips > 1) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    if (FAILED(pDevice->CreateTexture2D(&bDesc, nullptr, &g_blackTex)) || !g_blackTex) {
        bDesc.MipLevels = 1;
        bDesc.MiscFlags = 0;
        if (FAILED(pDevice->CreateTexture2D(&bDesc, nullptr, &g_blackTex)) || !g_blackTex) {
            return false;
        }
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = (bDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? DXGI_FORMAT_B8G8R8A8_UNORM
                   : (bDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM
                   : bDesc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(pDevice->CreateRenderTargetView(g_blackTex, &rtvDesc, &g_blackRtv)) || !g_blackRtv) {
        ReleaseBlackTexture();
        return false;
    }

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    pContext->ClearRenderTargetView(g_blackRtv, black);
    if (bDesc.MipLevels > 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = rtvDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = bDesc.MipLevels;
        if (SUCCEEDED(pDevice->CreateShaderResourceView(g_blackTex, &srvDesc, &g_blackSrv)) && g_blackSrv) {
            pContext->GenerateMips(g_blackSrv);
        }
    }
    pContext->Flush();
    g_blackMips = bDesc.MipLevels;
    return true;
}

const bigscreen_projected_video::State* GetProjectedVideoState() {
    if (!g_projectedState) {
        if (!g_projectedMapping) {
            g_projectedMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, bigscreen_projected_video::kMappingName);
            if (!g_projectedMapping) {
                g_projectedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_projected_video::kMappingName);
            }
        }
        if (g_projectedMapping) {
            g_projectedState = static_cast<bigscreen_projected_video::State*>(
                MapViewOfFile(g_projectedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_projected_video::State)));
            if (!g_projectedState) {
                g_projectedState = static_cast<bigscreen_projected_video::State*>(
                    MapViewOfFile(g_projectedMapping, FILE_MAP_READ, 0, 0, sizeof(bigscreen_projected_video::State)));
            }
        }
    }
    if (!g_projectedState || g_projectedState->magic != bigscreen_projected_video::kMagic) {
        return nullptr;
    }
    return g_projectedState;
}

DXGI_FORMAT ResolveRtvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return fmt;
    }
}

void ReleaseRouteStaging() {
    if (g_routeStagingSrv) { g_routeStagingSrv->Release(); g_routeStagingSrv = nullptr; }
    if (g_routeStagingRtv) { g_routeStagingRtv->Release(); g_routeStagingRtv = nullptr; }
    if (g_routeStagingTex) { g_routeStagingTex->Release(); g_routeStagingTex = nullptr; }
    g_routeStagingW = 0;
    g_routeStagingH = 0;
    g_routeStagingMips = 0;
    g_routeStagingFmt = DXGI_FORMAT_UNKNOWN;
}

bool EnsureRouteStaging(ID3D11Device* pDevice, uint32_t w, uint32_t h, uint32_t mips, DXGI_FORMAT fmt) {
    if (!pDevice || w == 0 || h == 0) return false;
    if (!EnsurePeerPipeline(pDevice)) return false;
    UINT targetMips = (std::max)(1u, mips);
    if (g_routeStagingTex && g_routeStagingRtv &&
        g_routeStagingW == w && g_routeStagingH == h &&
        (g_routeStagingMips == targetMips || g_routeStagingMips == 1) && g_routeStagingFmt == fmt) {
        return true;
    }
    ReleaseRouteStaging();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = targetMips;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = (targetMips > 1) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    if (FAILED(pDevice->CreateTexture2D(&td, nullptr, &g_routeStagingTex)) || !g_routeStagingTex) {
        td.MipLevels = 1;
        td.MiscFlags = 0;
        if (FAILED(pDevice->CreateTexture2D(&td, nullptr, &g_routeStagingTex)) || !g_routeStagingTex) {
            return false;
        }
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = ResolveRtvFormat(fmt);
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(pDevice->CreateRenderTargetView(g_routeStagingTex, &rtvDesc, &g_routeStagingRtv)) || !g_routeStagingRtv) {
        ReleaseRouteStaging();
        return false;
    }
    if (td.MipLevels > 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = rtvDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = td.MipLevels;
        pDevice->CreateShaderResourceView(g_routeStagingTex, &srvDesc, &g_routeStagingSrv);
    }
    g_routeStagingW = w;
    g_routeStagingH = h;
    g_routeStagingMips = td.MipLevels;
    g_routeStagingFmt = fmt;
    return true;
}

bool BlitSharedVideoToDesktop(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                              ID3D11Texture2D* pDesktopTex, const D3D11_TEXTURE2D_DESC& deskDesc,
                              ID3D11Texture2D* pSharedTex) {
    if (!pDevice || !pContext || !pDesktopTex || !pSharedTex) return false;

    D3D11_TEXTURE2D_DESC vidDesc{};
    pSharedTex->GetDesc(&vidDesc);

    if (vidDesc.Width == deskDesc.Width && vidDesc.Height == deskDesc.Height &&
        vidDesc.Format == deskDesc.Format && vidDesc.MipLevels == deskDesc.MipLevels &&
        vidDesc.ArraySize == deskDesc.ArraySize && vidDesc.SampleDesc.Count == deskDesc.SampleDesc.Count) {
        pContext->CopyResource(pDesktopTex, pSharedTex);
        return true;
    }

    if (!g_sharedVideoSrv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = ResolveRtvFormat(vidDesc.Format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(pDevice->CreateShaderResourceView(pSharedTex, &srvDesc, &g_sharedVideoSrv)) || !g_sharedVideoSrv) {
            return false;
        }
    }

    if (!EnsureRouteStaging(pDevice, deskDesc.Width, deskDesc.Height, deskDesc.MipLevels, deskDesc.Format)) {
        return false;
    }

    SavedD3D11State saved;
    saved.Save(pContext);

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    pContext->ClearRenderTargetView(g_routeStagingRtv, black);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(deskDesc.Width), static_cast<float>(deskDesc.Height), 0.0f, 1.0f };
    pContext->OMSetRenderTargets(1, &g_routeStagingRtv, nullptr);
    pContext->RSSetViewports(1, &vp);
    pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pContext->IASetInputLayout(nullptr);
    pContext->VSSetShader(g_peerVs, nullptr, 0);
    pContext->PSSetShader(g_peerPsRgba, nullptr, 0);
    pContext->PSSetShaderResources(0, 1, &g_sharedVideoSrv);
    pContext->PSSetSamplers(0, 1, &g_peerSampler);
    pContext->OMSetBlendState(g_peerBlend, nullptr, 0xFFFFFFFF);
    pContext->OMSetDepthStencilState(g_peerDepth, 0);
    pContext->RSSetState(g_peerRaster);
    pContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    pContext->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    pContext->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (g_routeStagingMips == deskDesc.MipLevels && g_routeStagingSrv) {
        pContext->GenerateMips(g_routeStagingSrv);
        pContext->CopyResource(pDesktopTex, g_routeStagingTex);
    } else {
        pContext->CopySubresourceRegion(pDesktopTex, 0, 0, 0, 0, g_routeStagingTex, 0, nullptr);
    }
    pContext->Flush();

    saved.Restore(pContext);
    return true;
}

ID3D11Texture2D* ResolveDesktopTexture2D(void* nativePtr) {
    if (!nativePtr) return nullptr;
    IUnknown* unk = reinterpret_cast<IUnknown*>(nativePtr);
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
        return tex;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&srv))) && srv) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (res) {
            res->QueryInterface(IID_PPV_ARGS(&tex));
            res->Release();
        }
        srv->Release();
    }
    return tex;
}

void RouteDesktopTexture() {
    const auto* pState = GetProjectedVideoState();
    if (!pState) {
        return;
    }

    if (g_projectedState) {
        g_projectedState->diagHookLocal = g_localHookInstalled ? 1u : 0u;
    }

    const auto state = *pState;
    if (!state.active && !state.privacyMode) {
        if (g_sharedVideoSrv) { g_sharedVideoSrv->Release(); g_sharedVideoSrv = nullptr; }
        if (g_sharedVideoTex) { g_sharedVideoTex->Release(); g_sharedVideoTex = nullptr; }
        g_currentSharedHandle = 0;
        return;
    }

    if (!g_pfnGetNativeDesktop) return;

    void* rawDesktopPtr = g_pfnGetNativeDesktop(0);
    ID3D11Texture2D* pDesktopTex = ResolveDesktopTexture2D(rawDesktopPtr);
    if (!pDesktopTex) {
        if (g_projectedState) g_projectedState->diagDesktopCount = 0;
        return;
    }

    ID3D11Device* pDevice = nullptr;
    pDesktopTex->GetDevice(&pDevice);
    if (!pDevice) {
        pDesktopTex->Release();
        return;
    }

    ID3D11DeviceContext* pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext) {
        pDevice->Release();
        pDesktopTex->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC deskDesc{};
    pDesktopTex->GetDesc(&deskDesc);
    if (g_projectedState) {
        g_projectedState->diagDesktopCount = 1;
        g_projectedState->diagDeskFmt = static_cast<uint32_t>(deskDesc.Format);
    }

    if (state.active && state.sharedHandle) {
        if (!g_sharedVideoTex || g_currentSharedHandle != state.sharedHandle) {
            if (g_sharedVideoSrv) { g_sharedVideoSrv->Release(); g_sharedVideoSrv = nullptr; }
            if (g_sharedVideoTex) { g_sharedVideoTex->Release(); g_sharedVideoTex = nullptr; }
            g_currentSharedHandle = 0;
            HRESULT hr = pDevice->OpenSharedResource(reinterpret_cast<HANDLE>(state.sharedHandle), IID_PPV_ARGS(&g_sharedVideoTex));
            if (g_projectedState) {
                g_projectedState->diagOpenHr = static_cast<int32_t>(hr);
            }
            if (SUCCEEDED(hr) && g_sharedVideoTex) {
                g_currentSharedHandle = state.sharedHandle;
            }
        }

        if (g_sharedVideoTex) {
            D3D11_TEXTURE2D_DESC vidDesc{};
            g_sharedVideoTex->GetDesc(&vidDesc);
            if (g_projectedState) {
                g_projectedState->diagVidFmt = static_cast<uint32_t>(vidDesc.Format);
            }
            if (BlitSharedVideoToDesktop(pDevice, pContext, pDesktopTex, deskDesc, g_sharedVideoTex)) {
                if (g_projectedState) {
                    ++g_projectedState->diagCopiedCount;
                }
            }
        }
    } else if (state.privacyMode) {
        if (EnsureBlackTexture(pDevice, pContext, deskDesc) && g_blackTex) {
            if (g_blackMips == deskDesc.MipLevels) {
                pContext->CopyResource(pDesktopTex, g_blackTex);
            } else {
                pContext->CopySubresourceRegion(pDesktopTex, 0, 0, 0, 0, g_blackTex, 0, nullptr);
            }
        }
    }

    pContext->Release();
    pDevice->Release();
    pDesktopTex->Release();
}

// --------------------------------------------------------------------------
// Render-Thread Hooks
// --------------------------------------------------------------------------
void Hooked_ProcessLocalDesktops(int eventId) {
    if (g_originalProcessLocalDesktops) {
        g_originalProcessLocalDesktops(eventId);
    }
    RouteDesktopTexture();
    if (!g_mediaHookInstalled && g_pfnMediaGetTexId && g_peerCsInit) {
        if (TryEnterCriticalSection(&g_peerCs)) {
            ExportBigMediaPlayerMovie();
            LeaveCriticalSection(&g_peerCs);
        }
    }
}

void Hooked_ProcessPeerDesktops(int eventId) {
    if (g_originalProcessPeerDesktops) {
        g_originalProcessPeerDesktops(eventId);
    }
    if (g_peerCsInit) {
        EnterCriticalSection(&g_peerCs);
        ExportBigSoupPeerMovie();
        LeaveCriticalSection(&g_peerCs);
    }
}

void Hooked_MediaRenderNextFrame(int eventId) {
    if (g_originalMediaRenderNextFrame) {
        g_originalMediaRenderNextFrame(eventId);
    }
    if (g_peerCsInit) {
        EnterCriticalSection(&g_peerCs);
        ExportBigMediaPlayerMovie();
        LeaveCriticalSection(&g_peerCs);
    }
}

// Expected 14-byte non-RIP-relative prologue in BigSoup callbacks (0x77E50 & 0x77EB0):
// 40 53 48 83 EC 30 0F 57 C0 0F 11 44 24 20
bool InstallSimple14Hook(uint8_t* pTarget, void* pDetour, uint8_t** ppTrampoline, UnityRenderEvent* ppOriginal) {
    if (!pTarget) return false;
    if (pTarget[0] == 0xFF && pTarget[1] == 0x25) return true;

    static const uint8_t expected[14] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x44, 0x24, 0x20 };
    if (std::memcmp(pTarget, expected, sizeof(expected)) != 0) {
        return false;
    }

    uint8_t* pTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!pTramp) return false;

    std::memcpy(pTramp, pTarget, 14);
    pTramp[14] = 0xFF;
    pTramp[15] = 0x25;
    *reinterpret_cast<uint32_t*>(pTramp + 16) = 0;
    *reinterpret_cast<uint64_t*>(pTramp + 20) = reinterpret_cast<uint64_t>(pTarget + 14);
    FlushInstructionCache(GetCurrentProcess(), pTramp, 32);

    *ppTrampoline = pTramp;
    *ppOriginal = reinterpret_cast<UnityRenderEvent>(pTramp);

    DWORD oldProtect = 0;
    if (!VirtualProtect(pTarget, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    pTarget[0] = 0xFF;
    pTarget[1] = 0x25;
    *reinterpret_cast<uint32_t*>(pTarget + 2) = 0;
    *reinterpret_cast<uint64_t*>(pTarget + 6) = reinterpret_cast<uint64_t>(pDetour);
    DWORD tmp = 0;
    VirtualProtect(pTarget, 14, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), pTarget, 14);
    return true;
}

void ResolveBigSoupPeerOffsets(HMODULE hBigSoup) {
    if (g_ppDesktopManager) return;
    g_pfnGetIncomingStreamCount = reinterpret_cast<pfnBigSoupGetNumberOfIncomingVideoStreams>(
        GetProcAddress(hBigSoup, "BigSoupGetNumberOfIncomingVideoStreams"));
    if (!g_pfnGetIncomingStreamCount) return;

    const uint8_t* fn = reinterpret_cast<const uint8_t*>(g_pfnGetIncomingStreamCount);
    // Scan first 64 bytes for: 48 8B 05 <disp32> ... 48 8B 98 <endOff32> 48 2B 98 <beginOff32>
    for (int i = 0; i < 48; ++i) {
        if (fn[i] == 0x48 && fn[i + 1] == 0x8B && fn[i + 2] == 0x05) {
            int32_t disp = *reinterpret_cast<const int32_t*>(fn + i + 3);
            g_ppDesktopManager = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(fn) + i + 7 + disp);
        }
        if (fn[i] == 0x48 && fn[i + 1] == 0x8B && fn[i + 2] == 0x98 &&
            fn[i + 7] == 0x48 && fn[i + 8] == 0x2B && fn[i + 9] == 0x98) {
            g_peerVecEndOff = *reinterpret_cast<const uint32_t*>(fn + i + 3);
            g_peerVecBeginOff = *reinterpret_cast<const uint32_t*>(fn + i + 10);
        }
    }
}

void TryInstallMediaHook() {
    HMODULE hMedia = GetModuleHandleW(L"BigMediaPlayer.dll");
    if (!hMedia) return;

    if (!g_pfnMediaGetTexId) {
        g_pfnMediaGetTexId = reinterpret_cast<pfnBigMediaGetVideoTextureId>(GetProcAddress(hMedia, "BigMediaPlayerAPI_GetVideoTextureId"));
        g_pfnMediaGetWidth = reinterpret_cast<pfnBigMediaGetVideoTextureWidth>(GetProcAddress(hMedia, "BigMediaPlayerAPI_GetVideoTextureWidth"));
        g_pfnMediaGetHeight = reinterpret_cast<pfnBigMediaGetVideoTextureHeight>(GetProcAddress(hMedia, "BigMediaPlayerAPI_GetVideoTextureHeight"));
    }

    if (g_mediaHookInstalled) return;
    auto fnRenderFunc = reinterpret_cast<pfnBigMediaRenderNextFrameFunc>(
        GetProcAddress(hMedia, "BigMediaPlayerAPI_RenderNextFrameFunc"));
    if (!fnRenderFunc) return;

    uint8_t* pTarget = reinterpret_cast<uint8_t*>(fnRenderFunc());
    if (!pTarget) return;
    if (pTarget[0] == 0xFF && pTarget[1] == 0x25) { g_mediaHookInstalled = true; return; }

    // Expected 17-byte prologue:
    // 48 89 5C 24 10 57 48 83 EC 20 48 8B 3D <disp32>
    static const uint8_t expected[13] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D };
    if (std::memcmp(pTarget, expected, sizeof(expected)) != 0) return;

    int32_t disp = *reinterpret_cast<int32_t*>(pTarget + 13);
    uint64_t absPtrAddr = reinterpret_cast<uint64_t>(pTarget + 17 + disp);

    uint8_t* pTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!pTramp) return;

    std::memcpy(pTramp, pTarget, 10);
    pTramp[10] = 0x48; pTramp[11] = 0xBF; // movabs rdi, absPtrAddr
    *reinterpret_cast<uint64_t*>(pTramp + 12) = absPtrAddr;
    pTramp[20] = 0x48; pTramp[21] = 0x8B; pTramp[22] = 0x3F; // mov rdi, [rdi]
    pTramp[23] = 0xFF; pTramp[24] = 0x25;
    *reinterpret_cast<uint32_t*>(pTramp + 25) = 0;
    *reinterpret_cast<uint64_t*>(pTramp + 29) = reinterpret_cast<uint64_t>(pTarget + 17);
    FlushInstructionCache(GetCurrentProcess(), pTramp, 48);

    g_pTrampolineMedia = pTramp;
    g_originalMediaRenderNextFrame = reinterpret_cast<UnityRenderEvent>(pTramp);

    DWORD oldProtect = 0;
    if (VirtualProtect(pTarget, 17, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        pTarget[0] = 0xFF; pTarget[1] = 0x25;
        *reinterpret_cast<uint32_t*>(pTarget + 2) = 0;
        *reinterpret_cast<uint64_t*>(pTarget + 6) = reinterpret_cast<uint64_t>(Hooked_MediaRenderNextFrame);
        pTarget[14] = 0x90; pTarget[15] = 0x90; pTarget[16] = 0x90;
        DWORD tmp = 0;
        VirtualProtect(pTarget, 17, oldProtect, &tmp);
        FlushInstructionCache(GetCurrentProcess(), pTarget, 17);
        g_mediaHookInstalled = true;
    }
}

bool InstallHookInternal(HMODULE hBigSoup) {
    if (!hBigSoup) return false;

    if (!g_pfnGetNativeDesktop) {
        g_pfnGetNativeDesktop = reinterpret_cast<pfnBigSoupGetNativeDesktopTexturePointer>(
            GetProcAddress(hBigSoup, "BigSoupGetNativeDesktopTexturePointer"));
    }
    ResolveBigSoupPeerOffsets(hBigSoup);

    if (!g_localHookInstalled) {
        auto fnProcessLocal = reinterpret_cast<pfnBigSoupProcessLocalDesktops>(
            GetProcAddress(hBigSoup, "BigSoupProcessLocalDesktops"));
        if (fnProcessLocal) {
            UnityRenderEvent targetLocal = fnProcessLocal();
            g_localHookInstalled = InstallSimple14Hook(
                reinterpret_cast<uint8_t*>(targetLocal),
                Hooked_ProcessLocalDesktops,
                &g_pTrampolineLocal,
                &g_originalProcessLocalDesktops);
        }
    }

    if (!g_peerHookInstalled) {
        auto fnProcessPeer = reinterpret_cast<pfnBigSoupProcessPeerDesktops>(
            GetProcAddress(hBigSoup, "BigSoupProcessPeerDesktops"));
        if (fnProcessPeer) {
            UnityRenderEvent targetPeer = fnProcessPeer();
            g_peerHookInstalled = InstallSimple14Hook(
                reinterpret_cast<uint8_t*>(targetPeer),
                Hooked_ProcessPeerDesktops,
                &g_pTrampolinePeer,
                &g_originalProcessPeerDesktops);
        }
    }

    TryInstallMediaHook();
    return g_localHookInstalled;
}

bool InvokeBoolGetter(const void* method, void* instance, bool defaultVal) {
    if (!g_il2cppInvoke || !g_il2cppUnbox || !method || !instance) return defaultVal;
    void* exc = nullptr;
    void* ret = g_il2cppInvoke(method, instance, nullptr, &exc);
    if (exc || !ret) return defaultVal;
    return (*reinterpret_cast<const uint8_t*>(g_il2cppUnbox(ret))) != 0;
}

void OnAppTickMainThread(void* pApp) {
    if (!g_il2cppInvoke || !pApp || !g_peerCsInit) return;
    ULONGLONG now = GetTickCount64();

    const auto* projState = GetProjectedVideoState();
    const uint64_t reqSeq = projState ? projState->requestOwnRoomSeq : 0;
    const bool leaveRequested = (reqSeq != 0 && reqSeq != g_handledOwnRoomSeq);

    if (!leaveRequested && (now - g_lastRoomPollMs < 100)) {
        return;
    }
    g_lastRoomPollMs = now;

    bool isLoading = false;
    bool inOwnRoom = false;

    __try {
        isLoading = InvokeBoolGetter(g_mGetIsLoadingScene, pApp, false);
        void* exc = nullptr;
        void* pRoom = g_mGetCurrentRoom ? g_il2cppInvoke(g_mGetCurrentRoom, pApp, nullptr, &exc) : nullptr;
        if (!exc && pRoom) {
            const bool isSinglePlayer = InvokeBoolGetter(g_mGetIsSinglePlayer, pRoom, false);
            const bool isOwner = InvokeBoolGetter(g_mGetIsLocalUserOwner, pRoom, false);
            const bool inLobby = InvokeBoolGetter(g_mInLobby, pRoom, false);
            inOwnRoom = !isLoading && (isSinglePlayer || (isOwner && !inLobby));
        }

        if (leaveRequested) {
            g_handledOwnRoomSeq = reqSeq;
            if (!inOwnRoom && !isLoading && g_mLeaveRoom && (now - g_lastLeaveRoomMs > 2500)) {
                g_lastLeaveRoomMs = now;
                uint8_t disconnecting = 0;
                uint8_t resetToSinglePlayer = 1;
                void* args[2] = { &disconnecting, &resetToSinglePlayer };
                void* leaveExc = nullptr;
                g_il2cppInvoke(g_mLeaveRoom, pApp, args, &leaveExc);
                isLoading = true;
                inOwnRoom = false;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    EnterCriticalSection(&g_peerCs);
    EnsurePeerMovieIpc();
    if (g_peerState) {
        g_peerState->roomHookReady = 1;
        g_peerState->inOwnRoom = inOwnRoom ? 1 : 0;
        g_peerState->isLoadingRoom = isLoading ? 1 : 0;
        g_peerState->handledOwnRoomSeq = g_handledOwnRoomSeq;
    }
    LeaveCriticalSection(&g_peerCs);
}

float GetActiveAspectOverride() {
    const auto* st = GetProjectedVideoState();
    if (!st || st->magic != bigscreen_projected_video::kMagic) return 0.0f;
    return (st->aspectOverride > 0.1f && st->aspectOverride < 20.0f) ? st->aspectOverride : 0.0f;
}

Il2CppVec3 ComputeFittedScale(const Il2CppVec3& frame, float targetAspect) {
    Il2CppVec3 out = frame;
    if (targetAspect <= 0.1f || frame.x <= 0.0001f) return out;
    if (frame.y > 0.0001f) {
        float frameAspect = frame.x / frame.y;
        if (frameAspect <= targetAspect) {
            out.x = frame.x;
            out.y = frame.x / targetAspect;
        } else {
            out.x = frame.y * targetAspect;
            out.y = frame.y;
        }
    } else {
        out.x = frame.x;
        out.y = frame.x / targetAspect;
    }
    return out;
}

void RecordTrackedSizer(void* ptr, int w, int h) {
    if (!ptr || w <= 0 || h <= 0) return;
    for (auto& e : g_trackedSizers) {
        if (e.ptr == ptr) { e.origW = w; e.origH = h; return; }
    }
    for (auto& e : g_trackedSizers) {
        if (!e.ptr) { e.ptr = ptr; e.origW = w; e.origH = h; return; }
    }
    g_trackedSizers[0] = { ptr, w, h };
}

void RecordTrackedMirrored(void* ptr, void* tex) {
    if (!ptr) return;
    for (auto& e : g_trackedMirrored) {
        if (e.ptr == ptr) { if (tex) e.lastTex = tex; return; }
    }
    for (auto& e : g_trackedMirrored) {
        if (!e.ptr) { e.ptr = ptr; e.lastTex = tex; return; }
    }
    g_trackedMirrored[0] = { ptr, tex };
}

void* FindMirroredTexFallback(void* ptr) {
    if (!ptr) return nullptr;
    for (const auto& e : g_trackedMirrored) {
        if (e.ptr == ptr && e.lastTex) return e.lastTex;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ptr) + 0x78);
}

void ApplyTransformScale(void* obj, bool isGameObject, const Il2CppVec3& scale) {
    if (!obj || !g_il2cppInvoke || !g_mTransformSetLocalScale) return;
    const void* mGetTr = isGameObject ? g_mGoGetTransform : g_mCompGetTransform;
    if (!mGetTr) return;
    void* exc = nullptr;
    void* tr = g_il2cppInvoke(mGetTr, obj, nullptr, &exc);
    if (exc || !tr) return;
    Il2CppVec3 s = scale;
    void* args[1] = { &s };
    exc = nullptr;
    g_il2cppInvoke(g_mTransformSetLocalScale, tr, args, &exc);
}

void Hooked_MonitorSetAspect(void* self, int width, int height, const void* mi) {
    if (!g_inAspectReapply && width > 0 && height > 0) {
        RecordTrackedSizer(self, width, height);
    }
    float ov = GetActiveAspectOverride();
    if (ov > 0.1f && !g_inAspectReapply) {
        width = static_cast<int>(ov * 1000.0f + 0.5f);
        height = 1000;
    }
    if (g_origMonitorSetAspect) {
        g_origMonitorSetAspect(self, width, height, mi);
    }
}

void Hooked_MonitorLerpAspect(void* self, int width, int height, const void* mi) {
    if (!g_inAspectReapply && width > 0 && height > 0) {
        RecordTrackedSizer(self, width, height);
    }
    float ov = GetActiveAspectOverride();
    if (ov > 0.1f && !g_inAspectReapply) {
        width = static_cast<int>(ov * 1000.0f + 0.5f);
        height = 1000;
    }
    if (g_origMonitorLerpAspect) {
        g_origMonitorLerpAspect(self, width, height, mi);
    }
}

void Hooked_MirroredUpdateAspect(void* self, void* tex, const void* mi) {
    if (!g_inAspectReapply) {
        RecordTrackedMirrored(self, tex);
    }
    if (g_origMirroredUpdateAspect) {
        g_origMirroredUpdateAspect(self, tex, mi);
    }
    float ov = GetActiveAspectOverride();
    if (self && tex && ov > 0.1f) {
        const auto* orig = reinterpret_cast<const Il2CppVec3*>(reinterpret_cast<const uint8_t*>(self) + 0x80);
        if (orig->x > 0.001f && orig->x < 5000.0f && orig->z > 0.0001f && orig->z < 5000.0f) {
            ApplyTransformScale(self, false, ComputeFittedScale(*orig, ov));
        }
    }
}

bool InstallNonRipPrologueHook(const void* methodInfo, const uint8_t* prefix, size_t prefixLen,
                               size_t stolenLen, void* hookFn, void** outOrigFn, uint8_t** outTramp) {
    if (!methodInfo || stolenLen < 14 || stolenLen > 32) return false;
    uint8_t* pTarget = *reinterpret_cast<uint8_t* const*>(methodInfo);
    if (!pTarget) return false;
    if (pTarget[0] == 0xFF && pTarget[1] == 0x25) return true; // already hooked
    if (std::memcmp(pTarget, prefix, prefixLen) != 0) return false;

    uint8_t* pTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!pTramp) return false;

    std::memcpy(pTramp, pTarget, stolenLen);
    size_t k = stolenLen;
    pTramp[k++] = 0xFF; pTramp[k++] = 0x25;
    *reinterpret_cast<uint32_t*>(pTramp + k) = 0; k += 4;
    *reinterpret_cast<uint64_t*>(pTramp + k) = reinterpret_cast<uint64_t>(pTarget + stolenLen); k += 8;
    FlushInstructionCache(GetCurrentProcess(), pTramp, 64);

    *outOrigFn = pTramp;
    *outTramp = pTramp;

    DWORD oldProtect = 0;
    if (!VirtualProtect(pTarget, stolenLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    pTarget[0] = 0xFF; pTarget[1] = 0x25;
    *reinterpret_cast<uint32_t*>(pTarget + 2) = 0;
    *reinterpret_cast<uint64_t*>(pTarget + 6) = reinterpret_cast<uint64_t>(hookFn);
    for (size_t i = 14; i < stolenLen; ++i) pTarget[i] = 0x90;
    DWORD tmp = 0;
    VirtualProtect(pTarget, stolenLen, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), pTarget, stolenLen);
    return true;
}

bool GetTransformScale(void* obj, bool isGameObject, Il2CppVec3* outScale) {
    if (!obj || !outScale || !g_il2cppInvoke || !g_mTransformGetLocalScale || !g_il2cppUnbox) return false;
    const void* mGetTr = isGameObject ? g_mGoGetTransform : g_mCompGetTransform;
    if (!mGetTr) return false;
    void* exc = nullptr;
    void* tr = g_il2cppInvoke(mGetTr, obj, nullptr, &exc);
    if (exc || !tr) return false;
    exc = nullptr;
    void* boxed = g_il2cppInvoke(g_mTransformGetLocalScale, tr, nullptr, &exc);
    if (exc || !boxed) return false;
    const auto* vec = reinterpret_cast<const Il2CppVec3*>(g_il2cppUnbox(boxed));
    if (!vec) return false;
    *outScale = *vec;
    return true;
}

void ApplyScreenAspectOverridesOnMainThread() {
    float ov = GetActiveAspectOverride();
    ULONGLONG now = GetTickCount64();
    bool changed = (std::fabs(ov - g_lastAppliedAspectOverride) > 0.001f);
    bool periodic = (ov > 0.1f) && (now - g_lastAspectSweepMs >= 100);
    if (!changed && !periodic) return;

    g_lastAppliedAspectOverride = ov;
    g_lastAspectSweepMs = now;

    uint32_t diagMask = 0;
    if (g_mFindObjectsOfType) diagMask |= 0x01;
    if (g_mTransformSetLocalScale) diagMask |= 0x02;
    uint32_t appliedCount = 0;

    // 1. MonitorSizer (desktop monitors / personal & presentation screens)
    if (g_mFindObjectsOfType && g_typeMonitorSizer && g_origMonitorSetAspect && g_fnArrayLength) {
        void* exc = nullptr;
        void* args[1] = { g_typeMonitorSizer };
        void* arr = g_il2cppInvoke(g_mFindObjectsOfType, nullptr, args, &exc);
        if (!exc && arr) {
            uint32_t n = g_fnArrayLength(arr);
            if (n > 0) diagMask |= 0x04;
            auto** elems = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                void* sizer = elems[i];
                if (!sizer) continue;

                int origW = 0, origH = 0;
                for (const auto& e : g_trackedSizers) {
                    if (e.ptr == sizer) { origW = e.origW; origH = e.origH; break; }
                }
                if (origW == 0 && g_fldSizerAspectRatio && g_fnFieldGetOffset) {
                    size_t off = g_fnFieldGetOffset(g_fldSizerAspectRatio);
                    float curAsp = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(sizer) + off);
                    if (curAsp > 0.1f && curAsp < 20.0f) {
                        origW = static_cast<int>(curAsp * 1000.0f + 0.5f);
                        origH = 1000;
                        RecordTrackedSizer(sizer, origW, origH);
                    }
                }
                if (origW <= 0 || origH <= 0) { origW = 16; origH = 9; }

                int applyW = (ov > 0.1f) ? static_cast<int>(ov * 1000.0f + 0.5f) : origW;
                int applyH = (ov > 0.1f) ? 1000 : origH;
                g_inAspectReapply = true;
                g_origMonitorSetAspect(sizer, applyW, applyH, g_mMonitorSetAspect);
                g_inAspectReapply = false;
                ++appliedCount;
            }
        }
    }

    // 2. MirroredScreen (theater cinema screens)
    if (g_mFindObjectsOfType && g_typeMirroredScreen && g_origMirroredUpdateAspect && g_fnArrayLength) {
        void* exc = nullptr;
        void* args[1] = { g_typeMirroredScreen };
        void* arr = g_il2cppInvoke(g_mFindObjectsOfType, nullptr, args, &exc);
        if (!exc && arr) {
            uint32_t n = g_fnArrayLength(arr);
            if (n > 0) diagMask |= 0x08;
            auto** elems = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
            for (uint32_t i = 0; i < n && i < 32; ++i) {
                void* mirrored = elems[i];
                if (!mirrored) continue;
                void* trackedTex = nullptr;
                for (const auto& e : g_trackedMirrored) {
                    if (e.ptr == mirrored) { trackedTex = e.lastTex; break; }
                }
                if (changed && ov <= 0.1f) {
                    if (trackedTex) {
                        g_inAspectReapply = true;
                        g_origMirroredUpdateAspect(mirrored, trackedTex, g_mMirroredUpdateAspect);
                        g_inAspectReapply = false;
                        ++appliedCount;
                    }
                } else if (ov > 0.1f) {
                    const auto* orig = reinterpret_cast<const Il2CppVec3*>(reinterpret_cast<const uint8_t*>(mirrored) + 0x80);
                    if (orig->x > 0.001f && orig->x < 5000.0f && orig->z > 0.0001f && orig->z < 5000.0f) {
                        ApplyTransformScale(mirrored, false, ComputeFittedScale(*orig, ov));
                        ++appliedCount;
                    }
                }
            }
        }
    }

    // 3. BigMediaRenderer (BigMediaPlayer movies)
    if (g_mFindObjectsOfType && g_typeBigMediaRenderer && g_fnArrayLength && g_fnFieldGetOffset && g_fldMediaOrigScale) {
        void* exc = nullptr;
        void* args[1] = { g_typeBigMediaRenderer };
        void* arr = g_il2cppInvoke(g_mFindObjectsOfType, nullptr, args, &exc);
        if (!exc && arr) {
            uint32_t n = g_fnArrayLength(arr);
            if (n > 0) diagMask |= 0x10;
            auto** elems = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
            size_t offOrig = g_fnFieldGetOffset(g_fldMediaOrigScale);
            size_t offGo = g_fldMediaScreensGo ? g_fnFieldGetOffset(g_fldMediaScreensGo) : 0;
            size_t offRend = g_fldMediaRegularRenderer ? g_fnFieldGetOffset(g_fldMediaRegularRenderer) : 0;
            size_t offResize = g_fldMediaResizeRequired ? g_fnFieldGetOffset(g_fldMediaResizeRequired) : 0;
            size_t offSrc = g_fldMediaCurrentSource ? g_fnFieldGetOffset(g_fldMediaCurrentSource) : 0;
            for (uint32_t i = 0; i < n && i < 16; ++i) {
                void* mr = elems[i];
                if (!mr) continue;
                if (offSrc && !*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mr) + offSrc)) continue;
                if (changed && ov <= 0.1f) {
                    if (offResize) *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(mr) + offResize) = 1;
                    ++appliedCount;
                } else if (ov > 0.1f) {
                    Il2CppVec3 orig = *reinterpret_cast<const Il2CppVec3*>(reinterpret_cast<const uint8_t*>(mr) + offOrig);
                    if (orig.x > 0.001f && orig.x < 5000.0f && orig.z > 0.0001f && orig.z < 5000.0f) {
                        Il2CppVec3 fitted = ComputeFittedScale(orig, ov);
                        void* go = offGo ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mr) + offGo) : nullptr;
                        if (go) {
                            ApplyTransformScale(go, true, fitted);
                            ++appliedCount;
                        } else if (offRend) {
                            void* rend = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mr) + offRend);
                            if (rend) {
                                ApplyTransformScale(rend, false, fitted);
                                ++appliedCount;
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. PresentationSectionSizer (VR User & Theater presentation sections)
    if (g_mFindObjectsOfType && g_typePresentationSectionSizer && g_fnArrayLength) {
        void* exc = nullptr;
        void* args[1] = { g_typePresentationSectionSizer };
        void* arr = g_il2cppInvoke(g_mFindObjectsOfType, nullptr, args, &exc);
        if (!exc && arr) {
            uint32_t n = g_fnArrayLength(arr);
            if (n > 0) diagMask |= 0x20;
            auto** elems = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20);
            for (uint32_t i = 0; i < n && i < 32; ++i) {
                void* ps = elems[i];
                if (!ps) continue;

                Il2CppVec3 baseScale{};
                bool haveBase = false;
                for (const auto& e : g_trackedPresSizers) {
                    if (e.ptr == ps) { baseScale = e.origScale; haveBase = true; break; }
                }
                if (!haveBase && GetTransformScale(ps, false, &baseScale)) {
                    if (baseScale.x > 0.001f && baseScale.y > 0.001f) {
                        for (auto& e : g_trackedPresSizers) {
                            if (!e.ptr) { e = { ps, baseScale }; haveBase = true; break; }
                        }
                        if (!haveBase) {
                            g_trackedPresSizers[0] = { ps, baseScale };
                            haveBase = true;
                        }
                    }
                }
                if (!haveBase || baseScale.x <= 0.001f || baseScale.y <= 0.001f) continue;

                if (changed && ov <= 0.1f) {
                    ApplyTransformScale(ps, false, baseScale);
                    ++appliedCount;
                } else if (ov > 0.1f) {
                    ApplyTransformScale(ps, false, ComputeFittedScale(baseScale, ov));
                    ++appliedCount;
                }
            }
        }
    }

    if (g_projectedState) {
        g_projectedState->diagAspectMask = diagMask;
        g_projectedState->diagAspectAppliedCount += appliedCount;
    }
}

void Hooked_AppTick(void* appThis, const void* methodInfo) {
    if (g_originalAppTick) {
        g_originalAppTick(appThis, methodInfo);
    }
    if (appThis) {
        OnAppTickMainThread(appThis);
        ApplyScreenAspectOverridesOnMainThread();
    }
}

void TryInstallAppTickHook() {
    if (g_appTickHookInstalled) return;
    HMODULE hGame = GetModuleHandleW(L"GameAssembly.dll");
    if (!hGame) return;

    auto fnDomainGet = reinterpret_cast<pfnIl2CppDomainGet>(GetProcAddress(hGame, "il2cpp_domain_get"));
    auto fnDomainAssemblies = reinterpret_cast<pfnIl2CppDomainGetAssemblies>(GetProcAddress(hGame, "il2cpp_domain_get_assemblies"));
    auto fnAsmImage = reinterpret_cast<pfnIl2CppAssemblyGetImage>(GetProcAddress(hGame, "il2cpp_assembly_get_image"));
    auto fnClassFromName = reinterpret_cast<pfnIl2CppClassFromName>(GetProcAddress(hGame, "il2cpp_class_from_name"));
    auto fnMethodFromName = reinterpret_cast<pfnIl2CppClassGetMethodFromName>(GetProcAddress(hGame, "il2cpp_class_get_method_from_name"));
    auto fnThreadAttach = reinterpret_cast<pfnIl2CppThreadAttach>(GetProcAddress(hGame, "il2cpp_thread_attach"));
    auto fnThreadDetach = reinterpret_cast<pfnIl2CppThreadDetach>(GetProcAddress(hGame, "il2cpp_thread_detach"));
    g_il2cppInvoke = reinterpret_cast<pfnIl2CppRuntimeInvoke>(GetProcAddress(hGame, "il2cpp_runtime_invoke"));
    g_il2cppUnbox = reinterpret_cast<pfnIl2CppObjectUnbox>(GetProcAddress(hGame, "il2cpp_object_unbox"));

    if (!fnDomainGet || !fnDomainAssemblies || !fnAsmImage || !fnClassFromName ||
        !fnMethodFromName || !fnThreadAttach || !fnThreadDetach || !g_il2cppInvoke || !g_il2cppUnbox) {
        return;
    }

    void* domain = fnDomainGet();
    if (!domain) return;
    void* thread = fnThreadAttach(domain);
    if (!thread) return;

    void* appClass = nullptr;
    void* roomClass = nullptr;
    void* unityObjClass = nullptr;
    void* unityCompClass = nullptr;
    void* unityGoClass = nullptr;
    void* unityTransformClass = nullptr;
    size_t asmCount = 0;
    const void** assemblies = fnDomainAssemblies(domain, &asmCount);
    if (assemblies) {
        for (size_t i = 0; i < asmCount; ++i) {
            const void* img = fnAsmImage(assemblies[i]);
            if (!img) continue;
            if (!appClass) appClass = fnClassFromName(img, "Bigscreen", "App");
            if (!roomClass) roomClass = fnClassFromName(img, "Bigscreen.Cloud", "Room");
            if (!g_klassMonitorSizer) g_klassMonitorSizer = fnClassFromName(img, "Bigscreen.Monitors", "MonitorSizer");
            if (!g_klassMirroredScreen) g_klassMirroredScreen = fnClassFromName(img, "Bigscreen.Effects", "MirroredScreen");
            if (!g_klassBigMediaRenderer) g_klassBigMediaRenderer = fnClassFromName(img, "Bigscreen.Media", "BigMediaRenderer");
            if (!g_klassPresentationSectionSizer) g_klassPresentationSectionSizer = fnClassFromName(img, "Bigscreen.Monitors.Presentation", "PresentationSectionSizer");
            if (!unityObjClass) unityObjClass = fnClassFromName(img, "UnityEngine", "Object");
            if (!unityCompClass) unityCompClass = fnClassFromName(img, "UnityEngine", "Component");
            if (!unityGoClass) unityGoClass = fnClassFromName(img, "UnityEngine", "GameObject");
            if (!unityTransformClass) unityTransformClass = fnClassFromName(img, "UnityEngine", "Transform");
        }
    }

    if (!g_screenAspectHooksAttempted && g_klassMonitorSizer && g_klassMirroredScreen && unityObjClass && unityCompClass && unityGoClass && unityTransformClass) {
        g_screenAspectHooksAttempted = true;
        g_fnClassGetType = reinterpret_cast<pfnIl2CppClassGetType>(GetProcAddress(hGame, "il2cpp_class_get_type"));
        g_fnTypeGetObject = reinterpret_cast<pfnIl2CppTypeGetObject>(GetProcAddress(hGame, "il2cpp_type_get_object"));
        g_fnClassGetField = reinterpret_cast<pfnIl2CppClassGetFieldFromName>(GetProcAddress(hGame, "il2cpp_class_get_field_from_name"));
        g_fnFieldGetOffset = reinterpret_cast<pfnIl2CppFieldGetOffset>(GetProcAddress(hGame, "il2cpp_field_get_offset"));
        g_fnArrayLength = reinterpret_cast<pfnIl2CppArrayLength>(GetProcAddress(hGame, "il2cpp_array_length"));
        auto fnClassGetMethods = reinterpret_cast<pfnIl2CppClassGetMethods>(GetProcAddress(hGame, "il2cpp_class_get_methods"));
        auto fnMethodGetName = reinterpret_cast<pfnIl2CppMethodGetName>(GetProcAddress(hGame, "il2cpp_method_get_name"));
        auto fnMethodGetParamCount = reinterpret_cast<pfnIl2CppMethodGetParamCount>(GetProcAddress(hGame, "il2cpp_method_get_param_count"));
        auto fnMethodIsGeneric = reinterpret_cast<pfnIl2CppMethodIsGeneric>(GetProcAddress(hGame, "il2cpp_method_is_generic"));

        auto findNonGeneric = [&](void* klass, const char* name, uint32_t paramCount) -> const void* {
            if (!klass || !fnClassGetMethods || !fnMethodGetName || !fnMethodGetParamCount) {
                return fnMethodFromName(klass, name, static_cast<int>(paramCount));
            }
            void* iter = nullptr;
            while (const void* m = fnClassGetMethods(klass, &iter)) {
                const char* mn = fnMethodGetName(m);
                if (mn && std::strcmp(mn, name) == 0 &&
                    fnMethodGetParamCount(m) == paramCount &&
                    (!fnMethodIsGeneric || fnMethodIsGeneric(m) == 0)) {
                    return m;
                }
            }
            return fnMethodFromName(klass, name, static_cast<int>(paramCount));
        };

        g_mFindObjectsOfType = findNonGeneric(unityObjClass, "FindObjectsOfType", 1);
        g_mCompGetTransform = findNonGeneric(unityCompClass, "get_transform", 0);
        g_mGoGetTransform = findNonGeneric(unityGoClass, "get_transform", 0);
        g_mTransformGetLocalScale = findNonGeneric(unityTransformClass, "get_localScale", 0);
        g_mTransformSetLocalScale = findNonGeneric(unityTransformClass, "set_localScale", 1);

        if (g_fnClassGetType && g_fnTypeGetObject) {
            const void* tSizer = g_fnClassGetType(g_klassMonitorSizer);
            const void* tMirrored = g_fnClassGetType(g_klassMirroredScreen);
            if (tSizer) g_typeMonitorSizer = g_fnTypeGetObject(tSizer);
            if (tMirrored) g_typeMirroredScreen = g_fnTypeGetObject(tMirrored);
            if (g_klassBigMediaRenderer) {
                const void* tMedia = g_fnClassGetType(g_klassBigMediaRenderer);
                if (tMedia) g_typeBigMediaRenderer = g_fnTypeGetObject(tMedia);
            }
            if (g_klassPresentationSectionSizer) {
                const void* tPres = g_fnClassGetType(g_klassPresentationSectionSizer);
                if (tPres) g_typePresentationSectionSizer = g_fnTypeGetObject(tPres);
            }
        }
        if (g_fnClassGetField) {
            g_fldSizerAspectRatio = g_fnClassGetField(g_klassMonitorSizer, "AspectRatio");
            if (g_klassBigMediaRenderer) {
                g_fldMediaOrigScale = g_fnClassGetField(g_klassBigMediaRenderer, "originalScreenScale");
                g_fldMediaScreensGo = g_fnClassGetField(g_klassBigMediaRenderer, "screensGameObject");
                g_fldMediaRegularRenderer = g_fnClassGetField(g_klassBigMediaRenderer, "regularScreenRenderer");
                g_fldMediaResizeRequired = g_fnClassGetField(g_klassBigMediaRenderer, "resizeRequired");
                g_fldMediaCurrentSource = g_fnClassGetField(g_klassBigMediaRenderer, "currentMediaSource");
            }
        }

        const uint8_t prefMovRbxRsiRdi[15] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x48,0x89,0x7C,0x24,0x18 };
        const uint8_t prefLerpAspect[17] = { 0x66,0x0F,0x6E,0xCA, 0x33,0xD2, 0x66,0x41,0x0F,0x6E,0xC0, 0x0F,0x5B,0xC9, 0x0F,0x5B,0xC0 };

        const void* mSetAspect = fnMethodFromName(g_klassMonitorSizer, "SetAspectRatio", 2);
        const void* mLerpAspect = fnMethodFromName(g_klassMonitorSizer, "LerpAspectRatio", 2);
        const void* mMirroredUpdate = fnMethodFromName(g_klassMirroredScreen, "UpdateScreenAspectRatio", 1);
        g_mMonitorSetAspect = mSetAspect;
        g_mMirroredUpdateAspect = mMirroredUpdate;

        InstallNonRipPrologueHook(mSetAspect, prefMovRbxRsiRdi, 15, 15, reinterpret_cast<void*>(Hooked_MonitorSetAspect),
                                  reinterpret_cast<void**>(&g_origMonitorSetAspect), &g_pTrampMonitorSet);
        InstallNonRipPrologueHook(mLerpAspect, prefLerpAspect, 17, 17, reinterpret_cast<void*>(Hooked_MonitorLerpAspect),
                                  reinterpret_cast<void**>(&g_origMonitorLerpAspect), &g_pTrampMonitorLerp);
        InstallNonRipPrologueHook(mMirroredUpdate, prefMovRbxRsiRdi, 15, 15, reinterpret_cast<void*>(Hooked_MirroredUpdateAspect),
                                  reinterpret_cast<void**>(&g_origMirroredUpdateAspect), &g_pTrampMirroredUpdate);
    }

    if (appClass && roomClass) {
        const void* mTick = fnMethodFromName(appClass, "Tick", 0);
        g_mGetCurrentRoom = fnMethodFromName(appClass, "get_CurrentRoom", 0);
        g_mGetIsLoadingScene = fnMethodFromName(appClass, "get_IsLoadingScene", 0);
        g_mLeaveRoom = fnMethodFromName(appClass, "LeaveRoom", 2);
        g_mGetIsLocalUserOwner = fnMethodFromName(roomClass, "get_IsLocalUserOwner", 0);
        g_mGetIsSinglePlayer = fnMethodFromName(roomClass, "get_IsSinglePlayer", 0);
        g_mInLobby = fnMethodFromName(roomClass, "InLobby", 0);

        if (mTick && g_mGetCurrentRoom && g_mGetIsLocalUserOwner && g_mGetIsSinglePlayer && g_mLeaveRoom) {
            uint8_t* pTarget = *reinterpret_cast<uint8_t* const*>(mTick); // MethodInfo::methodPointer at +0
            if (pTarget && pTarget[0] == 0xFF && pTarget[1] == 0x25) {
                g_appTickHookInstalled = true;
            } else if (pTarget &&
                       pTarget[0] == 0x40 && pTarget[1] == 0x53 &&
                       pTarget[2] == 0x48 && pTarget[3] == 0x83 && pTarget[4] == 0xEC && pTarget[5] == 0x70 &&
                       pTarget[6] == 0x80 && pTarget[7] == 0x3D && pTarget[12] == 0x00 &&
                       pTarget[13] == 0x48 && pTarget[14] == 0x8B && pTarget[15] == 0xD9) {
                // Expected 16-byte prologue: push rbx; sub rsp,70h; cmp byte ptr [rip+disp32],0; mov rbx,rcx
                int32_t disp = *reinterpret_cast<int32_t*>(pTarget + 8);
                uint64_t absInitByte = reinterpret_cast<uint64_t>(pTarget + 13 + disp);

                uint8_t* pTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
                if (pTramp) {
                    size_t k = 0;
                    pTramp[k++] = 0x40; pTramp[k++] = 0x53; // push rbx
                    pTramp[k++] = 0x48; pTramp[k++] = 0x83; pTramp[k++] = 0xEC; pTramp[k++] = 0x70; // sub rsp, 70h
                    pTramp[k++] = 0x50; // push rax
                    pTramp[k++] = 0x48; pTramp[k++] = 0xB8; // movabs rax, absInitByte
                    *reinterpret_cast<uint64_t*>(pTramp + k) = absInitByte; k += 8;
                    pTramp[k++] = 0x80; pTramp[k++] = 0x38; pTramp[k++] = 0x00; // cmp byte ptr [rax], 0
                    pTramp[k++] = 0x58; // pop rax (preserves RFLAGS)
                    pTramp[k++] = 0x48; pTramp[k++] = 0x8B; pTramp[k++] = 0xD9; // mov rbx, rcx
                    pTramp[k++] = 0xFF; pTramp[k++] = 0x25;
                    *reinterpret_cast<uint32_t*>(pTramp + k) = 0; k += 4;
                    *reinterpret_cast<uint64_t*>(pTramp + k) = reinterpret_cast<uint64_t>(pTarget + 16); k += 8;
                    FlushInstructionCache(GetCurrentProcess(), pTramp, 64);

                    g_pTrampolineAppTick = pTramp;
                    g_originalAppTick = reinterpret_cast<pfnAppTick>(pTramp);

                    DWORD oldProtect = 0;
                    if (VirtualProtect(pTarget, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                        pTarget[0] = 0xFF; pTarget[1] = 0x25;
                        *reinterpret_cast<uint32_t*>(pTarget + 2) = 0;
                        *reinterpret_cast<uint64_t*>(pTarget + 6) = reinterpret_cast<uint64_t>(Hooked_AppTick);
                        pTarget[14] = 0x90; pTarget[15] = 0x90;
                        DWORD tmp = 0;
                        VirtualProtect(pTarget, 16, oldProtect, &tmp);
                        FlushInstructionCache(GetCurrentProcess(), pTarget, 16);
                        g_appTickHookInstalled = true;
                    }
                }
            }
        }
    }

    fnThreadDetach(thread);
}

DWORD WINAPI HookInitThread(LPVOID) {
    HMODULE hBigSoup = nullptr;
    for (int i = 0; ; ++i) {
        if (!hBigSoup) hBigSoup = GetModuleHandleW(L"BigSoup.dll");
        if (hBigSoup) {
            InstallHookInternal(hBigSoup);
        }
        TryInstallMediaHook();
        TryInstallAppTickHook();
        if (g_localHookInstalled && g_peerHookInstalled && g_mediaHookInstalled && g_appTickHookInstalled) break;
        Sleep(i < 120 ? 250 : 2000);
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

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        if (!g_peerCsInit) {
            InitializeCriticalSectionAndSpinCount(&g_peerCs, 4000);
            g_peerCsInit = true;
        }
        CreateThread(nullptr, 0, HookInitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_peerState) { g_peerState->active = 0; UnmapViewOfFile(g_peerState); g_peerState = nullptr; }
        if (g_peerMapping) { CloseHandle(g_peerMapping); g_peerMapping = nullptr; }
        ReleasePeerExportTarget();
        ReleasePeerPipeline();
        if (g_peerDev) { g_peerDev->Release(); g_peerDev = nullptr; }

        if (g_projectedState) { UnmapViewOfFile(g_projectedState); g_projectedState = nullptr; }
        if (g_projectedMapping) { CloseHandle(g_projectedMapping); g_projectedMapping = nullptr; }
        if (g_sharedVideoSrv) { g_sharedVideoSrv->Release(); g_sharedVideoSrv = nullptr; }
        if (g_sharedVideoTex) { g_sharedVideoTex->Release(); g_sharedVideoTex = nullptr; }
        ReleaseRouteStaging();
        ReleaseBlackTexture();
        if (g_pTrampolineLocal) { VirtualFree(g_pTrampolineLocal, 0, MEM_RELEASE); g_pTrampolineLocal = nullptr; }
        if (g_pTrampolinePeer) { VirtualFree(g_pTrampolinePeer, 0, MEM_RELEASE); g_pTrampolinePeer = nullptr; }
        if (g_pTrampolineMedia) { VirtualFree(g_pTrampolineMedia, 0, MEM_RELEASE); g_pTrampolineMedia = nullptr; }
        if (g_pTrampolineAppTick) { VirtualFree(g_pTrampolineAppTick, 0, MEM_RELEASE); g_pTrampolineAppTick = nullptr; }
        if (g_pTrampMonitorSet) { VirtualFree(g_pTrampMonitorSet, 0, MEM_RELEASE); g_pTrampMonitorSet = nullptr; }
        if (g_pTrampMonitorLerp) { VirtualFree(g_pTrampMonitorLerp, 0, MEM_RELEASE); g_pTrampMonitorLerp = nullptr; }
        if (g_pTrampMirroredUpdate) { VirtualFree(g_pTrampMirroredUpdate, 0, MEM_RELEASE); g_pTrampMirroredUpdate = nullptr; }
        if (g_peerCsInit) { DeleteCriticalSection(&g_peerCs); g_peerCsInit = false; }
    }
    return TRUE;
}
