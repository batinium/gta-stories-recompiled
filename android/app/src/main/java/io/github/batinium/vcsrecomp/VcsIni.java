// [android] The game's own settings file, files/vcs.ini, as the launcher sees it before a start. The game reads it once
// when it starts (src/main.cpp main(): settings::load(exeDirectory() / "vcs.ini"), exeDirectory() = VCS_APP_DIR on
// Android = getFilesDir(), VCSActivity) and rewrites it whenever a setting changes in its menu or the side panel
// (src/settings/settings.cpp save()). The format is that of settings.cpp: one "key=integer" a line; blank lines and
// lines starting with # or ; are comments; a key that is absent has the game's default.
//
// The launcher only changes the keys listed in SETTINGS (each one registered by the game: see the file:line beside it),
// re-reads the file before every write (so a value changed in the game since the last look is never overwritten), and
// rewrites the file as save() does: its keys in place, missing ones appended, every other line kept as it was.
package io.github.batinium.vcsrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class VcsIni {
    private VcsIni() {}

    /** One of the game's settings: its vcs.ini key, its choices (values and the game's own labels) and its default */
    static final class Setting {
        final String key, label, help;
        final int def;
        final int[] values;
        final String[] names;

        Setting(String key, String label, int def, int[] values, String[] names, String help) {
            this.key = key;
            this.label = label;
            this.def = def;
            this.values = values;
            this.names = names;
            this.help = help;
        }

        int index(int v) {
            for (int i = 0; i < values.length; i++)
                if (values[i] == v)
                    return i;
            return -1;
        }

        String name(int v) {
            int i = index(v);
            return i >= 0 ? names[i] : "Other (" + v + ")";
        }
    }

    private static final int[] OFF_ON = {0, 1};
    private static final String[] OFF_ON_NAMES = {"Off", "On"};

    // The choices exactly as the game registers them; a value outside them is ignored by the game's load() (take()).
    static final Setting ASPECT = new Setting("display.aspect", "Aspect ratio", 43, // src/main.cpp:474
            new int[] {0, 43, 1610, 169, 219, 329, 54, 11, 34},
            new String[] {"Match window", "4:3 (original)", "16:10", "16:9", "21:9", "32:9", "5:4", "1:1", "3:4"},
            "Match window follows the screen's shape (folded or unfolded). 16:9 and wider show more to the sides, "
                    + "5:4, 1:1 and 3:4 more above and below; 4:3 is the original picture.");
    static final Setting HUD = new Setting("display.hud", "HUD", 0, // src/main.cpp:490
            new int[] {0, 2, 1}, new String[] {"Keep 4:3", "Screen edges", "Stretch"},
            "Away from 4:3: keep the HUD in a 4:3 box in the middle, move each part to the screen edge it is near, or "
                    + "stretch it.");
    static final Setting CUTSCENE_BARS = new Setting("display.cutscene_bars", "Cutscene black bars", 1, // src/main.cpp:504
            OFF_ON, OFF_ON_NAMES, "The black bars above and below cutscenes, as on the PS2 (always on at 4:3).");
    static final Setting SCALE = new Setting("display.scale", "Internal resolution", 1, // src/main.cpp:520
            new int[] {1, 2, 3, 4, 6, 8, 0},
            new String[] {"1x (original)", "2x", "3x", "4x", "6x", "8x", "Match window"},
            "Renders at a multiple of 640x448, or at the screen's own resolution. Sharper costs GPU time and heat: "
                    + "2x is a good start on a phone.");
    static final Setting TIMING = new Setting("display.timing", "Game timing", 1, // src/main.cpp:584
            new int[] {1, 0}, new String[] {"Frame-rate independent", "Original (PS2)"},
            "Frame-rate independent runs the game up to the frame rate cap; Original paces it on the PS2's VBlank "
                    + "(30 fps, 60 with a cap of 60 or more).");
    static final Setting FPS = new Setting("display.fps", "Frame rate cap", 60, // src/main.cpp:600 (-1 / 0: vcs_framerate.h:23-24)
            new int[] {30, 60, 100, 120, 144, 165, 240, -1, 0},
            new String[] {"30 (original)", "60", "100", "120", "144", "165", "240", "Display refresh", "Unlimited (up to 240)"},
            "60 is the default, 30 the original. Above 60 only with frame-rate independent timing and a fast display.");
    static final Setting VSYNC = new Setting("display.vsync", "Vertical sync", 0, // src/main.cpp:624 (kVsyncDefault 0 off Apple)
            new int[] {0, 1, 2}, new String[] {"Off", "On", "Off, allow tearing"},
            "On waits for the display's refresh (smoother, a little more delay).");
    static final Setting MSAA = new Setting("display.msaa", "Anti-aliasing (MSAA)", 1, // src/main.cpp:675
            new int[] {1, 2, 4, 8}, new String[] {"Off (original)", "2x", "4x", "8x"},
            "Smooths the edges of the 3D picture. Costs GPU time.");
    static final Setting ANISO = new Setting("display.aniso", "Texture filtering", 0, // src/main.cpp:689
            new int[] {0, 2, 4, 8, 16},
            new String[] {"Bilinear (original)", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"},
            "Keeps textures sharp at a slant (roads, walls into the distance).");
    static final Setting REDUCE_POSTFX = new Setting("enhance.reduce_postfx", "Reduce post processing", 0, // src/game/vcs_effects_timing.cpp:697
            OFF_ON, OFF_ON_NAMES, "Lighter, shorter trails, smoke, sprays and shakes above 30 fps. Off: as on the PS2.");
    static final Setting SKIP_INTRO = new Setting("display.skip_intro", "Skip intro movies", 0, // src/main.cpp:650
            OFF_ON, OFF_ON_NAMES, "Straight to the game at start-up.");
    static final Setting FPS_COUNTER = new Setting("display.fps_counter", "Show frame rate", 0, // src/main.cpp:639
            new int[] {0, 1, 2, 3, 4}, new String[] {"Off", "Top left", "Top right", "Bottom left", "Bottom right"},
            "The game's own frames-per-second counter in a corner (the stats overlay below has more).");
    static final Setting PAD_VIBRATION = new Setting("pad.vibration", "Controller vibration", 100, // src/game/vcs_input.cpp:1083 (Integer 0..200)
            new int[] {0, 25, 50, 75, 100, 125, 150, 200},
            new String[] {"Off", "25%", "50%", "75%", "100% (as on the PS2)", "125%", "150%", "200%"},
            "The game's rumble on a connected gamepad.");

    static final Setting[] SETTINGS = {ASPECT, HUD, CUTSCENE_BARS, SCALE, TIMING, FPS, VSYNC, MSAA, ANISO, REDUCE_POSTFX,
            SKIP_INTRO, FPS_COUNTER, PAD_VIBRATION};

    // The game's presets (src/main.cpp:366-388, kBalanced / applyAuthentic): actions in the game, values here
    static final String[] PRESET_KEYS = {"display.aspect", "display.hud", "display.cutscene_bars", "display.scale",
            "display.msaa", "display.aniso", "display.timing", "display.fps", "display.vsync"};

    static Map<String, Integer> balanced() {
        Map<String, Integer> m = new LinkedHashMap<>();
        m.put("display.aspect", 0);
        m.put("display.hud", 2);
        m.put("display.scale", 2);
        m.put("display.msaa", 1);
        m.put("display.aniso", 4);
        m.put("display.timing", 1);
        m.put("display.fps", 60);
        m.put("display.vsync", 0);
        return m;
    }

    static Map<String, Integer> authentic() {
        Map<String, Integer> m = new LinkedHashMap<>();
        for (String k : PRESET_KEYS)
            m.put(k, setting(k).def);
        m.put("display.timing", 0);
        m.put("display.fps", 30);
        return m;
    }

    static Setting setting(String key) {
        for (Setting s : SETTINGS)
            if (s.key.equals(key))
                return s;
        throw new IllegalArgumentException(key);
    }

    static File file(Context c) {
        return new File(c.getFilesDir(), "vcs.ini");
    }

    // ---- reading: a snapshot, taken fresh each time (the game may have changed the file) ----

    private final Map<String, String> found = new HashMap<>();

    static VcsIni read(Context c) {
        VcsIni ini = new VcsIni();
        for (String line : lines(file(c))) {
            String key = keyOf(line);
            if (key != null)
                ini.found.put(key, line.trim().substring(line.trim().indexOf('=') + 1).trim());
        }
        return ini;
    }

    /** The value the game will start with: the file's when it is valid, else the default (as settings.cpp load()) */
    int value(Setting s) {
        String t = found.get(s.key);
        if (t == null)
            return s.def;
        int v = atoi(t);
        return s.index(v) >= 0 || s == PAD_VIBRATION && v >= 0 && v <= 200 ? v : s.def;
    }

    String presetName() {
        if (matches(balanced()))
            return "Balanced";
        if (matches(authentic()))
            return "Authentic (PS2)";
        return "Custom";
    }

    private boolean matches(Map<String, Integer> preset) {
        for (Map.Entry<String, Integer> e : preset.entrySet())
            if (value(setting(e.getKey())) != e.getValue())
                return false;
        return true;
    }

    // ---- writing ----

    static boolean set(Context c, Setting s, int v) {
        Map<String, Integer> m = new LinkedHashMap<>();
        m.put(s.key, v);
        return write(c, m);
    }

    /** Re-reads the file and changes only these keys: in place, else appended; the rest of the file stays as it was */
    static synchronized boolean write(Context c, Map<String, Integer> changes) {
        File f = file(c);
        List<String> lines = lines(f);
        Map<String, Boolean> done = new HashMap<>();
        for (int i = 0; i < lines.size(); i++) {
            String key = keyOf(lines.get(i));
            if (key != null && changes.containsKey(key)) {
                lines.set(i, key + "=" + changes.get(key));
                done.put(key, true);
            }
        }
        for (Map.Entry<String, Integer> e : changes.entrySet())
            if (!done.containsKey(e.getKey()))
                lines.add(e.getKey() + "=" + e.getValue());
        File tmp = new File(f.getPath() + ".launcher.tmp");
        try (Writer out = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            for (String line : lines)
                out.write(line + "\n");
        } catch (IOException e) {
            Log.e("vcs", "[launcher] can't write " + f + ": " + e);
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(f)) {
            tmp.delete();
            return false;
        }
        Log.i("vcs", "[launcher] vcs.ini: " + changes);
        return true;
    }

    private static List<String> lines(File f) {
        List<String> out = new ArrayList<>();
        if (!f.isFile())
            return out;
        try (BufferedReader in = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            for (String line; (line = in.readLine()) != null; )
                out.add(line);
        } catch (IOException e) {
            Log.e("vcs", "[launcher] can't read " + f + ": " + e);
        }
        return out;
    }

    /** settings.cpp's rule: trimmed, not empty, not a # or ; comment, has '=' */
    private static String keyOf(String line) {
        String t = line.trim();
        int eq = t.indexOf('=');
        if (t.isEmpty() || t.charAt(0) == '#' || t.charAt(0) == ';' || eq < 0)
            return null;
        return t.substring(0, eq).trim();
    }

    /** std::atoi: leading digits with an optional sign, 0 if none */
    private static int atoi(String t) {
        int i = 0, n = t.length();
        boolean neg = false;
        if (i < n && (t.charAt(i) == '-' || t.charAt(i) == '+'))
            neg = t.charAt(i++) == '-';
        long v = 0;
        while (i < n && Character.isDigit(t.charAt(i)) && v < Integer.MAX_VALUE)
            v = v * 10 + (t.charAt(i++) - '0');
        return (int) (neg ? -v : v);
    }

    // ---- [lcs] the settings the library's game locks (settings::lockForRun, src/game/lcs_settings.cpp) ----

    static final String LOCKED_FILE = "locked_settings.txt";

    /**
     * The settings that do nothing for the library's game, key -> the game's note ("Vice City Stories only for now:
     * ..."). The game writes the list at each start into its own data folder (src/android/side_panel.cpp
     * writeLockedSettings), so the runtime's list is the only one; before the game's first start there is none.
     */
    static Map<String, String> lockedForGame(Context c) {
        Map<String, String> out = new HashMap<>();
        for (String line : lines(new File(GameFiles.dataDir(c), LOCKED_FILE))) {
            int tab = line.indexOf('\t');
            String key = (tab < 0 ? line : line.substring(0, tab)).trim();
            if (!key.isEmpty())
                out.put(key, tab < 0 ? "" : line.substring(tab + 1).trim());
        }
        return out;
    }

    /** The game's process (:game) alive: it holds its settings in memory and its next save() rewrites every key */
    static boolean gameRunning(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> ps = am != null ? am.getRunningAppProcesses() : null;
        if (ps != null)
            for (ActivityManager.RunningAppProcessInfo p : ps)
                if (p.processName != null && p.processName.endsWith(":game"))
                    return true;
        return false;
    }
}
