/**
 * @file llvrbackend.h
 * @brief SLVR: thin VR runtime interface. No runtime (OpenXR) types leak out of here,
 * so the viewer-side code (llviewerVR) does not depend on the VR API in use.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#ifndef LL_LLVRBACKEND_H
#define LL_LLVRBACKEND_H

#include <string>

// One eye for the frame being rendered, in the runtime's tracking space:
// right-handed, +Y up, -Z forward, metres, origin where the headset was at start (seated/LOCAL).
struct LLVREye
{
    float pos[3];
    float rot[4];    // quaternion x, y, z, w
    float tan_fov[4]; // tangents of the left, right, up, down half-angles (left and down are negative)
};

class LLVRBackend
{
public:
    // The viewer's GL context must be current on the calling thread for every call below.

    // Connects to the runtime and creates session and per-eye images. On failure
    // returns false with a user-readable reason; details go to the viewer log (tag "VR").
    bool init(std::string& error);
    void shutdown();
    bool isInitialized() const;

    // False once the runtime ended or lost the session; the caller should shutdown().
    bool isAlive() const;

    // Starts a runtime frame (blocks until the runtime's next frame slot). Returns true
    // and fills both eyes when this frame should be rendered in stereo.
    bool beginFrame(LLVREye eyes[2]);

    // Copies a rectangle of the window's back buffer into the image for one eye.
    bool submitEye(int eye, int x, int y, int width, int height);

    // Hands the frame to the runtime. Safe to call when no frame was begun.
    void endFrame();
};

#endif
