// [lcs] The trainer menu's view of the trainer (src/ui/trainer_menu.cpp: `namespace tr = game::trainer`): VCS's
// (game/vcs_trainer.h) or, when the game is Liberty City Stories, LCS's (game/lcs_trainer.h). Types, toggles, notices
// and anything not declared here are vcs::trainer's (the using-directive); the calls below pick the game at run time.
#pragma once

#include "game/game_table.h"
#include "game/lcs_save.h"
#include "game/lcs_trainer.h"
#include "game/vcs_save_anywhere.h"
#include "game/vcs_trainer.h"

#include <string>
#include <vector>

namespace game::trainer
{
    using namespace vcs::trainer;

#define GAME_TRAINER_PICK(call) (game::isLcs() ? lcs::trainer::call : vcs::trainer::call)
    inline bool toggle(Toggle t) { return GAME_TRAINER_PICK(toggle(t)); }
    inline void setToggle(Toggle t, bool on) { GAME_TRAINER_PICK(setToggle(t, on)); }
    inline bool ready() { return GAME_TRAINER_PICK(ready()); }
    inline bool inVehicle() { return GAME_TRAINER_PICK(inVehicle()); }
    inline void heal() { GAME_TRAINER_PICK(heal()); }
    inline void addMoney(int amount) { GAME_TRAINER_PICK(addMoney(amount)); }
    inline void setWanted(int stars) { GAME_TRAINER_PICK(setWanted(stars)); }
    inline void giveWeapon(int type) { GAME_TRAINER_PICK(giveWeapon(type)); }
    inline void giveLoadout(int which) { GAME_TRAINER_PICK(giveLoadout(which)); }
    inline void removeWeapons() { GAME_TRAINER_PICK(removeWeapons()); }
    inline void spawnVehicle(int model) { GAME_TRAINER_PICK(spawnVehicle(model)); }
    inline void repairVehicle() { GAME_TRAINER_PICK(repairVehicle()); }
    inline void flipVehicle() { GAME_TRAINER_PICK(flipVehicle()); }
    inline void boostVehicle() { GAME_TRAINER_PICK(boostVehicle()); }
    inline void paintVehicle() { GAME_TRAINER_PICK(paintVehicle()); }
    inline void teleport(int landmark) { GAME_TRAINER_PICK(teleport(landmark)); }
    inline void savePosition() { GAME_TRAINER_PICK(savePosition()); }
    inline void notePosition() { GAME_TRAINER_PICK(notePosition()); }
    inline void returnToSaved() { GAME_TRAINER_PICK(returnToSaved()); }
    inline bool hasSavedPosition() { return GAME_TRAINER_PICK(hasSavedPosition()); }
    inline void teleportToCamera() { GAME_TRAINER_PICK(teleportToCamera()); }
    inline void setTime(int hour) { GAME_TRAINER_PICK(setTime(hour)); }
    inline void setWeather(int weather) { GAME_TRAINER_PICK(setWeather(weather)); }
    inline void launchMission(int index) { GAME_TRAINER_PICK(launchMission(index)); }
    inline bool missionRunning() { return GAME_TRAINER_PICK(missionRunning()); }
    inline void setGameSpeed(int percent) { GAME_TRAINER_PICK(setGameSpeed(percent)); }
    inline int gameSpeed() { return GAME_TRAINER_PICK(gameSpeed()); }
    inline int clockHour() { return GAME_TRAINER_PICK(clockHour()); }
    inline std::vector<Entry> vehicles() { return GAME_TRAINER_PICK(vehicles()); }
    inline std::vector<Entry> weapons() { return GAME_TRAINER_PICK(weapons()); }
    inline std::vector<Entry> loadouts() { return GAME_TRAINER_PICK(loadouts()); }
    inline std::vector<Entry> landmarks() { return GAME_TRAINER_PICK(landmarks()); }
    inline std::vector<Entry> weathers() { return GAME_TRAINER_PICK(weathers()); }
    inline std::vector<Entry> missions(bool story) { return GAME_TRAINER_PICK(missions(story)); }
#undef GAME_TRAINER_PICK

    // the rows the current game has, a row's help text in this game (`fallback`: the menu's), the menu's title
    inline bool shown(const std::string &label) { return !game::isLcs() || lcs::trainer::shown(label); }
    inline std::string help(const std::string &label, const std::string &fallback)
    {
        if (!game::isLcs())
            return fallback;
        const std::string h = lcs::trainer::help(label);
        return h.empty() ? fallback : h;
    }
    inline const char *title() { return game::isLcs() ? "LCS TRAINER" : "VCS TRAINER"; }
}

// The trainer's Save game page (`namespace sa = game::saves`): VCS's save anywhere (game/vcs_save_anywhere.h) or
// LCS's (game/lcs_save.h). Both use slot kQuickSlot (8) for the quick save; LCS also has a quick load and no
// "put me back after loading" (the row is hidden there: lcs::trainer::shown).
namespace game::saves
{
    using vcs::saveanywhere::kQuickSlot;
    using vcs::saveanywhere::Slot;
    static_assert(vcs::saveanywhere::kQuickSlot == lcs::save::kQuickSlot);

    inline void saveHere() { game::isLcs() ? lcs::save::saveHere() : vcs::saveanywhere::saveHere(); }
    inline void quickSave() { game::isLcs() ? lcs::save::quickSave() : vcs::saveanywhere::quickSave(); }
    inline std::string whyNot() { return game::isLcs() ? lcs::save::whyNot() : vcs::saveanywhere::whyNot(); }
    inline bool busy() { return game::isLcs() ? lcs::save::busy() : vcs::saveanywhere::busy(); }
    inline bool restoreAfterLoad() { return !game::isLcs() && vcs::saveanywhere::restoreAfterLoad(); }
    inline void setRestoreAfterLoad(bool on)
    {
        if (!game::isLcs())
            vcs::saveanywhere::setRestoreAfterLoad(on);
    }
    inline bool hasQuickLoad() { return game::isLcs(); }
    inline void quickLoad()
    {
        if (game::isLcs())
            lcs::save::quickLoad();
    }
    inline std::vector<Slot> slots()
    {
        if (!game::isLcs())
            return vcs::saveanywhere::slots();
        std::vector<Slot> out;
        for (const lcs::save::Slot &s : lcs::save::slots())
            out.push_back({s.slot, s.name});
        return out;
    }
}
