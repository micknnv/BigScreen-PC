#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace bigscreen_peer_movie {

constexpr const wchar_t* kMappingName = L"Local\\BigscreenDesktopPeerMovie";
constexpr uint32_t kMagic = 0x504D4F56; // 'PMOV'
constexpr uint32_t kVersion = 1;

enum class SourceKind : uint32_t {
    None = 0,
    BigSoupPeer = 1,
    BigMediaPlayer = 2,
};

struct State {
    uint32_t magic;
    uint32_t version;
    uint32_t active;           // 1 when a remote peer video or BigMediaPlayer stream is active
    uint32_t sourceKind;       // SourceKind
    uint64_t sharedHandle;     // D3D11 shared texture handle (BGRA8, KeyedMutex)
    uint32_t width;            // Width of isolated video texture
    uint32_t height;           // Height of isolated video texture
    uint32_t format;           // DXGI_FORMAT_B8G8R8A8_UNORM
    uint64_t generation;       // Incremented whenever shared texture is recreated
    uint64_t frameSequence;    // Incremented every time a new video frame is copied
    uint64_t lastFrameMicros;  // GetTickCount64() * 1000
};

} // namespace bigscreen_peer_movie
