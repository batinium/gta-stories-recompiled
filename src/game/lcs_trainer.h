// [lcs] The trainer (docs/TRAINER.md) for Liberty City Stories: the same menu (src/ui/trainer_menu.cpp, through
// game/trainer_select.h) and the same control-channel command (`trainer ...`), with LCS's own script commands,
// addresses and lists. What it does goes through the game's own script commands run as short scripts on LCS's script
// VM (its own copy of the rig's StartNewScript: the rig's is VCS's), started at CTheScripts::Process.
//
// Not in LCS (rows hidden in the menu, see shown()): the save page's put-back after a load. The save page drives game/lcs_save.h; the free camera is game/lcs_freecam.h, Hide HUD lcs_hud.cpp's.
#pragma once

#include "game/vcs_trainer.h"

#include <string>
#include <vector>

class PS2Runtime;

namespace lcs::trainer
{
    using vcs::trainer::Entry;
    using vcs::trainer::Toggle;

    // From applyLcs (lcs_overrides.cpp): the control-channel command and the per-frame hook.
    void install(PS2Runtime &runtime);

    // The menu's API, as vcs::trainer's (game/vcs_trainer.h); notices go to vcs::trainer's list.
    bool toggle(Toggle t);
    void setToggle(Toggle t, bool on);
    bool ready();
    bool inVehicle();
    bool nearVehicle(); // [android] on foot with an enterable vehicle in reach (touch overlay's door icon); see docs/ANDROID.md
    void heal();
    void addMoney(int amount);
    void setWanted(int stars);
    void giveWeapon(int type);
    void giveLoadout(int which);
    void removeWeapons();
    void spawnVehicle(int model);
    void repairVehicle();
    void flipVehicle();
    void boostVehicle();
    void paintVehicle();
    void teleport(int landmark);
    // [dualworld] the border handover (game/dualworld.h): as vcs::trainer::arrive / carry
    void arrive(float x, float y, float z, float heading, int model, float speed);
    void carry(int money, int health);
    // [dualworld] the rest of the kit: wanted level cleared (crossing is an escape), weapons replaced by these (this
    // game's types; ammo), `current` (a type, 0 none) put in hand, armour set as it was (< 0: left alone)
    void carryKit(int armour, const int (*weapons)[2], int count, int current);
    // [dualworld] `type` (a weapon the player has) put in hand: the one carried across, after the vehicle's drive-by
    // weapon took the hand
    void selectWeapon(int type);
    void savePosition();
    void notePosition(); // "Note position for the bridge": a line into stories/bridge_points.txt (game/bridge_points.h)
    void returnToSaved();
    bool hasSavedPosition();
    void teleportToCamera(); // the free camera's "Teleport player here" (game/lcs_freecam.h)
    void setTime(int hour);
    void setWeather(int weather);
    void launchMission(int index);
    bool missionRunning();
    void setGameSpeed(int percent);
    int gameSpeed();
    int clockHour();

    std::vector<Entry> vehicles();
    std::vector<Entry> weapons();
    std::vector<Entry> loadouts();
    std::vector<Entry> landmarks();
    std::vector<Entry> weathers();
    std::vector<Entry> missions(bool story);
    // Before the game starts (main.cpp): the disc's DATA/MAIN.SCM, for the mission list's names
    void setMissionScript(const std::vector<uint8_t> &mainScm);

    // The menu's rows LCS has (false: hidden), its help text for a row (empty: the menu's own), the menu's title.
    bool shown(const std::string &label);
    std::string help(const std::string &label);
}
