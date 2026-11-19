/**
 * @file llvrkeyboard.h
 * @brief SLVR: the on-screen keyboard shown in the headset, on a layer of its own.
 *
 * Draws itself into a fixed-size image and says what lies at a point of it; where the image
 * is placed and how the lasers reach it is up to LLViewerVR. Types into whatever text field
 * has the keyboard focus, like the real keyboard, and suggests word completions learned
 * from the chat logs and from what is typed.
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

    // Trigger pressed on a key or suggestion. True for a key that repeats while held.
    bool press(const Hit& hit);

    // Back to letters, shift off; when the keyboard goes away.
    void reset();

    static void selfCheck();

private:
    void type(const LLWString& text);
    void learnText(const LLWString& text, U32 weight);
    void refreshSuggestions();

    S32 mPage = 0;          // 0 letters, 1 digits and symbols
    bool mShift = false, mCapsLock = false;

    // Word completion. Words are kept lower case (UTF-8) with how often they were seen.
    typedef std::map<std::string, U32> word_map_t;
    std::future<word_map_t> mLoading; // the chat logs, read on a worker thread
    bool mLoadStarted = false;
    word_map_t mLogWords;   // from the chat logs, once loaded
    word_map_t mTypedWords; // typed or picked on this keyboard, weighted higher
    LLWString mWord;        // the word before the text cursor, as typed
    std::string mWordKey;   // what mSuggestions were worked out for
    LLWString mSuggestions[3];
};

#endif
