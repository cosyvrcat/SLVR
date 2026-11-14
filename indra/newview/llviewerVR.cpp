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
#include "llcallbacklist.h"
#include "llgl.h"
#include "llglstates.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

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

    // The UI panel: a rectangle fixed in tracking space, straight ahead of where the user
    // sat when VR started, facing them. Its height follows the window's aspect ratio.
    // shortcut: fixed place and size; make them settings (or grabbable) when that annoys.
    const float PANEL_CENTRE[3] = { 0.f, -0.1f, -1.5f };
    const F32 PANEL_WIDTH = 1.8f;

    const F32 SNAP_TURN = 30.f * DEG_TO_RAD;
    const F32 STICK_ON = 0.6f;       // a thumbstick counts as pushed beyond this
    const F32 STICK_OFF = 0.3f;      // and as let go below this
    const F32 LASER_LENGTH = 20.f;   // drawn length when it hits nothing
    const F32 PICK_DISTANCE = 512.f; // as far as the mouse picks

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
    // Toggled with Ctrl+Alt+F7, see LLViewerWindow::handleTranslatedKeyDown().
    static LLCachedControl<bool> enabled(gSavedSettings, "VREnabled", false);

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
        mPanelVisible = true;
        gIdleCallbacks.addFunction(onIdle, this);
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
    if (mBackend.isInitialized() && gViewerWindow)
    {
        gViewerWindow->getWindow()->toggleVSync(gSavedSettings.getBOOL("RenderVSyncEnable"));
        mInputFresh = false;
        applyInput(); // lets go of any button or stick still held
    }
    gIdleCallbacks.deleteFunction(onIdle, this);
    mLaserValid = mPanelHit = false;
    mBackend.shutdown();
}

