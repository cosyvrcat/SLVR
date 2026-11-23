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
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llchat.h"
#include "lldraghandle.h"
#include "llfocusmgr.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llimagepng.h"
#include "llimview.h"
#include "llglstates.h"
#include "llkeyboard.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llrootview.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llspatialpartition.h"
#include "lltoast.h"
#include "lltoastalertpanel.h"
#include "lltoastnotifypanel.h"
#include "lltoastscriptquestion.h"
#include "llpanelgenerictip.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoiceclient.h"
#include "llvrlanguage.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "llworld.h"
#include "piemenu.h"
#include "pipeline.h"
#include "rlvactions.h"

#include "llfile.h"
#include "llwin32headers.h"

#include <ctime>
#include <memory>
#include <shlobj.h>

extern bool gCubeSnapshot;
extern bool gGLCoverageAlpha; // llrender.cpp
void render_hud_attachments(); // llviewerdisplay.cpp

LLViewerVR gVR;

namespace
{
    // A controller's model, made from Meta's controller art by
    // scripts/slvr/convert_controller_models.py into the user settings folder (the models are
    // Meta's and not part of SLVR). Metres in the frame of the aim pose (x right, y up, -z
    // along the laser), triangles counter-clockwise seen from outside. Without one, a box.
    struct ControllerModel
    {
        std::vector<LLVector3> positions, normals;
        std::vector<LLVector2> uvs;
        std::vector<U32> indices;
        LLPointer<LLViewerTexture> texture; // grey without it
    };
    // By the physical hand, right and left; loaded on first use, freed as VR shuts down (not
    // at exit, when GL is gone). nullptr until tried; empty when there is none.
    ControllerModel* sControllerModels[2] = { nullptr, nullptr };
    const char* const CONTROLLER_SIDES[2] = { "right", "left" };

