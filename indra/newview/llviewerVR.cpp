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

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llchat.h"
#include "llfocusmgr.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llimview.h"
#include "llglstates.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerobject.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "pipeline.h"

extern bool gCubeSnapshot;
extern bool gGLCoverageAlpha; // llrender.cpp
void render_hud_attachments(); // llviewerdisplay.cpp

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

    // Orientation (quaternion x, y, z, w) of a panel whose face, its +Z, looks back along
    // dir: yaw about +Y, then pitch, so the panel never rolls.
    void facing(const float dir[3], float out[4])
    {
        const F32 yaw = atan2f(-dir[0], -dir[2]);
        const F32 pitch = asinf(llclamp(dir[1], -1.f, 1.f));
        const F32 sy = sinf(yaw / 2.f), cy = cosf(yaw / 2.f);
        const F32 sp = sinf(pitch / 2.f), cp = cosf(pitch / 2.f);
        out[0] = cy * sp;
        out[1] = sy * cp;
        out[2] = -sy * sp;
        out[3] = cy * cp;
    }

    // While the left grip holds the panel, the right stick moves it along the laser and
    // resizes it, within these limits (metres).
    // shortcut: the panel's place and size are forgotten when the viewer closes; make them
    // settings when setting them up every session annoys.
    const F32 PANEL_NEAR = 0.3f, PANEL_FAR = 5.f;
    const F32 PANEL_NARROW = 0.4f, PANEL_WIDE = 4.f;
    const F32 PANEL_RATE = 1.5f; // e-foldings per second at full stick

    // The on-screen keyboard's quad: its width in metres, and where it first appears relative
    // to the head (metres ahead and below), tilted to face it.
    const F32 KEYBOARD_WIDTH = 0.55f, KEYBOARD_NARROW = 0.25f, KEYBOARD_WIDE = 1.5f;
    const F32 KEYBOARD_AHEAD = 0.5f, KEYBOARD_BELOW = 0.3f;
    const F32 KEY_REPEAT_DELAY = 0.5f, KEY_REPEAT_INTERVAL = 0.07f; // seconds, for backspace held down

    // What "chat only" keeps of the panel: Firestorm's conversation windows, docked or torn off.
    const char* const CHAT_FLOATERS[] = { "fs_im_container", "fs_nearby_chat", "fs_impanel" };
    const F32 MESSAGE_DOT_RADIUS = 0.012f; // metres, a metre ahead

    const F32 SNAP_TURN = 30.f * DEG_TO_RAD;
    const F32 STICK_ON = 0.6f;       // a thumbstick counts as pushed beyond this
    const F32 STICK_OFF = 0.3f;      // and as let go below this
    const F32 LASER_LENGTH = 20.f;   // drawn length when it hits nothing
    const F32 PICK_DISTANCE = 512.f; // as far as the mouse picks
    const F32 FLY_HOLD = 0.5f;       // seconds the right stick is held up before flying starts
    const F32 ZOOM_RATE = 1.5f;      // zoom e-foldings, and orbit radians, per second at full stick
    const F32 DOUBLE_CLICK_TIME = 0.5f; // seconds
    const S32 DOUBLE_CLICK_SLOP = 40;   // pixels the laser may wander between the two clicks
    const F32 ZOOM_RETARGET = 0.1f;     // metres the laser's hit moves before the zoom focus follows it
    const F32 GRAB_HOLD = 0.4f;         // seconds the left grip is held before it takes the panel: a brush must not move it

    // Laser steadying, see LLViewerVR::steady(): cutoff frequencies in Hz, and how much a
    // swing of one radian per second raises the cutoff.
    const F32 STEADY_MIN_CUTOFF = 1.f;
    const F32 STEADY_BETA = 2.f; // 0.5 degree tremor at 30 fps comes out 0.08; a 30 degree swing settles in 4 frames
    const F32 STEADY_SPEED_CUTOFF = 1.f;

    // Share of the way a first-order low-pass filter moves towards its input in dt seconds.
    F32 lowPass(F32 cutoff, F32 dt)
    {
        return 1.f - expf(-dt * F_TWO_PI * cutoff);
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

        // A panel held along a laser pointing up and to the right faces back down it.
        const float dir[3] = { 0.48f, 0.6f, -0.64f };
        const float face[3] = { 0.f, 0.f, 1.f };
        float q[4];
        facing(dir, q);
        rotate(q, face, out);
        llassert_always(fabsf(out[0] + dir[0]) + fabsf(out[1] + dir[1]) + fabsf(out[2] + dir[2]) < 1e-4f);
    }
}

void LLViewerVR::updateEnabled()
{
    // Toggled with Ctrl+Alt+F7, see LLViewerWindow::handleTranslatedKeyDown().
    static LLCachedControl<bool> enabled(gSavedSettings, "VREnabled", false);

    if (enabled && !mBackend.isInitialized())
    {
        LL_INFOS("VR") << "VR enabled, starting OpenXR" << LL_ENDL;
        checkConventions();
        checkSteady();
        LLVRKeyboard::selfCheck();
        std::string error;
        if (!mBackend.init(error))
        {
            gSavedSettings.setBOOL("VREnabled", false);
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "VR could not be started: " + error));
            return;
        }
        mPanelVisible = true;
        mChatOnly = mUnread = false;
        gIdleCallbacks.addFunction(onIdle, this);
        // Instant messages; nearby chat comes through noteChat().
        mIMConnection = LLIMModel::instance().addNewMsgCallback([this](const LLSD& msg)
        {
            if (!msg["is_region_msg"].asBoolean())
            {
                noteMessage(msg["from_id"].asUUID());
            }
        });
        // The headset paces the frames; waiting for the monitor's vsync as well would cap and
        // judder VR at the monitor's refresh rate.
        gViewerWindow->getWindow()->toggleVSync(false);
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
    mEye = -1;
    mSecondPass = false;
    const bool was_running = mBackend.isInitialized();
    mBackend.shutdown();
    mLaserValid = mPanelHit = mWorldHit = mFirstPerson = mLeftLaserValid = mLeftPanelHit = false;
    mSteady.valid = mLeftSteady.valid = false;
    mKeyboardShown = mKeyboardPlaced = false;
    mKeyboardHit[0] = mKeyboardHit[1] = LLVRKeyboard::Hit();
    mDragHand = mRepeatHand = -1;
    mIMConnection.disconnect();
    if (was_running && gViewerWindow)
    {
        gViewerWindow->getWindow()->toggleVSync(gSavedSettings.getBOOL("RenderVSyncEnable"));
        mInputFresh = false;
        applyInput(); // lets go of any button or stick still held, shows the own avatar again
    }
    gIdleCallbacks.deleteFunction(onIdle, this);
}

