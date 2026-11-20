/**
 * @file llvrlanguage.h
 * @brief SLVR: the language of SLVR's own menu, settings and messages, chosen apart from the
 * viewer's, and the on-screen keyboard layouts. Both come from app_settings/slvr_languages.xml.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#ifndef LL_LLVRLANGUAGE_H
#define LL_LLVRLANGUAGE_H

#include <map>
#include <string>
#include <vector>

namespace LLVRLanguage
{
    struct Language
    {
        std::string code, name;             // "tr", and the name in that language
        std::vector<std::string> keyboard;  // three rows of letters (UTF-8); empty: no layout of its own
        std::map<std::string, std::string> strings;
    };

    const std::vector<Language>& all();

    // SLVRLanguage, or while that is empty the viewer's UI language; English when it has none.
    const Language& current();

    // SLVRKeyboardLayout, or while that is empty current(); English when that has no layout.
    const Language& keyboard();

    // A text of current(), else the English one, else the key itself (so a gap shows).
    std::string text(const std::string& key);

    // After the menus are built: puts the SLVR menu into current(), and again on every change.
    void init();
    void apply();

    // LLViewerFloaterReg: the SLVR settings window, "slvr_settings".
    void registerFloater();
}

#endif
