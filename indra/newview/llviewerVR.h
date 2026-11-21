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

#include "llframetimer.h"
#include "llhandle.h"
#include "llrect.h"
#include "llvrbackend.h"
#include "llvrkeyboard.h"
#include "v3math.h"

#include <boost/signals2/connection.hpp>
#include <future>
#include <vector>

class LLChat;
class LLFloater;

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

    // LLAgent / LLVOAvatarSelf hook: true while the headset shows the world from the avatar's
    // own eyes. The own avatar is then drawn as in mouselook (no head), without the viewer
    // being in mouselook, which would switch off the UI and the click tools.
    bool firstPerson() const { return mFirstPerson; }

    // LLVOAvatarSelf::updateAttachmentVisibility() hook: whatever called it, the attachments
    // are now set for this (firstPerson() at the time), so applyInput() knows when to set them again.
    void attachmentsSet(bool first_person) { mAttachmentsFirstPerson = first_person; }

    // LLNotificationManager::onChat() hook: nearby chat came in. While the panel is hidden,
    // a message from someone else lights a dot in view (instant messages are seen directly).
    void noteChat(const LLChat& chat);

    // Y button and the SLVR menu: saves what the right eye sees, the panel and keyboard
    // included, as a PNG in Pictures\SLVR.
    void requestScreenshot();

    void shutdown();

