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
#include "llframetimer.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llrender.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvrlanguage.h"
#include "llwin32headers.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    enum EAction { TEXT, SPACE, BACKSPACE, ENTER, SHIFT, PAGE, HIDE, GEAR, GAP };
    struct KeyDef { std::string text; F32 width; EAction action; };
    typedef std::vector<std::vector<KeyDef>> rows_t;

    // Rows are 13 units wide, laid out like the headset's own keyboard. The letters come
    // from the layout (see letterRows()); the second page has the digits and symbols.
    const rows_t SYMBOL_ROWS = {
        { { "1", 1, TEXT }, { "2", 1, TEXT }, { "3", 1, TEXT }, { "4", 1, TEXT }, { "5", 1, TEXT }, { "6", 1, TEXT },
          { "7", 1, TEXT }, { "8", 1, TEXT }, { "9", 1, TEXT }, { "0", 1, TEXT }, { "-", 1, TEXT }, { "/", 1, TEXT },
          { "", 1, BACKSPACE } },
        { { "", 0.5f, GAP }, { "@", 1, TEXT }, { "#", 1, TEXT }, { "$", 1, TEXT }, { "&", 1, TEXT }, { "*", 1, TEXT },
          { "(", 1, TEXT }, { ")", 1, TEXT }, { "'", 1, TEXT }, { "\"", 1, TEXT }, { "_", 1, TEXT }, { "+", 1, TEXT },
          { "", 1.5f, ENTER } },
        { { ":(", 1, TEXT }, { ";)", 1, TEXT }, { "%", 1, TEXT }, { "~", 1, TEXT }, { ":", 1, TEXT }, { ";", 1, TEXT },
          { "<", 1, TEXT }, { ">", 1, TEXT }, { "[", 1, TEXT }, { "]", 1, TEXT }, { "=", 1, TEXT }, { "|", 1, TEXT },
          { "^", 1, TEXT } },
        { { "ABC", 2, PAGE }, { ":)", 1, TEXT }, { "", 6, SPACE }, { ",", 1, TEXT }, { ".", 1, TEXT }, { "", 2, HIDE } },
    };
    const std::vector<KeyDef> LETTERS_BOTTOM_ROW = { { "?123", 2, PAGE }, { "", 1, GEAR }, { "", 6, SPACE },
                                                     { "?", 1, TEXT }, { "!", 1, TEXT }, { "", 2, HIDE } };

    // A layout's three rows of letters with backspace, enter, shift, comma and full stop
    // around them. Letters are one unit wide, narrower when a row holds too many; what is
    // left of a row goes to its indent and its keys at the ends.
    rows_t letterRows(const std::vector<std::string>& layout)
    {
        LLWString letters[3];
        for (size_t row = 0; row < 3 && row < layout.size(); ++row)
        {
            letters[row] = utf8str_to_wstring(layout[row]);
        }
        const F32 FIXED[3] = { 1.f, 1.5f, 3.5f }; // the narrowest the keys at the ends of each row get
        F32 unit = 1.f;
        for (S32 row = 0; row < 3; ++row)
        {
            if (!letters[row].empty())
            {
                unit = llmin(unit, (13.f - FIXED[row]) / letters[row].size());
            }
        }
        rows_t rows(4);
        for (S32 row = 0; row < 3; ++row)
        {
            const F32 extra = 13.f - FIXED[row] - letters[row].size() * unit;
            if (row == 2)
            {
                rows[row].push_back({ "", 1.5f + extra / 2.f, SHIFT });
            }
            else if (extra > 0.f)
            {
                rows[row].push_back({ "", extra / 2.f, GAP });
            }
            for (llwchar c : letters[row])
            {
                rows[row].push_back({ wstring_to_utf8str(LLWString(1, c)), unit, TEXT });
            }
            if (row == 0)
            {
                rows[row].push_back({ "", 1.f + extra / 2.f, BACKSPACE });
            }
            else if (row == 1)
            {
                rows[row].push_back({ "", 1.5f + extra / 2.f, ENTER });
            }
            else
            {
                rows[row].push_back({ ",", 1.f + extra / 4.f, TEXT });
                rows[row].push_back({ ".", 1.f + extra / 4.f, TEXT });
            }
        }
        rows[3] = LETTERS_BOTTOM_ROW;
        return rows;
    }

    F32 rowWidth(const std::vector<KeyDef>& row)
    {
        F32 width = 0.f;
        for (const KeyDef& key : row)
        {
            width += key.width;
        }
        return width;
    }

    // Image layout, pixels from the bottom left. Keys are UNIT square, rows hang down from
    // KEYS_TOP; the suggestion bar sits above them, the drag handle below the body.
    const F32 MARGIN = 24.f;
    const F32 UNIT = (LLVRKeyboard::WIDTH - 2.f * MARGIN) / 13.f;
    const F32 KEYS_TOP = 440.f;
    const F32 SUGGEST_BOTTOM = 452.f, SUGGEST_TOP = 524.f;
    const F32 BODY_BOTTOM = 50.f, BODY_RADIUS = 28.f;
    const F32 HANDLE_HALF = 130.f, HANDLE_BOTTOM = LLVRKeyboard::HANDLE_Y - 11.f, HANDLE_TOP = LLVRKeyboard::HANDLE_Y + 11.f, HANDLE_REACH = 220.f;
    const F32 KEY_GAP = 5.f, KEY_RADIUS = 12.f;

    const size_t LOG_BUDGET = 8 << 20; // bytes of chat log read for suggestions, newest first
    const U32 TYPED_WEIGHT = 5;        // a word typed here counts as much as this many in the logs
    const U32 PAIR_WEIGHT = 10;        // and a word seen after the previous one as this many seen anywhere
    const U32 LOG_PAIR_MIN = 2;        // a pair seen fewer times in the logs is forgotten: most are chance

    struct KeyBox { KeyDef def; LLRectf rect; };

    // The keys as laid out for the current layout; one keyboard is shown at a time.
    std::vector<KeyBox> sBoxes[2];
    std::string sLayout;              // the layout sBoxes[0] was built for
    std::atomic<bool> sTurkic{ false }; // Turkish and Azerbaijani case dotted and dotless i apart

    void layOut(const rows_t& rows, std::vector<KeyBox>& boxes)
    {
        boxes.clear();
        for (size_t row = 0; row < rows.size(); ++row)
        {
            F32 left = MARGIN;
            const F32 top = KEYS_TOP - row * UNIT;
            for (const KeyDef& key : rows[row])
            {
                if (key.action != GAP)
                {
                    boxes.push_back({ key, LLRectf(left, top, left + key.width * UNIT, top - UNIT) });
                }
                left += key.width * UNIT;
            }
        }
    }

    const std::vector<KeyBox>& keyBoxes(S32 page)
    {
        if (sBoxes[1].empty())
        {
            layOut(SYMBOL_ROWS, sBoxes[1]);
        }
        const LLVRLanguage::Language& layout = LLVRLanguage::keyboard();
        if (layout.code != sLayout || sBoxes[0].empty())
        {
            sLayout = layout.code;
            sTurkic = sLayout == "tr" || sLayout == "az";
            layOut(letterRows(layout.keyboard), sBoxes[0]);
        }
        return sBoxes[page];
    }

    LLRectf suggestionRect(S32 slot)
    {
        const F32 width = (LLVRKeyboard::WIDTH - 2.f * MARGIN) / 3.f;
        return LLRectf(MARGIN + slot * width, SUGGEST_TOP, MARGIN + (slot + 1) * width, SUGGEST_BOTTOM);
    }

    // Letters and case by Windows' own tables, so every layout's letters are known; only
    // Turkish and Azerbaijani i need their own rule.
    bool isLetter(llwchar c)
    {
        return c < 0x10000 && IsCharAlphaW((WCHAR)c);
    }

    llwchar lower(llwchar c)
    {
        if (c == 0x130) return 'i';
        if (c == 'I' && sTurkic) return 0x131;
        return c < 0x10000 ? (llwchar)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c) : c;
    }

    llwchar upper(llwchar c)
    {
        if (c == 'i' && sTurkic) return 0x130;
        if (c == 0x131) return 'I';
        return c < 0x10000 ? (llwchar)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)c) : c;
    }

    std::string wordKey(const LLWString& word)
    {
        LLWString key = word;
        std::transform(key.begin(), key.end(), key.begin(), lower);
        return wstring_to_utf8str(key);
    }

    // Counts the words (2 to 24 letters) in a text, and which word followed which within a
    // sentence: across spaces and commas, not across other punctuation.
    void countText(const LLWString& text, LLVRKeyboard::Learned& learned, U32 weight)
    {
        std::string previous;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            if (i < text.size() && isLetter(text[i]))
            {
                continue;
            }
            const size_t length = i - start;
            if (length > 24)
            {
                previous.clear();
            }
            else if (length > 0)
            {
                const std::string word = wordKey(text.substr(start, length));
                if (length >= 2)
                {
                    learned.words[word] += weight;
                }
                if (!previous.empty())
                {
                    learned.pairs[previous + ' ' + word] += weight;
                }
                previous = word;
            }
            if (i < text.size() && text[i] != ' ' && text[i] != ',')
            {
                previous.clear();
            }
            start = i + 1;
        }
    }

    // A chat log line's message: "[2026/11/20 21:03]  Name Resident: text" gives "text".
    LLWString logMessage(const LLWString& line)
    {
        size_t start = 0;
        if (!line.empty() && line[0] == '[')
        {
            const size_t close = line.find(']');
            start = close == LLWString::npos ? 0 : close + 1;
        }
        const size_t colon = line.find(':', start);
        if (colon != LLWString::npos && colon - start < 64) // names are short; a later colon is the message's
        {
            start = colon + 1;
        }
        return line.substr(start);
    }

    // Worker thread: what the newest chat logs say, up to LOG_BUDGET bytes.
    LLVRKeyboard::Learned loadLog(std::string dir)
    {
        namespace fs = std::filesystem;
        LLVRKeyboard::Learned learned;
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
            budget -= text.size();
            size_t line_start = 0;
            while (line_start < text.size())
            {
                size_t line_end = text.find('\n', line_start);
                line_end = line_end == std::string::npos ? text.size() : line_end;
                countText(logMessage(utf8str_to_wstring(text.substr(line_start, line_end - line_start))), learned, 1);
                line_start = line_end + 1;
            }
            if (budget == 0)
            {
                break;
            }
        }
        for (auto it = learned.pairs.begin(); it != learned.pairs.end();)
        {
            it = it->second < LOG_PAIR_MIN ? learned.pairs.erase(it) : std::next(it);
        }
        LL_INFOS("VR") << "Word suggestions: " << learned.words.size() << " words, " << learned.pairs.size()
                       << " word pairs from " << logs.size() << " chat logs" << LL_ENDL;
        return learned;
    }

    // Calls f(key, count) for the entries of a word map that begin with prefix.
    template <typename F>
    void withPrefix(const LLVRKeyboard::word_map_t& words, const std::string& prefix, F f)
    {
        for (auto it = words.lower_bound(prefix); it != words.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
        {
            f(it->first, it->second);
        }
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
        case GEAR:
        {
            // A gear: eight teeth round a ring.
            std::vector<LLVector2> outline, hole;
            const S32 TEETH = 8;
            for (S32 tooth = 0; tooth < TEETH; ++tooth)
            {
                const F32 a = F_TWO_PI * tooth / TEETH;
                const F32 offsets[4] = { -0.3f, -0.17f, 0.17f, 0.3f }, radii[4] = { 0.62f, 0.9f, 0.9f, 0.62f };
                for (S32 i = 0; i < 4; ++i)
                {
                    outline.emplace_back(cx + s * radii[i] * cosf(a + offsets[i]), cy + s * radii[i] * sinf(a + offsets[i]));
                }
            }
            for (S32 i = 0; i < 16; ++i)
            {
                hole.emplace_back(cx + s * 0.28f * cosf(F_TWO_PI * i / 16), cy + s * 0.28f * sinf(F_TWO_PI * i / 16));
            }
            stroke(outline, w, color, true);
            stroke(hole, w, color, true);
            break;
        }
        default:
            break;
        }
    }

    // Swipe typing: a word's path is the line through its letters' keys; the one the laser
    // drew is compared with each word's at SWIPE_SAMPLES points spread evenly along both.
    // A word is kept when they lie within SWIPE_MOST_APART keys of each other on average.
    const S32 SWIPE_SAMPLES = 32;
    const F32 SWIPE_MOST_APART = 1.2f;  // keys
    const F32 SWIPE_SHAPE_WEIGHT = 12.f; // against the log of how often a word was seen
    const F32 SWIPE_POINT_STEP = 4.f;   // pixels the laser moves before a new point is kept
    const F32 TRAIL_LIFE = 0.45f;       // seconds a point of the trail takes to fade

    // Spreads a path's length evenly over n points.
    std::vector<LLVector2> resample(const std::vector<LLVector2>& path, S32 n)
    {
        std::vector<LLVector2> out;
        if (path.empty())
        {
            return out;
        }
        F32 total = 0.f;
        for (size_t i = 1; i < path.size(); ++i)
        {
            total += dist_vec(path[i - 1], path[i]);
        }
        if (total <= 0.f)
        {
            return std::vector<LLVector2>(n, path[0]);
        }
        const F32 step = total / (n - 1);
        out.push_back(path[0]);
        F32 along = 0.f; // length of the path before point i - 1
        size_t i = 1;
        for (S32 k = 1; k < n - 1; ++k)
        {
            const F32 target = step * k;
            while (i < path.size() - 1 && along + dist_vec(path[i - 1], path[i]) < target)
            {
                along += dist_vec(path[i - 1], path[i]);
                ++i;
            }
            const F32 segment = dist_vec(path[i - 1], path[i]);
            out.push_back(lerp(path[i - 1], path[i], segment > 0.f ? llclamp((target - along) / segment, 0.f, 1.f) : 0.f));
        }
        out.push_back(path.back());
        return out;
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

void LLVRKeyboard::draw(const Hit hover[2], const LLColor4& trail) const
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

    // The swipe trail, over the keys: each stretch thins and fades as it ages, so the trail
    // grows at the laser and dies away from where it began.
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (size_t i = 1; i < mTrail.size(); ++i)
    {
        const F32 life = 1.f - (F32)(now - mTrail[i - 1].time) / TRAIL_LIFE;
        if (life > 0.f)
        {
            LLColor4 colour = trail;
            colour.mV[VALPHA] *= life;
            stroke({ mTrail[i - 1].pos, mTrail[i].pos }, 4.f + 12.f * life, colour);
        }
    }
}