    ControllerModel* loadControllerModel(S32 side)
    {
        ControllerModel* model = new ControllerModel;
        const std::string name = CONTROLLER_SIDES[side];
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vr_controllers", name + ".mesh");
        llifstream file(path.c_str());
        if (!file.is_open())
        {
            LL_INFOS("VR") << "No controller model " << path << ", drawing boxes" << LL_ENDL;
            return model;
        }
        std::string magic, word;
        S32 version = 0, vertex_count = 0, triangle_count = 0;
        bool ok = (file >> magic >> version >> word >> vertex_count) && magic == "slvr_controller" && version == 1
                  && word == "vertices" && vertex_count > 0 && vertex_count < 200000;
        for (S32 i = 0; ok && i < vertex_count; ++i)
        {
            LLVector3 position, normal;
            LLVector2 uv;
            ok = (bool)(file >> position.mV[VX] >> position.mV[VY] >> position.mV[VZ] >> normal.mV[VX] >> normal.mV[VY]
                             >> normal.mV[VZ] >> uv.mV[VX] >> uv.mV[VY]);
            model->positions.push_back(position);
            model->normals.push_back(normal);
            model->uvs.push_back(uv);
        }
        ok = ok && (file >> word >> triangle_count) && word == "triangles" && triangle_count > 0 && triangle_count < 400000;
        for (S32 i = 0; ok && i < 3 * triangle_count; ++i)
        {
            U32 index = 0;
            ok = (file >> index) && index < (U32)vertex_count;
            model->indices.push_back(index);
        }
        if (!ok)
        {
            LL_WARNS("VR") << "Controller model " << path << " is damaged; convert it again. Drawing boxes" << LL_ENDL;
            *model = ControllerModel();
            return model;
        }
        LLPointer<LLImagePNG> png = new LLImagePNG;
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (png->load(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vr_controllers", name + ".png")) && png->decode(raw, 0.f))
        {
            if (raw->getWidth() > 1024 || raw->getHeight() > 1024)
            {
                raw->scale(llmin(raw->getWidth(), 1024), llmin(raw->getHeight(), 1024));
            }
            model->texture = LLViewerTextureManager::getLocalTexture(raw.get(), true);
        }
        LL_INFOS("VR") << "Controller model " << name << ": " << vertex_count << " vertices, " << triangle_count
                       << " triangles, " << (model->texture ? "textured" : "no texture") << LL_ENDL;
        return model;
    }

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

    // Puts a quad `distance` metres along `look` (a unit vector) from the eyes (tracking
    // space), square on to it, with the point `dx`, `dy` metres from its middle (in its own
    // plane, right and up) on that line.
    void placeAlong(const float eyes[3], const float look[3], F32 distance, F32 dx, F32 dy, float centre[3], float rot[4])
    {
        const float RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f };
        float right[3], up[3];
        facing(look, rot);
        rotate(rot, RIGHT, right);
        rotate(rot, UP, up);
        for (S32 i = 0; i < 3; ++i)
        {
            centre[i] = eyes[i] + look[i] * distance - right[i] * dx - up[i] * dy;
        }
    }

    // The same along the line of sight.
    void placeAhead(const float eyes[3], const float eye_rot[4], F32 distance, F32 dx, F32 dy, float centre[3], float rot[4])
    {
        const float FORWARD[3] = { 0.f, 0.f, -1.f };
        float look[3];
        rotate(eye_rot, FORWARD, look);
        placeAlong(eyes, look, distance, dx, dy, centre, rot);
    }

    // Heading and pitch of a direction in tracking space (not necessarily a unit vector), and back.
    F32 headingOf(const float v[3]) { return atan2f(-v[0], -v[2]); }
    F32 pitchOf(const float v[3]) { return atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2])); }
    void direction(F32 heading, F32 pitch, float out[3])
    {
        out[0] = -sinf(heading) * cosf(pitch);
        out[1] = sinf(pitch);
        out[2] = -cosf(heading) * cosf(pitch);
    }

    // The on-screen keyboard comes up low in front, where seated hands type without lifting
    // the arms: straight ahead of the face (its heading only), this far below the eyes'
    // level, square on to the eyes, so it leans back like a laptop's keyboard.
    const F32 KEYBOARD_BELOW = 40.f * DEG_TO_RAD;

    // A notice that shows over a hidden panel or the chat alone, asked for or not: an alert or
    // tip from the grid (a teleport refused as too short, being ejected) or a question from an
    // object (an attach request). Not a chat or IM bubble, a friend coming online, a group notice.
    bool isNotice(const LLNotificationsUI::LLToast* toast)
    {
        const LLPanel* panel = toast->getPanel();
        return dynamic_cast<const LLToastAlertPanel*>(panel) || dynamic_cast<const LLToastNotifyPanel*>(panel)
               || dynamic_cast<const LLToastScriptQuestion*>(panel) || dynamic_cast<const LLPanelGenericTip*>(panel);
    }

    // What a press just sent to the UI landed on, told by what took the mouse: a field that
    // takes text, a window's title bar, or something else. Not by the views at the point:
    // there, full-window views that let the mouse through are in front of the windows, and
    // no press was ever found on a text field or a title bar (v0.6.2: no keyboard at all).
    enum class Pressed { TEXT, TITLE, OTHER };
    Pressed pressedOn(LLView** title)
    {
        LLMouseHandler* captor = gFocusMgr.getMouseCapture();
        if (LLDragHandle* handle = dynamic_cast<LLDragHandle*>(captor))
        {
            *title = handle;
            return Pressed::TITLE;
        }
        const LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(captor);
        return ctrl && ctrl->acceptsTextInput() ? Pressed::TEXT : Pressed::OTHER;
    }

    // While the left grip holds the panel, the right stick moves it along the laser and
    // resizes it, within these limits (metres).
    // shortcut: the panel's place and size are forgotten when the viewer closes; make them
    // settings when setting them up every session annoys.
    const F32 PANEL_NEAR = 0.3f, PANEL_FAR = 5.f;
    const F32 PANEL_NARROW = 0.4f, PANEL_WIDE = 4.f;
    const F32 PANEL_RATE = 1.5f; // e-foldings per second at full stick

    // The on-screen keyboard's quad: limits of its width in metres. Its width and how far
    // ahead of the eyes it appears (see updateKeyboard()) are kept in settings.
    const F32 KEYBOARD_NARROW = 0.25f, KEYBOARD_WIDE = 1.5f;
    const F32 KEY_REPEAT_DELAY = 0.5f, KEY_REPEAT_INTERVAL = 0.07f; // seconds, for backspace held down

    // What "chat only" keeps of the panel: Firestorm's conversation windows, docked or torn off.
    const char* const CHAT_FLOATERS[] = { "fs_im_container", "fs_nearby_chat", "fs_impanel" };
    bool isChatFloater(LLFloater* floater)
    {
        return std::any_of(std::begin(CHAT_FLOATERS), std::end(CHAT_FLOATERS),
                           [&](const char* name) { return floater->getInstanceName() == name; });
    }
    // Seconds after a press (on the panel or on something in the world) in which a window
    // that opens counts as asked for, see updatePopped(). A script dialog comes back from
    // the region a moment after the touch.
    const F32 POP_AFTER_PRESS = 5.f;
    const F32 MESSAGE_DOT_RADIUS = 0.012f; // metres, a metre ahead

    const F32 SMOOTH_TURN_FASTEST = 240.f * DEG_TO_RAD; // the fastest SLVRSmoothTurnSpeed, radians per second
    const F32 STICK_ON = 0.6f;       // a thumbstick counts as pushed beyond this
    // The left stick walks pushed past WALK_ON, and runs held all the way for RUN_HOLD seconds:
    // at 0.9 without a wait a slight push already ran.
    const F32 WALK_ON = 0.4f, RUN_ON = 0.95f, RUN_HOLD = 0.3f;
    const F32 RECENTER_HOLD = 0.8f;  // seconds X is held to recentre
    const F32 BLINK_TIME = 0.2f;     // seconds the view takes to come back from black after a jump
    const F32 TICK_STRENGTH = 0.12f, TICK_TIME = 0.012f;   // vibration as a laser moves onto a key or a button
    const F32 PRESS_STRENGTH = 0.35f, PRESS_TIME = 0.025f; // and as one is pressed
    const F32 EYES_BELOW_TOP = 0.11f; // metres from the eyes to the top of the head: height = eye height + this
    // Teleport arc: thrown from the hand along the laser, falling like a ball, in steps.
    // Some 27 metres on level ground (2.5 times the first arc's): many places send a short
    // teleport to their landing point.
    const F32 ARC_SPEED = 15.8f, ARC_GRAVITY = 9.8f; // m/s, m/s^2
    const F32 ARC_STEP = 0.05f;   // seconds of flight per step
    const S32 ARC_STEPS = 80;
    const F32 ARC_FLAT = 0.7f;    // a surface is stood on when its normal is this close to straight up
    const F32 ARC_RING = 0.35f;   // metres, the landing ring
    // Wrist menu: an image of 2 x 3 buttons over the other hand, while its face is turned to the eyes.
    const S32 WRIST_WIDTH = 480, WRIST_HEIGHT = 360; // image pixels
    const F32 WRIST_SIZE = 0.15f;                     // metres wide
    const F32 WRIST_SEEN = 35.f * DEG_TO_RAD, WRIST_KEPT = 50.f * DEG_TO_RAD; // off the line of sight to show it, and to keep it
    const F32 WRIST_FACING = 0.55f, WRIST_STILL_FACING = 0.25f; // how squarely its face is turned to the eyes, the same two
    const F32 WRIST_REACH = 0.8f;                     // metres from the eyes
    const S32 WRIST_ITEMS = 6;
    const char* const WRIST_TEXTS[WRIST_ITEMS] = { "wrist_panel", "wrist_chat", "wrist_settings", "wrist_screenshot", "wrist_recenter", "wrist_fly" };
    LLRect wristItemRect(S32 item) // image pixels, two columns, from the top
    {
        const S32 w = WRIST_WIDTH / 2, h = WRIST_HEIGHT / 3, col = item % 2, row = item / 2;
        return LLRect(col * w + 8, WRIST_HEIGHT - row * h - 8, (col + 1) * w - 8, WRIST_HEIGHT - (row + 1) * h + 8);
    }
    // SLVRPanelFollow: the panel glides back once the eyes are this far off it, until it is this near.
    const F32 PANEL_FOLLOW_AWAY = 40.f * DEG_TO_RAD, PANEL_FOLLOW_NEAR = 3.f * DEG_TO_RAD;
    const F32 PANEL_FOLLOW_RATE = 1.f; // Hz

    F32 snapTurn()
    {
        static LLCachedControl<S32> degrees(gSavedSettings, "SLVRSnapAngle", 30);
        return llclamp((S32)degrees, 5, 180) * DEG_TO_RAD;
    }

    F32 smoothTurnRate() // at full stick
    {
        static LLCachedControl<F32> degrees(gSavedSettings, "SLVRSmoothTurnSpeed", 120.f);
        return llclamp((F32)degrees * DEG_TO_RAD, 10.f * DEG_TO_RAD, SMOOTH_TURN_FASTEST);
    }

    // SLVRLeftHanded: the left hand points and clicks, the right one walks; the menu button,
    // which only the left controller has, stays.
    void swapHands(LLVRInput& in)
    {
        std::swap(in.move, in.turn);
        std::swap(in.trigger, in.zoom);
        std::swap(in.grip, in.grab);
        std::swap(in.teleport, in.recenter);
        std::swap(in.talk, in.shot);
        std::swap(in.aim_valid, in.left_aim_valid);
        std::swap(in.aim_pos, in.left_aim_pos);
        std::swap(in.aim_rot, in.left_aim_rot);
    }

    // The button or menu item of the UI under a point (window pixels), for the hover tick.
    const LLView* controlAt(S32 x, S32 y)
    {
        const LLVector2 scale = gViewerWindow->getDisplayScale();
        LLView* root = gViewerWindow->getRootView();
        for (LLView* view = root->childFromPoint(ll_round(x / scale.mV[VX]), ll_round(y / scale.mV[VY]), true);
             view && view != root; view = view->getParent())
        {
            if (dynamic_cast<LLButton*>(view) || dynamic_cast<LLMenuItemGL*>(view))
            {
                return view->getEnabled() ? view : nullptr;
            }
        }
        return nullptr;
    }
    const F32 STICK_OFF = 0.3f;      // and as let go below this
    const F32 LASER_LENGTH = 20.f;   // drawn length when it hits nothing
    const F32 LASER_MIN_OPACITY = 0.1f; // SLVRLaserOpacity never hides the laser altogether
    const F32 PICK_DISTANCE = 512.f; // as far as the mouse picks
    const F32 FLY_HOLD = 0.5f;       // seconds the right stick is held up before flying starts
    const F32 ZOOM_RATE = 1.5f;      // zoom e-foldings, and orbit radians, per second at full stick
    const F32 DOUBLE_CLICK_TIME = 0.5f; // seconds
    const S32 DOUBLE_CLICK_SLOP = 40;   // pixels the laser may wander between the two clicks
    const F32 ZOOM_RETARGET = 0.1f;     // metres the laser's hit moves before the zoom focus follows it
    const F32 ZOOM_HOLD = 0.35f;        // seconds the left trigger is held before the zoom starts; a press alone does nothing
    const F32 ZOOM_BACK = 2.2f, ZOOM_UP = 0.6f; // metres the camera steps back and up from the eyes as the zoom starts
    const F32 ZOOM_FREE_DISTANCE = 10.f; // metres along the laser for the focus when it hits no object
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

    // Comfort vignette, SLVRVignette 1 to 3 (low to high): the angle off the line of sight
    // where it starts to darken, and where it is black. As in Meta's comfort vignette, moving
    // and turning both close it in (but not a snap turn, which has no motion to feel), within
    // VIGNETTE_IN seconds. It stays closed while the motion goes on, through the pauses
    // between stick pushes and the frames in which the avatar's position does not move (it
    // opened a little in each and read as blinking), and opens again within VIGNETTE_OUT
    // once the view has been still for VIGNETTE_HOLD.
    const F32 VIGNETTE_CLEAR[3] = { 50.f, 38.f, 26.f }, VIGNETTE_BLACK[3] = { 70.f, 58.f, 45.f }; // degrees
    const F32 VIGNETTE_IN = 0.25f, VIGNETTE_OUT = 0.5f, VIGNETTE_HOLD = 0.4f;
    // Rising and sinking count for this much of their speed: flying up or down closed the
    // vignette almost over the whole view.
    const F32 VIGNETTE_VERTICAL = 0.35f;
    // In one frame: the view was sent somewhere (a teleport, a region crossing), not moved.
    const F32 VIGNETTE_JUMP = 1.5f;      // metres
    const F32 VIGNETTE_TURN_JUMP = 1.f;  // radians; snap turns are told apart by mSnapped

    // How far the vignette closes in while the view moves at `level_speed` and
    // `vertical_speed` (m/s) and turns at `turn_rate` (rad/s): fully from walking pace, or
    // from 60 degrees a second.
    F32 vignetteTarget(F32 level_speed, F32 vertical_speed, F32 turn_rate)
    {
        const F32 speed = sqrtf(level_speed * level_speed + powf(vertical_speed * VIGNETTE_VERTICAL, 2.f));
        const F32 moving = (speed - 0.5f) / 2.5f;
        const F32 turning = (fabsf(turn_rate) - 10.f * DEG_TO_RAD) / (50.f * DEG_TO_RAD);
        return llclamp(llmax(moving, turning), 0.f, 1.f);
    }

    // The strongest of `target` over the motion going on: it comes down only once it has been
    // below it for VIGNETTE_HOLD seconds. `held` and `since` are kept between frames.
    F32 holdPeak(F32 target, F32& held, F32& since, F32 dt)
    {
        since += dt;
        if (target >= held || since > VIGNETTE_HOLD)
        {
            held = target;
            since = 0.f;
        }
        return held;
    }

    // Change of heading from `from` to `to`, the short way round.
    F32 headingChange(F32 from, F32 to)
    {
        F32 change = fmodf(to - from, F_TWO_PI);
        if (change > F_PI)
        {
            change -= F_TWO_PI;
        }
        else if (change < -F_PI)
        {
            change += F_TWO_PI;
        }
        return change;
    }

    void checkVignette()
    {
        llassert_always(vignetteTarget(0.f, 0.f, 0.f) == 0.f);
        llassert_always(vignetteTarget(0.3f, 0.f, 5.f * DEG_TO_RAD) == 0.f); // standing: an avatar's sway is no motion
        llassert_always(vignetteTarget(3.2f, 0.f, 0.f) == 1.f);              // walking
        llassert_always(vignetteTarget(0.f, 0.f, -120.f * DEG_TO_RAD) == 1.f); // smooth turning at full stick
        llassert_always(vignetteTarget(0.f, 4.f, 0.f) < 0.4f);               // rising, as flying up does
        F32 held = 0.f, since = 0.f;
        llassert_always(holdPeak(1.f, held, since, 0.03f) == 1.f);
        llassert_always(holdPeak(0.f, held, since, 0.2f) == 1.f);  // a pause between pushes
        llassert_always(holdPeak(0.f, held, since, 0.15f) == 1.f);
        llassert_always(holdPeak(0.f, held, since, 0.1f) == 0.f);  // still for longer than VIGNETTE_HOLD
        llassert_always(holdPeak(0.6f, held, since, 0.03f) == 0.6f);
        llassert_always(fabsf(headingChange(179.f * DEG_TO_RAD, -179.f * DEG_TO_RAD) - 2.f * DEG_TO_RAD) < 1e-4f);
        llassert_always(fabsf(headingChange(-179.f * DEG_TO_RAD, 179.f * DEG_TO_RAD) + 2.f * DEG_TO_RAD) < 1e-4f);
        llassert_always(SMOOTH_TURN_FASTEST * 0.1f < VIGNETTE_TURN_JUMP); // the fastest smooth turn at 10 fps is no jump
        llassert_always(20.f / 15.f < VIGNETTE_JUMP);                     // nor is flying at 20 m/s at 15 fps
    }

    // SLVRLaserColor: the laser's colour by the name the settings window stores; red otherwise.
    LLColor4 laserColour(const std::string& name)
    {
        static const std::pair<const char*, LLColor4> COLOURS[] = {
            { "orange", LLColor4(1.f, 0.55f, 0.1f) }, { "yellow", LLColor4(1.f, 0.9f, 0.2f) },
            { "green", LLColor4(0.3f, 1.f, 0.35f) },  { "blue", LLColor4(0.25f, 0.55f, 1.f) },
            { "purple", LLColor4(0.7f, 0.35f, 1.f) }, { "white", LLColor4(1.f, 1.f, 1.f) } };
        for (const auto& colour : COLOURS)
        {
            if (name == colour.first)
            {
                return colour.second;
            }
        }
        return LLColor4(1.f, 0.25f, 0.2f);
    }

    LLColor4 laserColour() // as set, with its opacity
    {
        static LLCachedControl<std::string> colour_name(gSavedSettings, "SLVRLaserColor", "red");
        static LLCachedControl<F32> opacity(gSavedSettings, "SLVRLaserOpacity", 1.f);
        LLColor4 colour = laserColour(colour_name);
        colour.mV[VALPHA] = llclamp((F32)opacity, LASER_MIN_OPACITY, 1.f);
        return colour;
    }

    // The conversation window that "chat only" centres on the line of sight: the first one shown on its own.
    LLFloater* chatFloater()
    {
        for (const char* name : CHAT_FLOATERS)
        {
            for (LLFloater* floater : LLFloaterReg::getFloaterList(name))
            {
                if (floater->isShown() && !floater->getHost())
                {
                    return floater;
                }
            }
        }
        return nullptr;
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

        // Heading and pitch go there and back; none of either is straight ahead.
        direction(0.7f, -0.4f, out);
        llassert_always(fabsf(headingOf(out) - 0.7f) < 1e-4f && fabsf(pitchOf(out) + 0.4f) < 1e-4f);
        direction(0.f, 0.f, out);
        llassert_always(fabsf(out[0]) + fabsf(out[1]) + fabsf(out[2] + 1.f) < 1e-4f);

        // The keyboard comes up ahead and below the eyes, its face turned up towards them.
        const float eyes[3] = { 0.f, 0.f, 0.f };
        float ahead[3], centre[3], keyboard[4];
        direction(0.f, -KEYBOARD_BELOW, ahead);
        placeAlong(eyes, ahead, 0.8f, 0.f, 0.f, centre, keyboard);
        rotate(keyboard, face, out);
        llassert_always(centre[1] < -0.4f && centre[2] < -0.5f && out[1] > 0.5f && out[2] > 0.5f);
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
        checkPlacement();
        checkVignette();
        checkCurve();
        LLVRKeyboard::selfCheck();
        std::string error;
        if (!mBackend.init(error))
        {
            gSavedSettings.setBOOL("VREnabled", false);
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "VR could not be started: " + error));
            return;
        }
        mPanelVisible = mChatOnly = mUnread = false; // VR starts with the panel hidden; the menu button shows it
        // The sizes and distances the panel and keyboard were last given.
        mPanelWidth = llclamp(gSavedSettings.getF32("SLVRPanelWidth"), PANEL_NARROW, PANEL_WIDE);
        mPanelDistance = llclamp(gSavedSettings.getF32("SLVRPanelDistance"), PANEL_NEAR, PANEL_FAR);
        mKeyboardWidth = llclamp(gSavedSettings.getF32("SLVRKeyboardWidth"), KEYBOARD_NARROW, KEYBOARD_WIDE);
        mKeyboardAhead = llclamp(gSavedSettings.getF32("SLVRKeyboardReach"), PANEL_NEAR, PANEL_FAR);
        applyGraphics(true);
        gIdleCallbacks.addFunction(onIdle, this);
        // Instant messages; nearby chat comes through noteChat().
        mIMConnection = LLIMModel::instance().addNewMsgCallback([this](const LLSD& msg)
        {
            static LLCachedControl<bool> private_only(gSavedSettings, "SLVRMessageDotPrivateOnly", true);
            const LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(msg["session_id"].asUUID());
            if (!msg["is_region_msg"].asBoolean() && (!private_only || (session && session->isP2PSessionType())))
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
    mSteady.valid = mLeftSteady.valid = mMotionValid = false;
    mVignette = mBlink = mVignetteHeld = 0.f;
    for (ControllerModel*& model : sControllerModels)
    {
        delete model; // its texture with it, while GL is still there
        model = nullptr;
    }
    mKeyboardShown = mKeyboardWanted = mWristShown = mArcAiming = mArcValid = mOwnZoom = mPanelFollowing = mTitleDrag = false;
    mArc.clear();
    applyGraphics(false);
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

        if (mBackend.takeRecentered())
        {
            // The poses jumped: the steadied lasers start over, the panel and keyboard come
            // back in front of the eyes (the keyboard as it is shown again, see updateKeyboard()).
            mSteady.valid = mLeftSteady.valid = false;
            if (mPanelVisible)
            {
                placePanel(nullptr);
            }
            mKeyboardShown = false;
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
        // A script or a seat taking the camera (a follow camera, a fixed view) would move and
        // turn the view without the user, what sickens most in VR: with SLVRIgnoreScriptCamera
        // the view stays at the eyes then too. Only SLVR's own zoom moves it.
        static LLCachedControl<bool> keep_eyes(gSavedSettings, "SLVRIgnoreScriptCamera", true);
        const ECameraMode mode = gAgentCamera.getCameraMode();
        if (gAgentCamera.getFocusOnAvatar())
        {
            mOwnZoom = false;
        }
        const bool own_view = mode == CAMERA_MODE_THIRD_PERSON && gAgentCamera.getFocusOnAvatar();
        const bool taken = keep_eyes && !mOwnZoom
            && (mode == CAMERA_MODE_FOLLOW || (mode == CAMERA_MODE_THIRD_PERSON && !gAgentCamera.getFocusOnAvatar()));
        if ((own_view || taken) && isAgentAvatarValid() && gAgentAvatarp->mDrawable.notNull())
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
                // SLVREyeHeightMode 1: the eyes at the user's own height above the avatar's feet,
                // so the world has the size the body knows, however tall the avatar is.
                static LLCachedControl<S32> eye_mode(gSavedSettings, "SLVREyeHeightMode", 0);
                static LLCachedControl<F32> body_height(gSavedSettings, "SLVRBodyHeight", 1.7f);
                if (eye_mode == 1)
                {
                    const F32 feet = gAgentAvatarp->getRenderPosition().mV[VZ] - gAgentAvatarp->getPelvisToFoot();
                    mBaseOrigin.mV[VZ] = feet + llclamp((F32)body_height, 1.f, 2.3f) - EYES_BELOW_TOP;
                }
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
        updateVignette(yaw);
        mBaseYawCos = cosf(yaw);
        mBaseYawSin = sinf(yaw);
        mEye = 0;

        mInputFresh = mBackend.getInput(mInput);
        static LLCachedControl<bool> left_handed(gSavedSettings, "SLVRLeftHanded", false);
        if (mInputFresh && left_handed)
        {
            swapHands(mInput);
        }
        if (mInputFresh)
        {
            updatePointer();
        }
        followPanel();
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

    drawVignette(); // under the lasers: what is pointed at stays seen
    drawControllers();
    if (mLaserValid)
    {
        drawLaser(mLaserStart, mLaserDir, mLaserLength);
    }
    if (mLeftLaserValid)
    {
        drawLaser(mLeftLaserStart, mLeftLaserDir, mLeftLaserLength);
    }
    drawArc();
    if (mUnread && !mPanelVisible)
    {
        drawMessageDot(0.f, LLColor4(1.f, 0.55f, 0.1f));
    }
    if (mTalkDown || (LLVoiceClient::instanceExists() && LLVoiceClient::getInstance()->getPTTIsToggle()
                      && LLVoiceClient::getInstance()->getUserPTTState()))
    {
        drawMessageDot(0.04f, LLColor4(0.3f, 1.f, 0.4f)); // the microphone is on
    }
    drawBlink();
    const LLRect view = gViewerWindow->getWorldViewRectRaw();
    mBackend.submitEye(mEye, view.mLeft, view.mBottom, view.getWidth(), view.getHeight());

    if (mEye == 0)
    {
        mEye = 1;
        mSecondPass = true;
        return true;
    }

    mEye = -1;
    // A screenshot is what the right eye sees: its image, still in the window, with the
    // panel and keyboard as they are drawn now laid over it (see saveScreenshot()).
    std::unique_ptr<ShotLayer> shot_eye, shot_panel, shot_keyboard;
    if (mShotPending)
    {
        mShotPending = false;
        shot_eye = std::make_unique<ShotLayer>();
        shot_eye->width = view.getWidth();
        shot_eye->height = view.getHeight();
        shot_eye->rgba.resize((size_t)shot_eye->width * shot_eye->height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(view.mLeft, view.mBottom, shot_eye->width, shot_eye->height, GL_RGBA, GL_UNSIGNED_BYTE, shot_eye->rgba.data());
        shot_panel = std::make_unique<ShotLayer>();
        shot_keyboard = std::make_unique<ShotLayer>();
    }
    if (panelShown())
    {
        drawPanel(shot_panel.get());
    }
    if (mKeyboardShown)
    {
        drawKeyboard(shot_keyboard.get());
    }
    if (mWristShown)
    {
        drawWrist();
    }
    mBackend.endFrame();
    if (shot_eye)
    {
        saveScreenshot(std::move(*shot_eye), std::move(*shot_panel), std::move(*shot_keyboard));
    }

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
    updateWrist();
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

    // While A is held the arc takes the place of the laser; letting go teleports, see applyInput().
    mArcAiming = false;
    if (mInput.teleport && !mGrabbing)
    {
        updateArc(pos, dir);
        mLaserValid = false;
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }

    if (mTitleDrag) // see startTitleDrag()
    {
        float eyes[3], point[3], look[3];
        for (S32 i = 0; i < 3; ++i)
        {
            eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
            point[i] = pos[i] + dir[i] * mTitleDragAlong - eyes[i];
        }
        const F32 pitch = llclamp(pitchOf(point) + mTitleDragPitch, -80.f * DEG_TO_RAD, 80.f * DEG_TO_RAD);
        direction(headingOf(point) + mTitleDragHeading, pitch, look);
        placeAlong(eyes, look, mTitleDragDistance, mTitleDragX, mTitleDragY, mPanelCentre, mPanelRot);
        mLaserLength = mTitleDragAlong;
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }

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
        const LLVector3 from_eyes(mPanelCentre[0] - (mEyes[0].pos[0] + mEyes[1].pos[0]) / 2.f,
                                  mPanelCentre[1] - (mEyes[0].pos[1] + mEyes[1].pos[1]) / 2.f,
                                  mPanelCentre[2] - (mEyes[0].pos[2] + mEyes[1].pos[2]) / 2.f);
        mPanelDistance = llclamp(from_eyes.length(), PANEL_NEAR, PANEL_FAR); // placePanel() keeps it
        mLaserLength = mGrabDistance;
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }
    if (mGrabbing) // let go: the next session starts with this size and distance
    {
        gSavedSettings.setF32("SLVRPanelWidth", mPanelWidth);
        gSavedSettings.setF32("SLVRPanelDistance", mPanelDistance);
    }
    mGrabbing = false;

    if (pointAtWrist(pos, dir, mLaserLength))
    {
        mKeyboardHit[0] = LLVRKeyboard::Hit();
        return;
    }
    if (pointAtKeyboard(0, pos, dir, mLaserLength))
    {
        mKeyboard.swipeMove(mKeyboardX[0], mKeyboardY[0]);
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
// both hands can type; the rest of the time its trigger and grip have other jobs. Not while
// the right hand swipes a word, which takes no other hand.
void LLViewerVR::updateLeftPointer()
{
    if (!mInput.left_aim_valid || !mKeyboardShown || mKeyboard.swiping())
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
    if (!panelShown() || window.getWidth() <= 0 || window.getHeight() <= 0)
    {
        return false;
    }
    S32 px = 0, py = 0;
    F32 along = 0.f;
    const F32 height = mPanelWidth * window.getHeight() / window.getWidth(), radius = panelRadius();
    if (!(radius > 0.f ? hitCylinder(mPanelCentre, mPanelRot, mPanelWidth, height, radius, window.getWidth(), window.getHeight(), pos, dir, px, py, along)
                       : hitQuad(mPanelCentre, mPanelRot, mPanelWidth, height, window.getWidth(), window.getHeight(), pos, dir, px, py, along)))
    {
        return false;
    }
    // While a press on the panel lasts (dragging a window by its title), the whole panel
    // takes the laser: the window follows one frame behind and the laser would slip off it.
    const bool pressed = (mTriggerDown && mTriggerOnPanel) || (mGripDown && mGripOnPanel);
    if ((mChatOnly || !mPanelVisible) && !pressed
        && std::none_of(mPanelKeep.begin(), mPanelKeep.end(), [&](const LLRect& r) { return r.pointInRect(px, py); }))
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

// Self-check of placeAhead() as placePanel() uses it: looking up and to the left, the line
// of sight meets the placed panel square on, in the middle of the window it was placed for.
void LLViewerVR::checkPlacement()
{
    const F32 yaw = 0.6f, pitch = 0.3f; // radians
    const float eye_rot[4] = { cosf(yaw / 2.f) * sinf(pitch / 2.f), sinf(yaw / 2.f) * cosf(pitch / 2.f),
                               -sinf(yaw / 2.f) * sinf(pitch / 2.f), cosf(yaw / 2.f) * cosf(pitch / 2.f) };
    const float eyes[3] = { 0.1f, 1.6f, 0.2f }, FORWARD[3] = { 0.f, 0.f, -1.f }, FACE[3] = { 0.f, 0.f, 1.f };
    const F32 width = 1.8f, height = 0.9f;
    const S32 image_width = 1000, image_height = 500, window_x = 820, window_y = 340; // the window's middle, pixels
    float centre[3], rot[4], look[3], face[3];
    placeAhead(eyes, eye_rot, 1.2f, ((F32)window_x / image_width - 0.5f) * width, ((F32)window_y / image_height - 0.5f) * height,
               centre, rot);
    rotate(eye_rot, FORWARD, look);
    rotate(rot, FACE, face);
    S32 x = 0, y = 0;
    F32 along = 0.f;
    llassert_always(hitQuad(centre, rot, width, height, image_width, image_height, eyes, look, x, y, along));
    llassert_always(abs(x - window_x) <= 1 && abs(y - window_y) <= 1 && fabsf(along - 1.2f) < 1e-3f);
    llassert_always(fabsf(face[0] + look[0]) + fabsf(face[1] + look[1]) + fabsf(face[2] + look[2]) < 1e-4f);
}

// hitQuad() for the quad bent round a cylinder as LLVRBackend::endUI() bends it: the axis
// lies `radius` out of the face from the centre, which is in the middle of the arc.
bool LLViewerVR::hitCylinder(const float centre[3], const float rot[4], F32 width, F32 height, F32 radius, S32 image_width,
                             S32 image_height, const float pos[3], const float dir[3], S32& x, S32& y, F32& distance)
{
    // Into the flat quad's frame (x right, y up, z out of its face), then onto the axis: the
    // arc's middle is at (0, 0, -radius), and it is seen from inside.
    const float inverse[4] = { -rot[0], -rot[1], -rot[2], rot[3] };
    const float offset[3] = { pos[0] - centre[0], pos[1] - centre[1], pos[2] - centre[2] };
    float p[3], d[3];
    rotate(inverse, offset, p);
    rotate(inverse, dir, d);
    p[2] -= radius;
    const F32 a = d[0] * d[0] + d[2] * d[2], b = 2.f * (p[0] * d[0] + p[2] * d[2]), c = p[0] * p[0] + p[2] * p[2] - radius * radius;
    const F32 disc = b * b - 4.f * a * c;
    if (a < 1e-6f || disc < 0.f || radius <= 0.f)
    {
        return false;
    }
    const F32 root = sqrtf(disc), angle = width / radius;
    for (F32 along : { (-b - root) / (2.f * a), (-b + root) / (2.f * a) })
    {
        const F32 hx = p[0] + along * d[0], hy = p[1] + along * d[1], hz = p[2] + along * d[2];
        const F32 u = atan2f(hx, -hz) / angle + 0.5f, v = hy / height + 0.5f;
        if (along > 0.f && u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
        {
            x = llfloor(u * image_width);
            y = llfloor(v * image_height);
            distance = along;
            return true;
        }
    }
    return false;
}

// Self-check of hitCylinder(): a panel a metre ahead bent round the eyes is met in its middle
// straight ahead, and 20 degrees to the right 20 degrees of arc along; turned to face +X, it
// is met in its middle looking along -X.
void LLViewerVR::checkCurve()
{
    const float rot[4] = { 0.f, 0.f, 0.f, 1.f }, eyes[3] = { 0.f, 0.f, 0.f };
    const float centre[3] = { 0.f, 0.f, -1.f }, ahead[3] = { 0.f, 0.f, -1.f };
    const F32 width = 1.8f, height = 0.9f, a = 20.f * DEG_TO_RAD;
    const S32 image_width = 1800, image_height = 900; // 1000 pixels a radian of arc
    S32 x = 0, y = 0;
    F32 along = 0.f;
    llassert_always(hitCylinder(centre, rot, width, height, 1.f, image_width, image_height, eyes, ahead, x, y, along));
    llassert_always(abs(x - 900) <= 1 && abs(y - 450) <= 1 && fabsf(along - 1.f) < 1e-4f);
    const float right[3] = { sinf(a), 0.f, -cosf(a) };
    llassert_always(hitCylinder(centre, rot, width, height, 1.f, image_width, image_height, eyes, right, x, y, along));
    llassert_always(abs(x - (900 + ll_round(a * 1000.f))) <= 1 && abs(y - 450) <= 1 && fabsf(along - 1.f) < 1e-4f);
    const float turned[4] = { 0.f, sinf(F_PI_BY_TWO / 2.f), 0.f, cosf(F_PI_BY_TWO / 2.f) }; // +90 deg about +Y: faces +X
    const float side[3] = { -1.f, 0.f, 0.f };
    llassert_always(hitCylinder(side, turned, width, height, 1.f, image_width, image_height, eyes, side, x, y, along));
    llassert_always(abs(x - 900) <= 1 && abs(y - 450) <= 1 && fabsf(along - 1.f) < 1e-4f);
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
    const LLColor4 colour = laserColour();
    gViewerWindow->setup3DRender(); // this eye's matrices; render_ui() left its own behind
    LLGLSUIDefault gls_ui;
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);
    gGL.color4fv(colour.mV);
    gGL.vertex3fv(start.mV);
    gGL.vertex3fv((start + dir * length).mV);
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

// Closes the comfort vignette in while the view moves or turns by itself: walking, flying,
// riding, the zoom camera, smooth turning. Measured on the view rather than on the sticks,
// so whatever moves it counts; the head's own movement is not part of it.
void LLViewerVR::updateVignette(F32 yaw)
{
    const F32 dt = mMotionTimer.getElapsedTimeAndResetF32();
    F32 target = 0.f;
    if (mMotionValid && dt > 0.f)
    {
        const LLVector3 delta = mBaseOrigin - mMotionOrigin;
        const F32 moved = delta.length();
        const F32 turned = headingChange(mMotionYaw, yaw);
        if (mFirstPerson != mMotionFirstPerson || moved > VIGNETTE_JUMP || fabsf(turned) > VIGNETTE_TURN_JUMP)
        {
            blink(); // a teleport, a region crossing, the zoom starting or ending: no motion, a jump
            LL_INFOS("VR") << "Blink over a jump: " << moved << " m, " << turned << " rad, eyes view " << mMotionFirstPerson
                           << " to " << mFirstPerson << LL_ENDL;
        }
        else
        {
            const F32 level = sqrtf(delta.mV[VX] * delta.mV[VX] + delta.mV[VY] * delta.mV[VY]);
            target = vignetteTarget(level / dt, fabsf(delta.mV[VZ]) / dt, mSnapped ? 0.f : turned / dt);
        }
    }
    target = holdPeak(target, mVignetteHeld, mVignetteHeldFor, dt);
    mSnapped = false;
    mBlink = llmax(0.f, mBlink - llmin(dt, 0.05f) / BLINK_TIME); // at least a few frames, however long the jump took
    mMotionValid = true;
    mMotionFirstPerson = mFirstPerson;
    mMotionOrigin = mBaseOrigin;
    mMotionYaw = yaw;
    const F32 step = llmin(dt, 0.1f) / (target > mVignette ? VIGNETTE_IN : VIGNETTE_OUT);
    mVignette = target > mVignette ? llmin(target, mVignette + step) : llmax(target, mVignette - step);
}

// Draws the comfort vignette into the eye just rendered: dark all round the line of sight,
// hiding the edges of the view, where motion is felt most. Fully open it lies just past the
// image's farthest corner; it closes in to the size its setting gives.
void LLViewerVR::drawVignette()
{
    static LLCachedControl<S32> level(gSavedSettings, "SLVRVignette", 2);
    if (level < 1 || mVignette <= 0.f)
    {
        return;
    }
    const S32 size = llmin((S32)level, 3) - 1;
    const F32* tan = mEyes[mEye].tan_fov; // left, right, up, down
    const LLRect view = gViewerWindow->getWorldViewRectRaw();
    const F32 corner = sqrtf(llmax(tan[0] * tan[0], tan[1] * tan[1]) + llmax(tan[2] * tan[2], tan[3] * tan[3]));
    const F32 clear = tanf(VIGNETTE_CLEAR[size] * DEG_TO_RAD), black = tanf(VIGNETTE_BLACK[size] * DEG_TO_RAD);
    const F32 inner = corner + (clear - corner) * mVignette;
    const F32 radii[3] = { inner, inner + black - clear, llmax(inner + black - clear, corner) + 1.f }; // tangents
    const F32 alphas[3] = { 0.f, 1.f, 1.f };
    auto vertex = [&](S32 ring, F32 angle)
    {
        gGL.color4f(0.f, 0.f, 0.f, alphas[ring]);
        gGL.vertex2f(view.mLeft + (radii[ring] * cosf(angle) - tan[0]) / (tan[1] - tan[0]) * view.getWidth(),
                     view.mBottom + (radii[ring] * sinf(angle) - tan[3]) / (tan[2] - tan[3]) * view.getHeight());
    };

    gViewerWindow->setup2DRender(); // window pixels
    LLGLSUIDefault gls_ui;
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    const S32 STEPS = 48;
    for (S32 i = 0; i < STEPS; ++i)
    {
        const F32 a0 = F_TWO_PI * i / STEPS, a1 = F_TWO_PI * (i + 1) / STEPS;
        for (S32 ring = 0; ring < 2; ++ring) // the feathered edge, then black out past the corners
        {
            vertex(ring, a0);
            vertex(ring + 1, a0);
            vertex(ring + 1, a1);
            vertex(ring, a0);
            vertex(ring + 1, a1);
            vertex(ring, a1);
        }
    }
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

// SLVRBlink: the view goes black and fades back in over a jump (a snap turn, a teleport),
// which the eyes take far better than the jump itself.
void LLViewerVR::blink()
{
    static LLCachedControl<bool> on(gSavedSettings, "SLVRBlink", true);
    if (on)
    {
        mBlink = 1.f;
    }
}

void LLViewerVR::drawBlink()
{
    if (mBlink <= 0.f)
    {
        return;
    }
    const LLRect view = gViewerWindow->getWorldViewRectRaw();
    gViewerWindow->setup2DRender();
    LLGLSUIDefault gls_ui;
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(view.mLeft, view.mTop, view.mRight, view.mBottom, LLColor4(0.f, 0.f, 0.f, mBlink));
    gGL.flush();
    gUIProgram.unbind();
}

// Vibrates a hand (0 the pointing one) unless SLVRHaptics is off.
void LLViewerVR::buzz(S32 hand, F32 amplitude, F32 seconds)
{
    static LLCachedControl<bool> haptics(gSavedSettings, "SLVRHaptics", true);
    static LLCachedControl<bool> left_handed(gSavedSettings, "SLVRLeftHanded", false);
    if (haptics)
    {
        mBackend.vibrate(left_handed ? 1 - hand : hand, amplitude, seconds);
    }
}

// X held, or the wrist menu: where the head is now, facing where it faces, becomes straight
// ahead. The panel and keyboard follow once the new poses are in, see beginEye().
void LLViewerVR::recenter()
{
    mBackend.recenter();
    buzz(1, 0.5f, 0.05f);
}

// SLVRPanelFollow: once the eyes turn well away from the panel, it glides back in front of
// them, square on, at its distance.
void LLViewerVR::followPanel()
{
    static LLCachedControl<bool> follow(gSavedSettings, "SLVRPanelFollow", false);
    if (!follow || !mPanelVisible || mGrabbing || mTitleDrag || mEye < 0)
    {
        mPanelFollowing = false;
        return;
    }
    // With only the chat shown, the conversation window comes in front of the eyes, not the
    // panel's middle, which left the window off to the side.
    F32 dx = 0.f, dy = 0.f;
    windowOffset(mChatOnly ? chatFloater() : nullptr, dx, dy);
    const float FORWARD[3] = { 0.f, 0.f, -1.f }, RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f };
    float eyes[3], look[3], centre[3], rot[4], right[3], up[3];
    rotate(mPanelRot, RIGHT, right);
    rotate(mPanelRot, UP, up);
    for (S32 i = 0; i < 3; ++i)
    {
        eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
    }
    rotate(mEyes[0].rot, FORWARD, look);
    LLVector3 to_panel(mPanelCentre[0] + right[0] * dx + up[0] * dy - eyes[0], mPanelCentre[1] + right[1] * dx + up[1] * dy - eyes[1],
                       mPanelCentre[2] + right[2] * dx + up[2] * dy - eyes[2]);
    to_panel.normalize();
    const F32 cosine = to_panel.mV[0] * look[0] + to_panel.mV[1] * look[1] + to_panel.mV[2] * look[2];
    if (cosine < cosf(PANEL_FOLLOW_AWAY))
    {
        mPanelFollowing = true;
    }
    if (!mPanelFollowing)
    {
        return;
    }
    placeAhead(eyes, mEyes[0].rot, mPanelDistance, dx, dy, centre, rot);
    const F32 share = lowPass(PANEL_FOLLOW_RATE, llmin((F32)gFrameIntervalSeconds, 0.1f));
    const F32 same_way = mPanelRot[0] * rot[0] + mPanelRot[1] * rot[1] + mPanelRot[2] * rot[2] + mPanelRot[3] * rot[3] < 0.f ? -1.f : 1.f;
    F32 length = 0.f;
    for (S32 i = 0; i < 4; ++i)
    {
        if (i < 3)
        {
            mPanelCentre[i] += (centre[i] - mPanelCentre[i]) * share;
        }
        mPanelRot[i] += (same_way * rot[i] - mPanelRot[i]) * share;
        length += mPanelRot[i] * mPanelRot[i];
    }
    length = sqrtf(length);
    for (F32& part : mPanelRot)
    {
        part /= length;
    }
    if (cosine > cosf(PANEL_FOLLOW_NEAR))
    {
        mPanelFollowing = false;
    }
}

// The teleport arc (A held, in place of the laser): thrown from the hand along the laser,
// falling as a ball would. It lands on the ground or on the top of an object; on too steep a
// face, or where RLVa does not let the avatar go, it does not.
void LLViewerVR::updateArc(const float pos[3], const float dir[3])
{
    mArcAiming = true;
    mArcValid = false;
    mArc.clear();
    const LLVector3 from = mBaseOrigin + toAgent(trackingToLocal(pos));
    const LLVector3 velocity = toAgent(trackingToLocal(dir)) * ARC_SPEED;
    LLWorld* world = LLWorld::getInstance();
    LLVector3 point = from;
    mArc.push_back(point);
    for (S32 step = 1; step <= ARC_STEPS; ++step)
    {
        const F32 t = ARC_STEP * step;
        LLVector3 next = from + velocity * t - LLVector3::z_axis * (0.5f * ARC_GRAVITY * t * t);
        LLVector4a start, end, hit, normal;
        start.load3(point.mV);
        end.load3(next.mV);
        S32 face = -1;
        if (gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, nullptr, nullptr, &hit, nullptr, &normal))
        {
            next.set(hit.getF32ptr());
            LLVector3 up(normal.getF32ptr());
            up.normalize();
            mArc.push_back(next);
            mArcValid = up.mV[VZ] > ARC_FLAT; // a wall or a steep roof stops the throw without a landing
            mArcTarget = next;
            break;
        }
        const F32 ground = world->resolveLandHeightAgent(next);
        if (next.mV[VZ] <= ground)
        {
            // Back along the step to where it went into the ground.
            const F32 above = llmax(0.f, point.mV[VZ] - world->resolveLandHeightAgent(point)), below = ground - next.mV[VZ];
            next = point + (next - point) * (above + below > 0.f ? above / (above + below) : 1.f);
            next.mV[VZ] = world->resolveLandHeightAgent(next);
            mArc.push_back(next);
            mArcValid = true;
            mArcTarget = next;
            break;
        }
        mArc.push_back(next);
        point = next;
    }
    if (mArcValid && RlvActions::isRlvEnabled() && !RlvActions::canTeleportToLocal(gAgent.getPosGlobalFromAgent(mArcTarget)))
    {
        mArcValid = false;
    }
}

void LLViewerVR::drawArc()
{
    if (!mArcAiming || mArc.size() < 2)
    {
        return;
    }
    LLColor4 colour = laserColour();
    if (!mArcValid)
    {
        colour.setVec(0.6f, 0.6f, 0.6f, colour.mV[VALPHA]); // nowhere to land
    }
    gViewerWindow->setup3DRender();
    LLGLSUIDefault gls_ui;
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);
    gGL.color4fv(colour.mV);
    for (size_t i = 1; i < mArc.size(); ++i)
    {
        gGL.vertex3fv(mArc[i - 1].mV);
        gGL.vertex3fv(mArc[i].mV);
    }
    if (mArcValid)
    {
        const S32 STEPS = 32;
        const LLVector3 centre = mArcTarget + LLVector3::z_axis * 0.03f;
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS, a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.vertex3fv((centre + LLVector3(cosf(a0), sinf(a0), 0.f) * ARC_RING).mV);
            gGL.vertex3fv((centre + LLVector3(cosf(a1), sinf(a1), 0.f) * ARC_RING).mV);
        }
    }
    gGL.end();
    gGL.flush();
    gUIProgram.unbind();
}

// Where the hands are: each controller's model (see ControllerModel), or a small box behind
// where its laser leaves it.
void LLViewerVR::drawControllers()
{
    const F32 HALF[3] = { 0.018f, 0.015f, 0.055f }; // metres along the controller's right, up and forward
    const float RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f }, FORWARD[3] = { 0.f, 0.f, -1.f };
    static LLCachedControl<bool> left_handed(gSavedSettings, "SLVRLeftHanded", false);
    static LLCachedControl<bool> show(gSavedSettings, "SLVRShowControllers", true);
    if (!show)
    {
        return; // the lasers alone
    }
    gViewerWindow->setup3DRender();
    LLGLSUIDefault gls_ui;
    LLGLEnable cull(GL_CULL_FACE); // closed shapes: the faces turned to the eye alone draw them
    // A model is not convex: it needs depth for itself. The scene's depth is not in this
    // image, so start from empty depth; nothing drawn after the controllers uses it.
    LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
    glClear(GL_DEPTH_BUFFER_BIT);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gUIProgram.bind();
    for (S32 hand = 0; hand < 2; ++hand)
    {
        if (!(hand ? mInput.left_aim_valid : mInput.aim_valid))
        {
            continue;
        }
        const float* rot = hand ? mInput.left_aim_rot : mInput.aim_rot;
        float r[3], u[3], f[3];
        rotate(rot, RIGHT, r);
        rotate(rot, UP, u);
        rotate(rot, FORWARD, f);
        const LLVector3 axes[3] = { toAgent(trackingToLocal(r)), toAgent(trackingToLocal(u)), toAgent(trackingToLocal(f)) };
        const LLVector3 origin = mBaseOrigin + toAgent(trackingToLocal(hand ? mInput.left_aim_pos : mInput.aim_pos));

        const S32 side = left_handed ? 1 - hand : hand; // the physical controller: hands are swapped in input
        if (!sControllerModels[side])
        {
            sControllerModels[side] = loadControllerModel(side);
        }
        const ControllerModel& model = *sControllerModels[side];
        if (!model.indices.empty())
        {
            // Model space is the aim pose's: x right, y up, z back (against the laser).
            const LLVector3 back = -axes[2];
            const GLfloat matrix[16] = { axes[0].mV[VX], axes[0].mV[VY], axes[0].mV[VZ], 0.f,
                                         axes[1].mV[VX], axes[1].mV[VY], axes[1].mV[VZ], 0.f,
                                         back.mV[VX], back.mV[VY], back.mV[VZ], 0.f,
                                         origin.mV[VX], origin.mV[VY], origin.mV[VZ], 1.f };
            gGL.pushMatrix();
            gGL.multMatrix(matrix);
            if (model.texture)
            {
                gGL.getTexUnit(0)->bind(model.texture);
            }
            else
            {
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            // shortcut: immediate mode, some 17000 vertices a hand an eye; a vertex buffer if it costs frames.
            gGL.begin(LLRender::TRIANGLES);
            for (U32 index : model.indices)
            {
                // Lit from above: the normal's height in the world.
                const LLVector3& n = model.normals[index];
                const F32 up = n.mV[VX] * axes[0].mV[VZ] + n.mV[VY] * axes[1].mV[VZ] + n.mV[VZ] * back.mV[VZ];
                const F32 shade = model.texture ? 0.6f + 0.4f * llmax(up, 0.f) : 0.3f + 0.3f * llmax(up, 0.f);
                gGL.color4f(shade, shade, shade, 1.f);
                gGL.texCoord2fv(model.uvs[index].mV);
                gGL.vertex3fv(model.positions[index].mV);
            }
            gGL.end();
            gGL.popMatrix();
            continue;
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        const LLVector3 centre = origin - axes[2] * 0.06f;
        for (S32 axis = 0; axis < 3; ++axis)
        {
            for (F32 sign : { -1.f, 1.f })
            {
                // Corners in this order (a x b = the outward normal) wind counter-clockwise seen from outside.
                const LLVector3 n = axes[axis] * sign, a = axes[(axis + 1) % 3], b = n % a;
                const LLVector3 face = centre + n * HALF[axis];
                const LLVector3 da = a * HALF[(axis + 1) % 3], db = b * HALF[(axis + 2) % 3];
                const F32 shade = 0.2f + 0.14f * (n.mV[VZ] + 1.f); // lit from above
                gGL.color4f(shade, shade, shade * 1.1f, 1.f);
                const LLVector3 corners[4] = { face - da - db, face + da - db, face + da + db, face - da + db };
                for (S32 i : { 0, 1, 2, 0, 2, 3 })
                {
                    gGL.vertex3fv(corners[i].mV);
                }
            }
        }
        gGL.end();
    }
    gGL.flush();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gUIProgram.unbind();
}

// The wrist menu shows while the other hand's controller is held up in view with its face
// turned to the eyes, as one looks at a watch; it rides just over the controller. Not while
// typing, when that hand points at the keyboard.
void LLViewerVR::updateWrist()
{
    mWristX = -1; // until pointAtWrist() finds the laser on it
    if (!mInput.left_aim_valid || mKeyboardShown || mGrabLong)
    {
        mWristShown = false;
        mWristItem = -1;
        return;
    }
    const float FORWARD[3] = { 0.f, 0.f, -1.f }, UP[3] = { 0.f, 1.f, 0.f };
    float eyes[3], look[3], up[3];
    for (S32 i = 0; i < 3; ++i)
    {
        eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
    }
    rotate(mEyes[0].rot, FORWARD, look);
    rotate(mInput.left_aim_rot, UP, up);
    const float* hand = mInput.left_aim_pos;
    LLVector3 to_hand(hand[0] - eyes[0], hand[1] - eyes[1], hand[2] - eyes[2]);
    const F32 distance = to_hand.normalize();
    const F32 seen = look[0] * to_hand.mV[0] + look[1] * to_hand.mV[1] + look[2] * to_hand.mV[2];
    const F32 facing_eyes = -(up[0] * to_hand.mV[0] + up[1] * to_hand.mV[1] + up[2] * to_hand.mV[2]);
    mWristShown = distance < WRIST_REACH && (mWristShown ? seen > cosf(WRIST_KEPT) && facing_eyes > WRIST_STILL_FACING
                                                          : seen > cosf(WRIST_SEEN) && facing_eyes > WRIST_FACING);
    if (!mWristShown)
    {
        mWristItem = -1;
        return;
    }
    float dir[3];
    for (S32 i = 0; i < 3; ++i)
    {
        mWristCentre[i] = hand[i] + up[i] * 0.07f;
        dir[i] = mWristCentre[i] - eyes[i];
    }
    facing(dir, mWristRot); // square on to the eyes
}

bool LLViewerVR::pointAtWrist(const float pos[3], const float dir[3], F32& length)
{
    mWristItem = -1;
    S32 x = 0, y = 0;
    F32 along = 0.f;
    if (!mWristShown || !hitQuad(mWristCentre, mWristRot, WRIST_SIZE, WRIST_SIZE * WRIST_HEIGHT / WRIST_WIDTH,
                                 WRIST_WIDTH, WRIST_HEIGHT, pos, dir, x, y, along))
    {
        return false;
    }
    for (S32 item = 0; item < WRIST_ITEMS; ++item)
    {
        if (wristItemRect(item).pointInRect(x, y))
        {
            mWristItem = item;
        }
    }
    mWristX = x;
    mWristY = y;
    length = along;
    return true;
}

void LLViewerVR::drawWrist()
{
    if (!mBackend.beginUI(LLVRBackend::UI_WRIST, WRIST_WIDTH, WRIST_HEIGHT))
    {
        return;
    }
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glViewport(0, 0, WRIST_WIDTH, WRIST_HEIGHT);
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        LLGLSUIDefault gls_ui;
        gl_state_for_2d(WRIST_WIDTH, WRIST_HEIGHT);
        gGLCoverageAlpha = true; // see drawKeyboard()
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        gUIProgram.bind();
        gl_rect_2d(0, WRIST_HEIGHT, WRIST_WIDTH, 0, LLColor4(0.09f, 0.1f, 0.12f, 0.94f));
        const bool sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
        for (S32 item = 0; item < WRIST_ITEMS; ++item)
        {
            const LLRect r = wristItemRect(item);
            gl_rect_2d(r, item == mWristItem ? LLColor4(0.28f, 0.47f, 0.82f, 1.f) : LLColor4(0.2f, 0.22f, 0.26f, 1.f));
            const char* key = WRIST_TEXTS[item];
            if (item == 5)
            {
                key = sitting ? "wrist_stand" : gAgent.getFlying() ? "wrist_land" : "wrist_fly";
            }
            const LLWString text = utf8str_to_wstring(LLVRLanguage::text(key));
            const LLFontGL* font = LLFontGL::getFontSansSerifHuge();
            if (font && font->getWidthF32(text.c_str()) * LLFontGL::sScaleX > r.getWidth() - 12.f)
            {
                font = LLFontGL::getFontSansSerif(); // a long word in some languages
            }
            if (font)
            {
                // Fonts take UI units, which the UI scale turns into pixels.
                font->render(text, 0, r.getCenterX() / LLFontGL::sScaleX, r.getCenterY() / LLFontGL::sScaleY,
                             LLColor4(0.93f, 0.94f, 0.96f, 1.f), LLFontGL::HCENTER, LLFontGL::VCENTER);
            }
        }
        if (mWristX >= 0) // the laser is on it
        {
            gl_rect_2d(mWristX - 6, mWristY + 6, mWristX + 6, mWristY - 6, LLColor4::black);
            gl_rect_2d(mWristX - 4, mWristY + 4, mWristX + 4, mWristY - 4, LLColor4::white);
        }
        gGL.flush();
        gUIProgram.unbind();
        gGLCoverageAlpha = false;
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
    gGL.setColorMask(true, false);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    mBackend.endUI(mWristCentre, mWristRot, WRIST_SIZE, WRIST_SIZE * WRIST_HEIGHT / WRIST_WIDTH);
}

// Trigger on a wrist menu item.
void LLViewerVR::pressWrist()
{
    switch (mWristItem)
    {
    case 0: // the whole panel, or none
        // From the chat alone the panel stays where it is, as it does the other way (the menu
        // button): placed anew, the conversation window jumped aside to its spot on the panel.
        if (!mPanelVisible)
        {
            placePanel(nullptr);
        }
        mPanelVisible = mChatOnly ? true : !mPanelVisible;
        mChatOnly = false;
        break;
    case 1: // the chat alone, or none
        if (mPanelVisible && mChatOnly)
        {
            mPanelVisible = mChatOnly = false;
        }
        else
        {
            mPanelVisible = mChatOnly = true;
            showChat();
            placePanel(chatFloater());
        }
        break;
    case 2:
        showSettings();
        placePanel(LLFloaterReg::findInstance("slvr_settings"));
        break;
    case 3:
        requestScreenshot();
        break;
    case 4:
        recenter();
        break;
    case 5:
        if (isAgentAvatarValid() && gAgentAvatarp->isSitting())
        {
            gAgent.standUp();
        }
        else
        {
            gAgent.setFlying(!gAgent.getFlying());
        }
        break;
    default:
        return;
    }
    buzz(0, PRESS_STRENGTH, PRESS_TIME);
}

// SLVRVRGraphics: lighter graphics while in VR. Every eye is a frame of its own, and the frame
// rate is what comfort rests on most. Set as unsaved values: what is saved stays the user's
// own settings, even if the viewer goes down in VR, and they come back as VR is switched off.
// Called every frame in VR, so that switching the setting takes effect at once.
void LLViewerVR::applyGraphics(bool in_vr)
{
    struct Cap { const char* name; F32 most; };
    static const Cap CAPS[] = { { "RenderShadowDetail", 0.f }, { "RenderScreenSpaceReflections", 0.f },
                                { "RenderDepthOfField", 0.f }, { "RenderMirrors", 0.f },
                                { "RenderFarClip", 96.f }, { "RenderAvatarMaxNonImpostors", 8.f } };
    static LLCachedControl<bool> lighter(gSavedSettings, "SLVRVRGraphics", true);
    const bool vr = in_vr && lighter;
    if (vr == mGraphicsApplied)
    {
        return;
    }
    mGraphicsApplied = vr;
    for (const Cap& cap : CAPS)
    {
        LLPointer<LLControlVariable> control = gSavedSettings.getControl(cap.name);
        if (!control)
        {
            continue;
        }
        if (!vr)
        {
            control->setValue(control->getSaveValue(), true); // drops the unsaved value
        }
        else if (control->getValue().asReal() > cap.most)
        {
            const LLSD capped = control->type() == TYPE_BOOLEAN ? LLSD(false)
                              : control->type() == TYPE_F32 ? LLSD(cap.most) : LLSD((S32)cap.most);
            control->setValue(capped, false);
        }
    }
    LL_INFOS("VR") << (vr ? "Lighter VR graphics on" : "Lighter VR graphics off, the user's own back") << LL_ENDL;
}

bool LLViewerVR::measureHeight(F32& body_height)
{
    F32 eyes = 0.f;
    if (!mBackend.isInitialized() || !mBackend.eyeHeight(eyes) || eyes < 0.8f || eyes > 2.3f)
    {
        return false;
    }
    body_height = eyes + EYES_BELOW_TOP;
    gSavedSettings.setF32("SLVRBodyHeight", body_height);
    LL_INFOS("VR") << "Measured: eyes " << eyes << " m above the floor" << LL_ENDL;
    return true;
}

F32 LLViewerVR::panelRadius() const
{
    // Bent round the eyes at its distance; not past a half turn, however near and wide. Only
    // the whole panel: a window shown alone is put square on to the eyes as on a flat panel
    // (placePanel()), and bent with the panel it was turned aside by as much as it lay off
    // the panel's middle (pie menus and script dialogs seen at a slant).
    static LLCachedControl<bool> curved(gSavedSettings, "SLVRCurvedPanel", true);
    return curved && mBackend.canCurve() && mPanelVisible && !mChatOnly ? llmax(mPanelDistance, mPanelWidth / 3.f) : 0.f;
}

// Draws the HUD attachments and the flat UI into the panel image instead of the window.
void LLViewerVR::drawPanel(ShotLayer* shot)
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
    if (mPanelVisible && !mChatOnly) // the chat alone means the chat alone
    {
        gGLCoverageAlpha = true; // the HUD's translucent faces write their coverage, see LLDrawPoolAlpha::forwardRender()
        render_hud_attachments();
        gGLCoverageAlpha = false;
    }
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
        // The opaque HUD shaders do not write coverage to alpha. Make every pixel they drew
        // opaque: those are the ones nearer than the cleared depth. The translucent faces
        // write no depth here, only their coverage; marked opaque, a HUD's soft shadow
        // turned solid black (v0.5.3).
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
    if (mChatOnly || !mPanelVisible)
    {
        gUIProgram.bind();
        drawChatOnly();
        gUIProgram.unbind();
        gViewerWindow->setup2DRender();
    }
    else
    {
        gViewerWindow->draw();
    }

    // The UI leaves a plain window-pixel coordinate system behind.
    gUIProgram.bind();
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

    const F32 height_m = mPanelWidth * height / width;
    if (shot && mBackend.readUI(shot->rgba, shot->width, shot->height))
    {
        std::copy(mPanelCentre, mPanelCentre + 3, shot->centre);
        std::copy(mPanelRot, mPanelRot + 4, shot->rot);
        shot->width_m = mPanelWidth;
        shot->height_m = height_m;
        shot->radius = panelRadius();
    }
    mBackend.endUI(mPanelCentre, mPanelRot, mPanelWidth, height_m, panelRadius());
}

// With only the chat shown: instead of the whole UI, only the conversation windows and the
// open menus are drawn (back to front, as the UI would), so nothing that overlaps them leaks
// onto the panel. Their rectangles also bound where the lasers hit the panel. With the
// panel hidden, a context menu is drawn alone the same way. In both, so are the windows
// of updatePopped().
void LLViewerVR::drawChatOnly()
{
    std::vector<LLView*> views;
    for (auto it = gFloaterView->getChildList()->rbegin(); it != gFloaterView->getChildList()->rend(); ++it)
    {
        LLFloater* floater = dynamic_cast<LLFloater*>(*it);
        if (floater && floater->isShown() && !floater->getHost() && ((mPanelVisible && isChatFloater(floater)) || isPopped(floater)))
        {
            views.push_back(floater);
        }
    }
    for (auto it = gMenuHolder->getChildList()->rbegin(); it != gMenuHolder->getChildList()->rend(); ++it)
    {
        if ((*it)->getVisible()) // context and pie menus
        {
            views.push_back(*it);
        }
    }
    if (LLUICtrl* top = gFocusMgr.getTopCtrl()) // an open drop-down list
    {
        views.push_back(top);
    }

    const LLVector2 scale = gViewerWindow->getDisplayScale();
    mPanelKeep.clear();
    LLView::sIsDrawing = true;
    LLView::sDirtyRect = gViewerWindow->getWindowRectScaled();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadIdentity();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.pushMatrix();
    LLUI::pushMatrix();
    gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);
    for (LLView* view : views)
    {
        const LLRect r = view->calcScreenRect(); // UI units
        LLRect keep = r;
        if (dynamic_cast<PieMenu*>(view))
        {
            // A pie pops up at 1.7 times its radius, its shading ring 1.09 times that
            // (piemenu.cpp), and shrinks on the release of the press that opened it. In VR
            // that release goes to the tool, so it stays big; within its own rectangle only,
            // the outer half of the slices let the laser through.
            keep.stretch(ll_round(r.getWidth() / 2 * (1.7f * 1.09f - 1.f)));
        }
        mPanelKeep.emplace_back(llfloor(keep.mLeft * scale.mV[VX]), llceil(keep.mTop * scale.mV[VY]),
                                llceil(keep.mRight * scale.mV[VX]), llfloor(keep.mBottom * scale.mV[VY]));
        LLUI::pushMatrix();
        LLUI::translate((F32)r.mLeft, (F32)r.mBottom);
        // Alone on the panel a pie menu reads fainter than over the whole UI. Drawn over
        // itself, its 80% background covers 96%. Each time from the same origin: its draw()
        // moves the UI origin to its centre and leaves it there.
        for (S32 times = dynamic_cast<PieMenu*>(view) ? 2 : 1; times > 0; --times)
        {
            LLUI::pushMatrix();
            view->draw();
            LLUI::popMatrix();
        }
        LLUI::popMatrix();
    }
    LLUI::popMatrix();
    gGL.popMatrix();
    LLView::sIsDrawing = false;
}

// With the panel hidden or showing only the chat, a window the user asked for (an item of a
// pie or context menu, a link in the chat, the dialog of a chair just touched) would open
// out of sight. Those are drawn as well, until they close; windows and dialogs nobody asked
// for (a group's object calling) are not, but notices are (isNotice()). Back on the whole
// panel the list starts over.
void LLViewerVR::updatePopped()
{
    std::vector<LLFloater*> shown;
    for (LLView* view : *gFloaterView->getChildList())
    {
        LLFloater* floater = dynamic_cast<LLFloater*>(view);
        const LLNotificationsUI::LLToast* toast = dynamic_cast<LLNotificationsUI::LLToast*>(floater);
        if (floater && floater->isShown() && !floater->getHost() && (!toast || isNotice(toast))) // not chat bubbles
        {
            shown.push_back(floater);
        }
    }
    // A new conversation opens as a tab of a window that may be open already: the keyboard
    // focus moving into a window counts as that window opening.
    LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    LLFloater* focus_floater = focus && focus != mFocusBefore ? gFloaterView->getParentFloater(focus) : nullptr;

    if (mPanelVisible && !mChatOnly)
    {
        mPopped.clear();
    }
    else
    {
        mPopped.erase(std::remove_if(mPopped.begin(), mPopped.end(),
                                     [](const LLHandle<LLFloater>& h) { return h.isDead() || !h.get()->isShown() || h.get()->getHost(); }),
                      mPopped.end());
        const bool asked = mPressTimer.getStarted() && mPressTimer.getElapsedTimeF32() < POP_AFTER_PRESS;
        for (LLFloater* floater : shown)
        {
            const bool opened = floater == focus_floater || std::find(mShownBefore.begin(), mShownBefore.end(), floater) == mShownBefore.end();
            const bool notice = dynamic_cast<LLNotificationsUI::LLToast*>(floater) != nullptr; // see isNotice(), asked for or not
            if ((asked || notice) && opened && !isPopped(floater) && !(mChatOnly && isChatFloater(floater)))
            {
                mPopped.push_back(floater->getHandle());
                placePanel(floater); // where the user looks, not at the window's corner of the panel
            }
        }
    }
    mShownBefore.assign(shown.begin(), shown.end());
    mFocusBefore = focus;
}

bool LLViewerVR::isPopped(const LLFloater* floater) const
{
    return std::any_of(mPopped.begin(), mPopped.end(), [&](const LLHandle<LLFloater>& h) { return h.get() == floater; });
}

// The own avatar has been seen to vanish in the zoom now and then, leaving only its name
// tag (v0.5.1). Until the cause is known, every zoom logs what decides whether it is drawn.
void LLViewerVR::logZoom() const
{
    if (!isAgentAvatarValid())
    {
        return;
    }
    S32 attached = 0, hidden = 0;
    for (const auto& point : gAgentAvatarp->mAttachmentPoints)
    {
        for (const auto& object : point.second ? point.second->mAttachedObjects : LLViewerJointAttachment::attachedobjs_vec_t())
        {
            LLSpatialBridge* bridge = object && object->mDrawable.notNull() ? object->mDrawable.get()->getSpatialBridge() : nullptr;
            if (bridge)
            {
                ++attached;
                hidden += bridge->mDrawableType == 0;
            }
        }
    }
    LL_INFOS("VR") << "Zoom: avatar drawn " << gAgent.needsRenderAvatar() << ", shown " << gAgent.getShowAvatar()
                   << ", eyes view " << mFirstPerson << " (attachments set for " << mAttachmentsFirstPerson << ")"
                   << ", camera mode " << gAgentCamera.getCameraMode() << ", focus on avatar " << gAgentCamera.getFocusOnAvatar()
                   << ", attachments hidden " << hidden << " of " << attached << ", avatar visible " << gAgentAvatarp->isVisible()
                   << LL_ENDL;
}

// Draws the on-screen keyboard into its own quad layer, in front of the panel.
void LLViewerVR::drawKeyboard(ShotLayer* shot)
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
        mKeyboard.draw(mKeyboardHit, laserColour());
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
    const F32 height_m = mKeyboardWidth * height / width;
    if (shot && mBackend.readUI(shot->rgba, shot->width, shot->height))
    {
        std::copy(mKeyboardCentre, mKeyboardCentre + 3, shot->centre);
        std::copy(mKeyboardRot, mKeyboardRot + 4, shot->rot);
        shot->width_m = mKeyboardWidth;
        shot->height_m = height_m;
    }
    mBackend.endUI(mKeyboardCentre, mKeyboardRot, mKeyboardWidth, height_m);
}

// A dot low in the field of view while messages came in with the panel hidden; drawn into
// the eye just rendered, fixed to the head so it is seen wherever one looks.
void LLViewerVR::drawMessageDot(F32 aside, const LLColor4& colour)
{
    float head[3], offset[3], right[3], up[3];
    const float AHEAD[3] = { aside, -0.3f, -1.f }, RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f };
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
    gGL.color4f(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE], glow);
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
    static LLCachedControl<bool> private_only(gSavedSettings, "SLVRMessageDotPrivateOnly", true);
    if (chat.mSourceType == CHAT_SOURCE_AGENT && !private_only)
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

