/**
 * @file llvrkeyboard.h
 * @brief SLVR: the on-screen keyboard shown in the headset, on a layer of its own.
 *
 * Draws itself into a fixed-size image and says what lies at a point of it; where the image
 * is placed and how the lasers reach it is up to LLViewerVR. Types into whatever text field
 * has the keyboard focus, like the real keyboard, and suggests word completions and the
 * next word, learned from what the user wrote in private messages (the chat logs, and
 * typing here, kept between logins), over the keyboard language's word list. The letters come from the
 * layout chosen in the SLVR settings (see llvrlanguage.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#ifndef LL_LLVRKEYBOARD_H
#define LL_LLVRKEYBOARD_H

#include "llstring.h"
#include "v2math.h"

#include <future>
#include <map>
#include <string>
#include <vector>

class LLColor4;

class LLVRKeyboard
{
public:
    static const S32 WIDTH = 1280, HEIGHT = 540; // image pixels
    static const S32 HANDLE_Y = 25;              // middle of the drag handle below the keys

    // What lies at a point of the image.
    enum EPart { NONE, BODY, KEY, SUGGESTION, HANDLE };
    struct Hit
    {
        EPart part = NONE;
        S32 index = -1; // key or suggestion
        bool operator==(const Hit& other) const { return part == other.part && index == other.index; }
    };
    Hit hitAt(S32 x, S32 y) const; // image pixels, origin bottom left

    // Once per frame while shown: follows the word being typed in the focused text field.
    void update();

    // Into the bound WIDTH x HEIGHT image, in a matching 2D projection.
    // hover: what each hand's laser is on; trail: the colour of the swipe trail.
    void draw(const Hit hover[2], const LLColor4& trail) const;

    // Trigger pressed on a key or suggestion: typed, typed and repeats while held
    // (backspace), or the settings key, which the caller answers.
    enum EPress { TYPED, REPEATS, SETTINGS };
    EPress press(const Hit& hit);

    // Swipe typing (SLVRSwipeTyping): the trigger pressed on a letter starts a swipe instead
    // of typing it (true); the laser's points (image pixels) follow while it is held; on the
    // release the word whose keys the path went over is typed, or the letter alone if the
    // laser never left its key.
    bool swipeStart(const Hit& hit, S32 x, S32 y);
    void swipeMove(S32 x, S32 y);
    void swipeEnd();
    bool swiping() const { return mSwiping && mSwipeSure; } // a press not yet a swipe is not

    // Back to letters, shift off; when the keyboard goes away.
    void reset();

    // The settings' forget button: deletes what typing in private messages taught (its file),
    // and the keyboard lets go of everything it learned until the next login.
    static void forgetLearned();

    static void selfCheck();

    typedef std::map<std::string, U32> word_map_t;
    struct Learned { word_map_t words, pairs; }; // pairs: "word next", how often the one followed the other

private:
    void type(const LLWString& text);
    void learnText(const LLWString& text, U32 weight);
    void saveTyped();
    void refreshSuggestions();

    S32 mPage = 0;          // 0 letters, 1 digits and symbols
    bool mShift = false, mCapsLock = false;

    // Suggestions. Words are kept lower case (UTF-8) with how often they were seen.
    std::future<std::pair<Learned, Learned>> mLoading; // the private chat logs and the typed file, read on a worker thread
    bool mLoadStarted = false;
    Learned mLog;           // from the chat logs, once loaded
    Learned mTyped;         // typed or picked on this keyboard in private messages, weighted higher; kept
    bool mTypedLoaded = false, mTypedDirty = false;
    U32 mForgotten = 0, mLoadForgotten = 0; // sForgotten when last let go of, and when the load began
    static U32 sForgotten;
    std::future<word_map_t> mDictionaryLoading;
    word_map_t mDictionary;      // the keyboard language's word list: each word's rank, 0 the commonest
    std::string mDictionaryCode; // whose it is
    LLWString mWord;        // the word before the text cursor, as typed
    std::string mPrevious;  // the word before that in the same sentence, lower case; or empty
    std::string mContext;   // what mSuggestions were worked out for
    LLWString mSuggestions[3];

    // Swipe typing. The words whose key path is most like `path` (image pixels), likeliest
    // first, lower case; how many were found.
    S32 swipeWords(const std::vector<LLVector2>& path, LLWString out[3]) const;
    bool mSwiping = false;
    Hit mSwipeKey;                  // the key it started on
    bool mSwipeLeftKey = false;     // the laser has been off that key
    bool mSwipeSure = false;        // it went far and long enough to be a swipe, see SWIPE_MIN_PATH
    F32 mSwipeLength = 0.f;         // pixels the laser went
    F64 mSwipeStarted = 0.0;
    std::vector<LLVector2> mSwipePath;
    struct TrailPoint { LLVector2 pos; F64 time; };
    std::vector<TrailPoint> mTrail; // fades from its oldest point; stays after the release until gone
    LLWString mSwiped;              // the word a swipe typed, with its space, while still just before the cursor
    LLWString mSwipeOthers[2];      // the next likeliest words, offered in its place
};

#endif
