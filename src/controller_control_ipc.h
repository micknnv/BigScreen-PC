#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_controller_control_ipc {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopControllerControl";
constexpr uint32_t kMagic = 0x42444343; // BDCC
constexpr uint32_t kVersion = 1;

enum class ViewRequest : uint32_t {
    None = 0,
    First = 1,
    Third = 2,
};

#pragma pack(push, 1)
struct State {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    LONG sequence = 0;
    uint32_t viewRequest = static_cast<uint32_t>(ViewRequest::None);
    uint32_t resetEpoch = 0;
    uint32_t virtualControllersEnabled = 1;
    int32_t cameraIndex = 0;
    float cameraDistance = 2.5f;
    float cameraHeight = 0.6f;
    float cameraFov = 60.0f;
    float pointerYaw = 0.0f;
    float pointerPitch = 0.0f;
    uint32_t pointerTrigger = 0;
    uint32_t pointerActive = 0;
    float lookYaw = 0.0f;
    float lookPitch = 0.0f;
    uint32_t menuClick = 0;
};
#pragma pack(pop)

static_assert(sizeof(State) == 68, "Controller control IPC layout changed");

inline void Publish(State* state, uint32_t viewRequest, uint32_t resetEpoch,
                    bool virtualControllersEnabled, int32_t cameraIndex,
                    float cameraDistance, float cameraHeight, float cameraFov,
                    float pointerYaw, float pointerPitch, bool pointerTrigger,
                    bool pointerActive, float lookYaw, float lookPitch,
                    bool menuClick) {
    const LONG next = state->sequence + 1;
    InterlockedExchange(&state->sequence, next | 1);
    state->viewRequest = viewRequest;
    state->resetEpoch = resetEpoch;
    state->virtualControllersEnabled = virtualControllersEnabled ? 1u : 0u;
    state->cameraIndex = cameraIndex;
    state->cameraDistance = cameraDistance;
    state->cameraHeight = cameraHeight;
    state->cameraFov = cameraFov;
    state->pointerYaw = pointerYaw;
    state->pointerPitch = pointerPitch;
    state->pointerTrigger = pointerTrigger ? 1u : 0u;
    state->pointerActive = pointerActive ? 1u : 0u;
    state->lookYaw = lookYaw;
    state->lookPitch = lookPitch;
    state->menuClick = menuClick ? 1u : 0u;
    InterlockedExchange(&state->sequence, (next + 1) & ~1L);
}

inline void Publish(State* state, uint32_t viewRequest, uint32_t resetEpoch,
                    bool virtualControllersEnabled, int32_t cameraIndex,
                    float cameraDistance, float cameraHeight, float cameraFov) {
    State current{};
    const bool valid = (state && state->magic == kMagic && state->version == kVersion);
    const float py = valid ? state->pointerYaw : 0.0f;
    const float pp = valid ? state->pointerPitch : 0.0f;
    const bool pt = valid ? (state->pointerTrigger != 0) : false;
    const bool pa = valid ? (state->pointerActive != 0) : false;
    const float ly = valid ? state->lookYaw : 0.0f;
    const float lp = valid ? state->lookPitch : 0.0f;
    const bool mc = valid ? (state->menuClick != 0) : false;
    Publish(state, viewRequest, resetEpoch, virtualControllersEnabled, cameraIndex,
            cameraDistance, cameraHeight, cameraFov, py, pp, pt, pa, ly, lp, mc);
}

inline bool Read(const State* state, State& out) {
    if (!state || state->magic != kMagic || state->version != kVersion) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = state->sequence;
        if (before & 1) continue;
        out = *state;
        const LONG after = state->sequence;
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

inline void SetViewRequest(State* state, ViewRequest req) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, static_cast<uint32_t>(req), current.resetEpoch,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, current.lookYaw, current.lookPitch,
                current.menuClick != 0);
    }
}

inline void SetPointerState(State* state, float pointerYaw, float pointerPitch, bool pointerTrigger, bool pointerActive) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                pointerYaw, pointerPitch, pointerTrigger, pointerActive,
                current.lookYaw, current.lookPitch, current.menuClick != 0);
    }
}

inline void SetLookOffset(State* state, float lookYaw, float lookPitch) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, lookYaw, lookPitch, current.menuClick != 0);
    }
}

inline void PulseMenuClick(State* state) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, current.lookYaw, current.lookPitch, true);
    }
}

inline void ClearMenuClick(State* state) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, current.lookYaw, current.lookPitch, false);
    }
}

inline void TriggerReset(State* state) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch + 1,
                current.virtualControllersEnabled != 0, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                0.0f, 0.0f, false, false, 0.0f, 0.0f, false);
    }
}

inline void SetVirtualControllers(State* state, bool enabled) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                enabled, current.cameraIndex,
                current.cameraDistance, current.cameraHeight, current.cameraFov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, current.lookYaw, current.lookPitch,
                current.menuClick != 0);
    }
}

inline void SetCameraParams(State* state, int32_t cameraIndex, float distance, float height, float fov) {
    if (!state) return;
    State current{};
    if (Read(state, current)) {
        Publish(state, current.viewRequest, current.resetEpoch,
                current.virtualControllersEnabled != 0, cameraIndex,
                distance, height, fov,
                current.pointerYaw, current.pointerPitch, current.pointerTrigger != 0,
                current.pointerActive != 0, current.lookYaw, current.lookPitch,
                current.menuClick != 0);
    }
}

} // namespace bigscreen_controller_control_ipc
