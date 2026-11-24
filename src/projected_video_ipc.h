#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace bigscreen_projected_video {

constexpr const wchar_t* kMappingName = L"Local\\BigscreenDesktopProjectedVideo";
constexpr uint32_t kMagic = 0x50564944; // 'PVID'
constexpr uint32_t kVersion = 1;

struct State {
    uint32_t magic;
    uint32_t version;
    uint32_t active;          // 1 if projecting app video, 0 if not
    uint32_t privacyMode;     // 1 to clear desktop to solid black when inactive, 0 to allow real desktop
    uint64_t sharedHandle;    // D3D11 shared resource handle
    uint32_t width;           // Canvas width matching desktop (e.g. 5120)
    uint32_t height;          // Canvas height matching desktop (e.g. 1440)
    uint32_t videoWidth;      // Original video content width (e.g. 1920)
    uint32_t videoHeight;     // Original video content height (e.g. 1080)
    uint64_t frameSequence;   // Incremented on every new frame
    uint64_t lastFrameMicros; // Timestamp in microseconds
};

} // namespace bigscreen_projected_video