bool LLVRKeyboard::swipeStart(const Hit& hit, S32 x, S32 y)
{
    static LLCachedControl<bool> swipe(gSavedSettings, "SLVRSwipeTyping", true);
    const std::vector<KeyBox>& boxes = keyBoxes(mPage);
    if (!swipe || mPage != 0 || hit.part != KEY || hit.index < 0 || hit.index >= (S32)boxes.size())
    {
        return false;
    }
    const KeyDef& key = boxes[hit.index].def;
    const LLWString text = utf8str_to_wstring(key.text);
    if (key.action != TEXT || text.size() != 1 || !isLetter(text[0]))
    {
        return false;
    }
    mSwiping = true;
    mSwipeKey = hit;
    mSwipeLeftKey = false;
    mSwipePath.assign(1, LLVector2((F32)x, (F32)y));
    mTrail.assign(1, { mSwipePath[0], LLFrameTimer::getElapsedSeconds() });
    return true;
}

void LLVRKeyboard::swipeMove(S32 x, S32 y)
{
    const LLVector2 point((F32)x, (F32)y);
    if (!mSwiping || dist_vec(point, mSwipePath.back()) < SWIPE_POINT_STEP)
    {
        return;
    }
    mSwipePath.push_back(point);
    mTrail.push_back({ point, LLFrameTimer::getElapsedSeconds() });
    mSwipeLeftKey = mSwipeLeftKey || !(hitAt(x, y) == mSwipeKey);
}

