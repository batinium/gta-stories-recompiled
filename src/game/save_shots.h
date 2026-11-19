#pragma once
// Save pictures for both games: <data>/save_shots/slot<N>.png, shown by the launchers' Saves lists (Android's
// SavesMenu, the desktop launcher's Saves sheet through tools/backup.py). Split out of vcs_save_anywhere.cpp (which
// had them Android-only) so LCS and the desktop builds take them too.
//
// A PS2 save keeps no picture and the game's own Save screen covers the picture as a slot is written, so the picture
// is taken before: every 20 s of free play into latest.png (a safehouse / game-menu save gets that one), and once more
// as a save of ours starts (fresh.png: Save game here / Quick save). A slot saved gets the matching file copied in,
// off the game thread; the PNG is written off the main thread too (ps2control::requestScreenshot). `VCS_SAVE_SHOTS=0`
// turns all of it off.

namespace saveshots
{
    // Game thread, every frame the game is run: playing with no menu, cutscene or script camera up. A game that
    // stops calling this (a load, a pause with no frames) stops the pictures after 0.4 s.
    void setFreePlay(bool freePlay);

    // Main thread, each host frame (platform-independent): takes the periodic and the "fresh" pictures.
    void onHostFrame();

    // Game thread: a save of ours is about to start - take its picture now (the next host frame). freshDone() says
    // when the shot is on disk (or will not come), so a menu can wait for it before it opens.
    void requestFresh();
    bool freshDone();

    // Game thread or any: a slot (1..8) was saved / deleted. fresh: the save of ours that requestFresh() took the
    // picture for. The copy runs on its own thread.
    void pictureForSlot(int slot, bool fresh);
    void removePicture(int slot);
}