// Puts the panel square on to the line of sight, at its distance from the eyes, with the
// middle of `around` (a window; the whole panel when null) where the eyes look: text reads
// best seen straight on.
void LLViewerVR::placePanel(const LLView* around)
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (window.getWidth() <= 0 || window.getHeight() <= 0)
    {
        return;
    }
    F32 dx = 0.f, dy = 0.f;
    windowOffset(around, dx, dy);
    float eyes[3];
    for (S32 i = 0; i < 3; ++i)
    {
        eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
    }
    placeAhead(eyes, mEyes[0].rot, mPanelDistance, dx, dy, mPanelCentre, mPanelRot);
}

// Metres from the panel's middle to a window's (in the panel's plane, right and up); none
// for no window.
void LLViewerVR::windowOffset(const LLView* around, F32& dx, F32& dy) const
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    dx = dy = 0.f;
    if (around && window.getWidth() > 0 && window.getHeight() > 0)
    {
        const LLVector2 scale = gViewerWindow->getDisplayScale();
        const LLRect r = around->calcScreenRect(); // UI units
        dx = (r.getCenterX() * scale.mV[VX] / window.getWidth() - 0.5f) * mPanelWidth;
        dy = (r.getCenterY() * scale.mV[VY] / window.getHeight() - 0.5f) * mPanelWidth * window.getHeight() / window.getWidth();
    }
}