void LLVRKeyboard::swipeEnd()
{
    if (!mSwiping)
    {
        return;
    }
    mSwiping = false;
    if (!mSwipeLeftKey) // a tap: the letter alone
    {
        mTrail.clear();
        press(mSwipeKey);
        return;
    }
    LLWString words[3];
    const S32 found = swipeWords(mSwipePath, words);
    if (found == 0)
    {
        return; // no known word goes that way; the trail fades
    }
    // In the case shift asks for, as the suggestions are.
    for (LLWString& word : words)
    {
        for (size_t c = 0; c < word.size(); ++c)
        {
            if (mCapsLock || (mShift && c == 0))
            {
                word[c] = upper(word[c]);
            }
        }
    }
    // A space before it when it would run into the word before the cursor.
    LLWString text;
    S32 cursor = 0;
    const bool after_word = focusedText(text, cursor) && cursor > 0 && cursor <= (S32)text.size() && isLetter(text[cursor - 1]);
    if (after_word)
    {
        type(LLWString(1, ' '));
    }
    mSwiped = words[0] + LLWString(1, ' ');
    type(mSwiped);
    mSwipeOthers[0] = words[1];
    mSwipeOthers[1] = words[2];
    const std::string key = wordKey(words[0]);
    mTyped.words[key] += 1;
    if (!mPrevious.empty() && !after_word)
    {
        mTyped.pairs[mPrevious + ' ' + key] += 1;
    }
    mShift = mCapsLock;
}