void LLViewerVR::beginEye()
{
    if (!mSecondPass)
    {
        mEye = -1;
        mLaserValid = mPanelHit = mWorldHit = mFirstPerson = mLeftLaserValid = mLeftPanelHit = false;
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
        // Heading from the left axis: it stays horizontal (the camera never rolls), while the
        // at axis loses its horizontal part when looking straight up or down.
        F32 yaw = atan2f(-mBaseLeft.mV[VX], mBaseLeft.mV[VY]);

        // Unless the camera was sent somewhere (zoom, see applyInput()), look from the
        // avatar's own eyes instead: where mouselook puts the camera (see
        // LLAgentCamera::calcCameraPositionTargetGlobal()), heading where the avatar faces.
        if (gAgentCamera.getFocusOnAvatar() && gAgentCamera.getCameraMode() == CAMERA_MODE_THIRD_PERSON
            && isAgentAvatarValid() && gAgentAvatarp->mDrawable.notNull())
        {
            mFirstPerson = true;
            mZoomYawOffset = 0.f;
            LLQuaternion heading = gAgent.getFrameAgent().getQuaternion();
            LLVector3 head(0.f, 0.f, gAgentAvatarp->mHeadOffset.mV[VZ]);
            if (gAgentAvatarp->isSitting())
            {
                head.mV[VZ] += 0.1f;
            }
            LLViewerObject* seat = (LLViewerObject*)gAgentAvatarp->getParent();
            if (gAgentAvatarp->isSitting() && seat)
            {
                gAgentAvatarp->updateHeadOffset();
                head.mV[VX] = gAgentAvatarp->mHeadOffset.mV[VX];
                head.mV[VY] = gAgentAvatarp->mHeadOffset.mV[VY];
                head.mV[VZ] = gAgentAvatarp->mHeadOffset.mV[VZ] + 0.1f;
                mBaseOrigin = (gAgentAvatarp->getPosition() + head * gAgentAvatarp->getRotation()) * seat->getRenderMatrix();
                if (!((LLViewerObject*)gAgentAvatarp->getRoot())->flagCameraDecoupled())
                {
                    heading *= seat->getRenderRotation();
                }
            }
            else
            {
                F32 fixup = 0.f;
                if (!gAgentAvatarp->isSitting() && gAgentAvatarp->hasPelvisFixup(fixup))
                {
                    head.mV[VZ] -= fixup;
                }
                mBaseOrigin = gAgentAvatarp->getRenderPosition() + head * gAgentAvatarp->getRenderRotation();
            }
            const LLVector3 at = LLVector3::x_axis * heading;
            if (gAgentAvatarp->isSitting())
            {
                mYawOffset = 0.f;
            }
            yaw = atan2f(at.mV[VY], at.mV[VX]) + mYawOffset;
            mBaseAt.set(cosf(yaw), sinf(yaw), 0.f);
            mBaseLeft.set(-sinf(yaw), cosf(yaw), 0.f);
            mBaseUp = LLVector3::z_axis;
        }
        else
        {
            yaw += mZoomYawOffset;
        }
        mBaseYawCos = cosf(yaw);
        mBaseYawSin = sinf(yaw);
        mEye = 0;

        mInputFresh = mBackend.getInput(mInput);
        if (mInputFresh)
        {
            updatePointer();
        }
    }
    mSecondPass = false;

    if (mEye >= 0)
    {
        setEyeCamera();
    }
}

// Camera-local vector to agent (region) coordinates: turn by the base heading about +Z.
LLVector3 LLViewerVR::toAgent(const LLVector3& v) const
{
    return LLVector3(v.mV[VX] * mBaseYawCos - v.mV[VY] * mBaseYawSin,
                     v.mV[VX] * mBaseYawSin + v.mV[VY] * mBaseYawCos,
                     v.mV[VZ]);
}

