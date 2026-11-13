/**
 * @file llviewerVR.cpp
 * @brief SLVR: stereo rendering of the viewer into a VR headset.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llviewerVR.h"

#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

extern bool gCubeSnapshot;

LLViewerVR gVR;

namespace
{
    // Rotates a vector by a quaternion (x, y, z, w).
    void rotate(const float q[4], const float v[3], float out[3])
    {
        // out = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        const float tx = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
        const float ty = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
        const float tz = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
        out[0] = v[0] + 2.f * (q[1] * tz - q[2] * ty);
        out[1] = v[1] + 2.f * (q[2] * tx - q[0] * tz);
        out[2] = v[2] + 2.f * (q[0] * ty - q[1] * tx);
    }

    // Tracking space (+Y up, -Z forward, +X right) to the viewer's camera-local frame
    // (+X forward, +Y left, +Z up).
    LLVector3 trackingToLocal(const float v[3])
    {
        return LLVector3(-v[2], -v[0], v[1]);
    }

    // Self-check of the two conversions above; cheap, runs once when VR is switched on.
    void checkConventions()
    {
        const float identity[4] = { 0.f, 0.f, 0.f, 1.f };
        const float quarter_turn_left[4] = { 0.f, sinf(F_PI_BY_TWO / 2.f), 0.f, cosf(F_PI_BY_TWO / 2.f) }; // +90 deg about +Y
        const float forward[3] = { 0.f, 0.f, -1.f };
        float out[3];
        rotate(identity, forward, out);
        llassert_always(trackingToLocal(out) == LLVector3(1.f, 0.f, 0.f));              // looking ahead = +X
        rotate(quarter_turn_left, forward, out);
        llassert_always((trackingToLocal(out) - LLVector3(0.f, 1.f, 0.f)).length() < 1e-4f); // head turned left = +Y
    }
}

void LLViewerVR::updateEnabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "VREnabled", false);
    static LLCachedControl<bool> hide_ui(gSavedSettings, "VRHideUI", true);

    // Ctrl+Alt+F7 toggles VR. Polled like the original mod does, so no menu/keybinding files change.
    const bool hotkey = gKeyboard && gKeyboard->getKeyDown(KEY_F7)
                        && gKeyboard->currentMask(false) == (MASK_CONTROL | MASK_ALT);
    if (hotkey && !mHotkeyDown)
    {
        gSavedSettings.setBOOL("VREnabled", !enabled);
    }
    mHotkeyDown = hotkey;

    if (enabled && !mBackend.isInitialized())
    {
        LL_INFOS("VR") << "VR enabled, starting OpenXR" << LL_ENDL;
        checkConventions();
        std::string error;
        if (!mBackend.init(error))
        {
            gSavedSettings.setBOOL("VREnabled", false);
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "VR could not be started: " + error));
            return;
        }
        // The flat UI is drawn identically into both eyes, which the eyes cannot fuse, so it is
        // hidden while in VR. shortcut: show the UI on a panel in the world (quad layer) instead.
        if (hide_ui && gViewerWindow->getUIVisibility())
        {
            gViewerWindow->setUIVisibility(false);
            mUIHidden = true;
        }
    }
    else if (mBackend.isInitialized() && (!enabled || !mBackend.isAlive()))
    {
        LL_INFOS("VR") << (enabled ? "VR session ended by the runtime" : "VR disabled") << LL_ENDL;
        gSavedSettings.setBOOL("VREnabled", false);
        shutdown();
    }
}

void LLViewerVR::shutdown()
{
    mBackend.shutdown();
    mEye = -1;
    mSecondPass = false;
    if (mUIHidden && gViewerWindow)
    {
        gViewerWindow->setUIVisibility(true);
    }
    mUIHidden = false;
}

void LLViewerVR::beginEye()
{
    if (!mSecondPass)
    {
        mEye = -1;
        updateEnabled();
        if (!mBackend.isInitialized() || !mBackend.beginFrame(mEyes))
        {
            return; // not in VR, or the runtime does not want this frame: render normally
        }

        // The agent camera has just positioned the viewer camera. Use it as the origin of the
        // tracking space, keeping only its heading so that the horizon always stays level.
        LLViewerCamera& camera = LLViewerCamera::instance();
        mBaseOrigin = camera.getOrigin();
        mBaseAt = camera.getAtAxis();
        mBaseLeft = camera.getLeftAxis();
        mBaseUp = camera.getUpAxis();
        const F32 yaw = atan2f(mBaseAt.mV[VY], mBaseAt.mV[VX]);
        mBaseYawCos = cosf(yaw);
        mBaseYawSin = sinf(yaw);
        mEye = 0;
    }
    mSecondPass = false;

    if (mEye >= 0)
    {
        setEyeCamera();
    }
}

void LLViewerVR::setEyeCamera()
{
    const LLVREye& eye = mEyes[mEye];

    // Camera-local vector to agent (region) coordinates: turn by the base heading about +Z.
    auto toAgent = [this](const LLVector3& v)
    {
        return LLVector3(v.mV[VX] * mBaseYawCos - v.mV[VY] * mBaseYawSin,
                         v.mV[VX] * mBaseYawSin + v.mV[VY] * mBaseYawCos,
                         v.mV[VZ]);
    };

    const float forward[3] = { 0.f, 0.f, -1.f };
    const float upward[3] = { 0.f, 1.f, 0.f };
    float rotated[3];
    rotate(eye.rot, forward, rotated);
    const LLVector3 at = toAgent(trackingToLocal(rotated));
    rotate(eye.rot, upward, rotated);
    const LLVector3 up = toAgent(trackingToLocal(rotated));

    LLViewerCamera& camera = LLViewerCamera::instance();
    camera.setOrigin(mBaseOrigin + toAgent(trackingToLocal(eye.pos)));
    camera.setAxes(at, up % at, up);
}

bool LLViewerVR::endEye()
{
    if (mEye < 0)
    {
        mBackend.endFrame(); // no-op unless a frame was begun that the runtime did not want rendered
        return false;
    }

    const LLRect view = gViewerWindow->getWorldViewRectRaw();
    mBackend.submitEye(mEye, view.mLeft, view.mBottom, view.getWidth(), view.getHeight());

    if (mEye == 0)
    {
        mEye = 1;
        mSecondPass = true;
        return true;
    }

    mEye = -1;
    mBackend.endFrame();

    // Put the camera back so the rest of the frame sees what the agent camera intended.
    LLViewerCamera& camera = LLViewerCamera::instance();
    camera.setOrigin(mBaseOrigin);
    camera.setAxes(mBaseAt, mBaseLeft, mBaseUp);
    return false;
}

const F32* LLViewerVR::eyeTanFov() const
{
    // Reflection probes render through the same camera while an eye is in progress and must
    // keep their own 90 degree projection.
    return (mEye >= 0 && !gCubeSnapshot) ? mEyes[mEye].tan_fov : nullptr;
}