// The trigger pressed on the title bar of a window shown on its own (chat only, or a popped
// window): instead of moving the window across the panel, where it went out of the image's
// bottom and seemed to shrink, the panel goes round the eyes with the laser, the window
// staying square on to them at its distance. See updatePointer().
bool LLViewerVR::startTitleDrag(LLView* title)
{
    const LLView* window = title ? title->getParent() : nullptr;
    if (!window || !mSteady.valid)
    {
        return false;
    }
    windowOffset(window, mTitleDragX, mTitleDragY);
    const float RIGHT[3] = { 1.f, 0.f, 0.f }, UP[3] = { 0.f, 1.f, 0.f };
    float right[3], up[3], middle[3], point[3];
    rotate(mPanelRot, RIGHT, right);
    rotate(mPanelRot, UP, up);
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 eyes = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
        middle[i] = mPanelCentre[i] + right[i] * mTitleDragX + up[i] * mTitleDragY - eyes;
        point[i] = mSteady.pos[i] + mSteady.dir[i] * mLaserLength - eyes;
    }
    mTitleDragHeading = headingChange(headingOf(point), headingOf(middle));
    mTitleDragPitch = pitchOf(middle) - pitchOf(point);
    mTitleDragDistance = llclamp(sqrtf(middle[0] * middle[0] + middle[1] * middle[1] + middle[2] * middle[2]), PANEL_NEAR, PANEL_FAR);
    mTitleDragAlong = mLaserLength;
    mTitleDrag = true;
    return true;
}