S32 LLVRKeyboard::swipeWords(const std::vector<LLVector2>& path, LLWString out[3]) const
{
    // Where each letter of the layout is.
    std::map<llwchar, LLVector2> centres;
    for (const KeyBox& box : keyBoxes(0))
    {
        const LLWString text = utf8str_to_wstring(box.def.text);
        if (box.def.action == TEXT && text.size() == 1 && isLetter(text[0]))
        {
            centres[lower(text[0])] = LLVector2(box.rect.getCenterX(), box.rect.getCenterY());
        }
    }
    if (path.size() < 2)
    {
        return 0;
    }
    const std::vector<LLVector2> drawn = resample(path, SWIPE_SAMPLES);

    // How often each word was seen, raised when it followed the previous word (as the
    // suggestions count).
    std::map<std::string, F32> seen;
    for (const Learned* learned : { &mLog, &mTyped })
    {
        const U32 weight = learned == &mTyped ? TYPED_WEIGHT : 1;
        for (const auto& word : learned->words)
        {
            seen[word.first] += (F32)(word.second * weight);
        }
        if (!mPrevious.empty())
        {
            const std::string prefix = mPrevious + ' ';
            withPrefix(learned->pairs, prefix, [&](const std::string& key, U32 count)
                       { seen[key.substr(prefix.size())] += (F32)(count * weight * PAIR_WEIGHT); });
        }
    }

    std::vector<std::pair<F32, LLWString>> ranked;
    for (const auto& entry : seen)
    {
        const LLWString word = utf8str_to_wstring(entry.first);
        if (word.size() < 2)
        {
            continue;
        }
        std::vector<LLVector2> keys;
        bool known = true;
        for (llwchar c : word)
        {
            const auto it = centres.find(c);
            known = known && it != centres.end();
            if (!known)
            {
                break;
            }
            if (keys.empty() || keys.back() != it->second) // a doubled letter is one key
            {
                keys.push_back(it->second);
            }
        }
        // It begins on the key pressed and ends near where the laser let go.
        if (!known || dist_vec(keys.front(), path.front()) > UNIT * 0.75f || dist_vec(keys.back(), path.back()) > UNIT * 1.5f)
        {
            continue;
        }
        const std::vector<LLVector2> ideal = resample(keys, SWIPE_SAMPLES);
        F32 apart = 0.f;
        for (S32 i = 0; i < SWIPE_SAMPLES; ++i)
        {
            apart += dist_vec(drawn[i], ideal[i]);
        }
        apart /= SWIPE_SAMPLES * UNIT;
        if (apart < SWIPE_MOST_APART)
        {
            ranked.emplace_back(logf(1.f + entry.second) - SWIPE_SHAPE_WEIGHT * apart * apart, word);
        }
    }
    const size_t count = llmin(ranked.size(), (size_t)3);
    std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < count; ++i)
    {
        out[i] = ranked[i].second;
    }
    return (S32)count;
}