private:
    void updateEnabled();
    void setEyeCamera();
    LLVector3 toAgent(const LLVector3& local) const;
    void updatePointer();
    void updateLeftPointer();
    bool hitPanel(const float pos[3], const float dir[3], S32& x, S32& y, F32& distance) const;
    static bool hitQuad(const float centre[3], const float rot[4], F32 width, F32 height, S32 image_width, S32 image_height,
                        const float pos[3], const float dir[3], S32& x, S32& y, F32& distance);
    bool pointAtKeyboard(S32 hand, const float pos[3], const float dir[3], F32& length);
    void updateKeyboard();
    void startZoom();
    void pressKeyboard(S32 hand);
    void releaseKeyboard(S32 hand);
    void showChat();
    void noteMessage(const LLUUID& from);
    void drawLaser(const LLVector3& start, const LLVector3& dir, F32 length);
    // A screenshot's quad layers: their image and where they were.
    struct ShotLayer
    {
        std::vector<unsigned char> rgba; // premultiplied, bottom row first
        int width = 0, height = 0;
        float centre[3] = {}, rot[4] = {};
        F32 width_m = 0.f, height_m = 0.f;
    };
    void drawPanel(ShotLayer* shot);
    void drawChatOnly();
    void updatePopped();
    bool isPopped(const LLFloater* floater) const;
    bool panelShown() const { return mPanelVisible || mMenuOnly || !mPopped.empty(); }
    void logZoom() const;
    void drawKeyboard(ShotLayer* shot);
    void showSettings();
    void saveScreenshot(ShotLayer eye_image, ShotLayer panel, ShotLayer keyboard);
    void drawMessageDot();
    void applyInput();
    static void onIdle(void* self);

    LLVRBackend mBackend;
    LLVREye mEyes[2];
    S32 mEye = -1;            // eye being rendered; -1 = ordinary single-view rendering
    bool mSecondPass = false; // endEye() asked display() to go round again

    // Controllers: read while rendering, acted on from the next idle() like other input.
    LLVRInput mInput;
    bool mInputFresh = false;   // mInput was read this frame and not yet acted on
    bool mPanelVisible = false; // the menu button steps through all of it, chat only, and hidden
    bool mChatOnly = false;
    bool mMenuOnly = false;     // panel hidden, but a context menu opened by the right grip is shown alone
    std::vector<LLRect> mPanelKeep; // with chat only: what is left of the panel, window pixels
    // Windows shown on top of what the panel mode shows (see updatePopped()), until they close.
    std::vector<LLHandle<LLFloater>> mPopped;
    std::vector<const LLFloater*> mShownBefore; // last frame's open windows, only compared
    const void* mFocusBefore = nullptr;         // and the keyboard focus
    LLFrameTimer mPressTimer;                   // since the last press, but on the keyboard
    bool mUnread = false;       // a message came in while the panel was hidden
    boost::signals2::connection mIMConnection;
    bool mLaserValid = false;   // right hand is tracked: the laser is drawn and steers world picks
    bool mPanelHit = false;     // the laser points at the UI panel
    LLVector3 mLaserStart, mLaserDir; // agent coordinates
    F32 mLaserLength = 0.f;
    S32 mPointerX = 0, mPointerY = 0; // where the laser meets the panel: window pixels, origin bottom left
    bool mWorldHit = false;     // the laser lands on an object or avatar, at mWorldHitPos
    LLVector3 mWorldHitPos;
    // Hand poses steadied against tremor (see steady()), in tracking space.
    struct SteadyPose { bool valid = false; float pos[3]; float dir[3]; F32 speed = 0.f; };
    SteadyPose mSteady, mLeftSteady;
    static void steady(SteadyPose& s, const float pos[3], const float rot[4], F32 dt);
    static void checkSteady();
    // Left laser: only while the on-screen keyboard is shown, so both hands can type.
    bool mLeftLaserValid = false, mLeftPanelHit = false;
    LLVector3 mLeftLaserStart, mLeftLaserDir;
    F32 mLeftLaserLength = 0.f;
    S32 mLeftPointerX = 0, mLeftPointerY = 0;
    bool mMenuDown = false, mTriggerDown = false, mGripDown = false, mZoomDown = false, mTurned = false;
    bool mTriggerOnPanel = false, mGripOnPanel = false, mGripOnKeyboard = false; // where the press went; its release follows
    bool mTriggerOnKeyboard = false, mZoomOnKeyboard = false;
    bool mZoomStarted = false;  // the left trigger was held long enough to zoom
    bool mZoomLogged = false;   // see logZoom()
    LLFrameTimer mZoomTimer;
    F32 mZoomYawOffset = 0.f; // tracking space heading relative to the zoomed camera's, see applyInput()
    LLFrameTimer mClickTimer;   // since the last trigger press on the panel, for double clicks
    S32 mClickX = 0, mClickY = 0;
    bool mUpHeld = false, mFlyTried = false; // right stick pushed up, and flying already asked for
    LLFrameTimer mUpTimer;

    // The UI panel: a rectangle in tracking space, straight ahead of where the user sat when
    // VR started until it is moved (hold the left grip: it follows the laser).
    float mPanelCentre[3] = { 0.f, -0.1f, -1.2f };
    float mPanelRot[4] = { 0.f, 0.f, 0.f, 1.f }; // quaternion x, y, z, w; identity faces +Z
    F32 mPanelWidth = 1.8f;                      // metres; the height follows the window's aspect ratio
    bool mGrabbing = false;
    F32 mGrabDistance = 1.5f;
    LLFrameTimer mGrabTimer;  // left grip held for this long
    bool mGrabLong = false;   // and long enough to take the panel

    // On-screen keyboard: a quad of its own in tracking space while a text field has the
    // keyboard focus. Hands are 0 right, 1 left.
    LLVRKeyboard mKeyboard;
    bool mKeyboardShown = false, mKeyboardPlaced = false;
    float mKeyboardCentre[3] = { 0.f, -0.3f, -0.5f };
    float mKeyboardRot[4] = { 0.f, 0.f, 0.f, 1.f };
    F32 mKeyboardWidth = 0.55f;
    LLVRKeyboard::Hit mKeyboardHit[2]; // what each laser is on
    S32 mKeyboardX[2] = {}, mKeyboardY[2] = {}; // and where, keyboard image pixels
    S32 mDragHand = -1;       // hand moving the keyboard by its handle
    F32 mDragDistance = 0.5f;
    S32 mRepeatHand = -1;     // hand holding down a repeating key
    LLVRKeyboard::Hit mRepeatHit;
    LLFrameTimer mRepeatTimer;
    F32 mRepeatNext = 0.f;

    bool mShotPending = false, mShotDown = false;
    std::future<std::pair<bool, std::string>> mShotSaving; // written on a worker thread: saved, and where

    bool mFirstPerson = false;            // this frame is seen from the avatar's eyes
    bool mAttachmentsFirstPerson = false; // what the attachments' visibility was last set for

    // Viewer camera as the agent camera left it this frame: the VR tracking origin.
    LLVector3 mBaseOrigin, mBaseAt, mBaseLeft, mBaseUp;
    F32 mBaseYawCos = 1.f, mBaseYawSin = 0.f;
    F32 mYawOffset = 0.f; // tracking space heading relative to the avatar's, see applyInput()
};

extern LLViewerVR gVR;

#endif