// Where the line of sight meets the panel, window pixels (origin bottom left), whatever of
// it is shown.
bool LLViewerVR::gazePoint(S32& x, S32& y) const
{
    const LLRect window = gViewerWindow->getWindowRectRaw();
    if (window.getWidth() <= 0 || window.getHeight() <= 0)
    {
        return false;
    }
    const float FORWARD[3] = { 0.f, 0.f, -1.f };
    float eyes[3], look[3];
    rotate(mEyes[0].rot, FORWARD, look);
    for (S32 i = 0; i < 3; ++i)
    {
        eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
    }
    F32 along = 0.f;
    const F32 height = mPanelWidth * window.getHeight() / window.getWidth(), radius = panelRadius();
    return radius > 0.f ? hitCylinder(mPanelCentre, mPanelRot, mPanelWidth, height, radius, window.getWidth(), window.getHeight(), eyes, look, x, y, along)
                        : hitQuad(mPanelCentre, mPanelRot, mPanelWidth, height, window.getWidth(), window.getHeight(), eyes, look, x, y, along);
}

void LLViewerVR::onIdle(void* self)
{
    LLViewerVR* vr = static_cast<LLViewerVR*>(self);
    vr->applyGraphics(vr->mBackend.isInitialized());
    vr->applyInput();
    if (vr->mShotSaving.valid() && vr->mShotSaving.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const std::pair<bool, std::string> saved = vr->mShotSaving.get();
        LLStringUtil::format_map_t args;
        args["[PATH]"] = saved.second;
        std::string message = LLVRLanguage::text(saved.first ? "screenshot_saved" : "screenshot_failed");
        LLStringUtil::format(message, args);
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
    }
}