LLVRKeyboard::EPress LLVRKeyboard::press(const Hit& hit)
{
    if (hit.part == SUGGESTION && hit.index >= 0 && hit.index < 3 && !mSuggestions[hit.index].empty())
    {
        // Replace the word being typed by the suggestion, as typed keys would; just after a
        // swipe, the word it typed (see update()).
        const LLWString word = mSuggestions[hit.index];
        const bool swiped = !mSwiped.empty(); // the word before the cursor is then the one replaced
        const size_t typed = swiped ? mSwiped.size() : mWord.size();
        mSwiped.clear();
        mContext.clear();
        for (size_t i = 0; i < typed; ++i)
        {
            gViewerWindow->handleKey(KEY_BACKSPACE, MASK_NONE);
        }
        type(word + LLWString(1, ' '));
        const std::string key = wordKey(word);
        mTyped.words[key] += 1;
        if (!mPrevious.empty() && !swiped)
        {
            mTyped.pairs[mPrevious + ' ' + key] += 1;
        }
        mShift = mCapsLock;
        return TYPED;
    }
    const std::vector<KeyBox>& boxes = keyBoxes(mPage);
    if (hit.part != KEY || hit.index < 0 || hit.index >= (S32)boxes.size())
    {
        return TYPED;
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
        break;
    }
    case SPACE:
        type(LLWString(1, ' '));
        break;
    case BACKSPACE:
        gViewerWindow->handleKey(KEY_BACKSPACE, MASK_NONE);
        return REPEATS;
    case ENTER:
    {
        LLWString text;
        S32 cursor = 0;
        if (focusedText(text, cursor))
        {
            learnText(text, 1); // what the user writes is what they will write again
        }
        gViewerWindow->handleKey(KEY_RETURN, MASK_NONE);
        break;
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
        break;
    case PAGE:
        mPage = 1 - mPage;
        break;
    case HIDE:
        gFocusMgr.setKeyboardFocus(nullptr);
        break;
    case GEAR:
        return SETTINGS;
    case GAP:
        break;
    }
    return TYPED;
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
    countText(text, mTyped, weight);
}

