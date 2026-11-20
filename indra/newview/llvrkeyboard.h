/**
 * @file llvrkeyboard.h
 * @brief SLVR: the on-screen keyboard shown in the headset, on a layer of its own.
 *
 * Draws itself into a fixed-size image and says what lies at a point of it; where the image
 * is placed and how the lasers reach it is up to LLViewerVR. Types into whatever text field
 * has the keyboard focus, like the real keyboard, and suggests word completions and the
 * next word, learned from the chat logs and from what is typed. The letters come from the
 * layout chosen in the SLVR settings (see llvrlanguage.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#ifndef LL_LLVRKEYBOARD_H
#define LL_LLVRKEYBOARD_H

#include "llstring.h"

#include <future>
#include <map>
#include <string>

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
    // hover: what each hand's laser is on.
    void draw(const Hit hover[2]) const;

    // Trigger pressed on a key or suggestion: typed, typed and repeats while held
    // (backspace), or the settings key, which the caller answers.
    enum EPress { TYPED, REPEATS, SETTINGS };
    EPress press(const Hit& hit);

    // Back to letters, shift off; when the keyboard goes away.
    void reset();

    static void selfCheck();

    typedef std::map<std::string, U32> word_map_t;
    struct Learned { word_map_t words, pairs; }; // pairs: "word next", how often the one followed the other

private:
    void type(const LLWString& text);
    void learnText(const LLWString& text, U32 weight);
    void refreshSuggestions();

    S32 mPage = 0;          // 0 letters, 1 digits and symbols
    bool mShift = false, mCapsLock = false;

    // Suggestions. Words are kept lower case (UTF-8) with how often they were seen.
    std::future<Learned> mLoading; // the chat logs, read on a worker thread
    bool mLoadStarted = false;
    Learned mLog;           // from the chat logs, once loaded
    Learned mTyped;         // typed or picked on this keyboard, weighted higher
    LLWString mWord;        // the word before the text cursor, as typed
    std::string mPrevious;  // the word before that in the same sentence, lower case; or empty
    std::string mContext;   // what mSuggestions were worked out for
    LLWString mSuggestions[3];
};

#endif