void LLViewerVR::requestScreenshot()
{
    // One at a time: a new std::async future would wait for the old one where it is assigned.
    if (mBackend.isInitialized() && !mShotSaving.valid())
    {
        mShotPending = true;
    }
}

namespace
{
    // Pictures\SLVR: outside the viewer's folder, so screenshots survive reinstalling it.
    std::string screenshotFolder()
    {
        std::string folder;
        PWSTR pictures = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)))
        {
            folder = ll_convert_wide_to_string(std::wstring(pictures)) + "\\SLVR";
        }
        CoTaskMemFree(pictures);
        if (folder.empty())
        {
            folder = gDirUtilp->getOSUserDir() + "\\SLVR";
        }
        LLFile::mkdir(folder);
        return folder;
    }

    // An image's premultiplied RGBA pixel laid over an RGB one.
    void over(U8* dst, const U8* src)
    {
        const U32 keep = 255 - src[3];
        for (S32 c = 0; c < 3; ++c)
        {
            dst[c] = (U8)llmin(255U, src[c] + (dst[c] * keep + 127) / 255);
        }
    }
}

// Worker thread: lays the panel and the keyboard over the eye's image, as the headset's
// compositor does, and saves the result. The eye image has the window's shape; the
// screenshot gets the eye's own (the ratio of its field of view) at the window's width.
void LLViewerVR::saveScreenshot(ShotLayer eye_image, ShotLayer panel, ShotLayer keyboard)
{
    const LLVREye eye = mEyes[1];
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "SLVR_%Y-%m-%d_%H-%M-%S.png", &local);
    const std::string path = screenshotFolder() + "\\" + stamp;
    make_ui_sound("UISndSnapshot");

    mShotSaving = std::async(std::launch::async, [eye, path, eye_image = std::move(eye_image), panel = std::move(panel), keyboard = std::move(keyboard)]()
    {
        const F32 tan_width = eye.tan_fov[1] - eye.tan_fov[0], tan_height = eye.tan_fov[2] - eye.tan_fov[3];
        const S32 width = eye_image.width, height = llmax(1, ll_round(width * tan_height / tan_width));
        LLPointer<LLImageRaw> raw = new LLImageRaw(width, height, 3);
        U8* out = raw->getData();
        for (S32 y = 0; y < height; ++y)
        {
            for (S32 x = 0; x < width; ++x)
            {
                const F32 u = (x + 0.5f) / width, v = (y + 0.5f) / height;
                U8* pixel = out + ((size_t)y * width + x) * 3;
                const U8* source = &eye_image.rgba[(llmin((S32)(v * eye_image.height), eye_image.height - 1) * (size_t)eye_image.width
                                                   + llmin((S32)(u * eye_image.width), eye_image.width - 1)) * 4];
                std::copy(source, source + 3, pixel);

                // The ray through this pixel, in tracking space.
                const float local[3] = { eye.tan_fov[0] + u * tan_width, eye.tan_fov[3] + v * tan_height, -1.f };
                float dir[3];
                rotate(eye.rot, local, dir);
                for (const ShotLayer* layer : { &panel, &keyboard })
                {
                    S32 px = 0, py = 0;
                    F32 along = 0.f;
                    if (!layer->rgba.empty()
                        && (layer->radius > 0.f
                            ? hitCylinder(layer->centre, layer->rot, layer->width_m, layer->height_m, layer->radius, layer->width, layer->height, eye.pos, dir, px, py, along)
                            : hitQuad(layer->centre, layer->rot, layer->width_m, layer->height_m, layer->width, layer->height, eye.pos, dir, px, py, along)))
                    {
                        over(pixel, &layer->rgba[((size_t)py * layer->width + px) * 4]);
                    }
                }
            }
        }
        LLPointer<LLImagePNG> png = new LLImagePNG;
        bool saved = png->encode(raw, 0.f);
        if (saved)
        {
            llofstream file(path.c_str(), std::ios::binary);
            file.write((const char*)png->getData(), png->getDataSize());
            saved = file.good();
        }
        LL_INFOS("VR") << "Screenshot " << width << "x" << height << (saved ? " saved to " : " could not be saved to ") << path << LL_ENDL;
        return std::make_pair(saved, path);
    });
}