void LLViewerVR::setEyeCamera()
{
    const LLVREye& eye = mEyes[mEye];

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

    if (mLaserValid)
    {
        drawLaser(mLaserStart, mLaserDir, mLaserLength);
    }
    if (mLeftLaserValid)
    {
        drawLaser(mLeftLaserStart, mLeftLaserDir, mLeftLaserLength);
    }
    if (mUnread && !mPanelVisible)
    {
        drawMessageDot();
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
    if (mPanelVisible)
    {
        drawPanel();
    }
    if (mKeyboardShown)
    {
        drawKeyboard();
    }
    mBackend.endFrame();

    // Put the camera back so the rest of the frame sees what the agent camera intended.
    LLViewerCamera& camera = LLViewerCamera::instance();
    camera.setOrigin(mBaseOrigin);
    camera.setAxes(mBaseAt, mBaseLeft, mBaseUp);
    gViewerWindow->setup3DRender(); // re-cache matrices and frustum planes for that camera (picking, name tags)
    return false;
}

const F32* LLViewerVR::eyeTanFov() const
{
    // Reflection probes render through the same camera while an eye is in progress and must
    // keep their own 90 degree projection.
    return (mEye >= 0 && !gCubeSnapshot) ? mEyes[mEye].tan_fov : nullptr;
}

// Works out where the right controller points: at the keyboard, the UI panel, or else into
// the world. While the left grip is held the panel is carried along on the laser instead.
void LLViewerVR::updatePointer()
{
    updateLeftPointer();
    if (!mInput.aim_valid)
    {
        mGrabbing = false;
        mSteady.valid = false;
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }
    steady(mSteady, mInput.aim_pos, mInput.aim_rot, (F32)gFrameIntervalSeconds);
    const float* dir = mSteady.dir;
    const float* pos = mSteady.pos;

    mLaserValid = true;
    mLaserStart = mBaseOrigin + toAgent(trackingToLocal(pos));
    mLaserDir = toAgent(trackingToLocal(dir));
    mLaserLength = LASER_LENGTH;

    if (mGrabLong && mPanelVisible)
    {
        if (!mGrabbing)
        {
            mGrabbing = true;
            const LLVector3 to_panel(mPanelCentre[0] - pos[0], mPanelCentre[1] - pos[1], mPanelCentre[2] - pos[2]);
            mGrabDistance = to_panel.length();
        }
        // Right stick: forward and back moves the panel along the laser, sideways resizes it.
        const F32 step = PANEL_RATE * llmin((F32)gFrameIntervalSeconds, 0.1f);
        mGrabDistance = llclamp(mGrabDistance * expf(mInput.turn[1] * step), PANEL_NEAR, PANEL_FAR);
        mPanelWidth = llclamp(mPanelWidth * expf(mInput.turn[0] * step), PANEL_NARROW, PANEL_WIDE);
        for (S32 i = 0; i < 3; ++i)
        {
            mPanelCentre[i] = pos[i] + dir[i] * mGrabDistance;
        }
        facing(dir, mPanelRot);
        mLaserLength = mGrabDistance;
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }
    mGrabbing = false;

    if (pointAtKeyboard(0, pos, dir, mLaserLength))
    {
        return;
    }
    if (hitPanel(pos, dir, mPointerX, mPointerY, mLaserLength))
    {
        mPanelHit = true;
        return;
    }

    LLVector4a start, end, hit;
    start.load3(mLaserStart.mV);
    end.load3((mLaserStart + mLaserDir * PICK_DISTANCE).mV);
    S32 face = -1;
    if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, nullptr, nullptr, &hit))
    {
        mWorldHit = true;
        mWorldHitPos.set(hit.getF32ptr());
        mLaserLength = (mWorldHitPos - mLaserStart).length();
    }
}

// The left hand points only while the on-screen keyboard is up (shown last frame), so that
// both hands can type; the rest of the time its trigger and grip have other jobs.
void LLViewerVR::updateLeftPointer()
{
    if (!mInput.left_aim_valid || !mKeyboardShown)
    {
        mLeftSteady.valid = false;
        mKeyboardHit[1] = LLVRKeyboard::Hit();
        return;
    }
    steady(mLeftSteady, mInput.left_aim_pos, mInput.left_aim_rot, (F32)gFrameIntervalSeconds);
    mLeftLaserValid = true;
    mLeftLaserStart = mBaseOrigin + toAgent(trackingToLocal(mLeftSteady.pos));
    mLeftLaserDir = toAgent(trackingToLocal(mLeftSteady.dir));
    mLeftLaserLength = LASER_LENGTH;
    if (!pointAtKeyboard(1, mLeftSteady.pos, mLeftSteady.dir, mLeftLaserLength))
    {
        mLeftPanelHit = hitPanel(mLeftSteady.pos, mLeftSteady.dir, mLeftPointerX, mLeftPointerY, mLeftLaserLength);
    }
}

// Whether a hand's laser (tracking space) is on the keyboard, which lies in front of the
// panel; if so what it is on (mKeyboardHit) and how far along. A hand dragging the keyboard
// by its handle carries it along its laser, the right stick moving it nearer and resizing it.
bool LLViewerVR::pointAtKeyboard(S32 hand, const float pos[3], const float dir[3], F32& length)
{
    mKeyboardHit[hand] = LLVRKeyboard::Hit();
    if (!mKeyboardShown)
    {
        return false;
    }
    const F32 height = mKeyboardWidth * LLVRKeyboard::HEIGHT / LLVRKeyboard::WIDTH;
    if (mDragHand == hand)
    {
        const F32 step = PANEL_RATE * llmin((F32)gFrameIntervalSeconds, 0.1f);
        mDragDistance = llclamp(mDragDistance * expf(mInput.turn[1] * step), PANEL_NEAR, PANEL_FAR);
        mKeyboardWidth = llclamp(mKeyboardWidth * expf(mInput.turn[0] * step), KEYBOARD_NARROW, KEYBOARD_WIDE);
        facing(dir, mKeyboardRot);
        // The handle, near the bottom edge, stays under the laser.
        const float up_axis[3] = { 0.f, 1.f, 0.f };
        float up[3];
        rotate(mKeyboardRot, up_axis, up);
        const F32 above = (0.5f - (F32)LLVRKeyboard::HANDLE_Y / LLVRKeyboard::HEIGHT) * height;
        for (S32 i = 0; i < 3; ++i)
        {
            mKeyboardCentre[i] = pos[i] + dir[i] * mDragDistance + up[i] * above;
        }
        mKeyboardHit[hand].part = LLVRKeyboard::HANDLE;
        length = mDragDistance;
        return true;
    }
    S32 x = 0, y = 0;
    F32 along = 0.f;
    if (!hitQuad(mKeyboardCentre, mKeyboardRot, mKeyboardWidth, height, LLVRKeyboard::WIDTH, LLVRKeyboard::HEIGHT, pos, dir, x, y, along))
    {
        return false;
    }
    const LLVRKeyboard::Hit hit = mKeyboard.hitAt(x, y);
    if (hit.part == LLVRKeyboard::NONE)
    {
        return false; // the empty corners beside the handle
    }
    mKeyboardHit[hand] = hit;
    mKeyboardX[hand] = x;
    mKeyboardY[hand] = y;
    length = along;
    return true;
}

