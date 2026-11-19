// [android] The game's activity: SDL3's own (org.libsdl.app.SDLActivity) with the game's native library. Started by the
// launcher (LauncherActivity), never from the home screen.
package io.github.batinium.vcsrecomp;

import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

import java.io.IOException;

public class VCSActivity extends SDLActivity {
    static final String EXTRA_SMOKE_TEST = "smokeTest";
    static final String EXTRA_LOAD_SLOT = "loadSlot"; // 1..8: the boot's load takes this slot (Saves > Slot N)
    static final String EXTRA_GAME = "game"; // [lcs] "vcs" / "lcs": the launcher's selector at Play
    // [stories] both cities in one process (a --stories build): LCS in front, VCS behind (src/android/stories_host.cpp)
    static final String EXTRA_STORIES = "stories";
    private ParcelFileDescriptor disc2; // [stories] the second disc (Vice City's)
    private ParcelFileDescriptor disc; // open for as long as the game runs: its fd is the game's disc path
    private TouchControls controls;
    private SidePanel panel; // the game only (its native side is in libmain.so): Back opens it
    private Gyro gyro;
    private Rumble rumble;
    private android.hardware.display.DisplayManager.DisplayListener displayListener;

    // power.cpp (vcs_platform, in both libraries): the panel's refresh rate, which caps the game's frame rate on Android
    private static native void nativeDisplayRefresh(float hz);

