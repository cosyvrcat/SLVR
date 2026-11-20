/**
 * @file llvrlanguage.cpp
 * @brief SLVR: SLVR's own language and the keyboard layouts, and the SLVR settings window.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvrlanguage.h"

#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llmenugl.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"

#include <fstream>

namespace
{
    std::vector<LLVRLanguage::Language> load()
    {
        std::vector<LLVRLanguage::Language> languages;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "slvr_languages.xml");
        llifstream file(path.c_str(), std::ios::binary);
        LLSD data;
        if (!file.is_open() || LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE || !data.isArray())
        {
            LL_WARNS("VR") << "Cannot read " << path << ": SLVR texts show as their keys, the keyboard is English" << LL_ENDL;
        }
        for (const LLSD& entry : llsd::inArray(data))
        {
            LLVRLanguage::Language language;
            language.code = entry["code"].asString();
            language.name = entry["name"].asString();
            for (const LLSD& row : llsd::inArray(entry["keyboard"]))
            {
                language.keyboard.push_back(row.asString());
            }
            if (!language.keyboard.empty() && language.keyboard.size() != 3)
            {
                LL_WARNS("VR") << "Keyboard of " << language.code << " needs three rows; left out" << LL_ENDL;
                language.keyboard.clear();
            }
            for (const auto& text : llsd::inMap(entry["strings"]))
            {
                language.strings[text.first] = text.second.asString();
            }
            if (!language.code.empty())
            {
                languages.push_back(language);
            }
        }
        if (std::none_of(languages.begin(), languages.end(), [](const LLVRLanguage::Language& l) { return l.code == "en"; }))
        {
            languages.insert(languages.begin(), LLVRLanguage::Language{ "en", "English", { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, {} });
        }
        return languages;
    }

    const LLVRLanguage::Language* find(const std::string& code)
    {
        for (const LLVRLanguage::Language& language : LLVRLanguage::all())
        {
            if (language.code == code)
            {
                return &language;
            }
        }
        return nullptr;
    }

    const LLVRLanguage::Language& english()
    {
        return *find("en"); // load() makes sure it is there
    }

    // The SLVR settings: SLVR's language, the keyboard layout, word suggestions.
    class LLFloaterSLVRSettings : public LLFloater
    {
    public:
        LLFloaterSLVRSettings(const LLSD& key) : LLFloater(key) {}

        bool postBuild() override
        {
            getChild<LLComboBox>("language")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                { gSavedSettings.setString("SLVRLanguage", ctrl->getValue().asString()); });
            getChild<LLComboBox>("keyboard_layout")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                       { gSavedSettings.setString("SLVRKeyboardLayout", ctrl->getValue().asString()); });
            translate();
            return true;
        }

        void translate()
        {
            setTitle(LLVRLanguage::text("settings_title"));
            getChild<LLTextBox>("language_label")->setText(LLVRLanguage::text("language_label"));
            getChild<LLTextBox>("keyboard_label")->setText(LLVRLanguage::text("keyboard_label"));
            getChild<LLCheckBoxCtrl>("word_suggestions")->setLabel(LLVRLanguage::text("word_suggestions"));

            LLComboBox* language = getChild<LLComboBox>("language");
            LLComboBox* layout = getChild<LLComboBox>("keyboard_layout");
            language->removeall();
            layout->removeall();
            language->add(LLVRLanguage::text("same_as_viewer"), LLSD(""));
            layout->add(LLVRLanguage::text("same_as_language"), LLSD(""));
            for (const LLVRLanguage::Language& entry : LLVRLanguage::all())
            {
                language->add(entry.name, LLSD(entry.code));
                if (!entry.keyboard.empty())
                {
                    layout->add(entry.name, LLSD(entry.code));
                }
            }
            language->setValue(gSavedSettings.getString("SLVRLanguage"));
            layout->setValue(gSavedSettings.getString("SLVRKeyboardLayout"));
        }
    };

    // The SLVR menu's items, by name; their texts have the same keys.
    const char* const MENU_ITEMS[] = { "SLVR VR Headset", "SLVR Screenshot", "SLVR Settings" };
}

namespace LLVRLanguage
{
    const std::vector<Language>& all()
    {
        static const std::vector<Language> languages = load();
        return languages;
    }

    const Language& current()
    {
        static LLCachedControl<std::string> chosen(gSavedSettings, "SLVRLanguage", "");
        const std::string code = std::string(chosen).empty() ? LLUI::getLanguage().substr(0, 2) : std::string(chosen);
        const Language* language = find(code);
        return language ? *language : english();
    }

    const Language& keyboard()
    {
        static LLCachedControl<std::string> chosen(gSavedSettings, "SLVRKeyboardLayout", "");
        const Language* language = std::string(chosen).empty() ? &current() : find(chosen);
        return language && !language->keyboard.empty() ? *language : english();
    }

    std::string text(const std::string& key)
    {
        for (const Language* language : { &current(), &english() })
        {
            auto it = language->strings.find(key);
            if (it != language->strings.end())
            {
                return it->second;
            }
        }
        return key;
    }

    void apply()
    {
        for (const char* name : MENU_ITEMS)
        {
            if (LLMenuItemGL* item = gMenuHolder ? gMenuHolder->findChild<LLMenuItemGL>(name) : nullptr)
            {
                item->setLabel(text(name));
            }
        }
        if (LLMenuGL* menu = gMenuBarView ? gMenuBarView->findChildMenuByName("SLVR", true) : nullptr)
        {
            menu->needsArrange(); // the labels' widths changed
        }
        if (LLFloaterSLVRSettings* floater = LLFloaterReg::findTypedInstance<LLFloaterSLVRSettings>("slvr_settings"))
        {
            floater->translate();
        }
    }

    void init()
    {
        static bool connected = false;
        if (!connected) // init_menus() runs again after a relog
        {
            connected = true;
            gSavedSettings.getControl("SLVRLanguage")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { apply(); });
        }
        apply();
    }

    void registerFloater()
    {
        LLFloaterReg::add("slvr_settings", "floater_slvr_settings.xml", &LLFloaterReg::build<LLFloaterSLVRSettings>);
    }
}
