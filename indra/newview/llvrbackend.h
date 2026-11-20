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
#include <vector>

// One eye for the frame being rendered, in the runtime's tracking space:
// right-handed, +Y up, -Z forward, metres, origin where the headset was at start (seated/LOCAL).
struct LLVREye
{
    float pos[3];
    float rot[4];    // quaternion x, y, z, w
    float tan_fov[4]; // tangents of the left, right, up, down half-angles (left and down are negative)
};

// Controller state for the frame being rendered (Touch layout: thumbstick x right, y forward).
struct LLVRInput
{
    float move[2] = { 0.f, 0.f }; // left thumbstick
    float turn[2] = { 0.f, 0.f }; // right thumbstick
    bool trigger = false;         // right index trigger
    bool grip = false;            // right grip
    bool menu = false;            // left menu button
    bool zoom = false;            // left index trigger
    bool grab = false;            // left grip
    bool shot = false;            // Y button: screenshot
    bool aim_valid = false;       // right hand is tracked; aim_pos/aim_rot are usable
    float aim_pos[3] = { 0.f, 0.f, 0.f };     // right-hand pointing pose in tracking space,
    float aim_rot[4] = { 0.f, 0.f, 0.f, 1.f }; // pointing along its -Z
    bool left_aim_valid = false;  // the same for the left hand
    float left_aim_pos[3] = { 0.f, 0.f, 0.f };
    float left_aim_rot[4] = { 0.f, 0.f, 0.f, 1.f };
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

    // Reads the controllers. Call once per frame after beginFrame() returned true. Returns
    // false (input left untouched) when the controllers could not be set up or read.
    bool getInput(LLVRInput& input);

    // Copies a rectangle of the window's back buffer into the image for one eye.
    bool submitEye(int eye, int x, int y, int width, int height);

    // Flat images shown as quads in the headset, each layer over the ones before it: the UI
    // panel and the on-screen keyboard. beginUI() binds a width x height RGBA image of that
    // layer as the GL framebuffer; draw into it (alpha = coverage, premultiplied), then call
    // endUI() with the quad's centre and orientation (quaternion x, y, z, w; identity faces +Z)
    // in tracking space and its size in metres. The image has a depth buffer. endUI()
    // rebinds the window's framebuffer. One layer at a time, between beginFrame() and endFrame().
    enum { UI_PANEL = 0, UI_KEYBOARD = 1, UI_LAYERS = 2 };
    bool beginUI(int layer, int width, int height);
    void endUI(const float centre[3], const float rot[4], float width_m, float height_m);

    // Between beginUI() and endUI(): the layer's image as drawn so far, for a screenshot.
    // RGBA (premultiplied), bottom row first.
    bool readUI(std::vector<unsigned char>& rgba, int& width, int& height);

    // Hands the frame to the runtime. Safe to call when no frame was begun.
    void endFrame();
};

#endif