void LLVRKeyboard::reset()
{
    mPage = 0;
    mShift = mCapsLock = mSwiping = false;
    mTrail.clear();
    mSwiped.clear();
}

void LLVRKeyboard::update()
{
    static LLCachedControl<bool> suggest(gSavedSettings, "SLVRWordSuggestions", true);
    static LLCachedControl<bool> swipe(gSavedSettings, "SLVRSwipeTyping", true);
    if ((suggest || swipe) && !mLoadStarted) // swipe typing finds its words there too
    {
        const std::string dir = gDirUtilp->getPerAccountChatLogsDir();
        if (!dir.empty()) // empty until logged in
        {
            mLoadStarted = true;
            mLoading = std::async(std::launch::async, loadLog, dir);
        }
    }
    if (mLoading.valid() && mLoading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        mLog = mLoading.get();
        mContext.clear(); // suggest again with what the logs said
    }

    // The word before the text cursor, and the one before that in the same sentence.
    LLWString text;
    S32 cursor = 0;
    mWord.clear();
    mPrevious.clear();
    if (suggest && focusedText(text, cursor))
    {
        const S32 end = llclamp(cursor, 0, (S32)text.size());
        S32 start = end;
        while (start > 0 && isLetter(text[start - 1]))
        {
            --start;
        }
        mWord = text.substr(start, end - start);
        S32 before = start;
        while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == ','))
        {
            --before;
        }
        S32 first = before;
        while (first > 0 && isLetter(text[first - 1]))
        {
            --first;
        }
        if (before < start && first < before && before - first <= 24)
        {
            mPrevious = wordKey(text.substr(first, before - first));
        }
    }
    const std::string context = mPrevious + '|' + wordKey(mWord) + (mShift || mCapsLock ? "^" : "") + sLayout;
    if (context != mContext)
    {
        mContext = context;
        refreshSuggestions();
    }

    // Just after a swipe, while the word it typed is still just before the cursor, the other
    // words it may have meant are offered in its place.
    if (!mSwiped.empty())
    {
        const S32 length = (S32)mSwiped.size();
        if (focusedText(text, cursor) && cursor >= length && cursor <= (S32)text.size()
            && text.compare(cursor - length, length, mSwiped) == 0)
        {
            mSuggestions[0] = mSwipeOthers[0];
            mSuggestions[1] = mSwipeOthers[1];
            mSuggestions[2].clear();
        }
        else
        {
            mSwiped.clear();
            mContext.clear(); // the usual suggestions come back
        }
    }
}