    @Override
    protected void onCreate(Bundle state) {
        Log.i("vcs", "[android] VCS Recomp " + BuildConfig.VERSION_NAME + " (" + BuildConfig.VCS_BUILD_TAG + ")");
        CrashReport.installInGameProcess(this); // [report] a Java exception here lands in files/logs/game.log
        // [lcs] the game the launcher started (its selector), fixed for this process; its colours for the overlay
        String game = getIntent().getStringExtra(EXTRA_GAME);
        if (game != null && DiscCheck.game(game) != null)
            GameFiles.runningGame = game;
        GameFiles.runningGame = GameFiles.game(this);
        Palette.select(GameFiles.runningGame);
        // Where main.cpp keeps vcs.ini (VCS_APP_DIR: "next to the exe" on the desktop) and the memory card / caches
        // (VCS_DATA_DIR): the app's private folder. Set before SDL starts the native main.
        try {
            // [lcs] each game its own: files/userdata (VCS, as always), files/userdata-lcs (GameFiles.dataDir)
            java.io.File data = GameFiles.dataDir(this);
            data.mkdirs();
            Log.i("vcs", "[android] game " + GameFiles.libraryGame(this) + ", data " + data.getName());
            Os.setenv("VCS_APP_DIR", getFilesDir().getAbsolutePath(), true);
            Os.setenv("VCS_DATA_DIR", data.getAbsolutePath(), true);
            // the split build: the player's game library, loaded by libmain.so before the game starts (game_main.cpp)
            Os.setenv("VCS_GAME_LIBRARY", GameFiles.gameLibrary(this).getAbsolutePath(), true);
            // the launcher's Saves > Slot N: the boot loads that slot, not the newest (vcs_save_anywhere.cpp)
            int slot = getIntent().getIntExtra(EXTRA_LOAD_SLOT, 0);
            if (slot >= 1 && slot <= 8) {
                Os.setenv("VCS_LOAD_SLOT", Integer.toString(slot), true);
                Log.i("vcs", "[android] starting from slot " + slot);
            } else
                Os.unsetenv("VCS_LOAD_SLOT");
            // [stories] both cities: libmain.so's host loads both games' libraries (packaged in this APK) instead of the
            // code pack; Vice City behind keeps its card and settings in files/stories/, a copy of its own data's
            if (stories()) {
                Os.setenv("VCS_STORIES", "1", true);
                Os.setenv("VCS_STORIES_VCS_DATA", GameFiles.dataDir(this, DiscCheck.VCS.id).getAbsolutePath(), true);
                Log.i("vcs", "[android] both cities (prototype)");
            } else
                Os.unsetenv("VCS_STORIES");
            // [mods] the mods kept where they are: their documents opened here, like the disc, and handed over as fds
            Os.setenv("VCS_MODS_PLACES", openModPlaces(new java.io.File(data, "mods")), true);
        } catch (ErrnoException e) {
            Log.e("vcs", "setenv: " + e);
        }
        Lsfg.env(this); // frame generation: VCS_LSFG* from the launcher's card (Lsfg.java), read once by the Vulkan backend
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        preferDisplayMode();
        watchRefreshRate();
        applyOrientation();
        // the controller the game reads (Gamepads.java: Auto, a chosen pad, touch only); every pad-like device logged
        if (!mBrokenLibraries) {
            Gamepads.logDevices();
            Gamepads.apply(this);
        }
        // the on-screen controls over SDL's surface (TouchControls), once libmain.so is loaded: they call into it
        if (!mBrokenLibraries && mLayout != null) {
            // the stats overlay under the controls (it takes no touches), set in the controls' settings too
            StatsOverlay stats = new StatsOverlay(this);
            mLayout.addView(stats, new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            controls = new TouchControls(this);
            controls.setStatsOverlay(stats);
            mLayout.addView(controls, new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            // the controls' extras: gyro aiming (the camera drag's counts), the game's rumble on the phone's vibrator
            gyro = new Gyro(controls);
            rumble = new Rumble(this);
            // the side panel over everything (SidePanel; docs/ANDROID.md "Side panel"), the game only
            if (!getIntent().getBooleanExtra(EXTRA_SMOKE_TEST, false)) {
                panel = new SidePanel(this, controls, stats, gyro, rumble);
                controls.setPanel(panel);
                mLayout.addView(panel, new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            }
        }
    }

    // [mods] Each original a mod keeps where it is (ModsMenu writes mods/places.txt: "<handle> <uri>"), opened through
    // the launcher's persisted read permission and kept open while the game runs: "<handle>=<fd> ..." for the game's
    // loader, which reads /proc/self/fd/<fd> (src/game/vcs_modfiles.h). One that can't be opened is left out: the game
    // skips that mod ("original missing") and goes on.
    private final java.util.List<ParcelFileDescriptor> modPlaces = new java.util.ArrayList<>();

    private String openModPlaces(java.io.File modsDir) {
        java.io.File list = new java.io.File(modsDir, "places.txt");
        StringBuilder env = new StringBuilder();
        if (!list.isFile())
            return "";
        try {
            for (String line : java.nio.file.Files.readAllLines(list.toPath(), java.nio.charset.StandardCharsets.UTF_8)) {
                int sp = line.indexOf(' ');
                if (sp <= 0)
                    continue;
                String handle = line.substring(0, sp), uri = line.substring(sp + 1).trim();
                try {
                    ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
                    if (fd == null)
                        continue;
                    modPlaces.add(fd);
                    env.append(env.length() > 0 ? " " : "").append(handle).append('=').append(fd.getFd());
                } catch (IOException | RuntimeException e) {
                    Log.w("vcs", "[mods] " + handle + ": original missing (" + e.getMessage() + ")");
                }
            }
        } catch (IOException e) {
            Log.w("vcs", "[mods] places.txt: " + e);
        }
        return env.toString();
    }

    /** Frame generation needs display refreshes to put its frames in: with it on, ask for the panel's fastest mode at
     *  its current size (phones default to 60 Hz and ANativeWindow_setFrameRate alone did not move an Honor foldable's
     *  panel off 60). Without it (power, docs/ANDROID.md "Power and heat"): a frame rate cap of 60 or less asks for the
     *  60 Hz mode, so a panel set to "High" refresh does not run at 120 for a game that shows 60 (or 30) frames; a higher
     *  cap asks for the fastest. Again on each configuration change: a foldable's other screen has other mode ids. */
    private void preferDisplayMode() {
        boolean lsfg = Lsfg.on(this) && Lsfg.installed(this);
        int cap = VcsIni.read(this).value(VcsIni.FPS); // 30 / 60 ... 240; -1 display refresh, 0 unlimited
        boolean fastest = lsfg || cap > 60 || cap <= 0;
        android.view.Display d = getDisplay();
        if (d == null)
            return;
        android.view.Display.Mode cur = d.getMode(), best = null;
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight())
                continue;
            float r = m.getRefreshRate();
            if (fastest ? best == null || r > best.getRefreshRate()
                        : Math.abs(r - 60f) < 1f && (best == null || Math.abs(r - 60f) < Math.abs(best.getRefreshRate() - 60f)))
                best = m;
        }
        if (best == null)
            return; // no 60 Hz mode at this size: the system's choice
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (lp.preferredDisplayModeId != best.getModeId()) {
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
            Log.i("vcs", "[android] " + (lsfg ? "frame generation" : "power") + ": display mode " + best.getModeId() + " ("
                    + Math.round(best.getRefreshRate()) + " Hz) asked for (frame rate cap " + cap + ")");
        }
    }

    /** The refresh rate to native code now and at every display change (a mode switch, the other screen of a fold):
     *  the game's frame rate never goes above it (vcs_framerate.cpp, [android] power) */
    private void watchRefreshRate() {
        if (mBrokenLibraries)
            return;
        android.hardware.display.DisplayManager dm = getSystemService(android.hardware.display.DisplayManager.class);
        Runnable send = () -> {
            android.view.Display d = getDisplay();
            if (d == null)
                return;
            try {
                nativeDisplayRefresh(d.getRefreshRate());
            } catch (UnsatisfiedLinkError e) {
                Log.w("vcs", "nativeDisplayRefresh: " + e);
            }
        };
        send.run();
        if (dm == null)
            return;
        displayListener = new android.hardware.display.DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) {}
            @Override public void onDisplayRemoved(int id) {}
            @Override public void onDisplayChanged(int id) { send.run(); }
        };
        dm.registerDisplayListener(displayListener, new Handler(Looper.getMainLooper()));
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration c) {
        super.onConfigurationChanged(c);
        preferDisplayMode();
        applyOrientation(); // folded <-> unfolded: the screen in use changed
        if (controls != null)
            controls.screenChanged(); // its layout is per screen (TouchControls.profileKey)
    }

    // [android] Orientation by screen shape: a near-square screen (a foldable's inner one, a tablet) may be held either
    // way and the game fills it (Hor+ / Vert+ down to 3:4), so it follows the device; a phone-shaped screen stays
    // landscape (portrait there is a 3:4 picture at the top, presenter.cpp, with the on-screen controls below it) unless
    // "Portrait mode" (touch_controls portrait_outer) allows it. Landscape-only on the inner screen made
    // Android letterbox the game into a 2172x2006 box with the launcher showing below it (2026-11-17).
    void applyOrientation() {
        android.graphics.Rect b = getWindowManager().getMaximumWindowMetrics().getBounds();
        int w = b.width(), h = b.height();
        boolean nearSquare = Math.min(w, h) >= Math.max(w, h) * 0.8f;
        boolean portrait = !nearSquare && TouchControls.prefs(this).getBoolean("portrait_outer", false);
        int want = nearSquare || portrait ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_FULL_USER
                                          : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (getRequestedOrientation() != want) {
            Log.i("vcs", "[orientation] " + w + "x" + h + (nearSquare ? ": any way round"
                    : portrait ? ": any way round (portrait on the outer screen)" : ": landscape"));
            setRequestedOrientation(want);
        }
    }

    // SDL sets the orientation from its window and SDL_HINT_ORIENTATIONS when the window is made or resized; ours wins
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        applyOrientation();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (gyro != null)
            gyro.setResumed(true);
        if (rumble != null)
            rumble.setResumed(true);
    }

    @Override
    protected void onPause() {
        if (controls != null)
            controls.releaseAll(); // nothing stays held while the game is away
        if (gyro != null)
            gyro.setResumed(false);
        if (rumble != null)
            rumble.setResumed(false);
        super.onPause();
    }

    // Back opens / closes the side panel (the game's pause menu is its first rows and Start), instead of SDL's Escape;
    // a keyboard drives the panel while it is open. The smoke test has no panel: Back leaves it, as before.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (controls != null && controls.isEditing() && event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP)
                controls.endEditLayout(); // Back is Done in the layout editor
            return true;
        }
        if (panel != null && panel.handleKey(event))
            return true;
        return super.dispatchKeyEvent(event);
    }

    // One library per run, SDL3 linked into each (CMakeLists.txt): libmain.so the game, libsmoke.so the bring-up test
    @Override
    protected String[] getLibraries() {
        return new String[] { getIntent().getBooleanExtra(EXTRA_SMOKE_TEST, false) ? "smoke" : "main" };
    }

    // The game's main.cpp takes the disc as argv[1]. The picked document is opened here and handed over as
    // /proc/self/fd/N, which native code opens (and maps) like any file; the smoke test ignores it.
    private boolean stories() {
        return BuildConfig.VCS_STORIES && getIntent().getBooleanExtra(EXTRA_STORIES, false);
    }

    // [stories] a game's disc as a path for native code: the picked document's fd, else the adb test copy
    private String discPath(String game, boolean second) {
        Uri uri = GameFiles.disc(this, game);
        if (uri != null) {
            try {
                ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r");
                if (second)
                    disc2 = fd;
                else
                    disc = fd;
                return "/proc/self/fd/" + fd.getFd();
            } catch (IOException | SecurityException e) {
                Log.e("vcs", "can't open the " + game + " disc: " + e);
            }
        }
        java.io.File pushed = GameFiles.pushedDisc(this, game);
        return pushed != null ? pushed.getAbsolutePath() : "";
    }

    @Override
    protected String[] getArguments() {
        if (getIntent().getBooleanExtra(EXTRA_SMOKE_TEST, false))
            return new String[0];
        if (stories()) // [stories] argv[1] Liberty City's disc (in front), argv[2] Vice City's (behind)
            return new String[] { discPath(DiscCheck.LCS.id, false), discPath(DiscCheck.VCS.id, true) };
        Uri uri = GameFiles.disc(this);
        // [lcs] a picked disc of the other game (a VCS disc with LCS's library) gives way to the pushed copy of this one
        if (uri == null || (!GameFiles.discMatchesLibrary(this) && GameFiles.pushedDisc(this) != null)) {
            // No disc picked: Android/data/<app>/files/disc.iso (disc-lcs.iso for LCS), the app's own folder that adb
            // can push to (development)
            java.io.File pushed = GameFiles.pushedDisc(this);
            return pushed != null ? new String[] { pushed.getAbsolutePath() } : new String[0];
        }
        try {
            disc = getContentResolver().openFileDescriptor(uri, "r");
            return new String[] { "/proc/self/fd/" + disc.getFd() };
        } catch (IOException | SecurityException e) {
            Log.e("vcs", "can't open the disc: " + e);
            return new String[0];
        }
    }

    @Override
    protected void onDestroy() {
        if (displayListener != null) {
            android.hardware.display.DisplayManager dm = getSystemService(android.hardware.display.DisplayManager.class);
            if (dm != null)
                dm.unregisterDisplayListener(displayListener);
            displayListener = null;
        }
        super.onDestroy();
        if (disc != null) {
            try {
                disc.close();
            } catch (IOException ignored) {
            }
            disc = null;
        }
        if (disc2 != null) { // [stories]
            try {
                disc2.close();
            } catch (IOException ignored) {
            }
            disc2 = null;
        }
        for (ParcelFileDescriptor fd : modPlaces)
            try {
                fd.close();
            } catch (IOException ignored) {
            }
        modPlaces.clear();
        // The game's own process (:game in the manifest) ends with it: SDL's native side doesn't start twice in one process
        if (isFinishing())
            new Handler(Looper.getMainLooper()).postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
    }
}
