// What the launcher calls things on each OS, in one place: the binaries, the file manager, folder separators in text.
#pragma once

namespace launcher::os
{
#if defined(__APPLE__)
    constexpr bool kMac = true;
#else
    constexpr bool kMac = false;
#endif
#ifdef _WIN32
    constexpr bool kWindows = true;
    constexpr const char *kGameExe = "vcs.exe";
    constexpr const char *kLauncherExe = "GTAStoriesLauncher.exe";
    constexpr const char *kFileManager = "Explorer";
    constexpr const char *kShortcutName = "GTA Stories Recompiled.lnk";
    constexpr const char *kBuildGame = "build\\vcs.exe"; // as the player sees it in messages
    constexpr const char *kBuildIni = "build\\vcs.ini";
    constexpr const char *kTools = "tools\\";
#elif defined(__APPLE__)
    constexpr bool kWindows = false;
    constexpr const char *kGameExe = "vcs";
    constexpr const char *kLauncherExe = "GTAStoriesLauncher.app";
    constexpr const char *kFileManager = "Finder";
    constexpr const char *kShortcutName = "GTA Stories Recompiled";
    constexpr const char *kBuildGame = "build/vcs";
    constexpr const char *kBuildIni = "build/vcs.ini";
    constexpr const char *kTools = "tools/";
#else
    constexpr bool kWindows = false;
    constexpr const char *kGameExe = "vcs";
    constexpr const char *kLauncherExe = "GTAStoriesLauncher";
    constexpr const char *kFileManager = "Files";
    constexpr const char *kShortcutName = "gta-stories-launcher.desktop";
    constexpr const char *kBuildGame = "build/vcs";
    constexpr const char *kBuildIni = "build/vcs.ini";
    constexpr const char *kTools = "tools/";
#endif
}