// Shows the on-screen keyboard while a text field has the keyboard focus. It first appears
// below and ahead of the head; after that it stays where it was put.
void LLViewerVR::updateKeyboard()
{
    // Only once a text field was pressed (mKeyboardWanted): a press anywhere on a conversation
    // window gives its text field the focus, and the keyboard came up with every one.
    LLUICtrl* focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    const bool shown = panelShown() && mKeyboardWanted && focus && focus->acceptsTextInput(); // the panel, or windows of it
    if (!shown)
    {
        if (mKeyboardShown)
        {
            mKeyboard.reset();
        }
        mKeyboardShown = false;
        mKeyboardWanted = mKeyboardWanted && mTriggerDown; // the field may take the focus as the press ends
        mDragHand = mRepeatHand = -1;
        return;
    }
    if (!mKeyboardShown)
    {
        // Each time it comes up: low in front, see KEYBOARD_BELOW. Facing straight down, the
        // head's up axis gives the heading.
        const float FORWARD[3] = { 0.f, 0.f, -1.f }, UP[3] = { 0.f, 1.f, 0.f };
        float eyes[3], look[3], up[3], ahead[3];
        rotate(mEyes[0].rot, FORWARD, look);
        rotate(mEyes[0].rot, UP, up);
        const bool level = look[0] * look[0] + look[2] * look[2] > 0.1f;
        direction(headingOf(level ? look : up), -KEYBOARD_BELOW, ahead);
        for (S32 i = 0; i < 3; ++i)
        {
            eyes[i] = (mEyes[0].pos[i] + mEyes[1].pos[i]) / 2.f;
        }
        placeAlong(eyes, ahead, mKeyboardAhead, 0.f, 0.f, mKeyboardCentre, mKeyboardRot);
    }
    mKeyboardShown = true;
    mKeyboard.update();
}

// Starts the zoom camera with its focus where the laser points. From the avatar's eyes the
// camera first steps back and up, behind the avatar as in a third person game, which also
// shows that the view is no longer the avatar's own. The camera turns to the focus; the
// tracking space is turned back by as much, so the view keeps facing where it did.
void LLViewerVR::startZoom()
{
    if (!mLaserValid)
    {
        return;
    }
    const LLVector3 focus = mWorldHit ? mWorldHitPos : mLaserStart + mLaserDir * ZOOM_FREE_DISTANCE;
    LLVector3 camera = mBaseOrigin;
    if (mFirstPerson)
    {
        camera += LLVector3(-mBaseYawCos, -mBaseYawSin, 0.f) * ZOOM_BACK + LLVector3::z_axis * ZOOM_UP;
    }
    const LLVector3 to_focus = focus - camera;
    if (to_focus.length() < 0.5f)
    {
        return;
    }
    mZoomYawOffset = fabsf(to_focus.mV[VX]) + fabsf(to_focus.mV[VY]) > 0.1f // straight above or below gives no heading
        ? atan2f(mBaseYawSin, mBaseYawCos) - atan2f(to_focus.mV[VY], to_focus.mV[VX]) : 0.f;
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(camera), gAgent.getPosGlobalFromAgent(focus), LLUUID::null);
    gAgentCamera.setCameraAnimating(false); // no glide
    mOwnZoom = true; // the view follows this camera, see beginEye()
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
    else if (hand == 0 && mKeyboard.swipeStart(hit, mKeyboardX[0], mKeyboardY[0]))
    {
        // the right trigger held over the letters swipes a word; see LLVRKeyboard::swipeEnd()
    }
    else
    {
        const LLVRKeyboard::EPress result = mKeyboard.press(hit);
        if (result == LLVRKeyboard::REPEATS)
        {
            mRepeatHand = hand;
            mRepeatHit = hit;
            mRepeatTimer.reset();
            mRepeatNext = KEY_REPEAT_DELAY;
        }
        else if (result == LLVRKeyboard::SETTINGS)
        {
            showSettings();
        }
    }
}

// The SLVR settings (wrist menu, keyboard's settings key). Over a hidden panel or the chat
// alone they come up alone, in front of the eyes, as a window opened from a menu does (see
// updatePopped()); the whole panel showing with them was not asked for.
void LLViewerVR::showSettings()
{
    LLFloater* settings = LLFloaterReg::showInstance("slvr_settings", LLSD(), true);
    if (settings && (!mPanelVisible || mChatOnly))
    {
        if (!isPopped(settings))
        {
            mPopped.push_back(settings->getHandle());
        }
        placePanel(settings);
    }
}

