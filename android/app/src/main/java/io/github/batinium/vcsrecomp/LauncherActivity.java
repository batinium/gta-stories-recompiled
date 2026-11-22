// [android] The app's front end (docs/ANDROID_LAUNCHER.md), the phone's counterpart of the desktop launcher without the
// build steps: the player's disc (picked and checked in place), the game library built from it on their PC (imported
// into the app's private folder, read-only), the settings the game starts with, then Play. "Test graphics" starts the
// smoke test (src/android/smoke_main.cpp).
//
// The screen is two plain views: WaveView (the animated background) and MenuView (categories and their items, by touch
// or gamepad). This class only says what the items are and what they do. No AndroidX.
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.Map;

public class LauncherActivity extends Activity implements MenuView.Host {
    private static final int PICK_DISC = 1;
    private static final int PICK_LIBRARY = 2;
    private static final int PICK_LSFG = 3;
    private static final int PICK_LSFG_FOLDER = 4;
    private static final int PICK_GPU_DRIVER = 5; // [gpu-driver]
    private static final int SAVE_REPORT = 40; // [report]

    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private WaveView waves;
    private MenuView menu;
    private boolean busy;
    // a running task's line, or the last failure, shown in place of the item's state
    private String discNote, libraryNote, lsfgNote, driverNote;
    private boolean autoFindTried;
    // read in refresh(), so drawing never touches the disk
    private VcsIni ini;
    private boolean gameRunning;
    // [lcs] vcs.ini keys the library's game locks, with its note (VcsIni.lockedForGame)
    private Map<String, String> lockedIni = new java.util.HashMap<>();
    // [saves] the Saves category: import / export of memory-card saves (SavesMenu)
    private SavesMenu saves;
    // [mods] the Mods category: tuning mods for existing vehicle / weapon slots (ModsMenu)
    private ModsMenu mods;
    // [update] About's update rows: the fork's releases on GitHub (UpdateMenu)
    private UpdateMenu updates;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // [lcs] both games side by side: the layout from before the selector moved over once, the game's colours
        GameFiles.migrate(this);
        Palette.select(GameFiles.game(this));
        getWindow().setDecorFitsSystemWindows(false);
        root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000 | Palette.GLASS);
        waves = new WaveView(this);
        root.addView(waves, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        menu = new MenuView(this, this);
        menu.setBuildTag(versionName() + " · " + BuildConfig.VCS_BUILD_TAG);
        root.addView(menu, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        // the safe area: system bars and the camera cutout, whichever screen of the foldable is in use
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            Immersive.log(this, "launcher insets", insets);
            menu.setInsets(i.left, i.top, i.right, i.bottom);
            return WindowInsets.CONSUMED;
        });
        setContentView(root);
        buildMenu();
        refresh();
        if (state != null)
            menu.select(state.getInt("cat", 0), state.getInt("item", 0));
        menu.requestFocus();
    }

    // ---- [lcs] the game selector (MenuView's header switch, Y on a pad, Play > Game) ------------------------------------

    private static final DiscCheck.Game[] GAMES = DiscCheck.GAMES;

    // [stories] "Both cities" is a third choice of the one game selector (header switch, Play > Game, Y) in a
    // both-cities build (BuildConfig.VCS_STORIES); the choice is remembered. Public builds only have LCS / VCS.
    private static final String BOTH_NAME = "Both cities", BOTH_SHORT = "BOTH";

    private boolean bothMode() {
        return BuildConfig.VCS_STORIES && GameFiles.canChooseGame()
                && getSharedPreferences("launcher_ui", MODE_PRIVATE).getBoolean("both_cities", false);
    }

    private void setBothMode(boolean on) {
        getSharedPreferences("launcher_ui", MODE_PRIVATE).edit().putBoolean("both_cities", on).commit();
    }

    /** the selector's choices: the games, then (both-cities builds) "Both cities" */
    private int choiceCount() {
        return GAMES.length + (BuildConfig.VCS_STORIES ? 1 : 0);
    }

    private int gameIndex() {
        if (bothMode())
            return GAMES.length;
        String g = GameFiles.game(this);
        for (int i = 0; i < GAMES.length; i++)
            if (GAMES[i].id.equals(g))
                return i;
        return 0;
    }

    private String gameSwitchNote() {
        if (!GameFiles.canChooseGame())
            return "This personal build has " + GameFiles.libraryGameInfo(this).name + " inside: it runs that game only.";
        if (gameRunning)
            return "The game is running: quit it first to switch games.";
        return null;
    }

    /** A row's full explanation (MenuView: long press, X, the row's "i"): a plain, scrolling dialog */
    @Override
    public void showHelp(String title, String text) {
        android.widget.TextView t = new android.widget.TextView(this);
        t.setText(text);
        t.setTextSize(15);
        t.setPadding(48, 24, 48, 0);
        ScrollView sv = new ScrollView(this);
        sv.addView(t);
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setView(sv)
                .setPositiveButton("OK", null)
                .show());
    }

    @Override
    public void pickGame(int index) {
        if (index < 0 || index >= choiceCount())
            return;
        String note = gameSwitchNote();
        if (note != null || busy) {
            Toast.makeText(this, note != null ? note : "Busy…", Toast.LENGTH_SHORT).show();
            return;
        }
        if (index == GAMES.length) { // [stories] Both cities
            if (!bothMode()) {
                setBothMode(true);
                refresh();
            }
            return;
        }
        boolean wasBoth = bothMode();
        if (wasBoth)
            setBothMode(false);
        String id = GAMES[index].id;
        if (id.equals(GameFiles.game(this))) {
            if (wasBoth)
                refresh();
            return;
        }
        GameFiles.selectGame(this, id);
        Log.i("vcs", "[launcher] game: " + id);
        discNote = libraryNote = null;
        Palette.fadeTo(id, () -> {
            waves.invalidate();
            menu.invalidate();
            root.setBackgroundColor(0xFF000000 | Palette.GLASS);
        });
        refresh();
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("cat", menu.selectedCategory());
        out.putInt("item", menu.selectedItem());
    }

    // ---- the menu ---------------------------------------------------------------------------------------------------

    private static String[] percents(int[] values) {
        String[] s = new String[values.length];
        for (int i = 0; i < values.length; i++)
            s[i] = values[i] + " %";
        return s;
    }

    private static int indexOf(int[] values, int v) {
        for (int i = 0; i < values.length; i++)
            if (values[i] == v)
                return i;
        return -1;
    }

    /** The nearest of the values (a slider's value from the side panel may lie between our steps) */
    private static int nearest(int[] values, int v) {
        int best = 0;
        for (int i = 1; i < values.length; i++)
            if (Math.abs(values[i] - v) < Math.abs(values[best] - v))
                best = i;
        return best;
    }

    private static final String[] OFF_ON = {"Off", "On"};

    private String gameRunningNote() {
        return gameRunning ? "The game is running: it keeps its own copy of these and would overwrite them. Change them in "
                + "its side panel (Back in the game), or quit it first." : null;
    }

    /** A row for one of the game's own settings (vcs.ini) */
    private MenuView.Item iniItem(VcsIni.Setting s) {
        return new MenuView.Item(s.label)
                .help(() -> gameRunning ? gameRunningNote() : lockedIni.containsKey(s.key) ? lockedNote(s) : s.help)
                .enabled(() -> !gameRunning && !lockedIni.containsKey(s.key))
                .choice(s.names, () -> s.index(ini.value(s)), i -> {
                    if (!VcsIni.gameRunning(this))
                        VcsIni.set(this, s, s.values[i]);
                    refresh();
                });
    }

    /** [lcs] a row the library's game locks: greyed, with the game's reason */
    private String lockedNote(VcsIni.Setting s) {
        String note = lockedIni.get(s.key);
        return (note == null || note.isEmpty() ? "Not used by " + GameFiles.libraryGameInfo(this).name + "." : note)
                + " Your choice is kept for the other game.";
    }

    private MenuView.Item prefToggle(String label, String help, String store, String key, boolean def) {
        return new MenuView.Item(label).help(help)
                .choice(OFF_ON, () -> SidePanel.prefs(this, store).getBoolean(key, def) ? 1 : 0, i -> {
                    SidePanel.prefs(this, store).edit().putBoolean(key, i == 1).commit();
                    refresh();
                });
    }

    /** The controller the game reads (Gamepads.java): Auto, a connected pad (as Android lists it), or touch only; the
     *  options are read again each time the list opens */
    private MenuView.Item controllerItem() {
        final java.util.List<Gamepads.Pad> pads = new java.util.ArrayList<>(); // the options as last opened
        java.util.function.Supplier<String[]> names = () -> {
            pads.clear();
            pads.add(null); // Auto
            pads.addAll(Gamepads.android());
            int v = Gamepads.chosenVendor(this), p = Gamepads.chosenProduct(this);
            String n = Gamepads.chosenName(this);
            boolean here = false;
            for (int i = 1; i < pads.size(); i++)
                here |= pads.get(i).same(v, p, n);
            Gamepads.Pad away = null;
            if (!here && Gamepads.choice(this) == Gamepads.CHOSEN && !n.isEmpty()) {
                away = new Gamepads.Pad();
                away.name = n;
                away.vendor = v;
                away.product = p;
                pads.add(away);
            }
            pads.add(null); // Touch only
            String[] out = new String[pads.size()];
            out[0] = "Auto: the one pressed last";
            for (int i = 1; i < out.length - 1; i++)
                out[i] = pads.get(i) == away ? pads.get(i).name + " (not connected)" : pads.get(i).name;
            out[out.length - 1] = "Touch only: ignore controllers";
            return out;
        };
        return new MenuView.Item("Controller")
                .value(() -> Gamepads.summary(this))
                .help("Which controller the game reads. Auto takes the one you pressed last.")
                .choice(names, () -> {
                    int ch = Gamepads.choice(this);
                    if (ch == Gamepads.TOUCH_ONLY)
                        return pads.size() - 1;
                    if (ch == Gamepads.CHOSEN)
                        for (int i = 1; i < pads.size() - 1; i++)
                            if (pads.get(i).same(Gamepads.chosenVendor(this), Gamepads.chosenProduct(this), Gamepads.chosenName(this)))
                                return i;
                    return 0;
                }, i -> {
                    if (i <= 0)
                        Gamepads.choose(this, Gamepads.AUTO, null);
                    else if (i >= pads.size() - 1)
                        Gamepads.choose(this, Gamepads.TOUCH_ONLY, null);
                    else
                        Gamepads.choose(this, Gamepads.CHOSEN, pads.get(i));
                    refresh();
                });
    }

    private MenuView.Item prefInt(String label, String help, String store, String key, int def, int[] values, String[] names) {
        return new MenuView.Item(label).help(help)
                .choice(names, () -> nearest(values, SidePanel.prefs(this, store).getInt(key, def)), i -> {
                    SidePanel.prefs(this, store).edit().putInt(key, values[i]).commit();
                    refresh();
                });
    }

    /** touch_controls' opacity / scale are floats (fractions), shown as percents */
    private MenuView.Item prefPercent(String label, String help, String key, float def, int[] values) {
        return new MenuView.Item(label).help(help)
                .choice(percents(values), () -> nearest(values, Math.round(TouchControls.prefs(this).getFloat(key, def) * 100)), i -> {
                    TouchControls.prefs(this).edit().putFloat(key, values[i] / 100f).commit();
                    refresh();
                });
    }

    private void buildMenu() {
        // Play
        MenuView.Category play = menu.category("Play", MenuView.G_PLAY);
        play.add(new MenuView.Item("Play").primary()
                .value(() -> bothMode() ? storiesReadiness() : readiness())
                .help(() -> bothMode()
                        ? (storiesReady() ? "Liberty City in front, Vice City on the horizon east of Portland, in one program. "
                                + "Experimental: the side panel does nothing in this mode." : storiesNeedNote())
                        : readinessDetail())
                .enabled(() -> (bothMode() ? storiesReady() : ready()) && !busy)
                .action(() -> { if (bothMode()) playStories(); else play(false); }));
        // [lcs] the game Play starts (the header's switch too): each its own disc, library, saves and colours
        String[] gameNames = new String[choiceCount()];
        for (int i = 0; i < GAMES.length; i++)
            gameNames[i] = GAMES[i].name;
        if (BuildConfig.VCS_STORIES)
            gameNames[GAMES.length] = BOTH_NAME;
        play.add(new MenuView.Item("Game")
                .visible(GameFiles::canChooseGame)
                .help(() -> gameSwitchNote() != null ? gameSwitchNote() : "Each game keeps its own disc, game library, "
                        + "saves and colours; the settings, controls and frame generation are shared. Y on a "
                        + "controller or the switch at the top changes it too.")
                .enabled(() -> gameSwitchNote() == null && !busy)
                .choice(gameNames, this::gameIndex, this::pickGame));
        play.add(new MenuView.Item("Test graphics")
                .value(() -> "No game: a Vulkan and touch test")
                .help("The bring-up test: a spinning picture and touch markers. Back leaves it.")
                .enabled(() -> !busy)
                .action(() -> play(true)));

        // Disc
        MenuView.Category disc = menu.category("Disc", MenuView.G_DISC);
        disc.add(new MenuView.Item("Choose disc image…")
                .value(this::discState)
                .help("Your own PS2 disc, USA release (Vice City Stories SLUS-21590 or Liberty City Stories SLUS-21423), "
                        + "as an .iso image. It stays where it is; the app only reads it. Each game keeps its own disc: "
                        + "picking one switches to its game. Only these two USA discs work: other regions (PAL, ...), "
                        + "the PSP versions and other games do not, and the app will say so.")
                .enabled(() -> !busy && !gameRunning) // [lcs] a disc / library may switch the game
                .action(() -> pick(PICK_DISC)));
        disc.add(new MenuView.Item("Import game library…")
                .value(this::libraryState)
                .help(() -> (GameFiles.bundledGame(this) ? "A personal build from your disc is built into this app. "
                        + "Don't share this APK. " : "") + "Built on your PC from the same disc (" + GameFiles.GAME_LIBRARY + ", see the Player's guide in "
                        + "the project's README), then copied to this phone. The app always keeps its own copy (unlike "
                        + "the disc, it can't be read where it is: Android runs code only from the app's own storage), so "
                        + "you can delete the file afterwards. Each game has its own: importing one switches to its game.")
                .enabled(() -> !busy && !gameRunning)
                .action(() -> pick(PICK_LIBRARY)));
        // [phonebuild] the code pack built here from the chosen disc (experimental; PhoneBuild, BuildService)
        disc.add(new MenuView.Item("Build the library here…")
                .value(this::phoneBuildState)
                .help("Experimental. Makes this game's code pack here, from your disc, instead of on a PC: the app recompiles the "
                        + "game's executable and compiles it with a compiler it downloads once (Termux's clang, about "
                        + (PhoneBuild.downloadSize(this) >> 20) + " MB). Takes 5-30 minutes and about 1 GB of free space "
                        + "while it runs; keep the phone charging. Tap again while it runs to cancel; a build that stopped "
                        + "goes on where it was.")
                .visible(() -> !GameFiles.bundledGame(this)) // always tappable: phoneBuild() says why when it can't start
                .action(this::phoneBuild));
        disc.add(new MenuView.Item("Delete the compiler")
                .visible(() -> BuildService.building == null && PhoneBuild.toolsDir(this).exists())
                .value(() -> "Termux's clang, " + size(PhoneBuild.treeSize(PhoneBuild.toolsDir(this))))
                .help("The compiler the phone build downloaded into the app's folder. It is downloaded again for the "
                        + "next build on this phone.")
                .action(() -> {
                    PhoneBuild.deleteTree(PhoneBuild.toolsDir(this));
                    refresh();
                }));
        disc.add(new MenuView.Item("Delete the test copy")
                // [lcs] only while the chosen disc is the one used (the library's game)
                .visible(() -> GameFiles.disc(this) != null && GameFiles.discOk(this) && GameFiles.discMatchesLibrary(this)
                        && GameFiles.pushedDisc(this) != null)
                .value(() -> {
                    File f = GameFiles.pushedDisc(this);
                    return f == null ? "" : size(f.length()) + " in the app's folder, not used";
                })
                .help(() -> (GameFiles.pushedDisc(this) == null ? "" : GameFiles.pushedDisc(this).getName() + ". ")
                        + "A copy of the disc in Android/data (pushed there for testing). The game now reads the disc you "
                        + "chose, so this copy only takes space. Asks before deleting.")
                .enabled(() -> !busy && !gameRunning)
                .action(this::confirmDeleteCopy));

        // [saves] Saves: import / export (SavesMenu, docs/ANDROID.md "Saves: import and export")
        saves = new SavesMenu(this, this::refresh);
        saves.addTo(menu);
        // [mods] Mods (ModsMenu, docs/ANDROID.md "Mods")
        mods = new ModsMenu(this, this::refresh);
        mods.addTo(menu);

        // Settings: the game's vcs.ini, read fresh each time
        MenuView.Category set = menu.category("Settings", MenuView.G_SETTINGS);
        // [gpu-driver] first in Settings: the untested-phone notice's "Open settings" lands on it (GpuDriver.java)
        set.add(new MenuView.Item("Custom GPU driver")
                .value(() -> driverNote != null ? driverNote : GpuDriver.summary(this))
                .help(() -> GpuDriver.supported(this)
                        ? "Advanced. Use a Vulkan driver you downloaded yourself (e.g. Mesa Turnip, the .zip packages made "
                                + "for emulators) instead of the phone's own: can help when the game does not start or the "
                                + "graphics crash. Qualcomm Adreno GPUs only. If the game fails with it, the system driver "
                                + "comes back by itself. This app includes no drivers."
                        : "Only for phones with a Qualcomm Adreno GPU. This phone's GPU (" + DeviceNotice.chip()
                                + ") cannot load custom drivers; Mali and other GPUs use their system driver.")
                .enabled(() -> GpuDriver.supported(this) && !busy && !gameRunning)
                .action(this::gpuDriverMenu));
        String[] presets = {"Balanced", "Authentic (PS2)"};
        set.add(new MenuView.Item("Graphics preset")
                .value(() -> ini.presetName())
                .help(() -> gameRunning ? gameRunningNote() : "Balanced: the screen's shape, HUD at the edges, 2x resolution, "
                        + "60 fps, 4x texture filtering. Authentic: the PS2's picture and timing (4:3, 1x, 30 fps). Sets "
                        + "the rows below.")
                .enabled(() -> !gameRunning)
                .choice(presets, () -> {
                    String p = ini.presetName();
                    return p.equals("Balanced") ? 0 : p.startsWith("Authentic") ? 1 : -1;
                }, i -> {
                    if (!VcsIni.gameRunning(this)) {
                        Map<String, Integer> m = i == 0 ? VcsIni.balanced() : VcsIni.authentic();
                        VcsIni.write(this, m);
                    }
                    refresh();
                }));
        for (VcsIni.Setting s : new VcsIni.Setting[] {VcsIni.SCALE, VcsIni.FPS, VcsIni.TIMING, VcsIni.ASPECT, VcsIni.HUD,
                VcsIni.CUTSCENE_BARS, VcsIni.MSAA, VcsIni.ANISO, VcsIni.REDUCE_POSTFX, VcsIni.SKIP_INTRO, VcsIni.FPS_COUNTER})
            set.add(iniItem(s));
        set.add(new MenuView.Item("Compatibility mode")
                .value(() -> CompatMode.on(this) ? "On: safe graphics and threading"
                        : "Off · Try it if the game crashes")
                .help("Turn this on if the game won't start or the graphics driver crashes. It uses simpler shaders, "
                        + "stricter GPU sync and no worker threads, so it is slower but safer. It also turns on by "
                        + "itself after a driver crash.")
                .enabled(() -> !gameRunning)
                .choice(OFF_ON, () -> CompatMode.on(this) ? 1 : 0, i -> {
                    if (!VcsIni.gameRunning(this))
                        CompatMode.setOn(this, i == 1);
                    refresh();
                }));
        set.add(new MenuView.Item("Stats overlay")
                .value(() -> StatsOverlay.summary(this))
                .help("The phone's numbers over the game: frame rate and frame time, GPU time, CPU clock caps, heat, "
                        + "battery, memory.")
                .choice(OFF_ON, () -> StatsOverlay.enabled(this) ? 1 : 0, i -> {
                    StatsOverlay.setEnabled(this, i == 1);
                    refresh();
                }));
        set.add(new MenuView.Item("Stats overlay items…")
                .visible(() -> StatsOverlay.enabled(this))
                .help("Which numbers the overlay shows, and its compact one-line form.")
                .action(this::statsItems));

        // Controls: touch_controls (TouchControls, Gyro, Rumble) and the game's pad vibration
        MenuView.Category ctl = menu.category("Controls", MenuView.G_CONTROLS);
        ctl.add(controllerItem());
        ctl.add(new MenuView.Item("On-screen controls")
                .help("A PS2 pad on the touchscreen. Auto hides it while a gamepad is connected.")
                .choice(new String[] {"Auto: shown without a gamepad", "On: always shown", "Off: never shown"},
                        () -> TouchControls.mode(this), i -> {
                            TouchControls.setMode(this, i);
                            refresh();
                        }));
        ctl.add(prefPercent("Opacity", "How solid the on-screen controls are.", "opacity", 0.6f,
                new int[] {10, 20, 30, 40, 50, 60, 70, 80, 90, 100}));
        ctl.add(prefPercent("Size", "The on-screen controls' size (Edit the layout in the game's side panel moves them).",
                "scale", 1f, new int[] {60, 70, 80, 90, 100, 110, 120, 130, 140, 150}));
        ctl.add(prefToggle("Vibrate on press", "A short tick when an on-screen button is pressed.", "touch_controls", "haptics", true));
        ctl.add(prefInt("Camera", "Floating stick: a right stick starts under your thumb anywhere free on the right half. Drag: swipe there to look.", "touch_controls",
                "camera", TouchControls.CAMERA_DRAG, new int[] {0, 1}, TouchControls.CAMERA_NAMES));
        ctl.add(prefToggle("Double-tap fires", "Tap twice on the camera area and hold the second tap: Circle (fire) is held. "
                + "A drag still looks around.", "touch_controls", "gesture_dtap_fire", true)); // [android]
        ctl.add(prefToggle("Two-finger aim", "Tap the camera area with two fingers: free aim on / off, as the AIM button.",
                "touch_controls", "gesture_two_finger_aim", true));
        ctl.add(prefToggle("Weapon swipe", "On foot, swipe right on the weapon icon (top right) for the next weapon, left "
                + "for the previous.", "touch_controls", "gesture_weapon_swipe", true));
        ctl.add(prefInt("L3 / R3 buttons", "Hidden: the AIM button replaces L3 and R3 does little (docs/ANDROID.md). Their "
                + "layout positions are kept; Remap buttons can map any button to them.", "touch_controls", "extra_buttons",
                TouchControls.EXTRA_HIDDEN, new int[] {0, 1, 2}, TouchControls.EXTRA_NAMES));
        ctl.add(prefInt("Gyro aiming", "Turning the phone moves the camera as a drag does.", "touch_controls", "gyro",
                Gyro.OFF, new int[] {0, 1, 2}, Gyro.MODE_NAMES).visible(() -> Gyro.available(this)));
        int[] sens = {25, 50, 75, 100, 125, 150, 200, 300, 400};
        ctl.add(prefInt("Gyro sensitivity", null, "touch_controls", "gyro_sens", 100, sens, percents(sens))
                .visible(() -> Gyro.available(this) && TouchControls.prefs(this).getInt("gyro", Gyro.OFF) != Gyro.OFF));
        ctl.add(prefToggle("Phone rumble", "The game's rumble on the phone's vibrator while no controller is connected.",
                "touch_controls", "rumble", true).visible(() -> Rumble.available(this)));
        int[] strength = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
        ctl.add(prefInt("Phone rumble strength", null, "touch_controls", "rumble_strength", 100, strength, percents(strength))
                .visible(() -> Rumble.available(this) && TouchControls.prefs(this).getBoolean("rumble", true)));
        ctl.add(iniItem(VcsIni.PAD_VIBRATION));
        ctl.add(prefToggle("Vehicle layouts", "On foot and in a vehicle have their own layouts. In a vehicle Cross and Square become Gas and Brake pedals.",
                "touch_controls", "context_layouts", true));
        ctl.add(prefToggle("Accelerator lock", "A LOCK button in a vehicle: Gas held until Brake or a tap.",
                "touch_controls", "cruise_button", true));
        ctl.add(prefToggle("Look latch", "In a vehicle, a tap on L2 / R2 keeps looking left / right (a drive-by: drag to "
                + "aim, Circle to fire) until a tap on either.", "touch_controls", "look_latch", true)); // [android]
        ctl.add(prefToggle("Sprint toggle", "On foot, a tap on Cross sprints until the next tap.", "touch_controls",
                "sprint_toggle", false));
        ctl.add(prefToggle("Portrait mode", "Outer screen: turn the phone upright to play: the picture at the top, the "
                + "on-screen controls below it. Off: the outer screen stays landscape.", "touch_controls", "portrait_outer", false));

        // Frame generation: Lsfg (the player's own Lossless.dll)
        MenuView.Category fg = menu.category("Frame generation", MenuView.G_FRAMEGEN);
        fg.add(new MenuView.Item("Frame generation")
                .value(this::lsfgState)
                .help("Lossless Scaling's frame generation (LSFG): frames made between the game's own, x2 shows a 60 fps "
                        + "game at 120 (the display must run that fast). Applies at the next start.")
                .enabled(() -> BuildConfig.VCS_LSFG && Lsfg.installed(this) && !busy)
                .choice(OFF_ON, () -> Lsfg.on(this) ? 1 : 0, i -> {
                    Lsfg.setOn(this, i == 1);
                    refresh();
                }));
        fg.add(new MenuView.Item("Multiplier")
                .help("Frames shown for each frame the game makes.")
                .enabled(() -> BuildConfig.VCS_LSFG)
                .choice(new String[] {"2x", "3x", "4x"}, () -> indexOf(Lsfg.MULTS, Lsfg.multiplier(this)), i -> {
                    Lsfg.setMultiplier(this, Lsfg.MULTS[i]);
                    refresh();
                }));
        fg.add(new MenuView.Item("Flow scale")
                .help("The motion estimate's resolution: lower is faster.")
                .enabled(() -> BuildConfig.VCS_LSFG)
                .choice(percents(Lsfg.FLOWS), () -> indexOf(Lsfg.FLOWS, Lsfg.flow(this)), i -> {
                    Lsfg.setFlow(this, Lsfg.FLOWS[i]);
                    refresh();
                }));
        fg.add(new MenuView.Item("Find Lossless.dll…")
                .value(() -> {
                    Uri t = Lsfg.tree(this);
                    return t == null ? "Not set: pick its folder once" : "Folder: " + Lsfg.treeName(t);
                })
                .help("Pick the folder you keep Lossless.dll in, once: the app may then read that folder only, finds the "
                        + "file in it (or below) and copies it in. Choose it again to search again.")
                .enabled(() -> BuildConfig.VCS_LSFG && !busy)
                .action(this::pickLsfgFolder));
        fg.add(new MenuView.Item("Search the folder again")
                .visible(() -> Lsfg.tree(this) != null)
                .help("Copies Lossless.dll from the folder again, e.g. after Lossless Scaling updated it.")
                .enabled(() -> BuildConfig.VCS_LSFG && !busy)
                .action(() -> findLsfg(Lsfg.tree(this), true)));
        fg.add(new MenuView.Item("Choose the file…")
                .value(() -> Lsfg.installed(this) ? "Replace the copy" : "Pick Lossless.dll itself")
                .help("Your own Lossless.dll from Lossless Scaling's folder on your PC (Steam). It is copied into the app, "
                        + "never shared.")
                .enabled(() -> BuildConfig.VCS_LSFG && !busy)
                .action(() -> pick(PICK_LSFG)));

        // About
        MenuView.Category about = menu.category("About", MenuView.G_ABOUT);
        about.add(new MenuView.Item(getString(R.string.app_name))
                .value(() -> "Version " + versionName() + " · " + BuildConfig.VCS_BUILD_TAG)
                .help(() -> "The build tag (branch@commit) names the code this app was built from: give it with any report."
                        + (DeviceNotice.tested(this) ? "" : " This phone is not on the tested list yet."))); // [device]
        // [update] check / what's new / update / check at start (docs/ANDROID.md "Releases and updates")
        updates = new UpdateMenu(this, this::refresh);
        updates.addTo(about);
        about.add(new MenuView.Item("Bring your own disc")
                .value(() -> "This app contains no game code or data")
                .help("The game is recompiled from your own disc; everything else streams from the .iso at run time."));
        // [lcs] a neutral credit: both games are offered equally; the PC port of Vice City Stories is upstream's
        about.add(new MenuView.Item("Credits")
                .value(() -> "VCS PC port: gothamor/gta-vcs-recomp")
                .help("The recompilation of Vice City Stories comes from that project; Liberty City Stories and this "
                        + "Android app are this project's. Problems on the phone with either game: report them here."));
        about.add(new MenuView.Item("Licence")
                .value(() -> "GPL-3.0 (frame generation: or later)")
                .help("The frame generation port is GPL-3.0-or-later. Lossless.dll is not part of this app: you bring your own copy."));
        // [report] the game's log and a crash report a player can send without a PC (CrashReport.java)
        about.add(new MenuView.Item("Log and crash report…")
                .value(() -> reportNote != null ? reportNote : CrashReport.lastRunSummary(this))
                .help("Saves or shares a report: this phone's model and Android version, the app's version and the "
                        + "game's log, with how the last runs ended and the frame rate. Send it with any crash report. "
                        + "It holds no game data, nothing from your disc and nothing personal.")
                .action(this::reportMenu));
        about.add(new MenuView.Item("Where your data is")
                .value(() -> "Free " + StorageInfo.text(StorageInfo.free(getFilesDir())) + " · app's private folder")
                .help(() -> "Settings: files/vcs.ini. Saves: files/" + GameFiles.dataDir(this).getName() // [lcs] per game
                        + ". Inside the app's private folder. Uninstalling the app deletes them. Tap for the size of each "
                        + "part, the free space and how to move the disc image.")
                .action(this::storageMenu)); // [storage]
    }

    /** [storage] The sizes dialog (StorageInfo): folders are walked on a thread, the dialog opens when they are known */
    private void storageMenu() {
        Toast.makeText(this, "Measuring…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String text = StorageInfo.report(this);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed())
                    return;
                android.widget.TextView t = new android.widget.TextView(this);
                t.setText(text);
                t.setTypeface(android.graphics.Typeface.MONOSPACE);
                t.setTextSize(12);
                t.setPadding(48, 24, 48, 0);
                ScrollView sv = new ScrollView(this);
                sv.addView(t);
                Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle("Where your data is")
                        .setView(sv)
                        .setPositiveButton("OK", null)
                        .show());
            });
        }, "storage-sizes").start();
    }

    // ---- state ------------------------------------------------------------------------------------------------------

    private boolean haveDisc() {
        Uri disc = GameFiles.disc(this);
        // [lcs] the picked disc must be the library's game; a pushed copy of that game's disc stands in otherwise
        if (disc != null && GameFiles.discOk(this) && GameFiles.discMatchesLibrary(this))
            return true;
        return GameFiles.pushedDisc(this) != null;
    }

    private boolean haveLibrary() {
        return GameFiles.bundledGame(this) || GameFiles.gameLibrary(this).exists();
    }

    private boolean ready() {
        return haveDisc() && haveLibrary() && GameFiles.libraryProblem(this) == null;
    }

    private String readiness() {
        if (gameRunning)
            return "✓ Running: Play returns to it";
        if (busy)
            return "Busy…";
        if (!haveDisc() && GameFiles.disc(this) != null && GameFiles.discOk(this)) { // [lcs] the other game's disc
            DiscCheck.Game d = DiscCheck.game(GameFiles.discGame(this));
            return "✗ Disc and library: not the same game";
        }
        if (!haveDisc())
            return "✗ No disc yet: see Disc";
        if (!haveLibrary())
            return "✗ No game library: see Disc";
        String problem = GameFiles.libraryProblem(this);
        if (problem != null)
            return "✗ " + problem;
        String refused = GameFiles.lastLoadError(this);
        if (refused != null && !GameFiles.bundledGame(this))
            return "✗ The game refused the library";
        DiscCheck.Game g = GameFiles.libraryGameInfo(this); // [lcs] another game than VCS: named
        return "✓ Ready · " + ini.presetName() + ", " + VcsIni.SCALE.name(ini.value(VcsIni.SCALE)) + ", "
                + VcsIni.FPS.name(ini.value(VcsIni.FPS)) + " fps";
    }

    /** The long form of readiness(), for the Play row's explanation (null: nothing to add) */
    private String readinessDetail() {
        if (gameRunning || busy)
            return null;
        if (!haveDisc() && GameFiles.disc(this) != null && GameFiles.discOk(this)) {
            DiscCheck.Game d = DiscCheck.game(GameFiles.discGame(this));
            return "The disc is " + (d != null ? d.name : "another game") + ", the game library "
                    + GameFiles.libraryGameInfo(this).name + ": choose the matching one under Disc.";
        }
        if (!haveDisc())
            return "Choose your disc under Disc first.";
        if (!haveLibrary())
            return "Import the game library under Disc first.";
        String refused = GameFiles.lastLoadError(this);
        if (GameFiles.libraryProblem(this) == null && refused != null && !GameFiles.bundledGame(this))
            return "The game refused the library last time: " + refused;
        return "Starts " + GameFiles.libraryGameInfo(this).name + " with the settings shown (change them under Settings).";
    }

    private String discState() {
        if (discNote != null)
            return discNote;
        Uri disc = GameFiles.disc(this);
        boolean pushed = GameFiles.pushedDisc(this) != null;
        if (pushed && (disc == null || !GameFiles.discMatchesLibrary(this)))
            return "✓ " + GameFiles.pushedDiscName(this) + " (test copy)";
        if (disc == null)
            return "No disc chosen";
        if (GameFiles.discOk(this)) {
            DiscCheck.Game d = DiscCheck.game(GameFiles.discGame(this)); // [lcs] which game it is
            return (GameFiles.discMatchesLibrary(this) ? "✓ " : "✗ ") + GameFiles.discName(this)
                    + (d != null && !GameFiles.discMatchesLibrary(this) ? " · " + d.name : "");
        }
        return "✗ Unsupported: " + GameFiles.discName(this);
    }

    private String libraryState() {
        if (libraryNote != null)
            return libraryNote;
        if (GameFiles.bundledGame(this))
            return "✓ Built into this app";
        File lib = GameFiles.gameLibrary(this);
        String problem = GameFiles.libraryProblem(this);
        if (problem != null)
            return "✗ " + problem;
        if (lib.exists()) // [lcs] and which game it is (its stamp)
            return "✓ Imported (" + (lib.length() / (1024 * 1024)) + " MB)";
        return "Not imported yet"; // [lcs] per game
    }

    // [phonebuild] ----------------------------------------------------------------------------------------------
    private String phoneBuildState() {
        String game = GameFiles.game(this);
        if (game.equals(BuildService.building))
            return "⟳ " + BuildService.statusText;
        if (BuildService.building != null)
            return "Building " + DiscCheck.game(BuildService.building).name + "'s now";
        String err = BuildService.lastError(this, game);
        if (err != null)
            return err;
        if (game.equals(BuildService.pending(this)))
            return "Stopped before the end: tap to go on";
        if (GameFiles.disc(this) == null || !GameFiles.discOk(this) || !game.equals(GameFiles.discGame(this)))
            return "Choose " + DiscCheck.game(game).name + "'s disc first";
        return BuildService.statusText != null && game.equals(BuildService.statusGame) ? BuildService.statusText
                : "From your disc, here: no PC needed";
    }

    /** Why a phone build can't start now (shown in a dialog), or null */
    private String phoneBuildRefusal(String game) {
        String name = DiscCheck.game(game).name;
        if (gameRunning)
            return name + " is running. Close it first, then tap this row again.";
        if (busy)
            return "The app is busy with another step (importing or checking a file). Wait for it to end, then tap again.";
        if (game.equals(BuildService.pending(this)))
            return null; // a stopped build goes on without the disc check
        if (GameFiles.disc(this) == null)
            return "No disc is chosen yet. Use \"Choose disc image\" in this menu to pick " + name + "'s disc image (.iso), "
                    + "then tap this row again.";
        if (!GameFiles.discOk(this))
            return "The chosen disc image did not pass the check (see its row). Choose " + name + "'s disc again.";
        if (!game.equals(GameFiles.discGame(this)))
            return "The chosen disc is not " + name + "'s. Choose " + name + "'s disc (or switch the game at the top).";
        long free = getFilesDir().getUsableSpace();
        if (!PhoneBuild.toolsReady(this) && free < (1L << 30))
            return "Not enough free space: the build needs about 1 GB and this phone has " + size(free) + " free here.";
        return null;
    }

    private void phoneBuild() {
        if (BuildService.building != null) {
            startService(new Intent(this, BuildService.class).setAction(BuildService.ACTION_CANCEL));
            return;
        }
        String game = GameFiles.game(this);
        String why = phoneBuildRefusal(game);
        if (why != null) { // a refused start says why (a greyed row only flashed, with no word)
            Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Can't build the code pack yet")
                    .setMessage(why)
                    .setPositiveButton("OK", null)
                    .show());
            return;
        }
        if (android.os.Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 7301);
        if (PhoneBuild.toolsReady(this) || game.equals(BuildService.pending(this))) {
            BuildService.start(this, game);
            refresh();
            return;
        }
        StringBuilder lic = new StringBuilder();
        try {
            java.util.Set<String> seen = new java.util.LinkedHashSet<>();
            for (String[] p : PhoneBuild.packages(this))
                if (seen.add(p[0]))
                    lic.append("\n · ").append(p[0]).append(' ').append(p[1]).append(" - ").append(p[5].replace('_', ' '));
        } catch (java.io.IOException ignored) {
        }
        int cores = Runtime.getRuntime().availableProcessors();
        String text = "Experimental. The app builds " + DiscCheck.game(game).name + "'s code pack here from your disc, "
                + "as the PC build does: it recompiles the game's executable to C++ and compiles that.\n\n"
                + "• Time: about 5-10 minutes on a recent flagship, up to 30 on slower phones (" + cores + " cores here).\n"
                + "• Space: about 1 GB free while it runs; afterwards only the pack (~40 MB) and the compiler (~"
                + "220 MB, deletable in this menu) stay.\n"
                + "• Keep the phone charging. The screen may turn off; a notification shows the progress. If the phone "
                + "gets too hot it pauses. If it stops, tap the row again: it goes on where it was.\n\n"
                + "First it downloads the compiler once (" + (PhoneBuild.downloadSize(this) >> 20) + " MB): Termux's "
                + "official clang/LLVM packages from " + PhoneBuild.TERMUX + " (checked against SHA-256 sums in this "
                + "app). Licences:" + lic + "\nNothing from your disc leaves the phone; nothing of the game is downloaded.";
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Build the code pack on this phone")
                .setMessage(text)
                .setPositiveButton("Build", (dlg, w) -> {
                    BuildService.start(this, game);
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    private String lsfgState() {
        if (lsfgNote != null)
            return lsfgNote;
        if (!BuildConfig.VCS_LSFG)
            return "Not in this build";
        if (!Lsfg.installed(this))
            return "No Lossless.dll yet: add it below";
        return Lsfg.on(this) ? "✓ On · x" + Lsfg.multiplier(this) + " · flow " + Lsfg.flow(this) + " %"
                : "Off · Lossless.dll is in the app";
    }

    private static String size(long bytes) {
        return bytes >= (1L << 30) ? String.format(Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30))
                : (bytes >> 20) + " MB";
    }

    private void refresh() {
        ini = VcsIni.read(this); // fresh: the game may have changed it since the last look
        gameRunning = VcsIni.gameRunning(this);
        lockedIni = VcsIni.lockedForGame(this); // [lcs]
        menu.setBuildTag(versionName() + " · " + BuildConfig.VCS_BUILD_TAG + updates.cornerNote()); // [update]
        // [lcs] the header's game switch (the selector), or in a one-game personal build the game in the title
        DiscCheck.Game g = GameFiles.libraryGameInfo(this);
        if (GameFiles.canChooseGame()) {
            String[] names = new String[choiceCount()], shortNames = new String[choiceCount()];
            for (int i = 0; i < GAMES.length; i++) {
                names[i] = GAMES[i].name;
                shortNames[i] = GAMES[i].id.toUpperCase(Locale.ROOT);
            }
            if (BuildConfig.VCS_STORIES) { // [stories] the third choice
                names[GAMES.length] = BOTH_NAME;
                shortNames[GAMES.length] = BOTH_SHORT;
            }
            menu.setHeaderTitle(null);
            menu.setGameSwitch(names, shortNames, gameIndex(), gameSwitchNote() == null && !busy);
        } else {
            menu.setHeaderTitle(g == DiscCheck.VCS ? null : getString(R.string.app_name) + " · " + g.name);
            menu.setGameSwitch(null, null, 0, false);
        }
        menu.refresh();
        // the game's process ends a moment after its activity (VCSActivity.onDestroy): look again until it is gone
        ui.removeCallbacks(recheck);
        if (gameRunning && resumed)
            ui.postDelayed(recheck, 1500);
    }

    private boolean resumed;
    private final Runnable recheck = this::refresh;

    // ---- actions ----------------------------------------------------------------------------------------------------

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*"); // .iso, .so and .dll have no reliable MIME types
        startActivityForResult(i, what);
    }

    private void pickLsfgFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Lsfg.documentsUri());
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, PICK_LSFG_FOLDER);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (saves.onResult(request, result, data)) // [saves]
            return;
        if (mods.onResult(request, result, data)) // [mods]
            return;
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        if (request == SAVE_REPORT) { // [report]
            saveReport(uri);
            return;
        }
        if (request == PICK_DISC)
            checkDisc(uri);
        else if (request == PICK_LIBRARY)
            importLibrary(uri);
        else if (request == PICK_LSFG)
            importLsfg(uri);
        else if (request == PICK_LSFG_FOLDER)
            grantLsfgFolder(uri);
        else if (request == PICK_GPU_DRIVER)
            importGpuDriver(uri);
    }

    // ---- [report] the game's log and crash report (CrashReport.java) ---------------------------------------------------

    private String reportNote;

    private void reportMenu() {
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Game log and crash report")
                .setMessage(CrashReport.lastRunSummary(this) + "\n\nThe report holds: this phone's model, chip and "
                        + "Android version, the app's version, how the game's last runs ended (Android's own record), the "
                        + "phone's temperature, battery and free memory, the game's frame rate every 10 seconds, and the "
                        + "game's log from the last two runs.\n\nIt never holds: your disc or any game data, your saves, "
                        + "your files, your name, accounts or location. The only thing about you is the phone model. "
                        + "Save it to a file, or share it (long logs are cut there).")
                .setNegativeButton("Share…", (d, w) -> shareReport())
                .setPositiveButton("Save to a file…", (d, w) -> pickReportFile())
                .setNeutralButton("Close", null)
                .show());
    }

    /** The system's file picker for the report (the result arrives in onActivityResult as SAVE_REPORT) */
    private void pickReportFile() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, "gta-stories-report-" + new java.text.SimpleDateFormat(
                "yyyyMMdd-HHmm", Locale.ROOT).format(new java.util.Date()) + ".txt");
        startActivityForResult(i, SAVE_REPORT);
    }

    /** [device] The one notice dialog (compat note, untested phone, start-up error): OK, plus an optional action button */
    private void showNotice(String title, String message, String actionLabel, Runnable action) {
        AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title).setMessage(message).setNegativeButton("OK", null);
        if (actionLabel != null)
            b.setPositiveButton(actionLabel, (d, w) -> action.run());
        Dialogs.fit(b.show());
    }

    private void reportNote(String note) {
        runOnUiThread(() -> {
            reportNote = note;
            refresh();
        });
    }

    private void shareReport() {
        reportNote("Making the report…");
        new Thread(() -> {
            String report = CrashReport.shortened(CrashReport.build(this), 400 * 1024);
            runOnUiThread(() -> {
                reportNote = null;
                refresh();
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("text/plain");
                send.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.app_name) + " crash report");
                send.putExtra(Intent.EXTRA_TEXT, report);
                try {
                    startActivity(Intent.createChooser(send, "Share the report"));
                } catch (RuntimeException e) {
                    reportNote("✗ Couldn't share (" + e.getClass().getSimpleName() + "): save it to a file instead");
                }
            });
        }, "report").start();
    }

    private void saveReport(Uri uri) {
        reportNote("Saving the report…");
        new Thread(() -> {
            String report = CrashReport.build(this);
            try (java.io.OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                if (out == null)
                    throw new java.io.IOException("no stream");
                out.write(report.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                reportNote("✓ Saved " + displayName(uri) + " (" + report.length() / 1024 + " KB)");
            } catch (java.io.IOException | RuntimeException e) {
                Log.w("vcs", "[report] save: " + e);
                reportNote("✗ Couldn't save the report: " + e.getMessage());
            }
        }, "report").start();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst())
                return c.getString(0);
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment();
    }

    // The disc: read in place (a persisted read permission, so the game can open it at every start), checked off the
    // UI thread
    private void checkDisc(Uri uri) {
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        String name = displayName(uri);
        busy = true;
        discNote = "Checking " + name + "…";
        refresh();
        new Thread(() -> {
            DiscCheck.Result r;
            try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                 FileInputStream in = new FileInputStream(pfd.getFileDescriptor())) {
                r = DiscCheck.check(in.getChannel(), name);
            } catch (IOException | RuntimeException e) {
                r = new DiscCheck.Result(false, "not_found", "Can't open it (" + e.getMessage() + ").");
            }
            final DiscCheck.Result result = r;
            ui.post(() -> {
                String was = GameFiles.game(this);
                GameFiles.setDisc(this, uri, name, result.ok, result.game); // [lcs] its game's slot, then shown
                if (!was.equals(GameFiles.game(this)))
                    Palette.fadeTo(GameFiles.game(this), () -> {
                        waves.invalidate();
                        menu.invalidate();
                        root.setBackgroundColor(0xFF000000 | Palette.GLASS);
                    });
                busy = false;
                discNote = result.ok ? null : "✗ " + result.message;
                refresh();
            });
        }).start();
    }

    // The game library: copied into the app's own folder and made read-only (Android 14 refuses to load writable code)
    private void importLibrary(Uri uri) {
        busy = true;
        libraryNote = "Importing…";
        refresh();
        new Thread(() -> {
            // [lcs] copied next to the game folders first; its stamp then says which game's folder it goes to
            File games = new File(getFilesDir(), "game");
            File tmp = new File(games, GameFiles.GAME_LIBRARY + ".part");
            File dest = null;
            String error = null, game = null;
            try {
                games.mkdirs();
                tmp.delete();
                try (InputStream raw = getContentResolver().openInputStream(uri);
                     InputStream in = unpacked(raw); OutputStream out = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[1 << 20];
                    for (int n; (n = in.read(buf)) > 0; )
                        out.write(buf, 0, n);
                }
                error = GameFiles.checkLibrary(tmp);
                if (error == null)
                    error = GameFiles.stampProblem(tmp); // built for this APK's game interface (game_abi.h)
                if (error == null) {
                    game = GameFiles.libraryFileGame(tmp);
                    if (game == null || DiscCheck.game(game) == null)
                        error = "A library of a game this app doesn't know (" + game + ").";
                    else if (!GameFiles.canChooseGame() && !game.equals(GameFiles.game(this)))
                        error = "This personal build runs " + GameFiles.libraryGameInfo(this).name + " only.";
                }
                if (error == null) {
                    dest = GameFiles.gameLibrary(this, game);
                    dest.getParentFile().mkdirs();
                    GameFiles.loadError(this).delete(); // a new library: the last refusal no longer applies
                    if (dest.exists()) {
                        dest.setWritable(true);
                        dest.delete();
                    }
                    if (!tmp.renameTo(dest) || !dest.setReadOnly())
                        error = "Couldn't store it in the app's folder.";
                }
            } catch (IOException | RuntimeException e) {
                error = "Can't read it (" + e.getMessage() + ").";
            } finally {
                tmp.delete();
            }
            final String failure = error, imported = failure == null ? game : null;
            ui.post(() -> {
                busy = false;
                libraryNote = failure != null ? "✗ " + failure : null;
                if (imported != null && !imported.equals(GameFiles.game(this)) && GameFiles.canChooseGame()) {
                    pickGame(gameIndexOf(imported)); // [lcs] the library's game is the one shown now
                    return;
                }
                refresh();
            });
        }).start();
    }

    /** [lcs] A code pack as built (libvcs_game.so), gzipped (libvcs_game.so.gz, what build-game also writes: a third
     *  of the size, under chat apps' file limits) or in a .zip (its first .so): the library's bytes either way */
    private static InputStream unpacked(InputStream raw) throws IOException {
        if (raw == null)
            throw new IOException("can't open it");
        java.io.BufferedInputStream in = new java.io.BufferedInputStream(raw, 1 << 16);
        in.mark(4);
        int a = in.read(), b = in.read();
        in.reset();
        if (a == 0x1f && b == 0x8b)
            return new java.util.zip.GZIPInputStream(in, 1 << 16);
        if (a == 'P' && b == 'K') {
            java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(in);
            for (java.util.zip.ZipEntry e; (e = z.getNextEntry()) != null; )
                if (!e.isDirectory() && e.getName().endsWith(".so"))
                    return z;
            throw new IOException("no .so in the .zip");
        }
        return in;
    }

    private static int gameIndexOf(String id) {
        for (int i = 0; i < GAMES.length; i++)
            if (GAMES[i].id.equals(id))
                return i;
        return 0;
    }

    // ---- [gpu-driver] Custom GPU driver (Adreno only): GpuDriver.java ---------------------------------------------------

    /** A line for the untested-phone notice and the start-up error, on Adreno phones only */
    private String gpuDriverHint() {
        return GpuDriver.supported(this) ? "\n\nAdreno: a newer driver may help (Settings > Custom GPU driver)." : "";
    }

    /** Choose the active driver (or the system's), import, remove */
    private void gpuDriverMenu() {
        java.util.List<GpuDriver.Meta> all = GpuDriver.installed(this);
        String[] names = new String[all.size() + 1];
        names[0] = GpuDriver.SYSTEM;
        GpuDriver.Meta active = GpuDriver.active(this);
        int checked = 0;
        for (int i = 0; i < all.size(); i++) {
            names[i + 1] = all.get(i).label();
            if (active != null && active.dir.equals(all.get(i).dir))
                checked = i + 1;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Custom GPU driver (Adreno only)")
                .setSingleChoiceItems(names, checked, (d, which) -> {
                    GpuDriver.setActive(this, which == 0 ? "" : all.get(which - 1).dir);
                    driverNote = null;
                    refresh();
                    d.dismiss();
                })
                .setPositiveButton("Import driver (.zip)…", (d, w) -> importGpuDriverAsk())
                .setNeutralButton("Close", null);
        if (!all.isEmpty())
            b.setNegativeButton("Remove…", (d, w) -> removeGpuDriver(all));
        Dialogs.fit(b.show());
    }

    private void importGpuDriverAsk() {
        if (GpuDriver.warned(this)) {
            pick(PICK_GPU_DRIVER);
            return;
        }
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Before you import a driver")
                .setMessage("A custom GPU driver is native code that runs inside the game, with the app's access. Import "
                        + "only drivers from sources you trust (e.g. Mesa Turnip builds made for emulators).\n\n"
                        + "Pick the driver package itself: a .zip with meta.json and the driver's .so. This app includes "
                        + "no drivers and downloads none. If the game fails with the driver, the system driver comes "
                        + "back by itself.")
                .setPositiveButton("I understand, choose the .zip…", (d, w) -> {
                    GpuDriver.setWarned(this);
                    pick(PICK_GPU_DRIVER);
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    private void importGpuDriver(Uri uri) {
        busy = true;
        driverNote = "Importing " + displayName(uri) + "…";
        refresh();
        new Thread(() -> {
            GpuDriver.Imported r = GpuDriver.install(this, uri);
            ui.post(() -> {
                busy = false;
                if (r.error != null) {
                    driverNote = "✗ " + r.error;
                } else {
                    driverNote = null;
                    GpuDriver.setActive(this, r.meta.dir); // imported to be used; the menu switches back
                    showNotice("Custom GPU driver", "Imported and selected: " + r.meta.label() + "\n\nIt is used from "
                            + "the next Play (Play > Test graphics is a quick check). Choose \"" + GpuDriver.SYSTEM + "\" in this "
                            + "menu to go back.", null, null);
                }
                refresh();
            });
        }).start();
    }

    private void removeGpuDriver(java.util.List<GpuDriver.Meta> all) {
        String[] names = new String[all.size()];
        for (int i = 0; i < names.length; i++)
            names[i] = all.get(i).label();
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Remove which driver?")
                .setItems(names, (d, which) -> {
                    GpuDriver.Meta m = all.get(which);
                    Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Remove " + m.name + "?")
                            .setMessage("Deletes the app's copy of this driver. The .zip you imported stays where it is.")
                            .setNegativeButton("Keep it", null)
                            .setPositiveButton("Remove", (d2, w2) -> {
                                driverNote = GpuDriver.remove(this, m.dir) ? null : "✗ Couldn't remove " + m.name;
                                refresh();
                            })
                            .show());
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    // Lossless.dll: copied into files/lsfg (Lsfg.install), off the UI thread
    private void importLsfg(Uri uri) {
        busy = true;
        lsfgNote = "Copying " + displayName(uri) + "…";
        refresh();
        new Thread(() -> {
            String error = Lsfg.install(this, uri);
            ui.post(() -> {
                busy = false;
                if (error == null && !Lsfg.on(this))
                    Lsfg.setOn(this, true); // picked to be used
                lsfgNote = error != null ? "✗ " + error : null;
                refresh();
            });
        }).start();
    }

    // The folder Lossless.dll lives in: a persisted read permission on that folder only, then a search
    private void grantLsfgFolder(Uri tree) {
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            lsfgNote = "✗ The folder's permission could not be kept (" + e.getMessage() + ")";
            refresh();
            return;
        }
        Uri old = Lsfg.tree(this);
        if (old != null && !old.equals(tree)) {
            try {
                getContentResolver().releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
            }
        }
        Lsfg.setTree(this, tree);
        findLsfg(tree, true);
    }

    /** Searches the granted folder and copies Lossless.dll in; quiet = an automatic look that reports only success */
    private void findLsfg(Uri tree, boolean loud) {
        if (tree == null || busy)
            return;
        busy = true;
        lsfgNote = "Looking for Lossless.dll in " + Lsfg.treeName(tree) + "…";
        refresh();
        new Thread(() -> {
            String error;
            Uri dll = Lsfg.findInTree(this, tree);
            if (dll == null)
                error = "No Lossless.dll in " + Lsfg.treeName(tree) + " or its folders";
            else
                error = Lsfg.install(this, dll);
            Log.i("vcs", "[launcher] Lossless.dll search in " + Lsfg.treeName(tree) + ": " + (error == null ? "copied" : error));
            ui.post(() -> {
                busy = false;
                if (error == null && !Lsfg.on(this))
                    Lsfg.setOn(this, true);
                lsfgNote = error != null && loud ? "✗ " + error : null;
                refresh();
            });
        }).start();
    }

    private void confirmDeleteCopy() {
        File f = GameFiles.pushedDisc(this);
        if (f == null)
            return;
        AlertDialog d = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Delete the test copy?")
                .setMessage("Deletes " + f.getName() + " (" + size(f.length()) + ") from the app's folder in Android/data. "
                        + "The disc you chose (" + GameFiles.discName(this) + ") stays where it is. This can't be undone.")
                .setNegativeButton("Keep it", null)
                .setPositiveButton("Delete", (dlg, w) -> {
                    boolean gone = f.delete();
                    Log.i("vcs", "[launcher] test copy " + f + (gone ? " deleted" : " NOT deleted"));
                    discNote = gone ? null : "✗ Couldn't delete " + f.getName();
                    refresh();
                })
                .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus()); // a stray A press keeps it
        d.show();
        Dialogs.fit(d);
    }

    private void statsItems() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(16 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        StatsOverlay.addSettings(this, box, null);
        ScrollView s = new ScrollView(this);
        s.addView(box);
        Dialogs.fit(new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Stats overlay")
                .setView(s)
                .setPositiveButton("Close", null)
                .setOnDismissListener(d -> refresh())
                .show());
    }

    @Override
    public void play() {
        if (bothMode()) { // [stories]
            if (storiesReady() && !busy)
                playStories();
        } else if (ready() && !busy)
            play(false);
    }

    private void play(boolean smokeTest) {
        Intent i = new Intent(this, VCSActivity.class);
        i.putExtra(VCSActivity.EXTRA_SMOKE_TEST, smokeTest);
        i.putExtra(VCSActivity.EXTRA_GAME, GameFiles.game(this)); // [lcs] the selector's game
        startActivity(i);
    }

    // [stories] both discs there (picked, or the adb test copies)
    private boolean storiesReady() {
        return (GameFiles.disc(this, DiscCheck.LCS.id) != null || GameFiles.pushedDisc(this, DiscCheck.LCS.id) != null)
                && (GameFiles.disc(this, DiscCheck.VCS.id) != null || GameFiles.pushedDisc(this, DiscCheck.VCS.id) != null);
    }

    private String storiesReadiness() {
        return storiesReady() ? "✓ Ready · Liberty City + Vice City" : "✗ A disc is missing: see Disc";
    }

    private String storiesNeedNote() {
        boolean l = GameFiles.disc(this, DiscCheck.LCS.id) != null || GameFiles.pushedDisc(this, DiscCheck.LCS.id) != null;
        boolean v = GameFiles.disc(this, DiscCheck.VCS.id) != null || GameFiles.pushedDisc(this, DiscCheck.VCS.id) != null;
        String miss = !l && !v ? "both discs" : !l ? "the Liberty City Stories disc" : "the Vice City Stories disc";
        return "Choose " + miss + " first: switch to LCS or VCS at the top and pick each disc under Disc.";
    }

    private void playStories() {
        Intent i = new Intent(this, VCSActivity.class);
        i.putExtra(VCSActivity.EXTRA_GAME, DiscCheck.LCS.id); // the game in front
        i.putExtra(VCSActivity.EXTRA_STORIES, true);
        Log.i("vcs", "[launcher] both cities");
        startActivity(i);
    }

    // [saves] Saves > Slot N (SavesMenu): the game started with that slot loaded instead of the newest
    boolean canPlay() {
        return ready() && !busy;
    }

    void playSlot(int slot) {
        Intent i = new Intent(this, VCSActivity.class);
        i.putExtra(VCSActivity.EXTRA_LOAD_SLOT, slot);
        i.putExtra(VCSActivity.EXTRA_GAME, GameFiles.game(this)); // [lcs]
        Log.i("vcs", "[launcher] start from slot " + slot);
        startActivity(i);
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    // ---- input and lifecycle -----------------------------------------------------------------------------------------

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (menu.handleKey(event))
            return true;
        return super.dispatchKeyEvent(event);
    }

    @SuppressWarnings("deprecation") // enableOnBackInvokedCallback=false in the manifest: Back arrives here
    @Override
    public void onBackPressed() {
        if (!menu.back())
            super.onBackPressed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        BuildService.listener = () -> ui.post(this::refresh); // [phonebuild] the build's progress on its row
        resumed = true;
        waves.setRunning(true);
        // [gpu-driver] a custom driver's run that failed: back to the system driver (takes its device-lost marker first)
        String gpuDriverNote = VcsIni.gameRunning(this) ? null : GpuDriver.checkLastRun(this);
        String compatNote = CompatMode.consumeMarker(this); // [compat] the renderer lost the device last run
        DeviceNotice.scan(this); // [device] the GPU the last run used, from its log
        refresh();
        GpuLog.Failure failure = VcsIni.gameRunning(this) ? null : DeviceNotice.pendingFailure(this);
        ReportLog.Problem problem;
        if (gpuDriverNote != null || compatNote != null || failure != null)
            DeviceNotice.problemShown(this); // the note below already says what went wrong last run: no second dialog for it
        if (gpuDriverNote != null) {
            if (failure != null)
                DeviceNotice.failureShown(this); // the same failed start: one dialog
            showNotice("Custom GPU driver", gpuDriverNote, "Open settings", () -> menu.select(MenuView.G_SETTINGS, 0));
        } else if (compatNote != null) {
            showNotice("Compatibility mode", compatNote, null, null);
        } else if (failure != null) { // [device] the last start failed in the graphics: say why
            DeviceNotice.failureShown(this);
            showNotice("The game could not start", GpuLog.explain(failure) + gpuDriverHint(), "Save report…", this::pickReportFile);
        } else if (!VcsIni.gameRunning(this) && (problem = DeviceNotice.pendingProblem(this)) != null) { // the last run's black picture / freeze
            DeviceNotice.problemShown(this);
            showNotice(problem.title, problem.message + gpuDriverHint(), "Save report…", this::pickReportFile);
        } else if (DeviceNotice.due(this)) { // [device] a phone we have not tested: once per GPU / driver
            DeviceNotice.markShown(this);
            showNotice("Untested phone", DeviceNotice.MESSAGE + gpuDriverHint(), "Open settings", () -> menu.select(MenuView.G_SETTINGS, 0));
        } else if (!VcsIni.gameRunning(this) && backupDue()) { // [saves] saves only the app holds, never backed up
            getSharedPreferences("launcher_ui", MODE_PRIVATE).edit().putString("backup_reminded", BuildConfig.VERSION_NAME).apply();
            showNotice("Back up your saves",
                    "Your saves live only inside this app. Uninstalling it, or clearing its data, deletes them for good, and a "
                            + "failed update can too.\n\nSaves > Back up all saves writes both games to one file. Keep it "
                            + "off the phone too.", "Open Saves", () -> menu.select(MenuView.G_SAVES, 0));
        }
        // the granted folder: copy Lossless.dll in by itself the first time it is missing
        Uri tree = Lsfg.tree(this);
        if (BuildConfig.VCS_LSFG && tree != null && !Lsfg.installed(this) && !autoFindTried) {
            autoFindTried = true;
            findLsfg(tree, false);
        }
        updates.onResume(); // [update] the daily check, if turned on
    }

    /** [saves] a save exists, no backup was ever made, and this version has not asked yet */
    private boolean backupDue() {
        android.content.SharedPreferences p = getSharedPreferences("launcher_ui", MODE_PRIVATE);
        if (p.getLong("backup_at", 0) > 0 || BuildConfig.VERSION_NAME.equals(p.getString("backup_reminded", null)))
            return false;
        return SaveBackup.hasSaves(GameFiles.dataDir(this, DiscCheck.VCS.id))
                || SaveBackup.hasSaves(GameFiles.dataDir(this, DiscCheck.LCS.id));
    }

    @Override
    protected void onPause() {
        resumed = false;
        ui.removeCallbacks(recheck);
        waves.setRunning(false);
        super.onPause();
    }
}
