/**
 * @file llviewerVR.h
 * @brief SLVR: stereo rendering of the viewer into a VR headset.
 *
 * Follows the P373R VR Mod's approach: display() renders the scene once per eye and
 * the finished window image is handed to the headset. Unlike the original, each eye
 * uses the pose and (asymmetric) field of view reported by the runtime.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#ifndef LL_LLVIEWERVR_H
#define LL_LLVIEWERVR_H

#include "llvrbackend.h"
#include "v3math.h"

class LLViewerVR
{
public:
    // display() hook, before the camera is applied: on the first pass of a frame starts the
    // VR frame, then points the viewer camera at the eye about to be rendered.
    void beginEye();

    // display() hook, after the UI is drawn: sends the finished image to the eye.
    // Returns true when display() must render the scene again for the other eye.
    bool endEye();

    // LLViewerCamera hook: tangents of the left/right/up/down half-angles of the eye being
    // rendered, or nullptr when the normal symmetric projection applies.
    const F32* eyeTanFov() const;

    // render_ui() hook: true while an eye is rendered. The flat UI is then left out of the
    // scene image (drawn identically into both eyes it cannot be fused) and shown on a
    // panel in front of the user instead.
    bool rendersEye() const { return mEye >= 0; }

    // LLViewerWindow::cursorIntersect() hook: while the controller points into the world,
    // replaces the mouse ray with the laser, so clicks hit what the laser points at.
    void overridePickRay(LLVector3& start, LLVector3& end, F32 depth) const;

    void shutdown();

private:
    void updateEnabled();
    void setEyeCamera();
    LLVector3 toAgent(const LLVector3& local) const;
    void updatePointer();
    void drawLaser();
    void drawPanel();
    void applyInput();
    static void onIdle(void* self);

    LLVRBackend mBackend;
    LLVREye mEyes[2];
    S32 mEye = -1;            // eye being rendered; -1 = ordinary single-view rendering
    bool mSecondPass = false; // endEye() asked display() to go round again

    // Controllers: read while rendering, acted on from the next idle() like other input.
    LLVRInput mInput;
    bool mInputFresh = false;   // mInput was read this frame and not yet acted on
    bool mPanelVisible = true;  // toggled with the menu button
    bool mLaserValid = false;   // right hand is tracked: the laser is drawn and steers world picks
    bool mPanelHit = false;     // the laser points at the UI panel
    LLVector3 mLaserStart, mLaserDir; // agent coordinates
    F32 mLaserLength = 0.f;
    S32 mPointerX = 0, mPointerY = 0; // where the laser meets the panel: window pixels, origin bottom left
    bool mMenuDown = false, mTriggerDown = false, mGripDown = false, mTurned = false;

    // Viewer camera as the agent camera left it this frame: the VR tracking origin.
    LLVector3 mBaseOrigin, mBaseAt, mBaseLeft, mBaseUp;
    F32 mBaseYawCos = 1.f, mBaseYawSin = 0.f;
};

extern LLViewerVR gVR;

#endif
