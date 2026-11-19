/**
 * @file llvrkeyboard.cpp
 * @brief SLVR: the on-screen keyboard shown in the headset, on a layer of its own.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvrkeyboard.h"

#include "lldir.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llrender.h"
#include "lltexteditor.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    enum EAction { TEXT, SPACE, BACKSPACE, ENTER, SHIFT, PAGE, HIDE };
    struct KeyDef { const char* text; F32 width; EAction action; };
    struct RowDef { F32 indent; std::vector<KeyDef> keys; };

    // Turkish Q, laid out like the headset's own keyboard: 13 units a row. Shift turns the
    // letters upper case; the second page has the digits and symbols. UTF-8 as byte
    // escapes so the source file's encoding cannot matter.
    // shortcut: one fixed layout; make it a setting when another language needs typing.
    const RowDef PAGES[2][4] = {
        {
            { 0.f, { { "q", 1, TEXT }, { "w", 1, TEXT }, { "e", 1, TEXT }, { "r", 1, TEXT }, { "t", 1, TEXT }, { "y", 1, TEXT },
                     { "u", 1, TEXT }, { "\xC4\xB1", 1, TEXT }, { "o", 1, TEXT }, { "p", 1, TEXT }, { "\xC4\x9F", 1, TEXT },
                     { "\xC3\xBC", 1, TEXT }, { "", 1, BACKSPACE } } },
            { 0.5f, { { "a", 1, TEXT }, { "s", 1, TEXT }, { "d", 1, TEXT }, { "f", 1, TEXT }, { "g", 1, TEXT }, { "h", 1, TEXT },
                      { "j", 1, TEXT }, { "k", 1, TEXT }, { "l", 1, TEXT }, { "\xC5\x9F", 1, TEXT }, { "i", 1, TEXT },
                      { "", 1.5f, ENTER } } },
            { 0.f, { { "", 2, SHIFT }, { "z", 1, TEXT }, { "x", 1, TEXT }, { "c", 1, TEXT }, { "v", 1, TEXT }, { "b", 1, TEXT },
                     { "n", 1, TEXT }, { "m", 1, TEXT }, { "\xC3\xB6", 1, TEXT }, { "\xC3\xA7", 1, TEXT }, { ",", 1, TEXT },
                     { ".", 1, TEXT } } },
            { 0.f, { { "?123", 2, PAGE }, { ":)", 1, TEXT }, { "", 6, SPACE }, { "?", 1, TEXT }, { "!", 1, TEXT }, { "", 2, HIDE } } },
        },
        {
            { 0.f, { { "1", 1, TEXT }, { "2", 1, TEXT }, { "3", 1, TEXT }, { "4", 1, TEXT }, { "5", 1, TEXT }, { "6", 1, TEXT },
                     { "7", 1, TEXT }, { "8", 1, TEXT }, { "9", 1, TEXT }, { "0", 1, TEXT }, { "-", 1, TEXT }, { "/", 1, TEXT },
                     { "", 1, BACKSPACE } } },
            { 0.5f, { { "@", 1, TEXT }, { "#", 1, TEXT }, { "$", 1, TEXT }, { "&", 1, TEXT }, { "*", 1, TEXT }, { "(", 1, TEXT },
                      { ")", 1, TEXT }, { "'", 1, TEXT }, { "\"", 1, TEXT }, { "_", 1, TEXT }, { "+", 1, TEXT },
                      { "", 1.5f, ENTER } } },
            { 0.f, { { ":(", 2, TEXT }, { "%", 1, TEXT }, { "~", 1, TEXT }, { ":", 1, TEXT }, { ";", 1, TEXT }, { "<", 1, TEXT },
                     { ">", 1, TEXT }, { "[", 1, TEXT }, { "]", 1, TEXT }, { "=", 1, TEXT }, { "|", 1, TEXT }, { "^", 1, TEXT } } },
            { 0.f, { { "ABC", 2, PAGE }, { ";)", 1, TEXT }, { "", 6, SPACE }, { ",", 1, TEXT }, { ".", 1, TEXT }, { "", 2, HIDE } } },
        },
    };

    // Image layout, pixels from the bottom left. Keys are UNIT square, rows hang down from
    // KEYS_TOP; the suggestion bar sits above them, the drag handle below the body.
    const F32 MARGIN = 24.f;
    const F32 UNIT = (LLVRKeyboard::WIDTH - 2.f * MARGIN) / 13.f;
    const F32 KEYS_TOP = 440.f;
    const F32 SUGGEST_BOTTOM = 452.f, SUGGEST_TOP = 524.f;
    const F32 BODY_BOTTOM = 50.f, BODY_RADIUS = 28.f;
    const F32 HANDLE_HALF = 130.f, HANDLE_BOTTOM = LLVRKeyboard::HANDLE_Y - 11.f, HANDLE_TOP = LLVRKeyboard::HANDLE_Y + 11.f, HANDLE_REACH = 220.f;
    const F32 KEY_GAP = 5.f, KEY_RADIUS = 12.f;

    const size_t LOG_BUDGET = 8 << 20; // bytes of chat log read for word completion, newest first
    const U32 TYPED_WEIGHT = 5;        // a word typed here counts as much as this many in the logs

    struct KeyBox { KeyDef def; LLRectf rect; };

    const std::vector<KeyBox>& keyBoxes(S32 page)
    {
        static std::vector<KeyBox> boxes[2];
        if (boxes[page].empty())
        {
            for (S32 row = 0; row < 4; ++row)
            {
                F32 left = MARGIN + PAGES[page][row].indent * UNIT;
                const F32 top = KEYS_TOP - row * UNIT;
                for (const KeyDef& key : PAGES[page][row].keys)
                {
                    boxes[page].push_back({ key, LLRectf(left, top, left + key.width * UNIT, top - UNIT) });
                    left += key.width * UNIT;
                }
            }
        }
        return boxes[page];
    }

    LLRectf suggestionRect(S32 slot)
    {
        const F32 width = (LLVRKeyboard::WIDTH - 2.f * MARGIN) / 3.f;
        return LLRectf(MARGIN + slot * width, SUGGEST_TOP, MARGIN + (slot + 1) * width, SUGGEST_BOTTOM);
    }

    bool isLetter(llwchar c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
    }

    // Case mapping for the letters this keyboard has (Turkish dotted and dotless i included).
    llwchar lower(llwchar c)
    {
        if (c == 0x130) return 'i';
        if (c >= 'A' && c <= 'Z') return c + 32;
        if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
        if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && !(c & 1)) return c + 1;
        return c;
    }

    llwchar upper(llwchar c)
    {
        if (c == 'i') return 0x130;
        if (c == 0x131) return 'I';
        if (c >= 'a' && c <= 'z') return c - 32;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        if (c >= 0x100 && c <= 0x17F && c != 0x131 && (c & 1)) return c - 1;
        return c;
    }

    std::string wordKey(const LLWString& word)
    {
        LLWString key = word;
        std::transform(key.begin(), key.end(), key.begin(), lower);
        return wstring_to_utf8str(key);
    }

    // Counts the words (letters only, 2 to 24 of them) in a text.
    void countWords(const LLWString& text, std::map<std::string, U32>& words, U32 weight)
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            if (i < text.size() && isLetter(text[i]))
            {
                continue;
            }
            if (i - start >= 2 && i - start <= 24)
            {
                words[wordKey(text.substr(start, i - start))] += weight;
            }
            start = i + 1;
        }
    }

    // Worker thread: the words of the newest chat logs, up to LOG_BUDGET bytes.
    std::map<std::string, U32> loadLogWords(std::string dir)
    {
        namespace fs = std::filesystem;
        std::map<std::string, U32> words;
        std::error_code error;
        std::vector<fs::directory_entry> logs;
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(ll_convert_string_to_wide(dir)), error))
        {
            if (entry.is_regular_file(error) && entry.path().extension() == L".txt")
            {
                logs.push_back(entry);
            }
        }
        std::sort(logs.begin(), logs.end(), [&](const fs::directory_entry& a, const fs::directory_entry& b)
                  { return a.last_write_time(error) > b.last_write_time(error); });
        size_t budget = LOG_BUDGET;
        for (const fs::directory_entry& log : logs)
        {
            std::ifstream file(log.path(), std::ios::binary);
            std::string text(std::min(budget, (size_t)log.file_size(error)), '\0');
            file.read(text.data(), text.size());
            text.resize((size_t)file.gcount());
            countWords(utf8str_to_wstring(text), words, 1);
            budget -= text.size();
            if (budget == 0)
            {
                break;
            }
        }
        LL_INFOS("VR") << "Word completion: " << words.size() << " words from " << logs.size() << " chat logs" << LL_ENDL;
        return words;
    }

    // The focused text field's text and cursor; false when the focus is not a text field.
    bool focusedText(LLWString& text, S32& cursor)
    {
        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
        if (LLLineEditor* line = dynamic_cast<LLLineEditor*>(focus))
        {
            text = line->getWText();
            cursor = line->getCursor();
            return true;
        }
        if (LLTextEditor* editor = dynamic_cast<LLTextEditor*>(focus))
        {
            text = editor->getWText();
            cursor = editor->getCursorPos();
            return true;
        }
        return false;
    }

    // Drawing in image pixels. Untextured, in the current 2D projection.
    void triangle(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2)
    {
        gGL.vertex2f(x0, y0);
        gGL.vertex2f(x1, y1);
        gGL.vertex2f(x2, y2);
    }

    void quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3)
    {
        triangle(x0, y0, x1, y1, x2, y2);
        triangle(x0, y0, x2, y2, x3, y3);
    }

    void roundedRect(const LLRectf& r, F32 radius, const LLColor4& color)
    {
        radius = llmin(radius, r.getWidth() / 2.f, r.getHeight() / 2.f);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        quad(r.mLeft + radius, r.mBottom, r.mRight - radius, r.mBottom, r.mRight - radius, r.mTop, r.mLeft + radius, r.mTop);
        quad(r.mLeft, r.mBottom + radius, r.mLeft + radius, r.mBottom + radius, r.mLeft + radius, r.mTop - radius, r.mLeft, r.mTop - radius);
        quad(r.mRight - radius, r.mBottom + radius, r.mRight, r.mBottom + radius, r.mRight, r.mTop - radius, r.mRight - radius, r.mTop - radius);
        const F32 cx[4] = { r.mRight - radius, r.mLeft + radius, r.mLeft + radius, r.mRight - radius };
        const F32 cy[4] = { r.mTop - radius, r.mTop - radius, r.mBottom + radius, r.mBottom + radius };
        const S32 STEPS = 6;
        for (S32 corner = 0; corner < 4; ++corner)
        {
            for (S32 i = 0; i < STEPS; ++i)
            {
                const F32 a0 = (corner + (F32)i / STEPS) * F_PI_BY_TWO, a1 = (corner + (F32)(i + 1) / STEPS) * F_PI_BY_TWO;
                triangle(cx[corner], cy[corner], cx[corner] + radius * cosf(a0), cy[corner] + radius * sinf(a0),
                         cx[corner] + radius * cosf(a1), cy[corner] + radius * sinf(a1));
            }
        }
        gGL.end();
    }

    // A stroke of the given width along a polyline.
    void stroke(const std::vector<LLVector2>& points, F32 width, const LLColor4& color, bool closed = false)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        const size_t count = points.size();
        for (size_t i = 0; i + (closed ? 0 : 1) < count; ++i)
        {
            const LLVector2& a = points[i];
            const LLVector2& b = points[(i + 1) % count];
            LLVector2 normal(a.mV[VY] - b.mV[VY], b.mV[VX] - a.mV[VX]);
            normal.normVec();
            normal *= width / 2.f;
            quad(a.mV[VX] - normal.mV[VX], a.mV[VY] - normal.mV[VY], b.mV[VX] - normal.mV[VX], b.mV[VY] - normal.mV[VY],
                 b.mV[VX] + normal.mV[VX], b.mV[VY] + normal.mV[VY], a.mV[VX] + normal.mV[VX], a.mV[VY] + normal.mV[VY]);
            // Square caps hide the notches where segments meet.
            const F32 h = width / 2.f;
            quad(b.mV[VX] - h, b.mV[VY] - h, b.mV[VX] + h, b.mV[VY] - h, b.mV[VX] + h, b.mV[VY] + h, b.mV[VX] - h, b.mV[VY] + h);
        }
        gGL.end();
    }

    // Icons for the keys that have no letter, s = half their size.
    void drawIcon(EAction action, F32 cx, F32 cy, F32 s, bool lit, bool locked, const LLColor4& color)
    {
        const F32 w = s * 0.16f;
        switch (action)
        {
        case BACKSPACE:
            stroke({ { cx - s, cy }, { cx - s * 0.45f, cy + s * 0.55f }, { cx + s, cy + s * 0.55f },
                     { cx + s, cy - s * 0.55f }, { cx - s * 0.45f, cy - s * 0.55f } }, w, color, true);
            stroke({ { cx - s * 0.05f, cy + s * 0.25f }, { cx + s * 0.5f, cy - s * 0.25f } }, w, color);
            stroke({ { cx - s * 0.05f, cy - s * 0.25f }, { cx + s * 0.5f, cy + s * 0.25f } }, w, color);
            break;
        case ENTER:
            stroke({ { cx + s * 0.7f, cy + s * 0.6f }, { cx + s * 0.7f, cy - s * 0.2f }, { cx - s * 0.8f, cy - s * 0.2f } }, w, color);
            stroke({ { cx - s * 0.35f, cy + s * 0.25f }, { cx - s * 0.8f, cy - s * 0.2f }, { cx - s * 0.35f, cy - s * 0.65f } }, w, color);
            break;
        case SHIFT:
        {
            const std::vector<LLVector2> arrow = { { cx, cy + s * 0.85f }, { cx + s * 0.8f, cy + s * 0.05f }, { cx + s * 0.38f, cy + s * 0.05f },
                                                   { cx + s * 0.38f, cy - s * 0.55f }, { cx - s * 0.38f, cy - s * 0.55f },
                                                   { cx - s * 0.38f, cy + s * 0.05f }, { cx - s * 0.8f, cy + s * 0.05f } };
            if (lit)
            {
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.color4fv(color.mV);
                gGL.begin(LLRender::TRIANGLES);
                triangle(cx, cy + s * 0.85f, cx + s * 0.8f, cy + s * 0.05f, cx - s * 0.8f, cy + s * 0.05f);
                quad(cx - s * 0.38f, cy - s * 0.55f, cx + s * 0.38f, cy - s * 0.55f, cx + s * 0.38f, cy + s * 0.05f, cx - s * 0.38f, cy + s * 0.05f);
                gGL.end();
            }
            stroke(arrow, w, color, true);
            if (locked)
            {
                stroke({ { cx - s * 0.5f, cy - s * 0.85f }, { cx + s * 0.5f, cy - s * 0.85f } }, w, color);
            }
            break;
        }
        case HIDE:
            stroke({ { cx - s * 0.6f, cy + s * 0.25f }, { cx, cy - s * 0.3f }, { cx + s * 0.6f, cy + s * 0.25f } }, w, color);
            break;
        default:
            break;
        }
    }

    void label(const LLWString& text, F32 cx, F32 cy, const LLColor4& color)
    {
        if (const LLFontGL* font = LLFontGL::getFontSansSerifHuge())
        {
            // Fonts take UI units, which the UI scale turns into pixels.
            font->render(text, 0, cx / LLFontGL::sScaleX, cy / LLFontGL::sScaleY, color, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
}

LLVRKeyboard::Hit LLVRKeyboard::hitAt(S32 px, S32 py) const
{
    const F32 x = (F32)px + 0.5f, y = (F32)py + 0.5f;
    Hit hit;
    if (x < 0.f || x >= WIDTH || y < 0.f || y >= HEIGHT)
    {
        return hit;
    }
    for (S32 slot = 0; slot < 3; ++slot)
    {
        if (suggestionRect(slot).pointInRect(x, y))
        {
            hit.part = SUGGESTION;
            hit.index = slot;
            return hit;
        }
    }
    const std::vector<KeyBox>& boxes = keyBoxes(mPage);
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        if (boxes[i].rect.pointInRect(x, y))
        {
            hit.part = KEY;
            hit.index = (S32)i;
            return hit;
        }
    }
    if (fabsf(x - WIDTH / 2.f) < HANDLE_REACH && y < BODY_BOTTOM)
    {
        hit.part = HANDLE;
        return hit;
    }
    hit.part = y >= BODY_BOTTOM ? BODY : NONE;
    return hit;
}

void LLVRKeyboard::draw(const Hit hover[2]) const
{
    auto hovered = [&](const Hit& hit) { return hover[0] == hit || hover[1] == hit; };
    const LLColor4 text_color(0.93f, 0.94f, 0.96f, 1.f);
    const LLColor4 hover_color(0.28f, 0.47f, 0.82f, 1.f);

    roundedRect(LLRectf(4.f, HEIGHT - 4.f, WIDTH - 4.f, BODY_BOTTOM), BODY_RADIUS, LLColor4(0.09f, 0.1f, 0.12f, 0.96f));

    for (S32 slot = 0; slot < 3; ++slot)
    {
        const LLRectf r = suggestionRect(slot);
        if (!mSuggestions[slot].empty() && hovered({ SUGGESTION, slot }))
        {
            roundedRect(r, KEY_RADIUS, hover_color);
        }
        if (slot > 0)
        {
            roundedRect(LLRectf(r.mLeft - 1.f, r.mTop - 14.f, r.mLeft + 1.f, r.mBottom + 14.f), 1.f, LLColor4(1.f, 1.f, 1.f, 0.18f));
        }
        if (!mSuggestions[slot].empty())
        {
            label(mSuggestions[slot], r.getCenterX(), r.getCenterY(), text_color);
        }
    }

    const std::vector<KeyBox>& boxes = keyBoxes(mPage);
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const KeyDef& key = boxes[i].def;
        const LLRectf& box = boxes[i].rect;
        const LLRectf r(box.mLeft + KEY_GAP, box.mTop - KEY_GAP, box.mRight - KEY_GAP, box.mBottom + KEY_GAP);
        LLColor4 color = key.action == TEXT || key.action == SPACE ? LLColor4(0.23f, 0.25f, 0.28f, 1.f)
                                                                     : LLColor4(0.16f, 0.17f, 0.2f, 1.f);
        if (key.action == ENTER)
        {
            color = LLColor4(0.1f, 0.42f, 0.9f, 1.f);
        }
        if (hovered({ KEY, (S32)i }))
        {
            color = key.action == ENTER ? LLColor4(0.3f, 0.6f, 1.f, 1.f) : hover_color;
        }
        roundedRect(r, KEY_RADIUS, color);

        if (key.action == TEXT || key.action == PAGE)
        {
            LLWString text = utf8str_to_wstring(key.text);
            if (key.action == TEXT && mPage == 0 && text.size() == 1 && (mShift || mCapsLock))
            {
                text[0] = upper(text[0]);
            }
            label(text, r.getCenterX(), r.getCenterY(), text_color);
        }
        else if (key.action == SPACE)
        {
            roundedRect(LLRectf(r.getCenterX() - UNIT, r.mBottom + UNIT * 0.32f, r.getCenterX() + UNIT, r.mBottom + UNIT * 0.26f),
                        2.f, LLColor4(1.f, 1.f, 1.f, 0.25f));
        }
        else
        {
            drawIcon(key.action, r.getCenterX(), r.getCenterY(), UNIT * 0.28f, mShift || mCapsLock, mCapsLock, text_color);
        }
    }

    const bool handle_hover = hovered({ HANDLE, -1 });
    roundedRect(LLRectf(WIDTH / 2.f - HANDLE_HALF, HANDLE_TOP, WIDTH / 2.f + HANDLE_HALF, HANDLE_BOTTOM), 11.f,
                handle_hover ? LLColor4(0.95f, 0.95f, 0.97f, 0.95f) : LLColor4(0.75f, 0.77f, 0.8f, 0.8f));
}

bool LLVRKeyboard::press(const Hit& hit)
{
    if (hit.part == SUGGESTION && hit.index >= 0 && hit.index < 3 && !mSuggestions[hit.index].empty())
    {
        // Replace the word being typed by the suggestion, as typed keys would.
        const LLWString word = mSuggestions[hit.index];
        for (size_t i = 0; i < mWord.size(); ++i)
        {
            gViewerWindow->handleKey(KEY_BACKSPACE, MASK_NONE);
        }
        type(word + LLWString(1, ' '));
        mTypedWords[wordKey(word)] += 1;
        return false;
    }
    const std::vector<KeyBox>& boxes = keyBoxes(mPage);
    if (hit.part != KEY || hit.index < 0 || hit.index >= (S32)boxes.size())
    {
        return false;
    }
    const KeyDef& key = boxes[hit.index].def;
    switch (key.action)
    {
    case TEXT:
    {
        LLWString text = utf8str_to_wstring(key.text);
        if (mPage == 0 && text.size() == 1 && (mShift || mCapsLock))
        {
            text[0] = upper(text[0]);
            mShift = mCapsLock; // shift is for one letter, caps lock stays
        }
        type(text);
        return false;
    }
    case SPACE:
        type(LLWString(1, ' '));
        return false;
    case BACKSPACE:
        gViewerWindow->handleKey(KEY_BACKSPACE, MASK_NONE);
        return true;
    case ENTER:
    {
        LLWString text;
        S32 cursor = 0;
        if (focusedText(text, cursor))
        {
            learnText(text, 1); // what the user writes is what they will write again
        }
        gViewerWindow->handleKey(KEY_RETURN, MASK_NONE);
        return false;
    }
    case SHIFT:
        // Once for one capital, twice for caps lock, a third time off.
        if (!mShift && !mCapsLock)
        {
            mShift = true;
        }
        else if (!mCapsLock)
        {
            mCapsLock = true;
        }
        else
        {
            mShift = mCapsLock = false;
        }
        return false;
    case PAGE:
        mPage = 1 - mPage;
        return false;
    case HIDE:
        gFocusMgr.setKeyboardFocus(nullptr);
        return false;
    }
    return false;
}

void LLVRKeyboard::type(const LLWString& text)
{
    for (llwchar c : text)
    {
        gViewerWindow->handleUnicodeChar(c, MASK_NONE);
    }
}

void LLVRKeyboard::learnText(const LLWString& text, U32 weight)
{
    countWords(text, mTypedWords, weight);
}

void LLVRKeyboard::reset()
{
    mPage = 0;
    mShift = mCapsLock = false;
}

void LLVRKeyboard::update()
{
    if (!mLoadStarted)
    {
        const std::string dir = gDirUtilp->getPerAccountChatLogsDir();
        if (!dir.empty()) // empty until logged in
        {
            mLoadStarted = true;
            mLoading = std::async(std::launch::async, loadLogWords, dir);
        }
    }
    if (mLoading.valid() && mLoading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        mLogWords = mLoading.get();
        mWordKey.clear(); // suggest again with the new words
    }

    LLWString text;
    S32 cursor = 0;
    mWord.clear();
    if (focusedText(text, cursor))
    {
        S32 start = llclamp(cursor, 0, (S32)text.size());
        while (start > 0 && isLetter(text[start - 1]))
        {
            --start;
        }
        mWord = text.substr(start, cursor - start);
    }
    const std::string key = wordKey(mWord);
    if (key != mWordKey || mWord.empty())
    {
        mWordKey = key;
        refreshSuggestions();
    }
}

// The three most frequent known words that the word being typed begins, in its case.
void LLVRKeyboard::refreshSuggestions()
{
    for (LLWString& suggestion : mSuggestions)
    {
        suggestion.clear();
    }
    if (mWordKey.empty())
    {
        return;
    }
    std::map<std::string, U32> scores;
    for (const word_map_t* words : { &mLogWords, &mTypedWords })
    {
        const U32 weight = words == &mTypedWords ? TYPED_WEIGHT : 1;
        for (auto it = words->lower_bound(mWordKey); it != words->end() && it->first.compare(0, mWordKey.size(), mWordKey) == 0; ++it)
        {
            if (it->first.size() > mWordKey.size())
            {
                scores[it->first] += it->second * weight;
            }
        }
    }
    std::vector<std::pair<U32, std::string>> ranked;
    for (const auto& score : scores)
    {
        ranked.emplace_back(score.second, score.first);
    }
    const size_t count = llmin(ranked.size(), (size_t)3);
    std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    const bool capital = upper(mWord[0]) == mWord[0] && lower(mWord[0]) != mWord[0];
    const bool all_capitals = capital && mWord.size() > 1 && upper(mWord[1]) == mWord[1] && lower(mWord[1]) != mWord[1];
    for (size_t i = 0; i < count; ++i)
    {
        LLWString word = utf8str_to_wstring(ranked[i].second);
        for (size_t c = 0; c < word.size(); ++c)
        {
            if (all_capitals || (capital && c == 0))
            {
                word[c] = upper(word[c]);
            }
        }
        mSuggestions[i] = word;
    }
}

// Runs once when VR is switched on.
void LLVRKeyboard::selfCheck()
{
    // Every page fills its 13 units a row, and the keys do not overlap the suggestion bar.
    for (S32 page = 0; page < 2; ++page)
    {
        for (const RowDef& row : PAGES[page])
        {
            F32 width = row.indent;
            for (const KeyDef& key : row.keys)
            {
                width += key.width;
            }
            llassert_always(fabsf(width - 13.f) < 0.01f);
        }
    }
    llassert_always(KEYS_TOP < SUGGEST_BOTTOM && KEYS_TOP - 4.f * UNIT > BODY_BOTTOM);

    // Turkish case mapping both ways, and words split on anything but letters.
    llassert_always(upper('i') == 0x130 && upper(0x131) == 'I' && lower(0x130) == 'i' && upper(0x15F) == 0x15E && lower(0xC7) == 0xE7);
    std::map<std::string, U32> words;
    countWords(utf8str_to_wstring("[12:00] Ali: Merhaba, merhaba d\xC3\xBCnya! a"), words, 1);
    llassert_always(words.size() == 3 && words["merhaba"] == 2 && words["d\xC3\xBCnya"] == 1 && words["ali"] == 1);

    // What lies where.
    LLVRKeyboard keyboard;
    const LLRectf q = keyBoxes(0)[0].rect;
    const Hit key = keyboard.hitAt((S32)q.getCenterX(), (S32)q.getCenterY());
    llassert_always(key.part == KEY && key.index == 0 && keyBoxes(0)[0].def.action == TEXT);
    llassert_always(keyboard.hitAt(WIDTH / 2, 20).part == HANDLE);
    llassert_always(keyboard.hitAt((S32)MARGIN + 10, (S32)SUGGEST_BOTTOM + 10).part == SUGGESTION);
    llassert_always(keyboard.hitAt(WIDTH + 5, 20).part == NONE);

    // Completion: most frequent first, in the case typed.
    keyboard.mTypedWords["merhaba"] = 1;
    keyboard.mTypedWords["merhem"] = 2;
    keyboard.mWord = utf8str_to_wstring("Mer");
    keyboard.mWordKey = "mer";
    keyboard.refreshSuggestions();
    llassert_always(wstring_to_utf8str(keyboard.mSuggestions[0]) == "Merhem" && wstring_to_utf8str(keyboard.mSuggestions[1]) == "Merhaba");
}
