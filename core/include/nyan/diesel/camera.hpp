#pragma once

#include <array>

namespace nyan::diesel {

// What a sprite drawn through a camera (SQSprite.setCamera, CSprite +0x63c) needs of it: Mware's
// camera as SQCamera last set it (vtable +0x10 perspective, +0x14 ortho, +0x18 the view). The vertex
// build (0x456400) then uses the view from `eye` towards `at` with `up` (LookAt) and the perspective
// (the field of view, the target's aspect, near and far) or the orthographic projection, instead of
// the task's view from z = -1000 and the screen's orthographic projection.
struct CameraView {
    bool made = false;     // create / createOrtho called
    bool ortho = false;    // +0xe0
    float fov = 0.0f;      // +0xc4: degrees
    float aspect = 0.0f;   // +0xc8
    float nearZ = 0.0f;    // +0xcc
    float farZ = 0.0f;     // +0xd0
    float width = 0.0f;    // +0xe4: ortho
    float height = 0.0f;   // +0xe8
    // The view as drawn (0x52b130 hands eye, target and up to the camera's view: +0xc0 slot +0x18)
    // and the look-at point kept at +0xec (lookAt; the free camera's target, which a fixed look-at's
    // shake moves only as drawn).
    std::array<float, 3> eye{}, at{0.0f, 0.0f, 1.0f}, up{0.0f, 1.0f, 0.0f};
    std::array<float, 3> target{0.0f, 0.0f, 1.0f};
};

}  // namespace nyan::diesel