void LLViewerVR::releaseKeyboard(S32 hand)
{
    if (mDragHand == hand)
    {
        mDragHand = -1;
        // The next time it comes up, and the next session: this size, this far ahead.
        const LLVector3 from_eyes(mKeyboardCentre[0] - (mEyes[0].pos[0] + mEyes[1].pos[0]) / 2.f,
                                  mKeyboardCentre[1] - (mEyes[0].pos[1] + mEyes[1].pos[1]) / 2.f,
                                  mKeyboardCentre[2] - (mEyes[0].pos[2] + mEyes[1].pos[2]) / 2.f);
        mKeyboardAhead = llclamp(from_eyes.length(), PANEL_NEAR, PANEL_FAR);
        gSavedSettings.setF32("SLVRKeyboardWidth", mKeyboardWidth);
        gSavedSettings.setF32("SLVRKeyboardReach", mKeyboardAhead);
    }
    if (mRepeatHand == hand)
    {
        mRepeatHand = -1;
    }
    if (hand == 0)
    {
        mKeyboard.swipeEnd();
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
            placePanel(nullptr);
        }
        else if (!mChatOnly)
        {
            // The panel stays where it is: the conversation window is seen where it just was.
            // The script dialogs open on the panel stay with it (see updatePopped()).
            mChatOnly = true;
            showChat();
            for (LLFloater* dialog : LLFloaterReg::getFloaterList("script_floater"))
            {
                if (dialog->isShown() && !dialog->getHost() && !isPopped(dialog))
                {
                    mPopped.push_back(dialog->getHandle());
                }
            }
        }
        else
        {
            mPanelVisible = mChatOnly = false;
        }
    }
    mMenuDown = input.menu;

    if (input.shot && !mShotDown)
    {
        requestScreenshot();
    }
    mShotDown = input.shot;

    // X held a moment recentres, once per hold.
    if (!input.recenter)
    {
        mRecenterTimer.stop();
        mRecentered = false;
    }
    else if (!mRecenterTimer.getStarted())
    {
        mRecenterTimer.start();
    }
    else if (!mRecentered && mRecenterTimer.getElapsedTimeF32() > RECENTER_HOLD)
    {
        mRecentered = true;
        recenter();
    }

    // B is the push to talk key, as the user set it up: held, or toggled.
    if (input.talk != mTalkDown)
    {
        mTalkDown = input.talk;
        LLVoiceClient::getInstance()->inputUserControlState(input.talk);
        if (input.talk)
        {
            buzz(0, PRESS_STRENGTH, PRESS_TIME);
        }
    }

    // A held aims the arc (updatePointer()); letting go teleports where it lands.
    if (!input.teleport && mTeleportDown && mArcValid)
    {
        LLVector3d target = gAgent.getPosGlobalFromAgent(mArcTarget);
        if (isAgentAvatarValid())
        {
            target.mdV[VZ] += gAgentAvatarp->getPelvisToFoot(); // the arc lands at the feet; the agent is at the pelvis
        }
        gAgent.teleportViaLocationLookAt(target);
        blink();
        buzz(0, PRESS_STRENGTH, PRESS_TIME);
    }
    if (!input.teleport)
    {
        mArcValid = false;
        mArc.clear();
    }
    mTeleportDown = input.teleport;

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
    // A context menu shown on the hidden panel goes with the panel once it closes.
    if (mMenuOnly && (mPanelVisible || (!mGripDown && !gMenuHolder->hasVisibleMenu() && !gFocusMgr.getTopCtrl())))
    {
        mMenuOnly = false;
    }
    updatePopped();

    // Left stick walks and strafes, like the arrow keys, and pushed all the way runs. Walking
    // brings a zoomed camera back to the avatar.
    const bool walk = fabsf(input.move[1]) > WALK_ON, strafe = fabsf(input.move[0]) > WALK_ON;
    const bool all_the_way = (walk || strafe) && sqrtf(input.move[0] * input.move[0] + input.move[1] * input.move[1]) > RUN_ON
                             && !gAgent.getFlying();
    if (!all_the_way)
    {
        mRunTimer.stop();
    }
    else if (!mRunTimer.getStarted())
    {
        mRunTimer.start();
    }
    const bool run = all_the_way && mRunTimer.getElapsedTimeF32() > RUN_HOLD;
    if (run != mRunning)
    {
        mRunning = run;
        run ? gAgent.setTempRun() : gAgent.clearTempRun();
        if (run)
        {
            buzz(1, PRESS_STRENGTH, PRESS_TIME); // felt in the walking hand: now running
        }
    }
    static LLCachedControl<S32> walk_direction(gSavedSettings, "SLVRWalkDirection", 0);
    if ((walk || strafe) && mFirstPerson && isAgentAvatarValid() && !gAgentAvatarp->isSitting())
    {
        // Forward is where the head looks (SLVRWalkDirection 0) or the walking hand points
        // (1), not where the avatar happened to face: turn the avatar there, and the tracking
        // space back by as much so that the view stays put.
        const bool by_hand = walk_direction == 1 && input.left_aim_valid;
        const float forward[3] = { 0.f, 0.f, -1.f };
        float rotated[3];
        rotate(by_hand ? input.left_aim_rot : mEyes[0].rot, forward, rotated);
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
    // shortcut: the laser's world hit does not include terrain; over bare ground the focus is
    // a point along the laser instead, and it does not follow the laser.
    updateKeyboard();
    if (input.zoom != mZoomDown)
    {
        mZoomDown = input.zoom;
        if (input.zoom)
        {
            mZoomOnKeyboard = mKeyboardHit[1].part != LLVRKeyboard::NONE;
            mZoomStarted = mZoomLogged = false;
            mZoomTimer.reset();
        }
        if (mZoomOnKeyboard)
        {
            input.zoom ? pressKeyboard(1) : releaseKeyboard(1);
            if (input.zoom)
            {
                buzz(1, PRESS_STRENGTH, PRESS_TIME);
            }
        }
    }
    // A press alone does nothing; held a moment, it starts the zoom.
    if (input.zoom && !mZoomOnKeyboard && !mZoomStarted && mZoomTimer.getElapsedTimeF32() > ZOOM_HOLD)
    {
        mZoomStarted = true;
        startZoom();
    }

    // Right stick: sideways turns in steps, one step per push, or smoothly (SLVRSmoothTurn) at
    // a rate that grows with the push past the dead zone; up and down is jump/fly up
    // and crouch/fly down like Page Up and Page Down, and held up it starts flying.
    const F32 x = input.turn[0], y = input.turn[1];
    const bool zooming = input.zoom && !mZoomOnKeyboard && mZoomStarted && !gAgentCamera.getFocusOnAvatar();
    const bool stick_free = !mGrabbing && !zooming && mDragHand < 0;
    if (zooming && !mZoomLogged && mZoomTimer.getElapsedTimeF32() > ZOOM_HOLD + 1.f)
    {
        mZoomLogged = true;
        logZoom();
    }
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
    static LLCachedControl<bool> smooth_turn(gSavedSettings, "SLVRSmoothTurn", false);
    if (fabsf(x) < STICK_OFF)
    {
        mTurned = false;
    }
    else if (smooth_turn)
    {
        if (stick_free && fabsf(x) >= fabsf(y))
        {
            const F32 push = (fabsf(x) - STICK_OFF) / (1.f - STICK_OFF);
            gAgent.yaw((x > 0.f ? -1.f : 1.f) * push * smoothTurnRate() * llmin((F32)gFrameIntervalSeconds, 0.1f));
        }
    }
    else if (stick_free && !mTurned && fabsf(x) > STICK_ON && fabsf(x) >= fabsf(y))
    {
        mTurned = true;
        gAgent.yaw(x > 0.f ? -snapTurn() : snapTurn());
        mSnapped = true;
        blink();
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
            mTriggerOnWrist = mWristItem >= 0;
            mTriggerOnKeyboard = on_keyboard;
            mTriggerOnPanel = on_panel;
            if (on_keyboard || on_panel)
            {
                buzz(0, PRESS_STRENGTH, PRESS_TIME);
            }
            // A press beside an open menu closes it, as a click beside it does on the screen,
            // and does nothing else: the menu holder never sees presses into the world.
            mTriggerClosedMenu = !mTriggerOnWrist && !on_keyboard && !on_panel && gMenuHolder->hasVisibleMenu();
            if (mTriggerClosedMenu)
            {
                gMenuHolder->hideMenus();
            }
        }
        if (!mTriggerOnKeyboard && !mTriggerOnWrist)
        {
            mPressTimer.start(); // the release too: menu items act on it
        }
        if (mTriggerOnWrist)
        {
            if (input.trigger)
            {
                pressWrist(); // which vibrates itself
            }
        }
        else if (mTriggerOnKeyboard)
        {
            input.trigger ? pressKeyboard(0) : releaseKeyboard(0);
        }
        else if (mTriggerClosedMenu)
        {
            // nothing more, see the press above
        }
        else if (mTitleDrag)
        {
            mTitleDrag = input.trigger; // the release ends it
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
        if (input.trigger && mTriggerOnPanel && !mTriggerOnWrist && !mTriggerOnKeyboard && !mTriggerClosedMenu)
        {
            LLView* title = nullptr;
            const Pressed pressed = pressedOn(&title);
            if (pressed != Pressed::TITLE) // dragging a window leaves the keyboard as it is
            {
                mKeyboardWanted = pressed == Pressed::TEXT;
            }
            if (pressed == Pressed::TITLE && (mChatOnly || !mPanelVisible) && startTitleDrag(title))
            {
                gFocusMgr.setMouseCapture(nullptr); // the panel goes round the eyes, not the window across it
            }
        }
    }
    if (input.grip != mGripDown)
    {
        mGripDown = input.grip;
        if (input.grip)
        {
            mGripOnKeyboard = on_keyboard; // nothing to click there
            mGripOnPanel = on_panel;
            mMenuOnly = !mPanelVisible; // the context menu opens on the panel, alone if it was hidden
            mMenuPlaced = false;
            if (on_panel)
            {
                buzz(0, PRESS_STRENGTH, PRESS_TIME);
            }
        }
        if (!mGripOnKeyboard)
        {
            mPressTimer.start();
        }
        if (mGripOnPanel)
        {
            input.grip ? gViewerWindow->handleRightMouseDown(window, pointer, mask)
                       : gViewerWindow->handleRightMouseUp(window, pointer, mask);
        }
        else if (LLTool* tool = mGripOnKeyboard ? nullptr : LLToolMgr::getInstance()->getCurrentTool())
        {
            // The pick follows the laser whatever the point; the point is where the menu
            // opens: where the eyes look on the panel, or its middle, not where the laser
            // last left the panel, out of sight.
            S32 x = gViewerWindow->getWindowWidthRaw() / 2, y = gViewerWindow->getWindowHeightRaw() / 2;
            gazePoint(x, y);
            const LLVector2 scale = gViewerWindow->getDisplayScale();
            x = ll_round(x / scale.mV[VX]);
            y = ll_round(y / scale.mV[VY]);
            input.grip ? tool->handleRightMouseDown(x, y, mask) : tool->handleRightMouseUp(x, y, mask);
        }
    }
    // The menu the right grip opened on the hidden panel: the panel comes to where the eyes
    // look, with the menu in the middle of the view. It may open a moment after the press.
    if (mMenuOnly && !mMenuPlaced && !mPanelVisible && gMenuHolder->hasVisibleMenu())
    {
        mMenuPlaced = true;
        placePanel(gMenuHolder->getVisibleMenu());
    }

    // A light tick as a laser moves onto a key, a wrist menu item, or a button or menu item
    // of the panel.
    for (S32 hand = 0; hand < 2; ++hand)
    {
        if (mKeyboardHit[hand].part != LLVRKeyboard::NONE && !(mKeyboardHit[hand] == mHoverKey[hand]))
        {
            buzz(hand, TICK_STRENGTH, TICK_TIME);
        }
        mHoverKey[hand] = mKeyboardHit[hand];
    }
    if (mWristItem >= 0 && mWristItem != mHoverWrist)
    {
        buzz(0, TICK_STRENGTH, TICK_TIME);
    }
    mHoverWrist = mWristItem;
    const LLView* hover = on_panel ? controlAt(mPointerX, mPointerY) : nullptr;
    if (hover && hover != mHoverView)
    {
        buzz(0, TICK_STRENGTH, TICK_TIME);
    }
    mHoverView = hover;

    // Backspace held down repeats, as on a real keyboard.
    if (mRepeatHand >= 0 && mRepeatTimer.getElapsedTimeF32() > mRepeatNext)
    {
        mKeyboard.press(mRepeatHit);
        mRepeatNext += KEY_REPEAT_INTERVAL;
    }
}