// Where a ray (tracking space) meets the UI panel: window pixels, origin bottom left, and
// the distance along the ray. Leaves the outputs alone when it misses. With only the chat
// shown, the rest of the panel lets the laser through.
bool LLViewerVR::hitPanel(const float pos[3], const float dir[3], S32& x, S32& y, F32& distance) const
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (!mPanelVisible || window.getWidth() <= 0 || window.getHeight() <= 0)
    {
        return false;
    }
    S32 px = 0, py = 0;
    F32 along = 0.f;
    if (!hitQuad(mPanelCentre, mPanelRot, mPanelWidth, mPanelWidth * window.getHeight() / window.getWidth(),
                 window.getWidth(), window.getHeight(), pos, dir, px, py, along))
    {
        return false;
    }
    if (mChatOnly && std::none_of(mPanelKeep.begin(), mPanelKeep.end(), [&](const LLRect& r) { return r.pointInRect(px, py); }))
    {
        return false;
    }
    x = px;
    y = py;
    distance = along;
    return true;
}

// Where a ray (tracking space) meets a quad of the given size in metres, showing an image
// of image_width x image_height pixels: image pixels, origin bottom left, and the distance.
bool LLViewerVR::hitQuad(const float centre[3], const float rot[4], F32 width, F32 height, S32 image_width, S32 image_height,
                         const float pos[3], const float dir[3], S32& x, S32& y, F32& distance)
{
    // Into the quad's own frame (x right, y up, z out of its face): it lies in z = 0.
    const float inverse[4] = { -rot[0], -rot[1], -rot[2], rot[3] };
    const float offset[3] = { pos[0] - centre[0], pos[1] - centre[1], pos[2] - centre[2] };
    float local_pos[3], local_dir[3];
    rotate(inverse, offset, local_pos);
    rotate(inverse, dir, local_dir);
    if (local_dir[2] >= -0.01f)
    {
        return false;
    }
    const F32 along = -local_pos[2] / local_dir[2];
    const F32 u = (local_pos[0] + along * local_dir[0]) / width + 0.5f;
    const F32 v = (local_pos[1] + along * local_dir[1]) / height + 0.5f;
    if (along <= 0.f || u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
    {
        return false;
    }
    x = llfloor(u * image_width);
    y = llfloor(v * image_height);
    distance = along;
    return true;
}

// Steadies a hand's pointing pose against tremor, which made the laser land on neighbouring
// keys: an adaptive low-pass (the 1-euro filter) that follows slowly while the hand is
// nearly still and at once when it swings.
void LLViewerVR::steady(SteadyPose& s, const float pos[3], const float rot[4], F32 dt)
{
    const float forward[3] = { 0.f, 0.f, -1.f };
    float dir[3];
    rotate(rot, forward, dir);
    if (!s.valid)
    {
        s.valid = true;
        s.speed = 0.f;
        std::copy(pos, pos + 3, s.pos);
        std::copy(dir, dir + 3, s.dir);
        return;
    }
    dt = llclamp(dt, 0.001f, 0.1f);
    const F32 cosine = llclamp(dir[0] * s.dir[0] + dir[1] * s.dir[1] + dir[2] * s.dir[2], -1.f, 1.f);
    s.speed += (acosf(cosine) / dt - s.speed) * lowPass(STEADY_SPEED_CUTOFF, dt); // radians per second
    const F32 share = lowPass(STEADY_MIN_CUTOFF + STEADY_BETA * s.speed, dt);
    F32 length = 0.f;
    for (S32 i = 0; i < 3; ++i)
    {
        s.pos[i] += (pos[i] - s.pos[i]) * share;
        s.dir[i] += (dir[i] - s.dir[i]) * share;
        length += s.dir[i] * s.dir[i];
    }
    length = sqrtf(length);
    for (S32 i = 0; i < 3; ++i)
    {
        s.dir[i] = length > 0.01f ? s.dir[i] / length : dir[i]; // short only if the hand turned right round in a frame
    }
}

// Self-check of steady(), the figures quoted at STEADY_BETA: runs once when VR is switched on.
void LLViewerVR::checkSteady()
{
    const float pos[3] = { 0.f, 0.f, 0.f };
    auto yawed = [](F32 degrees, float q[4]) // about +Y
    {
        const F32 half = degrees * DEG_TO_RAD / 2.f;
        q[0] = q[2] = 0.f;
        q[1] = sinf(half);
        q[3] = cosf(half);
    };
    auto off = [](const SteadyPose& s, F32 degrees) // how far the steadied laser is from that heading
    {
        return fabsf(atan2f(-s.dir[0], -s.dir[2]) * RAD_TO_DEG - degrees);
    };
    SteadyPose s;
    float q[4];
    F32 tremor = 0.f;
    for (S32 frame = 0; frame < 70; ++frame)
    {
        yawed(frame % 2 ? 0.5f : -0.5f, q);
        steady(s, pos, q, 1.f / 30.f);
        if (frame >= 60)
        {
            tremor = llmax(tremor, off(s, 0.f));
        }
    }
    llassert_always(tremor < 0.12f);
    yawed(30.f, q);
    for (S32 frame = 0; frame < 10; ++frame)
    {
        steady(s, pos, q, 1.f / 30.f);
    }
    llassert_always(off(s, 30.f) < 0.1f);
}

void LLViewerVR::overridePickRay(LLVector3& start, LLVector3& end, F32 depth) const
{
    // Also while it points at the panel: a click on an empty part of the panel falls
    // through to the world, and the laser is what the user sees passing through there.
    if (mLaserValid)
    {
        start = mLaserStart;
        end = mLaserStart + mLaserDir * depth;
    }
}

// Draws the laser into the eye that has just been rendered, over everything else.
void LLViewerVR::drawLaser(const LLVector3& start, const LLVector3& dir, F32 length)
{
    gViewerWindow->setup3DRender(); // this eye's matrices; render_ui() left its own behind
    LLGLSUIDefault gls_ui;
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);
    gGL.color3f(1.f, 0.25f, 0.2f);
    gGL.vertex3fv(start.mV);
    gGL.vertex3fv((start + dir * length).mV);
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

// Draws the HUD attachments and the flat UI into the panel image instead of the window.
void LLViewerVR::drawPanel()
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    const S32 width = window.getWidth(), height = window.getHeight();
    if (!mBackend.beginUI(LLVRBackend::UI_PANEL, width, height))
    {
        return;
    }
    gGL.setColorMask(true, true); // alpha is what lets the world show through the empty parts
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // HUD attachments belong to the flat screen like the UI does: render_ui() leaves them
    // out of the eye images, where they ended up at the edge of the field of view.
    GLint target = 0, after = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    gViewerWindow->setup3DRender();
    render_hud_attachments();
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &after);
    if (after != target)
    {
        // A pass that went through an LLRenderTarget ends on the window's framebuffer; that
        // left the panel empty in v0.3.0. Keep at least the UI on the panel.
        LL_WARNS_ONCE("VR") << "HUD pass left framebuffer " << after << " bound instead of " << target << LL_ENDL;
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glDrawBuffer(GL_COLOR_ATTACHMENT0); // LLRenderTarget::flush() selected GL_BACK
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    LLGLSUIDefault gls_ui;
    gViewerWindow->setup2DRender();
    {
        // The HUD shaders do not write coverage to alpha. Make every pixel they drew opaque:
        // those are the ones nearer than the cleared depth.
        // shortcut: see-through HUD faces (they write no depth) keep whatever alpha their
        // shader wrote; fix when a HUD that relies on them is unreadable.
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GREATER);
        glDepthRange(1.0, 1.0);
        gGL.setColorMask(false, true);
        gUIProgram.bind();
        gl_rect_2d(0, height, width, 0, LLColor4::white);
        gGL.flush();
        gUIProgram.unbind();
        glDepthRange(0.0, 1.0);
        gGL.setColorMask(true, true);
    }

    // Not render_ui_2d(): with RenderUIBuffer on it draws through a render target and then
    // returns to the window's framebuffer, leaving the panel image empty.
    // Ordinary blending squares alpha on an empty target, which left translucent UI (pie
    // menus) nearly invisible; gGLCoverageAlpha accumulates it as coverage instead.
    gGLCoverageAlpha = true;
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    gViewerWindow->draw();

    // The UI leaves a plain window-pixel coordinate system behind.
    gUIProgram.bind();
    if (mChatOnly)
    {
        eraseAllButChat(width, height);
    }
    // The mouse cursor is not part of the UI image; mark where the lasers land.
    for (S32 hand = 0; hand < 2; ++hand)
    {
        if (hand ? mLeftPanelHit : mPanelHit)
        {
            const S32 x = hand ? mLeftPointerX : mPointerX, y = hand ? mLeftPointerY : mPointerY;
            gl_rect_2d(x - 5, y + 5, x + 5, y - 5, LLColor4::black);
            gl_rect_2d(x - 3, y + 3, x + 3, y - 3, LLColor4::white);
        }
    }
    gGL.flush();
    gGLCoverageAlpha = false;
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    gUIProgram.unbind();
    gGL.setColorMask(true, false);

    mBackend.endUI(mPanelCentre, mPanelRot, mPanelWidth, mPanelWidth * height / width);
}

