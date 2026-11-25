#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace bigscreen_projected_video {

constexpr const wchar_t* kMappingName = L"Local\\BigscreenDesktopProjectedVideo";
constexpr const wchar_t* kFallbackMappingName = L"Local\\BigscreenProjectedFallbackPixels";
constexpr uint32_t kMagic = 0x50564944; // 'PVID'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFallbackMaxWidth = 1920;
constexpr uint32_t kFallbackMaxHeight = 1080;

struct FallbackHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t sequence;
};

struct State {
    uint32_t magic;
    uint32_t version;
    uint32_t active;           // 1 if projecting isolated video/screen, 0 if not
    uint32_t privacyMode;      // 1 to clear desktop to solid black when inactive, 0 to allow real desktop
    uint64_t sharedHandle;     // D3D11 shared resource handle
    uint32_t width;            // Canvas width
    uint32_t height;           // Canvas height
    uint32_t videoWidth;       // Original video content width
    uint32_t videoHeight;      // Original video content height
    uint64_t frameSequence;    // Incremented on every new frame
    uint64_t lastFrameMicros;  // Timestamp in microseconds
    uint64_t requestOwnRoomSeq; // Incremented by viewer to request Bigscreen leave current room -> own room
    float aspectOverride;      // > 0.0f to override film/screen aspect ratio (0.0f = Auto)
    uint32_t needCpuFallback;  // Set to 1 by Bigscreen DLL if cross-adapter OpenSharedResource fails
    // Diagnostics populated by BigscreenThirdPersonGpuExport.dll
    uint32_t diagHookLocal;
    uint32_t diagDesktopCount;
    int32_t  diagOpenHr;
    uint32_t diagDeskFmt;
    uint32_t diagVidFmt;
    uint32_t diagCopiedCount;
    uint32_t diagAspectMask;
    uint32_t diagAspectAppliedCount;
    uint32_t diagCapHookCalls;
    int32_t  diagCapOpenHr;
    uint32_t diagCapCopiedCount;
    uint32_t diagCapReserved;
};

} // namespace bigscreen_projected_video