void LLViewerVR::beginEye()
{
    if (!mSecondPass)
    {
        mEye = -1;
        mLaserValid = mPanelHit = false;
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
        const F32 yaw = atan2f(-mBaseLeft.mV[VX], mBaseLeft.mV[VY]);
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
        drawLaser();
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

// Works out where the right controller points: at the UI panel, or else into the world.
void LLViewerVR::updatePointer()
{
    if (!mInput.aim_valid)
    {
        return;
    }
    const float forward[3] = { 0.f, 0.f, -1.f };
    float dir[3];
    rotate(mInput.aim_rot, forward, dir);
    const float* pos = mInput.aim_pos;

    mLaserValid = true;
    mLaserStart = mBaseOrigin + toAgent(trackingToLocal(pos));
    mLaserDir = toAgent(trackingToLocal(dir));
    mLaserLength = LASER_LENGTH;

    // The panel lies in the plane z = PANEL_CENTRE[2] of tracking space. In mouselook the
    // mouse steers the view, so there is no pointer to put on the panel.
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (mPanelVisible && !gAgentCamera.cameraMouselook() && dir[2] < -0.01f
        && window.getWidth() > 0 && window.getHeight() > 0)
    {
        const F32 distance = (PANEL_CENTRE[2] - pos[2]) / dir[2];
        const F32 panel_height = PANEL_WIDTH * window.getHeight() / window.getWidth();
        const F32 u = (pos[0] + distance * dir[0] - PANEL_CENTRE[0]) / PANEL_WIDTH + 0.5f;
        const F32 v = (pos[1] + distance * dir[1] - PANEL_CENTRE[1]) / panel_height + 0.5f;
        if (distance > 0.f && u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
        {
            mPanelHit = true;
            mPointerX = llfloor(u * window.getWidth());
            mPointerY = llfloor(v * window.getHeight());
            mLaserLength = distance;
            return;
        }
    }

    LLVector4a start, end, hit;
    start.load3(mLaserStart.mV);
    end.load3((mLaserStart + mLaserDir * PICK_DISTANCE).mV);
    S32 face = -1;
    if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, nullptr, nullptr, &hit))
    {
        mLaserLength = (LLVector3(hit.getF32ptr()) - mLaserStart).length();
    }
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
void LLViewerVR::drawLaser()
{
    gViewerWindow->setup3DRender(); // this eye's matrices; render_ui() left its own behind
    LLGLSUIDefault gls_ui;
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);
    gGL.color3f(1.f, 0.25f, 0.2f);
    gGL.vertex3fv(mLaserStart.mV);
    gGL.vertex3fv((mLaserStart + mLaserDir * mLaserLength).mV);
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

// Draws the flat UI into the panel image instead of the window.
void LLViewerVR::drawPanel()
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (!mBackend.beginUI(window.getWidth(), window.getHeight()))
    {
        return;
    }
    LLGLSUIDefault gls_ui;
    gGL.setColorMask(true, true); // alpha is what lets the world show through the empty parts
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // Not render_ui_2d(): with RenderUIBuffer on it draws through a render target and then
    // returns to the window's framebuffer, leaving the panel image empty.
    // shortcut: translucent UI comes out too transparent (blending squares its alpha on
    // an empty target); needs a separate alpha blend factor the UI code does not reset.
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gViewerWindow->setup2DRender();
    gViewerWindow->draw();

    if (mPanelHit)
    {
        // The mouse cursor is not part of the UI image; mark where the laser lands.
        // The UI leaves a plain window-pixel coordinate system behind.
        const S32 x = mPointerX, y = mPointerY;
        gUIProgram.bind();
        gl_rect_2d(x - 5, y + 5, x + 5, y - 5, LLColor4::black);
        gl_rect_2d(x - 3, y + 3, x + 3, y - 3, LLColor4::white);
        gUIProgram.unbind();
    }
    gGL.flush();
    gGL.setColorMask(true, false);

    mBackend.endUI(PANEL_CENTRE, PANEL_WIDTH, PANEL_WIDTH * window.getHeight() / window.getWidth());
}

void LLViewerVR::onIdle(void* self)
{
    static_cast<LLViewerVR*>(self)->applyInput();
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

    if (input.menu && !mMenuDown)
    {
        mPanelVisible = !mPanelVisible;
    }
    mMenuDown = input.menu;

    // Left stick walks and strafes, like the arrow keys.
    if (fabsf(input.move[1]) > STICK_ON)
    {
        gAgent.moveAt(input.move[1] > 0.f ? 1 : -1, false);
    }
    if (fabsf(input.move[0]) > STICK_ON)
    {
        gAgent.moveLeft(input.move[0] > 0.f ? -1 : 1);
    }

    // Right stick turns in steps; one step per push.
    if (fabsf(input.turn[0]) < STICK_OFF)
    {
        mTurned = false;
    }
    else if (!mTurned && fabsf(input.turn[0]) > STICK_ON)
    {
        mTurned = true;
        gAgent.yaw(input.turn[0] > 0.f ? -SNAP_TURN : SNAP_TURN);
    }

    // Trigger is the left mouse button, grip the right one. On the panel they go through
    // the UI like a mouse click at that spot; elsewhere straight to the current tool, whose
    // pick then follows the laser (overridePickRay). A release takes the route its press took.
    LLWindow* window = gViewerWindow->getWindow();
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    const LLCoordGL pointer(mPointerX, mPointerY);
    const bool on_panel = mInput.aim_valid && mPanelHit;
    static S32 sLastX = -1, sLastY = -1;
    if (on_panel && (mPointerX != sLastX || mPointerY != sLastY))
    {
        sLastX = mPointerX;
        sLastY = mPointerY;
        gViewerWindow->handleMouseMove(window, pointer, mask);
    }

    const S32 x = gViewerWindow->getCurrentMouseX();
    const S32 y = gViewerWindow->getCurrentMouseY();
    if (input.trigger != mTriggerDown)
    {
        mTriggerDown = input.trigger;
        if (input.trigger)
        {
            mTriggerOnPanel = on_panel;
        }
        if (mTriggerOnPanel)
        {
            input.trigger ? gViewerWindow->handleMouseDown(window, pointer, mask)
                          : gViewerWindow->handleMouseUp(window, pointer, mask);
        }
        else if (LLTool* tool = LLToolMgr::getInstance()->getCurrentTool())
        {
            input.trigger ? tool->handleMouseDown(x, y, mask) : tool->handleMouseUp(x, y, mask);
        }
    }
    if (input.grip != mGripDown)
    {
        mGripDown = input.grip;
        if (input.grip)
        {
            mGripOnPanel = on_panel;
            mPanelVisible = true; // the context menu opens on the panel
        }
        if (mGripOnPanel)
        {
            input.grip ? gViewerWindow->handleRightMouseDown(window, pointer, mask)
                       : gViewerWindow->handleRightMouseUp(window, pointer, mask);
        }
        else if (LLTool* tool = LLToolMgr::getInstance()->getCurrentTool())
        {
            input.grip ? tool->handleRightMouseDown(x, y, mask) : tool->handleRightMouseUp(x, y, mask);
        }
    }
}
