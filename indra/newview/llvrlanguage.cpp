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

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerVR.h"
#include "llvrkeyboard.h"

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

    // The laser colours the settings offer, by the names SLVRLaserColor stores; their texts
    // have the keys "laser_" + name.
    const char* const LASER_COLOURS[] = { "red", "orange", "yellow", "green", "blue", "purple", "white" };

    // The step turns the settings offer, in degrees.
    const S32 SNAP_ANGLES[] = { 15, 30, 45, 90 };

    // The SLVR settings: SLVR's language, the keyboard layout, word suggestions, the lasers and
    // the panel on the left; turning, comfort, walking and height on the right.
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
            getChild<LLComboBox>("vignette")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                { gSavedSettings.setS32("SLVRVignette", ctrl->getValue().asInteger()); });
            getChild<LLComboBox>("laser_colour")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                    { gSavedSettings.setString("SLVRLaserColor", ctrl->getValue().asString()); });
            getChild<LLComboBox>("snap_angle")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                  { gSavedSettings.setS32("SLVRSnapAngle", ctrl->getValue().asInteger()); });
            getChild<LLComboBox>("walk_direction")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                      { gSavedSettings.setS32("SLVRWalkDirection", ctrl->getValue().asInteger()); });
            getChild<LLComboBox>("eye_height")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
                                                                  { gSavedSettings.setS32("SLVREyeHeightMode", ctrl->getValue().asInteger()); });
            getChild<LLButton>("measure_height")->setCommitCallback([this](LLUICtrl*, const LLSD&) { measureHeight(); });
            // Enabled even with suggestions off, so words kept from before can still be deleted.
            getChild<LLButton>("forget_words")->setCommitCallback([](LLUICtrl*, const LLSD&)
            {
                LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", LLVRLanguage::text("forget_words_confirm")), LLSD(),
                                         [](const LLSD& notification, const LLSD& response)
                                         {
                                             if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                                 return;
                                             LLVRKeyboard::forgetLearned();
                                             LLNotificationsUtil::add("SystemMessageTip",
                                                                      LLSD().with("MESSAGE", LLVRLanguage::text("forget_words_done")));
                                         });
            });
            translate();
            return true;
        }

        void onOpen(const LLSD&) override { gVR.settingsOpened(this); }

        void translate()
        {
            setTitle(LLVRLanguage::text("settings_title"));
            getChild<LLTextBox>("language_label")->setText(LLVRLanguage::text("language_label"));
            getChild<LLTextBox>("keyboard_label")->setText(LLVRLanguage::text("keyboard_label"));
            getChild<LLCheckBoxCtrl>("word_suggestions")->setLabel(LLVRLanguage::text("word_suggestions"));
            getChild<LLCheckBoxCtrl>("swipe_typing")->setLabel(LLVRLanguage::text("swipe_typing"));
            getChild<LLCheckBoxCtrl>("message_dot_private")->setLabel(LLVRLanguage::text("message_dot_private"));
            getChild<LLCheckBoxCtrl>("smooth_turn")->setLabel(LLVRLanguage::text("smooth_turn"));
            getChild<LLTextBox>("vignette_label")->setText(LLVRLanguage::text("vignette_label"));
            getChild<LLTextBox>("laser_colour_label")->setText(LLVRLanguage::text("laser_colour_label"));
            getChild<LLSliderCtrl>("laser_opacity")->setLabel(LLVRLanguage::text("laser_opacity"));
            for (const char* name : { "blink", "left_handed", "ignore_script_camera", "haptics", "show_controllers", "curved_panel",
                                      "panel_follow", "performance_preset" })
            {
                getChild<LLCheckBoxCtrl>(name)->setLabel(LLVRLanguage::text(name));
            }
            for (const char* name : { "snap_angle_label", "walk_direction_label", "eye_height_label" })
            {
                getChild<LLTextBox>(name)->setText(LLVRLanguage::text(name));
            }
            for (const char* name : { "turn_speed", "body_height" })
            {
                getChild<LLSliderCtrl>(name)->setLabel(LLVRLanguage::text(name));
            }
            getChild<LLButton>("measure_height")->setLabel(LLVRLanguage::text("measure_height"));
            getChild<LLButton>("forget_words")->setLabel(LLVRLanguage::text("forget_words"));

            LLComboBox* snap = getChild<LLComboBox>("snap_angle");
            snap->removeall();
            for (S32 degrees : SNAP_ANGLES)
            {
                snap->add(llformat("%d\xC2\xB0", degrees), LLSD(degrees)); // UTF-8 degree sign
            }
            snap->setValue(gSavedSettings.getS32("SLVRSnapAngle"));
            LLComboBox* walk = getChild<LLComboBox>("walk_direction");
            walk->removeall();
            walk->add(LLVRLanguage::text("walk_head"), LLSD(0));
            walk->add(LLVRLanguage::text("walk_hand"), LLSD(1));
            walk->setValue(gSavedSettings.getS32("SLVRWalkDirection"));
            LLComboBox* eye_height = getChild<LLComboBox>("eye_height");
            eye_height->removeall();
            eye_height->add(LLVRLanguage::text("eye_height_avatar"), LLSD(0));
            eye_height->add(LLVRLanguage::text("eye_height_mine"), LLSD(1));
            eye_height->setValue(gSavedSettings.getS32("SLVREyeHeightMode"));

            LLComboBox* vignette = getChild<LLComboBox>("vignette");
            vignette->removeall();
            const char* const VIGNETTE_LEVELS[] = { "vignette_off", "vignette_low", "vignette_medium", "vignette_high" };
            for (S32 level = 0; level < 4; ++level)
            {
                vignette->add(LLVRLanguage::text(VIGNETTE_LEVELS[level]), LLSD(level));
            }
            vignette->setValue(gSavedSettings.getS32("SLVRVignette"));
            LLComboBox* laser = getChild<LLComboBox>("laser_colour");
            laser->removeall();
            for (const char* colour : LASER_COLOURS)
            {
                laser->add(LLVRLanguage::text(std::string("laser_") + colour), LLSD(colour));
            }
            laser->setValue(gSavedSettings.getString("SLVRLaserColor"));

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

    private:
        // Measured, the height is the user's own from now on.
        void measureHeight()
        {
            F32 height = 0.f;
            if (!gVR.measureHeight(height))
            {
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", LLVRLanguage::text("height_not_measured")));
                return;
            }
            gSavedSettings.setF32("SLVRBodyHeight", height);
            gSavedSettings.setS32("SLVREyeHeightMode", 1);
            getChild<LLComboBox>("eye_height")->setValue(1);
            LLNotificationsUtil::add("SystemMessageTip",
                                     LLSD().with("MESSAGE", LLVRLanguage::text("height_measured") + llformat(" %.2f m", height)));
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