// The three likeliest words: ones that the word being typed begins, raised when they were
// seen after the previous word; with nothing typed yet, those seen after the previous word.
void LLVRKeyboard::refreshSuggestions()
{
    for (LLWString& suggestion : mSuggestions)
    {
        suggestion.clear();
    }
    const std::string word = wordKey(mWord);
    if (word.empty() && mPrevious.empty())
    {
        return;
    }
    std::map<std::string, U32> scores;
    for (const Learned* learned : { &mLog, &mTyped })
    {
        const U32 weight = learned == &mTyped ? TYPED_WEIGHT : 1;
        if (!word.empty())
        {
            withPrefix(learned->words, word, [&](const std::string& key, U32 count)
                       { if (key.size() > word.size()) scores[key] += count * weight; });
        }
        if (!mPrevious.empty())
        {
            const std::string prefix = mPrevious + ' ';
            withPrefix(learned->pairs, prefix + word, [&](const std::string& key, U32 count)
                       { if (key.size() > prefix.size() + word.size()) scores[key.substr(prefix.size())] += count * weight * PAIR_WEIGHT; });
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
    // In the case typed so far; a next word in the case shift asks for.
    bool capital = mShift || mCapsLock, all_capitals = mCapsLock;
    if (!mWord.empty())
    {
        capital = upper(mWord[0]) == mWord[0] && lower(mWord[0]) != mWord[0];
        all_capitals = capital && mWord.size() > 1 && upper(mWord[1]) == mWord[1] && lower(mWord[1]) != mWord[1];
    }
    for (size_t i = 0; i < count; ++i)
    {
        LLWString suggestion = utf8str_to_wstring(ranked[i].second);
        for (size_t c = 0; c < suggestion.size(); ++c)
        {
            if (all_capitals || (capital && c == 0))
            {
                suggestion[c] = upper(suggestion[c]);
            }
        }
        mSuggestions[i] = suggestion;
    }
}

// Runs once when VR is switched on.
void LLVRKeyboard::selfCheck()
{
    // Every layout fills its rows, with letters not too narrow to hit, and the keys stay
    // clear of the suggestion bar.
    for (const std::vector<KeyDef>& row : SYMBOL_ROWS)
    {
        llassert_always(fabsf(rowWidth(row) - 13.f) < 0.01f);
    }
    for (const LLVRLanguage::Language& language : LLVRLanguage::all())
    {
        for (const std::vector<KeyDef>& row : letterRows(language.keyboard))
        {
            llassert_always(fabsf(rowWidth(row) - 13.f) < 0.01f);
            for (const KeyDef& key : row)
            {
                llassert_always(key.action != TEXT || key.width > 0.8f);
            }
        }
    }
    llassert_always(KEYS_TOP < SUGGEST_BOTTOM && KEYS_TOP - 4.f * UNIT > BODY_BOTTOM);

    // Case mapping both ways, Turkish i included, and words split on anything but letters.
    const bool turkic = sTurkic;
    sTurkic = true;
    llassert_always(upper('i') == 0x130 && upper(0x131) == 'I' && lower(0x130) == 'i' && lower('I') == 0x131);
    llassert_always(upper(0x15F) == 0x15E && lower(0xC7) == 0xE7);
    sTurkic = false;
    llassert_always(upper('i') == 'I' && lower('I') == 'i' && upper(0x439) == 0x419 && lower(0x141) == 0x142);
    llassert_always(isLetter(0x259) && isLetter(0x44F) && !isLetter('3') && !isLetter(','));
    sTurkic = turkic;
    Learned learned;
    countText(logMessage(utf8str_to_wstring("[2026/11/20 21:03]  Ali Resident: Merhaba, merhaba d\xC3\xBCnya! a")), learned, 1);
    llassert_always(learned.words.size() == 2 && learned.words["merhaba"] == 2 && learned.words["d\xC3\xBCnya"] == 1);
    llassert_always(learned.pairs.size() == 2 && learned.pairs["merhaba merhaba"] == 1 && learned.pairs["merhaba d\xC3\xBCnya"] == 1);

    // What lies where.
    LLVRKeyboard keyboard;
    const LLRectf first = keyBoxes(0)[0].rect;
    const Hit key = keyboard.hitAt((S32)first.getCenterX(), (S32)first.getCenterY());
    llassert_always(key.part == KEY && key.index == 0 && keyBoxes(0)[0].def.action == TEXT);
    llassert_always(keyboard.hitAt(WIDTH / 2, 20).part == HANDLE);
    llassert_always(keyboard.hitAt((S32)MARGIN + 10, (S32)SUGGEST_BOTTOM + 10).part == SUGGESTION);
    llassert_always(keyboard.hitAt(WIDTH + 5, 20).part == NONE);

    // Completion: most frequent first, in the case typed.
    keyboard.mTyped.words["merhaba"] = 1;
    keyboard.mTyped.words["merhem"] = 2;
    keyboard.mWord = utf8str_to_wstring("Mer");
    keyboard.refreshSuggestions();
    llassert_always(wstring_to_utf8str(keyboard.mSuggestions[0]) == "Merhem" && wstring_to_utf8str(keyboard.mSuggestions[1]) == "Merhaba");

    // The next word: what followed the previous one, before what is merely frequent.
    const std::string nasilsin = "nas\xC4\xB1ls\xC4\xB1n";
    keyboard.mLog.words["nasa"] = 20;
    keyboard.mTyped.pairs["merhaba " + nasilsin] = 3;
    keyboard.mTyped.pairs["merhaba naber"] = 1;
    keyboard.mPrevious = "merhaba";
    keyboard.mWord.clear();
    keyboard.refreshSuggestions();
    llassert_always(wstring_to_utf8str(keyboard.mSuggestions[0]) == nasilsin && wstring_to_utf8str(keyboard.mSuggestions[1]) == "naber"
                    && keyboard.mSuggestions[2].empty());
    keyboard.mWord = utf8str_to_wstring("na");
    keyboard.refreshSuggestions();
    llassert_always(wstring_to_utf8str(keyboard.mSuggestions[0]) == nasilsin && wstring_to_utf8str(keyboard.mSuggestions[1]) == "naber"
                    && wstring_to_utf8str(keyboard.mSuggestions[2]) == "nasa");

    // Swipe typing. A path's length spreads evenly over its points.
    const std::vector<LLVector2> even = resample({ LLVector2(0.f, 0.f), LLVector2(2.f, 0.f), LLVector2(10.f, 0.f) }, 3);
    llassert_always(even.size() == 3 && dist_vec(even[1], LLVector2(5.f, 0.f)) < 1e-4f && dist_vec(even[2], LLVector2(10.f, 0.f)) < 1e-4f);
    // On this layout: the top row's first and last letters, and the bottom row's first. A
    // path down to the bottom row and up to the top right finds the word going that way,
    // however much more often the straight one along the top row was seen; and the reverse.
    std::vector<const KeyBox*> top, bottom;
    for (const KeyBox& box : keyBoxes(0))
    {
        const LLWString text = utf8str_to_wstring(box.def.text);
        if (box.def.action == TEXT && text.size() == 1 && isLetter(text[0]) && box.rect.mTop > KEYS_TOP - 0.5f * UNIT)
        {
            top.push_back(&box);
        }
        else if (box.def.action == TEXT && text.size() == 1 && isLetter(text[0]) && box.rect.mTop < KEYS_TOP - 1.5f * UNIT)
        {
            bottom.push_back(&box);
        }
    }
    if (top.size() >= 2 && !bottom.empty())
    {
        auto centre = [](const KeyBox* box) { return LLVector2(box->rect.getCenterX(), box->rect.getCenterY()); };
        const LLWString first = utf8str_to_wstring(top.front()->def.text), last = utf8str_to_wstring(top.back()->def.text),
                        low = utf8str_to_wstring(bottom.front()->def.text);
        const LLWString bent = first + low + last, straight = first + last;
        keyboard.mPrevious.clear();
        keyboard.mTyped.words[wordKey(bent)] = 1;
        keyboard.mTyped.words[wordKey(straight)] = 50;
        LLWString found[3];
        llassert_always(keyboard.swipeWords(resample({ centre(top.front()), centre(bottom.front()), centre(top.back()) }, 40), found) >= 1
                        && wordKey(found[0]) == wordKey(bent));
        llassert_always(keyboard.swipeWords(resample({ centre(top.front()), centre(top.back()) }, 40), found) >= 1
                        && wordKey(found[0]) == wordKey(straight));
    }
}
