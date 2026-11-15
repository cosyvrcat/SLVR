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
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llgl.h"
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
#include "llviewerobject.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "pipeline.h"

extern bool gCubeSnapshot;
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

    // On-screen keyboard: four rows of characters and a row of wide keys, KEY_COLUMNS wide.
    // UTF-8 as byte escapes so the source file's encoding cannot matter.
    // shortcut: one fixed (Turkish Q) layout, Shift stays on until pressed again, no key
    // repeat; make the layout a setting when another language needs typing.
    const S32 KEY_COLUMNS = 12;
    const S32 KEY_ROWS = 5;
    const char* const KEY_CHARS[2][4] = {
        { "1234567890'-", "qwertyu\xC4\xB1" "op\xC4\x9F\xC3\xBC", "asdfghjkl\xC5\x9F" "i,", "zxcvbnm\xC3\xB6\xC3\xA7" ".:?" },
        { "!\"#$%&/()=*_", "QWERTYUIOP\xC4\x9E\xC3\x9C", "ASDFGHJKL\xC5\x9E\xC4\xB0" ";", "ZXCVBNM\xC3\x96\xC3\x87" "@+<" },
    };
    const llwchar OSK_SHIFT = 1, OSK_BACKSPACE = 2, OSK_ENTER = 3, OSK_CLOSE = 4; // below any character
    struct WideKey { const char* label; S32 columns; llwchar code; };
    const WideKey WIDE_KEYS[] = {
        { "Shift", 2, OSK_SHIFT }, { "Space", 4, ' ' }, { "<--", 2, OSK_BACKSPACE }, { "Enter", 2, OSK_ENTER }, { "Close", 2, OSK_CLOSE },
    };

    // The key at a column and row (0 = top) of the keyboard; 0 where there is none.
    llwchar keyAt(S32 column, S32 row, bool shift)
    {
        if (column < 0 || column >= KEY_COLUMNS)
        {
            return 0;
        }
        if (row >= 0 && row < KEY_ROWS - 1)
        {
            return utf8str_to_wstring(KEY_CHARS[shift ? 1 : 0][row])[column];
        }
        if (row == KEY_ROWS - 1)
        {
            S32 first = 0;
            for (const WideKey& key : WIDE_KEYS)
            {
                if (column < first + key.columns)
                {
                    return key.code;
                }
                first += key.columns;
            }
        }
        return 0;
    }

    const F32 SNAP_TURN = 30.f * DEG_TO_RAD;
    const F32 STICK_ON = 0.6f;       // a thumbstick counts as pushed beyond this
    const F32 STICK_OFF = 0.3f;      // and as let go below this
    const F32 LASER_LENGTH = 20.f;   // drawn length when it hits nothing
    const F32 PICK_DISTANCE = 512.f; // as far as the mouse picks
    const F32 FLY_HOLD = 0.5f;       // seconds the right stick is held up before flying starts
    const F32 ZOOM_RATE = 1.5f;      // zoom e-foldings, and orbit radians, per second at full stick
    const F32 DOUBLE_CLICK_TIME = 0.5f; // seconds
    const S32 DOUBLE_CLICK_SLOP = 40;   // pixels the laser may wander between the two clicks

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

        // Every keyboard row is full, and the wide keys fill the last one.
        for (S32 row = 0; row < KEY_ROWS - 1; ++row)
        {
            llassert_always(utf8str_to_wstring(KEY_CHARS[0][row]).size() == (size_t)KEY_COLUMNS);
            llassert_always(utf8str_to_wstring(KEY_CHARS[1][row]).size() == (size_t)KEY_COLUMNS);
        }
        llassert_always(keyAt(0, 1, false) == 'q' && keyAt(0, 1, true) == 'Q' && keyAt(7, 1, false) == 0x131);
        llassert_always(keyAt(0, KEY_ROWS - 1, false) == OSK_SHIFT && keyAt(2, KEY_ROWS - 1, false) == ' ');
        llassert_always(keyAt(KEY_COLUMNS - 1, KEY_ROWS - 1, false) == OSK_CLOSE && keyAt(KEY_COLUMNS, 0, false) == 0);
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
    const bool was_running = mBackend.isInitialized();
    mBackend.shutdown();
    mLaserValid = mPanelHit = mWorldHit = mFirstPerson = false;
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
        mLaserValid = mPanelHit = mWorldHit = mFirstPerson = false;
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
// While the left grip is held the panel is carried along on the laser instead.
void LLViewerVR::updatePointer()
{
    if (!mInput.aim_valid)
    {
        mGrabbing = false;
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

    if (mInput.grab && mPanelVisible)
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
        return;
    }
    mGrabbing = false;

    // Into the panel's own frame (x right, y up, z out of its face): it lies in z = 0.
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (mPanelVisible && window.getWidth() > 0 && window.getHeight() > 0)
    {
        const float inverse[4] = { -mPanelRot[0], -mPanelRot[1], -mPanelRot[2], mPanelRot[3] };
        const float offset[3] = { pos[0] - mPanelCentre[0], pos[1] - mPanelCentre[1], pos[2] - mPanelCentre[2] };
        float local_pos[3], local_dir[3];
        rotate(inverse, offset, local_pos);
        rotate(inverse, dir, local_dir);
        if (local_dir[2] < -0.01f)
        {
            const F32 distance = -local_pos[2] / local_dir[2];
            const F32 panel_height = mPanelWidth * window.getHeight() / window.getWidth();
            const F32 u = (local_pos[0] + distance * local_dir[0]) / mPanelWidth + 0.5f;
            const F32 v = (local_pos[1] + distance * local_dir[1]) / panel_height + 0.5f;
            if (distance > 0.f && u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
            {
                mPanelHit = true;
                mPointerX = llfloor(u * window.getWidth());
                mPointerY = llfloor(v * window.getHeight());
                mLaserLength = distance;
                return;
            }
        }
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

// Draws the HUD attachments and the flat UI into the panel image instead of the window.
void LLViewerVR::drawPanel()
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    const S32 width = window.getWidth(), height = window.getHeight();
    if (!mBackend.beginUI(width, height))
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
        // shader wrote; fix together with the UI's alpha below.
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
    // shortcut: translucent UI comes out too transparent (blending squares its alpha on
    // an empty target); needs a separate alpha blend factor the UI code does not reset.
    gViewerWindow->draw();

    // The UI leaves a plain window-pixel coordinate system behind.
    gUIProgram.bind();
    if (mKeyboardShown)
    {
        const S32 size = mKeySize, gap = llmax(1, size / 16);
        gl_rect_2d(mKeyboardLeft - gap, mKeyboardTop + gap, mKeyboardLeft + KEY_COLUMNS * size + gap,
                   mKeyboardTop - KEY_ROWS * size - gap, LLColor4(0.08f, 0.08f, 0.1f, 1.f));
        const LLFontGL* font = LLFontGL::getFontSansSerifHuge();
        for (S32 row = 0; row < KEY_ROWS; ++row)
        {
            for (S32 column = 0; column < KEY_COLUMNS; )
            {
                const llwchar code = keyAt(column, row, mShift);
                S32 columns = 1;
                LLWString label(1, code);
                if (row == KEY_ROWS - 1)
                {
                    S32 first = 0;
                    for (const WideKey& key : WIDE_KEYS)
                    {
                        if (first == column)
                        {
                            columns = key.columns;
                            label = utf8str_to_wstring(key.label);
                        }
                        first += key.columns;
                    }
                }
                const S32 left = mKeyboardLeft + column * size, top = mKeyboardTop - row * size;
                const bool hover = row == mHoverRow && mHoverColumn >= column && mHoverColumn < column + columns;
                const bool lit = hover || (code == OSK_SHIFT && mShift);
                gl_rect_2d(left + gap, top - gap, left + columns * size - gap, top - size + gap,
                           lit ? LLColor4(0.2f, 0.45f, 0.8f, 1.f) : LLColor4(0.22f, 0.22f, 0.26f, 1.f));
                if (font)
                {
                    // Fonts take UI units, which the UI scale turns into pixels.
                    font->render(label, 0, (left + columns * size / 2) / LLFontGL::sScaleX, (top - size / 2) / LLFontGL::sScaleY,
                                 LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER);
                }
                column += columns;
            }
        }
    }
    if (mPanelHit)
    {
        // The mouse cursor is not part of the UI image; mark where the laser lands.
        const S32 x = mPointerX, y = mPointerY;
        gl_rect_2d(x - 5, y + 5, x + 5, y - 5, LLColor4::black);
        gl_rect_2d(x - 3, y + 3, x + 3, y - 3, LLColor4::white);
    }
    gGL.flush();
    gUIProgram.unbind();
    gGL.setColorMask(true, false);

    mBackend.endUI(mPanelCentre, mPanelRot, mPanelWidth, mPanelWidth * height / width);
}

void LLViewerVR::onIdle(void* self)
{
    static_cast<LLViewerVR*>(self)->applyInput();
}

// Shows the on-screen keyboard while a text field has the keyboard focus, away from that
// field: along the bottom of the panel, or along the top when the field is in the lower half.
void LLViewerVR::updateKeyboard()
{
    LLUICtrl* focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    const LLRect window = gViewerWindow->getWindowRectRaw();
    mKeySize = llmin(window.getHeight() / 14, window.getWidth() / KEY_COLUMNS);
    mKeyboardShown = mPanelVisible && focus && focus->acceptsTextInput() && mKeySize >= 8;
    mHoverColumn = mHoverRow = -1;
    if (!mKeyboardShown)
    {
        return;
    }
    const F32 focus_y = focus->calcScreenRect().getCenterY() * gViewerWindow->getDisplayScale().mV[VY];
    const S32 margin = mKeySize / 2;
    mKeyboardLeft = (window.getWidth() - KEY_COLUMNS * mKeySize) / 2;
    mKeyboardTop = focus_y < window.getHeight() / 2 ? window.getHeight() - margin : margin + KEY_ROWS * mKeySize;

    const S32 x = mPointerX - mKeyboardLeft, y = mKeyboardTop - 1 - mPointerY;
    if (mInput.aim_valid && mPanelHit && x >= 0 && x < KEY_COLUMNS * mKeySize && y >= 0 && y < KEY_ROWS * mKeySize)
    {
        mHoverColumn = x / mKeySize;
        mHoverRow = y / mKeySize;
    }
}

void LLViewerVR::pressKey(llwchar code)
{
    if (code == OSK_SHIFT)
    {
        mShift = !mShift;
    }
    else if (code == OSK_BACKSPACE)
    {
        gViewerWindow->handleKey(KEY_BACKSPACE, MASK_NONE);
    }
    else if (code == OSK_ENTER)
    {
        gViewerWindow->handleKey(KEY_RETURN, MASK_NONE);
    }
    else if (code == OSK_CLOSE)
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
    else if (code)
    {
        gViewerWindow->handleUnicodeChar(code, MASK_NONE);
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

    if (input.menu && !mMenuDown)
    {
        mPanelVisible = !mPanelVisible;
    }
    mMenuDown = input.menu;
    if (input.grab)
    {
        mPanelVisible = true; // updatePointer() then carries it on the laser
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

    // Left trigger is the Alt key of the mouse camera: pressing it while the laser is on an
    // object or avatar makes that the camera's focus, and while it is held the right stick
    // zooms and orbits. The view stays there (showing the own avatar) until the avatar walks.
    // shortcut: no zoom onto bare ground, the laser's world hit does not include terrain.
    if (input.zoom && !mZoomDown && mWorldHit && (mWorldHitPos - mBaseOrigin).length() > 0.5f)
    {
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(mBaseOrigin),
                                                gAgent.getPosGlobalFromAgent(mWorldHitPos), LLUUID::null);
        gAgentCamera.setCameraAnimating(false); // no glide: the view just turns to the new focus
    }
    mZoomDown = input.zoom;

    // Right stick: sideways turns in steps, one step per push; up and down is jump/fly up
    // and crouch/fly down like Page Up and Page Down, and held up it starts flying.
    const F32 x = input.turn[0], y = input.turn[1];
    const bool zooming = input.zoom && !gAgentCamera.getFocusOnAvatar();
    const bool stick_free = !input.grab && !zooming;
    if (zooming && !input.grab)
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
    updateKeyboard();
    LLWindow* window = gViewerWindow->getWindow();
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    const LLCoordGL pointer(mPointerX, mPointerY);
    const bool on_keyboard = mHoverRow >= 0;
    const bool on_panel = mInput.aim_valid && mPanelHit && !on_keyboard;
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
            if (input.trigger)
            {
                pressKey(keyAt(mHoverColumn, mHoverRow, mShift));
            }
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
            mGripOnPanel = mInput.aim_valid && mPanelHit; // over the keyboard too: nothing behind it to click
            mPanelVisible = true; // the context menu opens on the panel
        }
        if (mGripOnPanel)
        {
            input.grip ? gViewerWindow->handleRightMouseDown(window, pointer, mask)
                       : gViewerWindow->handleRightMouseUp(window, pointer, mask);
        }
        else if (LLTool* tool = LLToolMgr::getInstance()->getCurrentTool())
        {
            input.grip ? tool->handleRightMouseDown(mouse_x, mouse_y, mask) : tool->handleRightMouseUp(mouse_x, mouse_y, mask);
        }
    }
}