// With only the chat shown: the chat windows and open menus stay on the panel image, the
// rest is made transparent. Their rectangles also bound where the lasers hit the panel.
void LLViewerVR::eraseAllButChat(S32 width, S32 height)
{
    mPanelKeep.clear();
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    auto keep = [&](const LLView* view)
    {
        const LLRect r = view->calcScreenRect(); // UI units
        mPanelKeep.emplace_back(llfloor(r.mLeft * scale.mV[VX]), llceil(r.mTop * scale.mV[VY]),
                                llceil(r.mRight * scale.mV[VX]), llfloor(r.mBottom * scale.mV[VY]));
    };
    for (const char* name : CHAT_FLOATERS)
    {
        for (const LLFloater* floater : LLFloaterReg::getFloaterList(name))
        {
            if (floater->isShown())
            {
                keep(floater);
            }
        }
    }
    for (const LLView* menu : *gMenuHolder->getChildList()) // context and pie menus
    {
        if (menu->getVisible())
        {
            keep(menu);
        }
    }

    // Mark what stays in the depth buffer (the HUDs are done with it), then clear the rest.
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        glDepthRange(0.0, 0.0);
        gGL.setColorMask(false, false);
        for (const LLRect& r : mPanelKeep)
        {
            gl_rect_2d(r, LLColor4::white);
        }
        gGL.flush();
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_EQUAL);
        LLGLDisable blend(GL_BLEND);
        glDepthRange(1.0, 1.0);
        gGL.setColorMask(true, true);
        gl_rect_2d(0, height, width, 0, LLColor4(0.f, 0.f, 0.f, 0.f));
        gGL.flush();
    }
    glDepthRange(0.0, 1.0);
}

// Draws the on-screen keyboard into its own quad layer, in front of the panel.
void LLViewerVR::drawKeyboard()
{
    const S32 width = LLVRKeyboard::WIDTH, height = LLVRKeyboard::HEIGHT;
    if (!mBackend.beginUI(LLVRBackend::UI_KEYBOARD, width, height))
    {
        return;
    }
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glViewport(0, 0, width, height);
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        LLGLSUIDefault gls_ui;
        gl_state_for_2d(width, height);
        gGLCoverageAlpha = true;
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        gUIProgram.bind();
        mKeyboard.draw(mKeyboardHit);
        for (S32 hand = 0; hand < 2; ++hand)
        {
            if (mKeyboardHit[hand].part != LLVRKeyboard::NONE && mDragHand != hand)
            {
                const S32 x = mKeyboardX[hand], y = mKeyboardY[hand];
                gl_rect_2d(x - 6, y + 6, x + 6, y - 6, LLColor4::black);
                gl_rect_2d(x - 4, y + 4, x + 4, y - 4, LLColor4::white);
            }
        }
        gGL.flush();
        gUIProgram.unbind();
        gGLCoverageAlpha = false;
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
    gGL.setColorMask(true, false);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    mBackend.endUI(mKeyboardCentre, mKeyboardRot, mKeyboardWidth, mKeyboardWidth * height / width);
}

