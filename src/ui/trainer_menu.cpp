// See trainer_menu.h.
//
// Drawn straight into Dear ImGui's foreground draw list (no ImGui widgets): the look is the trainer's own, the input is
// read here (keyboard, numpad, gamepad, with key repeat), and nothing moves under the mouse. Pages are rebuilt every
// frame from the trainer's state, so a row always shows what is true now.
#include "ui/trainer_menu.h"

#include "game/trainer_select.h" // [lcs] VCS's or LCS's trainer
#include "game/vcs_trainer.h"
#include "game/vcs_save_anywhere.h" // [fork]
#include "game/vcs_input.h"
#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/theme.h" // [lcs] the overlay's colours per game

#include "imgui.h"

#ifdef __ANDROID__
#include "android/touch_controls.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        namespace tr = game::trainer; // [lcs] vcs::trainer, or LCS's when that game runs

        enum Page
        {
            Main,
            Player,
            Weapons,
            WeaponList,
            Vehicles,
            VehicleList,
            Teleport,
            Camera,
            World,
            Missions,
            StoryList,
            JobList,
            SaveGame, // [fork] save anywhere
            VehicleGame, // [crossover] one game's vehicles (g_vehOther): its categories
            VehicleCat,  // [crossover] one category of them (g_vehCat)
        };
        const char *const kPageTitles[] = {"MAIN MENU", "PLAYER", "WEAPONS", "GIVE WEAPON", "VEHICLES", "SPAWN VEHICLE",
                                           "TELEPORT", "CAMERA", "WORLD", "MISSIONS", "STORY", "SIDE JOBS", "SAVE GAME",
                                           "", ""};

        // [crossover] the spawn list split by game (this game's / the other Stories game's, from the player's disc:
        // tools/stories_pack.py) and by category
        bool g_vehOther = false;
        int g_vehCat = 0;
        const char *const kVehCats[] = {"Cars", "Bikes", "Boats", "Aircraft"};
        int vehCat(const std::string &cls) { return cls == "bike" ? 1 : cls == "boat" ? 2 : cls == "heli" || cls == "plane" ? 3 : 0; }
        const char *gameName(bool other) { return game::isLcs() != other ? "Liberty City Stories" : "Vice City Stories"; }
        // each game's colour: a stripe on its rows (LCS crimson, VCS pink)
        ImU32 gameColour(bool other) { return game::isLcs() != other ? IM_COL32(220, 40, 60, 255) : IM_COL32(255, 90, 200, 255); }
        std::string pageTitle(Page p)
        {
            if (p == VehicleGame)
                return game::isLcs() != g_vehOther ? "LCS VEHICLES" : "VCS VEHICLES";
            if (p == VehicleCat)
                return std::string(game::isLcs() != g_vehOther ? "LCS " : "VCS ") + kVehCats[g_vehCat];
            return kPageTitles[p];
        }

        enum class Value
        {
            None,
            On,
            Off,
            Choice,
            Page,
        };

        struct Row
        {
            std::string label;
            Value kind = Value::None;
            std::string value; // Choice
            std::string help;
            std::function<void()> select;    // Enter / A
            std::function<void(int)> change; // Left / Right: -1 / +1
            bool enabled = true;
            ImU32 stripe = 0; // [crossover] a coloured mark at the row's right (the game a vehicle comes from)
        };

        struct Level
        {
            Page page;
            int selected = 0;
            int top = 0; // first row shown
        };

        bool g_open = false;
        bool g_releaseWait = false;
        bool g_chordWasDown = false;
        bool g_camChordWasDown = false; // Back + LB: the free camera
        bool g_hideShotNotices = false; // this frame only: hideScreenshotNotices
        std::vector<Level> g_stack{{Main}};
        bool g_padWas[platform::GAMEPAD_BUTTON_COUNT] = {};

        // the choices' current values (the menu's own; applied with Enter, or at once where noted)
        int g_wanted = 3;
        int g_moneyIndex = 1;
        int g_loadout = 0;
        int g_hour = 12;
        int g_weather = 0; // index into tr::weathers()
        constexpr int kMoney[] = {1000, 10000, 100000, 1000000};

        // ---- Input ------------------------------------------------------------------------------------------------
        bool pressedKey(int k) { return platform::keyPressed(k) || ps2control::keyPressed(k); }
        bool heldKey(int k) { return platform::keyDown(k) || ps2control::keyDown(k); }
        bool padDown(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        // A direction held down repeats, like a text cursor.
        struct Repeat
        {
            double next = 0.0;
            bool fire(bool pressed, bool held)
            {
                const double t = ImGui::GetTime();
                if (pressed)
                {
                    next = t + 0.35;
                    return true;
                }
                if (held && t >= next)
                {
                    next = t + 0.06;
                    return true;
                }
                return false;
            }
        };
        Repeat g_up, g_down, g_left, g_right;

        struct Input
        {
            bool up = false, down = false, left = false, right = false, select = false, back = false;
        };

        Input readInput()
        {
            using namespace platform;
            bool padPressed[GAMEPAD_BUTTON_COUNT] = {};
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
            {
                const bool d = padDown(static_cast<GamepadButton>(b));
                padPressed[b] = d && !g_padWas[b];
                g_padWas[b] = d;
            }
            auto dir = [&](Repeat &r, int key, int numpad, GamepadButton pad) {
                return r.fire(pressedKey(key) || pressedKey(numpad) || padPressed[pad], heldKey(key) || heldKey(numpad) || padDown(pad));
            };
            Input in;
            in.up = dir(g_up, KEY_UP, KEY_KP_8, GAMEPAD_DPAD_UP);
            in.down = dir(g_down, KEY_DOWN, KEY_KP_2, GAMEPAD_DPAD_DOWN);
            in.left = dir(g_left, KEY_LEFT, KEY_KP_4, GAMEPAD_DPAD_LEFT);
            in.right = dir(g_right, KEY_RIGHT, KEY_KP_6, GAMEPAD_DPAD_RIGHT);
            const bool alt = heldKey(KEY_LEFT_ALT) || heldKey(KEY_RIGHT_ALT); // Alt+Enter is full screen
            in.select = (!alt && (pressedKey(KEY_ENTER) || pressedKey(KEY_KP_ENTER))) || pressedKey(KEY_KP_5) ||
                        padPressed[GAMEPAD_SOUTH];
            in.back = pressedKey(KEY_BACKSPACE) || pressedKey(KEY_KP_0) || pressedKey(KEY_ESCAPE) || padPressed[GAMEPAD_EAST];
            return in;
        }

        bool anyInputHeld()
        {
            for (int k = 1; k < platform::KEY_COUNT; ++k)
                if (platform::keyDown(k))
                    return true;
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (padDown(static_cast<platform::GamepadButton>(b)))
                    return true;
            return false;
        }

        // ---- Pages ------------------------------------------------------------------------------------------------
        Row pageRow(const char *label, Page page, const char *help)
        {
            Row r{label, Value::Page, {}, help};
            r.select = [page] { g_stack.push_back({page}); };
            return r;
        }

        Row toggleRow(const char *label, tr::Toggle t, const char *help)
        {
            Row r{label, tr::toggle(t) ? Value::On : Value::Off, {}, help};
            r.select = [t] { tr::setToggle(t, !tr::toggle(t)); };
            r.change = [t](int) { tr::setToggle(t, !tr::toggle(t)); };
            return r;
        }

        Row actionRow(const std::string &label, std::function<void()> fn, const std::string &help, bool enabled = true)
        {
            Row r{label, Value::None, {}, help};
            r.select = std::move(fn);
            r.enabled = enabled;
            return r;
        }

        Row choiceRow(const char *label, std::string value, std::function<void(int)> change, std::function<void()> apply,
                      const char *help)
        {
            Row r{label, Value::Choice, std::move(value), help};
            r.change = std::move(change);
            r.select = std::move(apply);
            return r;
        }

        int wrap(int v, int n) { return ((v % n) + n) % n; }

        std::string money(int v)
        {
            std::string digits = std::to_string(v), out;
            for (size_t k = 0; k < digits.size(); ++k)
            {
                if (k && (digits.size() - k) % 3 == 0)
                    out += ',';
                out += digits[k];
            }
            return "$" + out;
        }

        // [fork] The Save game page (game/vcs_save_anywhere.h, docs/TRAINER.md "Save anywhere"). The quick save
        // overwrites its slot: a used one asks for a second Enter within 3 s.
        double g_quickConfirmUntil = 0.0;
        void buildSavePage(std::vector<Row> &rows)
        {
            namespace sa = game::saves; // [lcs] vcs::saveanywhere, or LCS's saves (game/trainer_select.h)
            const std::string why = sa::whyNot();
            const bool can = why.empty() && !sa::busy();
            const std::string blocked = sa::busy() ? "Saving..." : why + ".";
            rows.push_back(actionRow("Save game here", sa::saveHere,
                                     can ? "The game's own Save menu, here: pick a slot (1-8); it asks before "
                                           "overwriting. Load Game brings you back to this spot."
                                         : blocked,
                                     can));
            const std::vector<sa::Slot> slots = sa::slots();
            const std::string quickName = slots[sa::kQuickSlot - 1].name;
            const bool confirming = ImGui::GetTime() < g_quickConfirmUntil;
            std::string quickHelp = !can ? blocked
                                    : confirming
                                        ? "Enter again to overwrite slot " + std::to_string(sa::kQuickSlot) + " (" + quickName + ")."
                                    : quickName.empty() ? "Saves to slot " + std::to_string(sa::kQuickSlot) + " (empty) at once, without the menu."
                                                        : "Saves to slot " + std::to_string(sa::kQuickSlot) + " at once, over \"" +
                                                              quickName + "\" (Enter twice).";
            rows.push_back(actionRow(confirming ? "Quick save - sure?" : "Quick save (slot " + std::to_string(sa::kQuickSlot) + ")",
                                     [quickName] {
                                         if (quickName.empty() || ImGui::GetTime() < g_quickConfirmUntil)
                                         {
                                             g_quickConfirmUntil = 0.0;
                                             sa::quickSave();
                                         }
                                         else
                                             g_quickConfirmUntil = ImGui::GetTime() + 3.0;
                                     },
                                     quickHelp, can));
            Row restore{"Put me back after loading", sa::restoreAfterLoad() ? Value::On : Value::Off, {},
                        "After loading a save made here: if the game's load put you elsewhere, the trainer moves you "
                        "to where you saved."};
            restore.select = [] { sa::setRestoreAfterLoad(!sa::restoreAfterLoad()); };
            restore.change = [](int) { sa::setRestoreAfterLoad(!sa::restoreAfterLoad()); };
            rows.push_back(restore);
            for (const sa::Slot &s : slots)
                rows.push_back(actionRow("Slot " + std::to_string(s.slot) + ": " + (s.name.empty() ? "(empty)" : s.name), {},
                                         "The card's saves (Load Game is on the pause menu's Game tab).", false));
            if (sa::hasQuickLoad()) // [lcs] LCS's quick load (game/lcs_save.h)
                rows.push_back(actionRow("Quick load (slot " + std::to_string(sa::kQuickSlot) + ")", sa::quickLoad,
                                         quickName.empty() ? "Slot " + std::to_string(sa::kQuickSlot) + " is empty."
                                                           : "Loads \"" + quickName + "\" now; what was not saved is lost.",
                                         !quickName.empty() && !sa::busy()));
            else
            rows.push_back(actionRow("Quick load", {}, "Not offered: the game loads only from its own pause menu "
                                                       "(Game > Load Game).", false));
        }

        std::vector<Row> buildPage(Page page)
        {
            const bool inCar = tr::inVehicle();
            std::vector<Row> rows;
            switch (page)
            {
            case Main:
                rows.push_back(pageRow("Player", Player, "God mode, health, wanted level, money."));
                rows.push_back(pageRow("Weapons", Weapons, "Any weapon, loadouts, unlimited ammo."));
                rows.push_back(pageRow("Vehicles", Vehicles, "Spawn any vehicle; repair, flip and boost the one you drive."));
                rows.push_back(pageRow("Teleport", Teleport, "Every district of Vice City, and a position of your own."));
                rows.push_back(pageRow("Camera", Camera, "Free camera: fly anywhere. Hide the HUD."));
                rows.push_back(pageRow("World", World, "Time of day, weather, game speed."));
                rows.push_back(pageRow("Missions", Missions, "Start any story mission or side job."));
                rows.push_back(pageRow("Save game", SaveGame, "Save here, away from a safehouse.")); // [fork]
                break;
            case SaveGame: // [fork] save anywhere (game/vcs_save_anywhere.h)
                buildSavePage(rows);
                break;
            case Player:
                rows.push_back(toggleRow("God mode", tr::GodMode,
                                         "No damage from bullets, fire, explosions, falls or fists; no drowning; health "
                                         "kept full."));
                rows.push_back(actionRow("Heal", tr::heal, "Health and armour to full."));
                rows.push_back(toggleRow("Never wanted", tr::NeverWanted, "The police forget you as soon as they notice you."));
                rows.push_back(choiceRow("Set wanted level", std::string(g_wanted, '*') + (g_wanted ? "" : "none"),
                                         [](int d) { g_wanted = std::clamp(g_wanted + d, 0, 6); },
                                         [] { tr::setWanted(g_wanted); }, "Left / Right: how many stars. Enter: set it."));
                rows.push_back(choiceRow("Add money", money(kMoney[g_moneyIndex]),
                                         [](int d) { g_moneyIndex = wrap(g_moneyIndex + d, static_cast<int>(std::size(kMoney))); },
                                         [] { tr::addMoney(kMoney[g_moneyIndex]); }, "Left / Right: how much. Enter: add it."));
                rows.push_back(toggleRow("Infinite sprint", tr::InfiniteSprint, "Run as long as you like."));
                rows.push_back(toggleRow("Police ignore you", tr::PoliceIgnore, "Cops leave you alone, whatever you do."));
                rows.push_back(toggleRow("Everyone ignores you", tr::EveryoneIgnores, "Nobody reacts to you: cops, gangs, people."));
                break;
            case Weapons:
            {
                rows.push_back(pageRow("Give weapon", WeaponList, "Any of the game's weapons, with ammunition."));
                const std::vector<tr::Entry> loadouts = tr::loadouts();
                g_loadout = std::clamp(g_loadout, 0, static_cast<int>(loadouts.size()) - 1);
                rows.push_back(choiceRow("Give loadout", loadouts[g_loadout].name,
                                         [n = static_cast<int>(loadouts.size())](int d) { g_loadout = wrap(g_loadout + d, n); },
                                         [] { tr::giveLoadout(g_loadout); },
                                         "Street: bat, molotovs, pistol, shotgun, micro SMG. Military: machete, grenades, "
                                         "Equalizer, Spaz, AK, sniper, rocket launcher. Heavy: chainsaw, grenades, "
                                         "Equalizer, Spaz, SMG, assault rifle, laser sniper, minigun."));
                rows.push_back(toggleRow("Unlimited ammo", tr::UnlimitedAmmo, "Every weapon you carry stays full."));
                rows.push_back(toggleRow("No reload", tr::NoReload, "The clip never runs out: fire without reloading."));
                rows.push_back(toggleRow("Fast reload", tr::FastReload, "The game's own quick reload."));
                rows.push_back(actionRow("Remove all weapons", tr::removeWeapons, "Empty hands."));
                break;
            }
            case WeaponList:
                for (const tr::Entry &e : tr::weapons())
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::giveWeapon(id); }, "Enter: give it, with ammunition, "
                                                                                       "and hold it."));
                break;
            case Vehicles:
            {
                const char *needCar = "Get in a vehicle first.";
                rows.push_back(pageRow("Spawn vehicle", VehicleList, "Every car, bike, boat and aircraft in the game."));
                rows.push_back(toggleRow("Get in spawned vehicles", tr::WarpIntoSpawned,
                                         "On: you are put behind the wheel. Off: it appears ahead of you."));
                rows.push_back(toggleRow("Vehicle invincible", tr::VehicleInvincible,
                                         "The vehicle you drive takes no damage and does not burn."));
                rows.push_back(actionRow("Repair vehicle", tr::repairVehicle,
                                         inCar ? "Engine and health to full; puts a fire out. (The dents stay: a Pay 'n' "
                                                 "Spray fixes those.)" : needCar, inCar));
                rows.push_back(actionRow("Flip upright", tr::flipVehicle, inCar ? "Back on its wheels, facing the same way." : needCar, inCar));
                rows.push_back(actionRow("Boost", tr::boostVehicle, inCar ? "A shove forward." : needCar, inCar));
                rows.push_back(actionRow("Random paint", tr::paintVehicle, inCar ? "New colours." : needCar, inCar));
                break;
            }
            case VehicleList:
            {
                const std::vector<tr::Entry> list = tr::vehicles();
                if (list.empty())
                    rows.push_back(actionRow("(the game's names are not loaded yet)", {}, "", false));
                // [crossover] the list may hold the other Stories game's vehicles too (from the player's disc,
                // tools/stories_pack.py): then one page per game, each by category
                const bool both = std::any_of(list.begin(), list.end(), [](const tr::Entry &e) { return e.other; });
                if (both)
                {
                    for (bool other : {false, true})
                    {
                        const int n = static_cast<int>(
                            std::count_if(list.begin(), list.end(), [&](const tr::Entry &e) { return e.other == other; }));
                        Row r = pageRow("", VehicleGame, "");
                        r.label = std::string(gameName(other)) + (other ? " vehicles (from your disc)" : " vehicles");
                        r.help = other ? std::to_string(n) + " vehicles of " + gameName(true) +
                                             ", read once from your own disc (tools/stories_pack.py); their names end in (" +
                                             (game::isLcs() ? "VCS" : "LCS") + ")."
                                       : std::to_string(n) + " vehicles of this game.";
                        r.select = [other] { g_vehOther = other, g_stack.push_back({VehicleGame}); };
                        r.stripe = gameColour(other);
                        rows.push_back(std::move(r));
                    }
                    break;
                }
                for (const tr::Entry &e : list)
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::spawnVehicle(id); },
                                             tr::toggle(tr::WarpIntoSpawned) ? "Enter: spawn it and get in." : "Enter: spawn it ahead of you."));
                break;
            }
            case VehicleGame: // [crossover] one game's vehicles: its categories
            {
                const std::vector<tr::Entry> list = tr::vehicles();
                for (int c = 0; c < 4; ++c)
                {
                    const int n = static_cast<int>(std::count_if(list.begin(), list.end(), [&](const tr::Entry &e) {
                        return e.other == g_vehOther && vehCat(e.cls) == c;
                    }));
                    if (!n)
                        continue;
                    Row r = pageRow("", VehicleCat, "");
                    r.label = std::string(kVehCats[c]) + " (" + std::to_string(n) + ")";
                    r.help = std::string(gameName(g_vehOther)) + ": " + (c == 3 ? "helicopters and planes." : "every one of them.");
                    r.select = [c] { g_vehCat = c, g_stack.push_back({VehicleCat}); };
                    r.stripe = gameColour(g_vehOther);
                    rows.push_back(std::move(r));
                }
                break;
            }
            case VehicleCat: // [crossover] one category of one game's vehicles
            {
                const std::string help = std::string(gameName(g_vehOther)) + (g_vehOther ? " (from your disc). " : ". ") +
                                         (tr::toggle(tr::WarpIntoSpawned) ? "Enter: spawn it and get in." : "Enter: spawn it ahead of you.");
                for (const tr::Entry &e : tr::vehicles())
                    if (e.other == g_vehOther && vehCat(e.cls) == g_vehCat)
                    {
                        Row r = actionRow(e.name, [id = e.id] { tr::spawnVehicle(id); }, help);
                        r.stripe = gameColour(g_vehOther);
                        rows.push_back(std::move(r));
                    }
                break;
            }
            case Missions:
                rows.push_back(pageRow("Story missions", StoryList, "Every story mission, in story order."));
                rows.push_back(pageRow("Side jobs", JobList, "Paramedic, Vigilante, Fire Truck, Air Rescue, Taxi."));
                break;
            case StoryList:
            case JobList:
            {
                const std::vector<tr::Entry> list = tr::missions(page == StoryList);
                if (list.empty())
                    rows.push_back(actionRow("(the game's names are not loaded yet)", {}, "", false));
                const bool running = tr::missionRunning();
                const std::string help = running ? "A mission is running: finish or fail it first."
                                                 : "Enter: start it here, as its marker would. Saving after a mission "
                                                   "played out of turn keeps what it changed.";
                for (const tr::Entry &e : list)
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::launchMission(id); }, help, !running));
                break;
            }
            case Teleport:
                rows.push_back(actionRow("Save position", tr::savePosition, "Remember where you are now."));
                rows.push_back(actionRow("Note position for the bridge", tr::notePosition,
                                         "Appends x y z (and Liberty City's x y z) to stories/bridge_points.txt in the data folder."));
                rows.push_back(actionRow("Return to saved position", tr::returnToSaved,
                                         tr::hasSavedPosition() ? "Back to the position you saved." : "Save a position first.",
                                         tr::hasSavedPosition()));
                for (const tr::Entry &e : tr::landmarks())
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::teleport(id); }, "Enter: go there (in your vehicle, if you drive one)."));
                break;
            case Camera:
            {
                rows.push_back(toggleRow("Free camera", tr::FreeCamera,
                                         "Fly: W / A / S / D, E up, Q down, the mouse looks, Shift faster, Ctrl slower, "
                                         "Z / X zoom. Gamepad: sticks, RT / LT, A / B, D-pad. The player waits. Hotkey: "
                                         "F6 (Back + LB)."));
                static const int kSpeeds[] = {5, 10, 20, 40, 80};
                int at = 0;
                while (at + 1 < static_cast<int>(std::size(kSpeeds)) && kSpeeds[at] < tr::freeCameraSpeed())
                    ++at;
                char speed[16];
                std::snprintf(speed, sizeof(speed), "%d m/s", kSpeeds[at]);
                rows.push_back(choiceRow("Free camera speed", speed,
                                         [at](int d) {
                                             const int n = static_cast<int>(std::size(kSpeeds));
                                             tr::setFreeCameraSpeed(kSpeeds[std::clamp(at + d, 0, n - 1)]);
                                         },
                                         [] { tr::setFreeCameraSpeed(20); }, "Left / Right: slower or faster. Enter: 20 m/s."));
                rows.push_back(actionRow("Teleport player here", tr::teleportToCamera,
                                         tr::toggle(tr::FreeCamera) ? "Puts you on the ground below the camera and ends it."
                                                                    : "Turn the free camera on first.",
                                         tr::toggle(tr::FreeCamera)));
                rows.push_back(toggleRow("Hide HUD", tr::HideHud, "No radar, money, weapon or stars: for clean pictures."));
                break;
            }
            case World:
            {
                char hour[16];
                std::snprintf(hour, sizeof(hour), "%02d:00", g_hour);
                rows.push_back(choiceRow("Time of day", hour, [](int d) { g_hour = wrap(g_hour + d, 24); tr::setTime(g_hour); },
                                         [] { tr::setTime(g_hour); }, "Left / Right: an hour back or on."));
                rows.push_back(toggleRow("Freeze time", tr::FreezeTime, "The clock stands still at the time it shows now."));
                const std::vector<tr::Entry> weathers = tr::weathers();
                g_weather = std::clamp(g_weather, 0, static_cast<int>(weathers.size()) - 1);
                rows.push_back(choiceRow("Weather", weathers[g_weather].name,
                                         [n = static_cast<int>(weathers.size())](int d) {
                                             g_weather = wrap(g_weather + d, n);
                                             tr::setWeather(tr::weathers()[g_weather].id);
                                         },
                                         [] { tr::setWeather(tr::weathers()[g_weather].id); },
                                         "Left / Right: change it now. Normal lets it change by itself again."));
                char speed[16];
                std::snprintf(speed, sizeof(speed), "%d%%", tr::gameSpeed());
                rows.push_back(choiceRow("Game speed", speed,
                                         [](int d) { tr::setGameSpeed(std::clamp(tr::gameSpeed() + d * 25, 25, 200)); },
                                         [] { tr::setGameSpeed(100); }, "Left / Right: slower or faster. Enter: normal speed."));
                break;
            }
            }
            std::erase_if(rows, [](const Row &r) { return !tr::shown(r.label); }); // [lcs] rows the game lacks
            for (Row &r : rows)                                                   // [lcs] and its own help texts
                r.help = tr::help(r.label, r.help);
            if (!tr::ready())
                for (Row &r : rows)
                    if (r.kind != Value::Page)
                    {
                        r.enabled = false;
                        r.help = "Available once you are in the game.";
                    }
            return rows;
        }

        void open()
        {
            g_open = true;
            g_hour = tr::clockHour();
        }

        void close()
        {
            g_open = false;
            g_releaseWait = true; // the game would otherwise see the keys that closed the menu held
        }

        // ---- Drawing ----------------------------------------------------------------------------------------------
        ImU32 rgba(int r, int g, int b, float a = 1.0f) { return theme::rgba(r, g, b, a); }

        ImVec2 textSize(float size, const char *text, float wrapWidth = 0.0f)
        {
            return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, wrapWidth, text);
        }

        void text(ImDrawList *dl, float size, ImVec2 at, ImU32 col, const std::string &s, bool shadow = false)
        {
            if (shadow)
                dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + size * 0.06f, at.y + size * 0.06f), rgba(0, 0, 0, 0.8f), s.c_str());
            dl->AddText(ImGui::GetFont(), size, at, col, s.c_str());
        }

        void drawNotices(const ImGuiViewport *vp, float s)
        {
            const double now = tr::now();
            std::vector<tr::Notice> list = tr::notices(4.0);
            if (g_hideShotNotices)
                std::erase_if(list, [](const tr::Notice &n) { return n.screenshot; });
            g_hideShotNotices = false;
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            const float size = 17.0f * s, pad = 8.0f * s;
            // [lcs] on the right under the HUD (clock, health, money: the top ~25%), newest first downwards. The bottom
            // right is where both games put mission subtitles' right half and the area / vehicle names.
            float y = vp->Pos.y + vp->Size.y * 0.30f;
            const size_t first = list.size() > 4 ? list.size() - 4 : 0;
            for (size_t k = list.size(); k-- > first;)
            {
                const float age = static_cast<float>(now - list[k].time);
                const float a = std::clamp((4.0f - age) / 0.5f, 0.0f, 1.0f) * std::clamp(age / 0.15f, 0.0f, 1.0f);
                const ImVec2 ts = textSize(size, list[k].text.c_str());
                const float w = ts.x + pad * 2.0f + 4.0f * s, h = ts.y + pad * 1.5f;
                const ImVec2 p0(vp->Pos.x + vp->Size.x - 24.0f * s - w, y);
                dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), theme::withAlpha(theme::palette().notice, 0.82f * a));
                dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * s, p0.y + h), theme::withAlpha(theme::palette().accent, a));
                text(dl, size, ImVec2(p0.x + 4.0f * s + pad, p0.y + pad * 0.75f), rgba(255, 255, 255, a), list[k].text);
                y += h + 6.0f * s;
            }
        }

        void drawPanel(const ImGuiViewport *vp, float s, const std::vector<Row> &rows, Level &level)
        {
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            const float w = 400.0f * s;
            const float x = vp->Pos.x + 36.0f * s;
            float y = vp->Pos.y + 48.0f * s;
            const theme::Palette &pal = theme::palette(); // [lcs] VCS: pink / violet / cyan
            const ImU32 pink = pal.accent, cyan = pal.path;

            // header: the Vice City sunset (LCS: crimson)
            const float headerH = 66.0f * s;
            dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + headerH), pal.headerA, pal.headerB, pal.headerB, pal.headerA);
            const std::string title = tr::title(); // [lcs]
            const float titleSize = 34.0f * s;
            const ImVec2 ts = textSize(titleSize, title.c_str());
            text(dl, titleSize, ImVec2(x + (w - ts.x) * 0.5f, y + (headerH - ts.y) * 0.5f), rgba(255, 255, 255), title, true);
            y += headerH;

            // the path and the position in the list
            const float subH = 28.0f * s, rowSize = 18.0f * s;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + subH), rgba(0, 0, 0, 0.9f));
            std::string path;
            for (const Level &l : g_stack)
                path += (path.empty() ? "" : "  >  ") + pageTitle(l.page);
            if (g_stack.size() > 2) // the last two are enough
                path = pageTitle(g_stack[g_stack.size() - 2].page) + "  >  " + pageTitle(g_stack.back().page);
            text(dl, 15.0f * s, ImVec2(x + 12.0f * s, y + (subH - 15.0f * s) * 0.5f), cyan, path);
            char count[32];
            std::snprintf(count, sizeof(count), "%d / %d", rows.empty() ? 0 : level.selected + 1, static_cast<int>(rows.size()));
            const float cw = textSize(15.0f * s, count).x;
            text(dl, 15.0f * s, ImVec2(x + w - 12.0f * s - cw, y + (subH - 15.0f * s) * 0.5f), rgba(220, 220, 220), count);
            y += subH;

            // the rows: as many as fit above the help and the keys (12 at most)
            const float rowH = 32.0f * s, padX = 14.0f * s;
            const float hs = 15.0f * s, wrapW = w - 2.0f * padX, ks = 13.0f * s;
            const std::string &help = rows.empty() ? std::string() : rows[level.selected].help;
            const float helpH = help.empty() ? 0.0f : textSize(hs, help.c_str(), wrapW).y + 20.0f * s;
            const float below = 20.0f * s + helpH + ks + 10.0f * s + 12.0f * s; // arrows, help, keys, margin
            const int visible = std::clamp(static_cast<int>((vp->Pos.y + vp->Size.y - y - below) / rowH), 4, 12);
            level.top = std::clamp(level.top, std::max(0, level.selected - visible + 1), std::max(0, level.selected));
            level.top = std::clamp(level.top, 0, std::max(0, static_cast<int>(rows.size()) - visible));
            const int end = std::min(static_cast<int>(rows.size()), level.top + visible);
            for (int i = level.top; i < end; ++i)
            {
                const Row &r = rows[i];
                const bool sel = i == level.selected;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + rowH), sel ? pal.rowSel : pal.row);
                if (sel)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 5.0f * s, y + rowH), pink);
                if (r.stripe) // [crossover]
                    dl->AddRectFilled(ImVec2(x + w - 5.0f * s, y), ImVec2(x + w, y + rowH), r.stripe);
                const ImU32 col = !r.enabled ? (sel ? pal.textOffSel : pal.textOff) : sel ? pal.textSel : pal.text;
                const float ty = y + (rowH - rowSize) * 0.5f;
                {
                    // [crossover] a label too long for the row (beside its value) is drawn smaller
                    const float room = w - 2.0f * padX - (r.kind == Value::None ? 0.0f : 3.0f * rowSize);
                    const float lw = textSize(rowSize, r.label.c_str()).x;
                    const float ls = lw > room && lw > 0.0f ? std::max(rowSize * 0.7f, rowSize * room / lw) : rowSize;
                    text(dl, ls, ImVec2(x + padX, ty + (rowSize - ls) * 0.5f), col, r.label);
                }
                std::string value;
                ImU32 vcol = col;
                switch (r.kind)
                {
                case Value::On: value = "ON", vcol = sel ? pal.onSel : pal.on; break;
                case Value::Off: value = "OFF", vcol = sel ? pal.offSel : pal.off; break;
                case Value::Choice: value = sel ? "<  " + r.value + "  >" : r.value; break;
                case Value::Page: value = ">"; break;
                case Value::None: break;
                }
                if (!value.empty())
                    text(dl, rowSize, ImVec2(x + w - padX - textSize(rowSize, value.c_str()).x, ty), vcol, value);
                y += rowH;
            }
            // more above / below
            if (level.top > 0 || end < static_cast<int>(rows.size()))
            {
                const float ah = 14.0f * s;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ah + 6.0f * s), rgba(0, 0, 0, 0.9f));
                const float cx = x + w * 0.5f, cy = y + 3.0f * s;
                const ImU32 on = rgba(240, 240, 240), off = rgba(90, 90, 90);
                dl->AddTriangleFilled(ImVec2(cx - 18.0f * s, cy + ah * 0.5f), ImVec2(cx - 10.0f * s, cy), ImVec2(cx - 2.0f * s, cy + ah * 0.5f),
                                      level.top > 0 ? on : off);
                dl->AddTriangleFilled(ImVec2(cx + 2.0f * s, cy + ah * 0.5f - 2.0f * s), ImVec2(cx + 18.0f * s, cy + ah * 0.5f - 2.0f * s),
                                      ImVec2(cx + 10.0f * s, cy + ah), end < static_cast<int>(rows.size()) ? on : off);
                y += ah + 6.0f * s;
            }

            // the selected row's help, and the keys
            if (!help.empty())
            {
                const float h = helpH - 4.0f * s;
                y += 4.0f * s;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), rgba(0, 0, 0, 0.85f));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 2.0f * s), pink);
                dl->AddText(ImGui::GetFont(), hs, ImVec2(x + padX, y + 8.0f * s), rgba(225, 225, 225), help.c_str(), nullptr, wrapW);
                y += h;
            }
            // the pad's names while one is there (the on-screen controls are a PlayStation pad: Cross / Circle)
            const platform::GamepadFamily family = platform::gamepadConnected() ? platform::gamepadFamily() : platform::GamepadFamily::None;
            const std::string keys = family == platform::GamepadFamily::PlayStation ? "D-pad: select / change   Cross: apply   Circle: back"
                                     : family == platform::GamepadFamily::Nintendo  ? "D-pad: select / change   B: apply   A: back"
                                     : family != platform::GamepadFamily::None      ? "D-pad: select / change   A: apply   B: back"
                                                                                    : "Up/Down: select   Left/Right: change   Enter: apply   Backspace: back";
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ks + 10.0f * s), rgba(0, 0, 0, 0.7f));
            text(dl, ks, ImVec2(x + padX, y + 5.0f * s), rgba(170, 170, 180), keys);
        }
    }

    bool trainerMenuOpen() { return g_open; }
    bool trainerMenuBlocksInput() { return g_open || g_releaseWait; }
    void hideScreenshotNotices() { g_hideShotNotices = true; }

    void drawTrainerMenu()
    {
        const int trainerButton = vcs::input::shortcutButton(vcs::input::Shortcut::Trainer); // Gamepad > Shortcuts
        const bool chord = trainerButton >= 0 && padDown(platform::GAMEPAD_BACK) &&
                           padDown(static_cast<platform::GamepadButton>(trainerButton));
        const bool chordPressed = chord && !g_chordWasDown;
        g_chordWasDown = chord;
        const int key = settings::value("controls.trainer_key");
        // the free camera's own hotkey (any time, the trainer open or not)
        const int camButton = vcs::input::shortcutButton(vcs::input::Shortcut::FreeCamera);
        const bool camChord = camButton >= 0 && padDown(platform::GAMEPAD_BACK) &&
                              padDown(static_cast<platform::GamepadButton>(camButton));
        const bool camChordPressed = camChord && !g_camChordWasDown;
        g_camChordWasDown = camChord;
        if (!settingsMenuOpen() && (pressedKey(settings::value("controls.freecam_key")) || camChordPressed))
            tr::setToggle(tr::FreeCamera, !tr::toggle(tr::FreeCamera));
#ifdef __ANDROID__
        // [android] the on-screen controls' trainer button ([stories] the game in front's only)
        const bool touchToggle = !platform::windowHidden() && vcs_android::takeTrainerToggle();
#else
        const bool touchToggle = false;
#endif
        if (settingsMenuOpen()) // one menu at a time
        {
            if (g_open)
                close();
        }
        else if (pressedKey(key) || chordPressed || touchToggle)
            g_open ? close() : open();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float s = std::clamp(vp->Size.y / 720.0f, 0.75f, 4.0f);
        drawNotices(vp, s);
        if (!g_open)
            return;

        Level &level = g_stack.back();
        std::vector<Row> rows = buildPage(level.page);
        const int n = static_cast<int>(rows.size());
        level.selected = n ? std::clamp(level.selected, 0, n - 1) : 0;
        const Input in = readInput();
        if (n)
        {
            if (in.up)
                level.selected = wrap(level.selected - 1, n);
            if (in.down)
                level.selected = wrap(level.selected + 1, n);
            Row &r = rows[level.selected]; // (a row's action may push a page: `level` is not used after it)
            if (r.enabled && r.change && (in.left || in.right))
                r.change(in.right ? 1 : -1);
            else if (r.enabled && r.select && in.select)
                r.select();
        }
        if (in.back)
        {
            if (g_stack.size() > 1)
                g_stack.pop_back();
            else
                close();
        }
        if (!g_open)
            return;
        // drawn after the input, so a press shows in the same frame (the page may have changed)
        Level &shown = g_stack.back();
        const std::vector<Row> now = buildPage(shown.page);
        shown.selected = now.empty() ? 0 : std::clamp(shown.selected, 0, static_cast<int>(now.size()) - 1);
        drawPanel(vp, s, now, shown);
    }
}