// A dot low in the field of view while messages came in with the panel hidden; drawn into
// the eye just rendered, fixed to the head so it is seen wherever one looks.
void LLViewerVR::drawMessageDot()
{
    float head[3], offset[3], right[3], up[3];
    const float AHEAD[3] = { 0.f, -0.3f, -1.f }, RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f };
    rotate(mEyes[0].rot, AHEAD, offset);
    rotate(mEyes[0].rot, RIGHT, right);
    rotate(mEyes[0].rot, UP, up);
    for (S32 i = 0; i < 3; ++i)
    {
        head[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f + offset[i];
    }
    const LLVector3 centre = mBaseOrigin + toAgent(trackingToLocal(head));
    const LLVector3 x_axis = toAgent(trackingToLocal(right)) * MESSAGE_DOT_RADIUS;
    const LLVector3 y_axis = toAgent(trackingToLocal(up)) * MESSAGE_DOT_RADIUS;
    const F32 glow = 0.7f + 0.3f * sinf((F32)LLFrameTimer::getElapsedSeconds() * 4.f);

    gViewerWindow->setup3DRender();
    LLGLSUIDefault gls_ui;
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    gGL.color4f(1.f, 0.55f, 0.1f, glow);
    const S32 STEPS = 24;
    for (S32 i = 0; i < STEPS; ++i)
    {
        const F32 a0 = F_TWO_PI * i / STEPS, a1 = F_TWO_PI * (i + 1) / STEPS;
        gGL.vertex3fv(centre.mV);
        gGL.vertex3fv((centre + x_axis * cosf(a0) + y_axis * sinf(a0)).mV);
        gGL.vertex3fv((centre + x_axis * cosf(a1) + y_axis * sinf(a1)).mV);
    }
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

void LLViewerVR::noteChat(const LLChat& chat)
{
    if (chat.mSourceType == CHAT_SOURCE_AGENT)
    {
        noteMessage(chat.mFromID);
    }
}

void LLViewerVR::noteMessage(const LLUUID& from)
{
    if (mBackend.isInitialized() && !mPanelVisible && from.notNull() && from != gAgentID)
    {
        mUnread = true;
    }
}

// Entering "chat only" with no conversation window open would leave nothing: open one.
void LLViewerVR::showChat()
{
    for (const char* name : CHAT_FLOATERS)
    {
        for (const LLFloater* floater : LLFloaterReg::getFloaterList(name))
        {
            if (floater->isShown())
            {
                return;
            }
        }
    }
    LLFloaterReg::showInstance("fs_im_container");
}

void LLViewerVR::onIdle(void* self)
{
    static_cast<LLViewerVR*>(self)->applyInput();
}

// Shows the on-screen keyboard while a text field has the keyboard focus. It first appears
// below and ahead of the head; after that it stays where it was put.
void LLViewerVR::updateKeyboard()
{
    LLUICtrl* focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    const bool shown = mPanelVisible && focus && focus->acceptsTextInput();
    if (!shown)
    {
        if (mKeyboardShown)
        {
            mKeyboard.reset();
        }
        mKeyboardShown = false;
        mDragHand = mRepeatHand = -1;
        return;
    }
    if (!mKeyboardPlaced)
    {
        mKeyboardPlaced = true;
        const float FORWARD[3] = { 0.f, 0.f, -1.f };
        float look[3], head[3], dir[3];
        rotate(mEyes[0].rot, FORWARD, look);
        F32 level = sqrtf(look[0] * look[0] + look[2] * look[2]);
        if (level < 0.1f) // looking straight up or down: put it ahead of the seat instead
        {
            look[0] = 0.f;
            look[2] = -1.f;
            level = 1.f;
        }
        for (S32 i = 0; i < 3; ++i)
        {
            head[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
        }
        mKeyboardWidth = KEYBOARD_WIDTH;
        mKeyboardCentre[0] = head[0] + look[0] / level * KEYBOARD_AHEAD;
        mKeyboardCentre[1] = head[1] - KEYBOARD_BELOW;
        mKeyboardCentre[2] = head[2] + look[2] / level * KEYBOARD_AHEAD;
        const LLVector3 to_keyboard(mKeyboardCentre[0] - head[0], mKeyboardCentre[1] - head[1], mKeyboardCentre[2] - head[2]);
        const F32 distance = to_keyboard.length();
        for (S32 i = 0; i < 3; ++i)
        {
            dir[i] = to_keyboard.mV[i] / distance;
        }
        facing(dir, mKeyboardRot);
    }
    mKeyboardShown = true;
    mKeyboard.update();
}

// Trigger pressed while a hand's laser is on the keyboard: the handle starts a drag, a
// key or suggestion is typed (backspace repeats while held).
void LLViewerVR::pressKeyboard(S32 hand)
{
    const LLVRKeyboard::Hit hit = mKeyboardHit[hand];
    if (hit.part == LLVRKeyboard::HANDLE)
    {
        mDragHand = hand;
        mDragDistance = hand ? mLeftLaserLength : mLaserLength;
    }
    else if (mKeyboard.press(hit))
    {
        mRepeatHand = hand;
        mRepeatHit = hit;
        mRepeatTimer.reset();
        mRepeatNext = KEY_REPEAT_DELAY;
    }
}

void LLViewerVR::releaseKeyboard(S32 hand)
{
    if (mDragHand == hand)
    {
        mDragHand = -1;
    }
    if (mRepeatHand == hand)
    {
        mRepeatHand = -1;
    }
}

// Turns the controller state into the viewer's own walking, turning and mouse input.
void LLViewerVR::applyInput()
{
    // No new state from the headset (the runtime skipped a frame): keep what is held, so a
    // drag survives it. When VR is shutting down, let go of everything.
    if (!mInputFresh && mBackend.isAlive() && gSavedSettings.getBOOL("VREnabled"))
    {
        return;
    }
    const LLVRInput input = mInputFresh ? mInput : LLVRInput();
    mInputFresh = false;

    // Mouselook switches off the UI and the tools that click on things, and steers with the
    // mouse. VR has its own first person view (see beginEye()), so stay out of mouselook.
    if (mBackend.isInitialized() && gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK)
    {
        gAgentCamera.changeCameraToThirdPerson(false);
    }
    if (mFirstPerson != mAttachmentsFirstPerson && isAgentAvatarValid())
    {
        // Hides or shows what is worn on the head; see the hook in updateAttachmentVisibility().
        mAttachmentsFirstPerson = mFirstPerson;
        gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
    }

    // The menu button steps through the whole panel, the chat alone, and nothing.
    if (input.menu && !mMenuDown)
    {
        if (!mPanelVisible)
        {
            mPanelVisible = true;
        }
        else if (!mChatOnly)
        {
            mChatOnly = true;
            showChat();
        }
        else
        {
            mPanelVisible = mChatOnly = false;
        }
    }
    mMenuDown = input.menu;

    // The left grip takes the panel only when held a moment: brushing it must not move it.
    if (!input.grab)
    {
        mGrabTimer.stop();
    }
    else if (!mGrabTimer.getStarted())
    {
        mGrabTimer.start();
    }
    mGrabLong = input.grab && mGrabTimer.getElapsedTimeF32() > GRAB_HOLD;
    if (mGrabLong && !mPanelVisible)
    {
        mPanelVisible = true; // updatePointer() then carries it on the laser
        mChatOnly = false;
    }
    if (mPanelVisible)
    {
        mUnread = false;
    }

    // Left stick walks and strafes, like the arrow keys. Walking brings a zoomed camera
    // back to the avatar.
    const bool walk = fabsf(input.move[1]) > STICK_ON, strafe = fabsf(input.move[0]) > STICK_ON;
    if ((walk || strafe) && mFirstPerson && isAgentAvatarValid() && !gAgentAvatarp->isSitting())
    {
        // Forward is where the head looks, not where the avatar happened to face: turn the
        // avatar there, and the tracking space back by as much so that the view stays put.
        const float forward[3] = { 0.f, 0.f, -1.f };
        float rotated[3];
        rotate(mEyes[0].rot, forward, rotated);
        LLVector3 look = toAgent(trackingToLocal(rotated));
        look.mV[VZ] = 0.f;
        if (look.normalize() > 0.3f) // looking straight up or down gives no heading
        {
            const F32 base = atan2f(mBaseYawSin, mBaseYawCos);
            gAgent.resetAxes(look);
            mYawOffset = base - atan2f(look.mV[VY], look.mV[VX]);
        }
    }
    if (walk)
    {
        gAgent.moveAt(input.move[1] > 0.f ? 1 : -1, false);
    }
    if (strafe)
    {
        gAgent.moveLeft(input.move[0] > 0.f ? -1 : 1);
    }
    if ((walk || strafe) && !gAgentCamera.getFocusOnAvatar())
    {
        gAgentCamera.resetView(true, false, false);
    }

    // Left trigger, while the left laser is on the on-screen keyboard, works it like the right.
    // Otherwise it is the Alt key of the mouse camera: pressing it while the laser is on an
    // object or avatar makes that the camera's focus, and while it is held the right stick
    // zooms and orbits. The view stays there (showing the own avatar) until the avatar walks.
    // shortcut: no zoom onto bare ground, the laser's world hit does not include terrain.
    updateKeyboard();
    if (input.zoom != mZoomDown)
    {
        mZoomDown = input.zoom;
        if (input.zoom)
        {
            mZoomOnKeyboard = mKeyboardHit[1].part != LLVRKeyboard::NONE;
        }
        if (mZoomOnKeyboard)
        {
            input.zoom ? pressKeyboard(1) : releaseKeyboard(1);
        }
        else if (input.zoom && mWorldHit && (mWorldHitPos - mBaseOrigin).length() > 0.5f)
        {
            // The camera turns to the new focus; turn the tracking space back by as much, so
            // that the view stays where the user was looking (it used to jump).
            const LLVector3 to_hit = mWorldHitPos - mBaseOrigin;
            mZoomYawOffset = fabsf(to_hit.mV[VX]) + fabsf(to_hit.mV[VY]) > 0.1f
                ? atan2f(mBaseYawSin, mBaseYawCos) - atan2f(to_hit.mV[VY], to_hit.mV[VX]) : 0.f;
            gAgentCamera.setFocusOnAvatar(false, false);
            gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(mBaseOrigin),
                                                    gAgent.getPosGlobalFromAgent(mWorldHitPos), LLUUID::null);
            gAgentCamera.setCameraAnimating(false); // no glide
        }
    }

    // Right stick: sideways turns in steps, one step per push; up and down is jump/fly up
    // and crouch/fly down like Page Up and Page Down, and held up it starts flying.
    const F32 x = input.turn[0], y = input.turn[1];
    const bool zooming = input.zoom && !mZoomOnKeyboard && !gAgentCamera.getFocusOnAvatar();
    const bool stick_free = !mGrabbing && !zooming && mDragHand < 0;
    if (zooming && !mGrabbing && fabsf(x) < STICK_OFF && mWorldHit)
    {
        // While held, the focus follows the laser: zoom in to the top of a tree after pressing
        // at its foot. Not while orbiting, where the laser sweeps past the focus. The camera
        // stays put but turns to the new focus; turning the tracking space back by as much
        // keeps the view (and the laser's hit) still, which would otherwise feed back.
        const LLVector3 focus = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusTargetGlobal());
        const LLVector3 to_old = focus - mBaseOrigin, to_new = mWorldHitPos - mBaseOrigin;
        auto level = [](const LLVector3& v) { return sqrtf(v.mV[VX] * v.mV[VX] + v.mV[VY] * v.mV[VY]); };
        if ((mWorldHitPos - focus).length() > ZOOM_RETARGET && to_new.length() > 0.5f
            && level(to_old) > 0.1f && level(to_new) > 0.1f) // straight above or below gives no heading
        {
            mZoomYawOffset += atan2f(to_old.mV[VY], to_old.mV[VX]) - atan2f(to_new.mV[VY], to_new.mV[VX]);
            gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(mBaseOrigin),
                                                    gAgent.getPosGlobalFromAgent(mWorldHitPos), LLUUID::null);
            gAgentCamera.setCameraAnimating(false);
        }
    }
    if (zooming && !mGrabbing)
    {
        const F32 step = ZOOM_RATE * llmin((F32)gFrameIntervalSeconds, 0.1f);
        if (fabsf(y) > STICK_OFF)
        {
            gAgentCamera.cameraZoomIn(expf(-y * step));
        }
        if (fabsf(x) > STICK_OFF)
        {
            gAgentCamera.cameraOrbitAround(-x * step);
        }
    }
    if (fabsf(x) < STICK_OFF)
    {
        mTurned = false;
    }
    else if (stick_free && !mTurned && fabsf(x) > STICK_ON && fabsf(x) >= fabsf(y))
    {
        mTurned = true;
        gAgent.yaw(x > 0.f ? -SNAP_TURN : SNAP_TURN);
    }
    const bool vertical = stick_free && fabsf(y) > STICK_ON && fabsf(y) > fabsf(x);
    if (vertical)
    {
        gAgent.moveUp(y > 0.f ? 1 : -1);
    }
    if (!vertical || y < 0.f)
    {
        mUpHeld = mFlyTried = false;
    }
    else if (!mUpHeld)
    {
        mUpHeld = true;
        mUpTimer.reset();
    }
    else if (!mFlyTried && mUpTimer.getElapsedTimeF32() > FLY_HOLD)
    {
        mFlyTried = true; // once per push: where flying is not allowed, each try complains
        if (!gAgent.getFlying())
        {
            gAgent.setFlying(true);
        }
    }

    // Trigger is the left mouse button, grip the right one. On the panel they go through
    // the UI like a mouse click at that spot; elsewhere straight to the current tool, whose
    // pick then follows the laser (overridePickRay). A release takes the route its press took.
    LLWindow* window = gViewerWindow->getWindow();
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    const LLCoordGL pointer(mPointerX, mPointerY);
    const bool on_keyboard = mKeyboardHit[0].part != LLVRKeyboard::NONE;
    const bool on_panel = mInput.aim_valid && mPanelHit;
    static S32 sLastX = -1, sLastY = -1;
    if (on_panel && (mPointerX != sLastX || mPointerY != sLastY))
    {
        sLastX = mPointerX;
        sLastY = mPointerY;
        gViewerWindow->handleMouseMove(window, pointer, mask);
    }

    const S32 mouse_x = gViewerWindow->getCurrentMouseX();
    const S32 mouse_y = gViewerWindow->getCurrentMouseY();
    if (input.trigger != mTriggerDown)
    {
        mTriggerDown = input.trigger;
        if (input.trigger)
        {
            mTriggerOnKeyboard = on_keyboard;
            mTriggerOnPanel = on_panel;
        }
        if (mTriggerOnKeyboard)
        {
            input.trigger ? pressKeyboard(0) : releaseKeyboard(0);
        }
        else if (!mTriggerOnPanel)
        {
            if (LLTool* tool = LLToolMgr::getInstance()->getCurrentTool())
            {
                input.trigger ? tool->handleMouseDown(mouse_x, mouse_y, mask) : tool->handleMouseUp(mouse_x, mouse_y, mask);
            }
        }
        else if (!input.trigger)
        {
            gViewerWindow->handleMouseUp(window, pointer, mask);
        }
        else if (mClickTimer.getStarted() && mClickTimer.getElapsedTimeF32() < DOUBLE_CLICK_TIME
                 && abs(mPointerX - mClickX) <= DOUBLE_CLICK_SLOP && abs(mPointerY - mClickY) <= DOUBLE_CLICK_SLOP)
        {
            // The second press of a double click, as the window system would report it. At
            // the first click's spot: a hand does not hold the laser still between the two.
            mClickTimer.stop();
            gViewerWindow->handleDoubleClick(window, LLCoordGL(mClickX, mClickY), mask);
        }
        else
        {
            mClickTimer.start();
            mClickX = mPointerX;
            mClickY = mPointerY;
            gViewerWindow->handleMouseDown(window, pointer, mask);
        }
    }
    if (input.grip != mGripDown)
    {
        mGripDown = input.grip;
        if (input.grip)
        {
            mGripOnKeyboard = on_keyboard; // nothing to click there
            mGripOnPanel = on_panel;
            mPanelVisible = true; // the context menu opens on the panel
        }
        if (mGripOnPanel)
        {
            input.grip ? gViewerWindow->handleRightMouseDown(window, pointer, mask)
                       : gViewerWindow->handleRightMouseUp(window, pointer, mask);
        }
        else if (LLTool* tool = mGripOnKeyboard ? nullptr : LLToolMgr::getInstance()->getCurrentTool())
        {
            input.grip ? tool->handleRightMouseDown(mouse_x, mouse_y, mask) : tool->handleRightMouseUp(mouse_x, mouse_y, mask);
        }
    }

    // Backspace held down repeats, as on a real keyboard.
    if (mRepeatHand >= 0 && mRepeatTimer.getElapsedTimeF32() > mRepeatNext)
    {
        mKeyboard.press(mRepeatHit);
        mRepeatNext += KEY_REPEAT_INTERVAL;
    }
}
