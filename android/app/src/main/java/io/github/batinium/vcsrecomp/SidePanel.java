// [android] The side panel (docs/ANDROID.md "Side panel"): slides in from the right over the running game, in the
// overlay's own "dusk" look, with what a phone player wants to reach without a keyboard: pause, the game's pause menu,
// a screenshot (to Pictures/ through MediaStore), the trainer, the free camera, the settings menu, quit; the frame rate,
// the stats overlay, frame generation; the picture; the on-screen controls and their extras (right stick, gyro aiming,
// rumble, layouts by context, sprint toggle, accelerator lock); the volumes.
//
// It complements the game's settings menu (the gear, F2: every setting, by page) and the controls' own dialog (hold the
// show / hide button): it links to both and shows the same values. The game's settings go through its registry
// (src/android/side_panel.cpp -> settings::set on the main thread), so vcs.ini stays the one store; "next start" marks
// those that apply after a restart. The panel's own settings are SharedPreferences ("touch_controls", "stats_overlay",
// "side_panel", "framegen").
//
// Open: Back, the fourth top button of the overlay, a swipe in from the right edge, a controller's Home or Select + Start
// held. Close: Back, a tap beside it, its X, Circle / B, Start. Focus by touch, by the overlay's pad (its buttons still
// work beside the panel: D-pad / stick move, left / right change, Cross select, Circle close, L1 / R1 tabs) and by a
// real controller (read from native code, so HIDAPI pads work too) or keyboard; the focused row has a teal frame.
// While it is open the game reads no pad (touch_controls.h setPanelOpen).
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SDLControllerManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class SidePanel extends FrameLayout {
    // ---- native side (src/android/side_panel.cpp, in the game only) ----
    private static native boolean nativeReady();
    private static native String nativeSettingInfo(String key);
    private static native void nativeSettingSet(String key, int value);
    private static native void nativeSettingPress(String key);
    private static native void nativeRefresh();
    private static native void nativeRequest(int what); // 1 free camera, 2 the game's pause menu, 3 perf CSV
    private static native void nativeScreenshot(String dir);
    // side_panel.cpp (vcs_save_anywhere.h, [lcs] lcs_save.h): 0 = the game's Save menu here, 1 = quick save to slot 8,
    // 2 = quick load from slot 8 (LCS only); the panel confirms 1 and 2 first
    static native void nativeSaveAnywhere(int what);
    private static native String nativeSaveWhyNot(); // "" when saving here is possible now, else why not
    private static native String nativeTakeScreenshot();
    private static native int nativeNavPad(float[] axes);
    private static native boolean nativeTakeOpenRequest();
    private static native boolean nativeTakeTouchToggle(); // Back + the touch shortcut (Gamepad > Shortcuts)
    private static native void nativeSetOpen(boolean open);
    private static native void nativeSetPaused(boolean paused);
    private static native void nativeFrameGen(boolean on, int multiplier, int flow); // live (platform::setFrameGen)
    private static native void nativeFrameGenDll(String path); // the launcher's copy: one added since the start (setFrameGenDll)
    private static native String nativePadBindings(); // the controller's pad.<id> keys, a line each
    private static native int nativeRealPad();        // the real controller alone: 1 << GamepadButton, 14 / 15 LT / RT

    private static final int REQ_FREE_CAMERA = 1, REQ_PAUSE_MENU = 2, REQ_PERF_CSV = 3;

    // short, so none is cut on a phone's cover screen (the card is 320 dp there); the strip shrinks its text if needed
    static final String[] TABS = {"Game", "Perf", "Screen", "Input", "Audio"};
    private static final int TAB_INPUT = 3;
    // the Input tab's sub-screens
    private static final int PAGE_MAIN = 0, PAGE_REMAP_PAD = 1, PAGE_REMAP_TOUCH = 2;
    private int page = PAGE_MAIN;

    @SuppressWarnings("deprecation")
    static SharedPreferences prefs(Context c, String name) {
        return c.getSharedPreferences(name, Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS);
    }

    // ---- the look: the overlay's dusk palette ----

    private static int col(float a, int rgb) {
        return (Math.max(0, Math.min(255, Math.round(255 * a))) << 24) | (rgb & 0xFFFFFF);
    }

    private final Activity activity;
    private final TouchControls controls;
    private final StatsOverlay stats;
    private final Gyro gyro;
    private final Rumble rumble;
    private final float dp;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Veil veil;
    private final LinearLayout card;
    private final TabStrip tabStrip;
    private final ScrollView scroll;
    private final LinearLayout list;
    private final List<Row> rows = new ArrayList<>();
    private int tab, focus = -1;
    private boolean open, paused, ready;
    private int shotsPending;

    SidePanel(Activity a, TouchControls controls, StatsOverlay stats, Gyro gyro, Rumble rumble) {
        super(a);
        activity = a;
        this.controls = controls;
        this.stats = stats;
        this.gyro = gyro;
        this.rumble = rumble;
        dp = getResources().getDisplayMetrics().density;
        tab = Math.max(0, Math.min(TABS.length - 1, prefs(a, "side_panel").getInt("tab", 0)));

        veil = new Veil(a);
        addView(veil, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(col(0.94f, Palette.GLASS));
        bg.setStroke(Math.max(1, Math.round(dp)), col(0.25f, Palette.INK));
        float rad = 14 * dp;
        bg.setCornerRadii(new float[] {rad, rad, 0, 0, 0, 0, rad, rad});
        card.setBackground(bg);
        card.setPadding(Math.round(14 * dp), Math.round(10 * dp), Math.round(10 * dp), Math.round(8 * dp));
        card.setClickable(true); // touches on it stay in it

        LinearLayout head = new LinearLayout(a);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(a);
        title.setText("Quick menu");
        title.setTextColor(col(1f, Palette.INK));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setLetterSpacing(0.04f);
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView close = new TextView(a);
        close.setText("✕");
        close.setTextColor(col(0.9f, Palette.PINK));
        close.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        close.setGravity(Gravity.CENTER);
        close.setOnClickListener(v -> close());
        head.addView(close, new LinearLayout.LayoutParams(Math.round(44 * dp), Math.round(44 * dp)));
        card.addView(head);

        tabStrip = new TabStrip(a);
        card.addView(tabStrip, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.round(40 * dp)));

        scroll = new ScrollView(a);
        scroll.setVerticalFadingEdgeEnabled(true);
        scroll.setFadingEdgeLength(Math.round(16 * dp));
        list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        card.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        addView(card, new LayoutParams(Math.round(380 * dp), ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END));
        card.setVisibility(GONE);
        setVisibility(GONE);
        handler.post(poll);
    }

    // ---- open / close / pause ----

    boolean isOpen() {
        return open;
    }

    void toggle() {
        if (open)
            close();
        else
            open();
    }

    void open() {
        if (open)
            return;
        open = true;
        page = PAGE_MAIN;
        ready = nativeReady();
        if (ready)
            nativeRefresh(); // the F3 overlay's live state and the like, before the rows read them
        controls.releaseAll();
        nativeSetOpen(true);
        gyro.setBlocked(true);
        applyPause();
        setVisibility(VISIBLE);
        card.setVisibility(VISIBLE);
        int w = cardWidth();
        card.getLayoutParams().width = w;
        card.requestLayout();
        card.setTranslationX(w);
        card.animate().translationX(0).setDuration(170).start();
        veil.invalidate();
        padPrev = nativeNavPad(axes); // what is held now (the open gesture) presses nothing
        dirHeld = padPrev & (UP | DOWN | LEFT | RIGHT);
        dirNextMs = android.os.SystemClock.uptimeMillis() + 380;
        // rebuilt once the native side has run the refresh (next host frame)
        handler.postDelayed(() -> {
            if (open)
                buildTab(true);
        }, ready ? 50 : 0);
        buildTab(true);
        Log.i("vcs", "[panel] open (" + TABS[tab] + ")");
    }

    void close() {
        if (!open)
            return;
        open = false;
        confirmQuit = confirmReset = false;
        if (confirmQuickSave && quickSaveRow != null) {
            confirmQuickSave = false;
            quickSaveRow.label = "Quick save";
        }
        if (confirmQuickLoad && quickLoadRow != null) { // [lcs]
            confirmQuickLoad = false;
            quickLoadRow.label = "Quick load";
        }
        cancelCapture();
        nativeSetOpen(false);
        gyro.setBlocked(false);
        applyPause();
        card.animate().translationX(card.getWidth()).setDuration(150).withEndAction(() -> {
            if (!open) {
                card.setVisibility(GONE);
                if (!paused)
                    setVisibility(GONE);
                veil.invalidate();
            }
        }).start();
        veil.invalidate();
    }

    private int cardWidth() {
        int w = getWidth() > 0 ? getWidth() : getResources().getDisplayMetrics().widthPixels;
        return Math.round(Math.min(w * 0.92f, Math.max(320 * dp, Math.min(440 * dp, w * 0.42f))));
    }

    private boolean pauseWhileOpen() {
        return prefs(getContext(), "side_panel").getBoolean("pause_open", false);
    }

    private void setPaused(boolean p) {
        paused = p;
        applyPause();
        if (!open)
            setVisibility(paused ? VISIBLE : GONE);
        veil.invalidate();
    }

    private void applyPause() {
        boolean p = paused || (open && pauseWhileOpen());
        nativeSetPaused(p);
        rumble.setPaused(p);
    }

    /** Called by the controls' dialog when it closes: the same settings show here */
    void prefsChanged() {
        controls.loadPrefs();
        gyro.loadPrefs();
        rumble.loadPrefs();
        if (stats != null)
            stats.refresh();
        if (open)
            buildTab(false);
    }

    // ---- touches: on the card its own; beside it a control of the overlay keeps working, anything else closes ----
    private boolean forwarding, outside;

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        int act = e.getActionMasked();
        if (!open) {
            // the paused veil: a tap resumes
            if (paused && act == MotionEvent.ACTION_UP) {
                setPaused(false);
                Log.i("vcs", "[panel] resumed by a tap");
            }
            return paused;
        }
        if (act == MotionEvent.ACTION_DOWN) {
            cancelCapture(); // a touch ends a wait for a controller button (a tap on its row starts it again)
            float x = e.getX(), y = e.getY();
            boolean onCard = card.getVisibility() == VISIBLE && x >= card.getLeft() + card.getTranslationX();
            forwarding = !onCard && controls.hitsControl(x, y);
            outside = !onCard && !forwarding;
        }
        if (forwarding) {
            controls.dispatchTouchEvent(e); // same place, both fill the layout
            if (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_CANCEL)
                forwarding = false;
            return true;
        }
        if (outside) {
            if (act == MotionEvent.ACTION_UP) {
                outside = false;
                close();
            }
            return true;
        }
        return super.dispatchTouchEvent(e);
    }

    // ---- keys: Back always; a keyboard while open (a controller's keys go to SDL: the pad is polled below) ----

    boolean handleKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_BACK) {
            if (down && e.getRepeatCount() == 0) {
                // a menu of the game's is up: Back closes it, as Escape did before the panel took Back
                int st = TouchControls.nativeGameState();
                if (!open && !paused && (st & TouchControls.STATE_SETTINGS_MENU) != 0) {
                    Log.i("vcs", "[panel] Back: the settings menu closes");
                    TouchControls.nativeToggleMenu();
                } else if (!open && !paused && (st & TouchControls.STATE_TRAINER) != 0) {
                    Log.i("vcs", "[panel] Back: the trainer closes");
                    TouchControls.nativeToggleTrainer();
                } else if (!open && !paused && (st & TouchControls.STATE_PAUSE_MENU) != 0) {
                    Log.i("vcs", "[panel] Back: Escape for the game's pause menu");
                    nativeRequest(REQ_PAUSE_MENU);
                } else if (open && (capturing != null || page != PAGE_MAIN)) {
                    back(); // a capture, or a sub-screen: one step back
                } else {
                    Log.i("vcs", "[panel] Back");
                    toggle();
                }
            }
            return true;
        }
        if (!open || SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId()))
            return false;
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_MUTE)
            return false;
        if (!down)
            return true; // nothing reaches the game while the panel is open
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_W: moveFocus(-1); break;
            case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_S: moveFocus(1); break;
            case KeyEvent.KEYCODE_DPAD_LEFT: case KeyEvent.KEYCODE_A: adjust(-1); break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: case KeyEvent.KEYCODE_D: adjust(1); break;
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: case KeyEvent.KEYCODE_SPACE:
            case KeyEvent.KEYCODE_DPAD_CENTER: activate(); break;
            case KeyEvent.KEYCODE_ESCAPE: back(); break;
            case KeyEvent.KEYCODE_TAB: switchTab(e.isShiftPressed() ? -1 : 1); break;
            case KeyEvent.KEYCODE_PAGE_UP: case KeyEvent.KEYCODE_Q: switchTab(-1); break;
            case KeyEvent.KEYCODE_PAGE_DOWN: case KeyEvent.KEYCODE_E: switchTab(1); break;
            default: break;
        }
        return true;
    }

    // ---- the pad: polled 60 times a second while open, 10 while closed (open requests, screenshots) ----
    private static final int CROSS = 1, CIRCLE = 2, START = 32, L1 = 256, R1 = 512, UP = 1 << 10, DOWN = 1 << 11,
            LEFT = 1 << 12, RIGHT = 1 << 13, L2 = 1 << 14, R2 = 1 << 15;
    private final float[] axes = new float[4];
    private int padPrev, dirHeld;
    private long dirNextMs;
    private int ticks;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!isAttachedToWindow() && ticks > 0) {
                handler.postDelayed(this, 250);
                return;
            }
            ticks++;
            if (shotsPending > 0)
                takeShots();
            if (nativeTakeTouchToggle() && controls != null)
                controls.toggle();
            if (!open) {
                if (nativeTakeOpenRequest()) {
                    Log.i("vcs", "[panel] a controller's Home / Select + Start");
                    open();
                } else if (paused) {
                    int b = nativeNavPad(axes), pressed = b & ~padPrev;
                    padPrev = b;
                    if ((pressed & (CROSS | START)) != 0)
                        setPaused(false);
                }
                handler.postDelayed(this, paused ? 33 : 100);
                return;
            }
            if (nativeTakeOpenRequest())
                close(); // Home again
            padTick();
            if (ticks % 30 == 0) {
                refreshValues(); // the values something else changed (a hotkey, a preset, the settings menu)
                tabStrip.invalidate(); // L1 / R1 shown while a controller is connected
                if (tab == TAB_INPUT && page == PAGE_MAIN && capturing == null && !padsSignature().equals(shownPads))
                    buildTab(false); // a controller came or went
            }
            if (!ready && nativeReady()) {
                ready = true;
                buildTab(false);
            }
            handler.postDelayed(this, 16);
        }
    };

    private void padTick() {
        int b = nativeNavPad(axes), pressed = b & ~padPrev;
        padPrev = b;
        if (capturing != null) { // the Remap screen waits for a button: every button binds, none navigates
            captureTick();
            return;
        }
        if ((pressed & START) != 0) {
            close();
            return;
        }
        if ((pressed & CIRCLE) != 0) {
            back();
            return;
        }
        if ((pressed & CROSS) != 0)
            activate();
        if ((pressed & (L1 | L2)) != 0)
            switchTab(-1);
        if ((pressed & (R1 | R2)) != 0)
            switchTab(1);
        // directions from the D-pad or the stick, repeated while held
        int dir = b & (UP | DOWN | LEFT | RIGHT);
        if (dir == 0) {
            float x = axes[0], y = axes[1];
            if (Math.abs(y) > 0.55f && Math.abs(y) >= Math.abs(x))
                dir = y < 0 ? UP : DOWN;
            else if (Math.abs(x) > 0.55f)
                dir = x < 0 ? LEFT : RIGHT;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (dir != dirHeld) {
            dirHeld = dir;
            dirNextMs = now + 380;
            fire(dir);
        } else if (dir != 0 && now >= dirNextMs) {
            dirNextMs = now + 110;
            fire(dir);
        }
    }

    private void fire(int dir) {
        if ((dir & UP) != 0)
            moveFocus(-1);
        else if ((dir & DOWN) != 0)
            moveFocus(1);
        else if ((dir & LEFT) != 0)
            adjust(-1);
        else if ((dir & RIGHT) != 0)
            adjust(1);
    }

    // ---- focus ----

    private void moveFocus(int d) {
        if (rows.isEmpty())
            return;
        int i = focus;
        for (int k = 0; k < rows.size(); k++) {
            i = i < 0 ? (d > 0 ? 0 : rows.size() - 1) : (i + d + rows.size()) % rows.size();
            if (rows.get(i).focusable())
                break;
        }
        setFocusRow(i, true);
    }

    private void setFocusRow(int i, boolean scrollTo) {
        int was = focus;
        focus = i >= 0 && i < rows.size() && rows.get(i).focusable() ? i : -1;
        if (was >= 0 && was < rows.size() && was != focus)
            rows.get(was).focusChanged();
        if (focus >= 0) {
            Row r = rows.get(focus);
            r.focusChanged();
            if (scrollTo)
                r.post(() -> {
                    int top = r.getTop() - Math.round(24 * dp), bottom = r.getBottom() + Math.round(24 * dp);
                    if (top < scroll.getScrollY())
                        scroll.smoothScrollTo(0, Math.max(0, top));
                    else if (bottom > scroll.getScrollY() + scroll.getHeight())
                        scroll.smoothScrollTo(0, bottom - scroll.getHeight());
                });
        }
    }

    private void activate() {
        if (focus >= 0)
            rows.get(focus).activate();
    }

    private void adjust(int d) {
        if (focus >= 0)
            rows.get(focus).step(d);
    }

    private void switchTab(int d) {
        cancelCapture();
        page = PAGE_MAIN;
        tab = (tab + d + TABS.length) % TABS.length;
        prefs(getContext(), "side_panel").edit().putInt("tab", tab).apply();
        tabStrip.invalidate();
        buildTab(true);
    }

    // ---- screenshots: the game writes a PNG into the app's cache, copied to Pictures/VCS Recomp here ----

    private void screenshot() {
        File dir = new File(getContext().getCacheDir(), "screenshots");
        nativeScreenshot(dir.getAbsolutePath());
        shotsPending++;
        Log.i("vcs", "[panel] screenshot asked");
    }

    private void takeShots() {
        for (String path; (path = nativeTakeScreenshot()) != null; ) {
            shotsPending = Math.max(0, shotsPending - 1);
            if (path.isEmpty()) {
                toast("Screenshot failed");
                continue;
            }
            File f = new File(path);
            new Thread(() -> {
                String done = export(f);
                post(() -> toast(done != null ? "Screenshot saved: Pictures/VCS Recomp/" + done : "Screenshot failed: couldn't save it"));
            }, "vcs-shot").start();
        }
    }

    /** The file into the shared Pictures/VCS Recomp (MediaStore: no storage permission on Android 10+); its name */
    private String export(File f) {
        ContentResolver cr = getContext().getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.Images.Media.DISPLAY_NAME, f.getName());
        v.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
        v.put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/VCS Recomp");
        v.put(MediaStore.Images.Media.IS_PENDING, 1);
        Uri uri = null;
        try {
            uri = cr.insert(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), v);
            if (uri == null)
                return null;
            try (InputStream in = new FileInputStream(f); OutputStream out = cr.openOutputStream(uri)) {
                if (out == null)
                    throw new java.io.IOException("no output stream");
                byte[] buf = new byte[1 << 16];
                for (int k; (k = in.read(buf)) > 0; )
                    out.write(buf, 0, k);
            }
            v.clear();
            v.put(MediaStore.Images.Media.IS_PENDING, 0);
            cr.update(uri, v, null, null);
            Log.i("vcs", "[panel] screenshot " + f.getName() + " -> " + uri);
            return f.getName();
        } catch (Exception ex) {
            Log.e("vcs", "[panel] screenshot export: " + ex);
            if (uri != null)
                cr.delete(uri, null, null);
            return null;
        } finally {
            f.delete();
        }
    }

    private void toast(String s) {
        Toast.makeText(getContext(), s, Toast.LENGTH_SHORT).show();
    }

    // ---- the tabs' rows ----
    private boolean confirmQuickSave;
    private Row quickSaveRow;
    private boolean confirmQuickLoad; // [lcs]
    private Row quickLoadRow;

    /** [lcs] the game this process runs is LCS (the library's game: GameFiles.libraryGame) */
    private boolean lcsGame() {
        return DiscCheck.LCS.id.equals(GameFiles.libraryGame(getContext()));
    }
    private boolean confirmQuit, confirmReset;

    private void buildTab(boolean resetFocus) {
        int keepFocus = resetFocus ? -1 : focus;
        int keepScroll = resetFocus ? 0 : scroll.getScrollY();
        list.removeAllViews();
        rows.clear();
        focus = -1;
        switch (tab) {
            case 0: buildGame(); break;
            case 1: buildPerformance(); break;
            case 2: buildDisplay(); break;
            case TAB_INPUT:
                if (page == PAGE_REMAP_PAD)
                    buildRemapPad();
                else if (page == PAGE_REMAP_TOUCH)
                    buildRemapTouch();
                else
                    buildControls();
                break;
            default: buildAudio(); break;
        }
        if (keepFocus >= 0 && keepFocus < rows.size())
            setFocusRow(keepFocus, false);
        else
            moveFocus(1);
        scroll.post(() -> scroll.scrollTo(0, keepScroll));
    }

    /** Back inside the panel: a capture is cancelled, a sub-screen goes back to its tab, else the panel closes */
    private void back() {
        if (capturing != null) {
            cancelCapture();
            return;
        }
        if (page != PAGE_MAIN) {
            page = PAGE_MAIN;
            buildTab(true);
            return;
        }
        close();
    }

    private void openPage(int p) {
        page = p;
        buildTab(true);
    }

    private void buildGame() {
        header("Game");
        action("Resume", null, this::close);
        toggle("Pause", "Tap, Cross or Start resumes", () -> paused ? 1 : 0, v -> {
            setPaused(v != 0);
            if (v != 0)
                close();
        });
        toggle("Pause while open", null,
                () -> pauseWhileOpen() ? 1 : 0, v -> {
                    prefs(getContext(), "side_panel").edit().putBoolean("pause_open", v != 0).apply();
                    applyPause();
                });
        action("Pause menu", "Map, stats, save", () -> {
            close();
            nativeRequest(REQ_PAUSE_MENU);
        });
        action("Screenshot", "To Pictures/VCS Recomp", this::screenshot);
        // [lcs] the save rows reach either game's own save code (side_panel.cpp); saving refused right now says why
        String why = nativeReady() ? nativeSaveWhyNot() : null;
        String notNow = why != null && !why.isEmpty() ? " · Not now: " + why : "";
        action("Save game here", "The game's save menu, wherever you are" + notNow, () -> {
            close();
            nativeSaveAnywhere(0);
        });
        Row quick = quickSaveRow = action("Quick save", "Slot 8, no menu" + notNow, null);
        quick.onActivate = () -> {
            if (!confirmQuickSave) {
                confirmQuickSave = true;
                quick.label = "Press again to save to slot 8";
                quick.invalidate();
                return;
            }
            confirmQuickSave = false;
            quick.label = "Quick save";
            quick.invalidate();
            close();
            nativeSaveAnywhere(1);
        };
        if (lcsGame()) { // [lcs] LCS loads a slot from anywhere (lcs_save.h quickLoad); VCS only from its frontend
            Row load = quickLoadRow = action("Quick load", "Slot 8: progress since that save is lost", null);
            load.onActivate = () -> {
                if (!confirmQuickLoad) {
                    confirmQuickLoad = true;
                    load.label = "Press again to load slot 8";
                    load.invalidate();
                    return;
                }
                confirmQuickLoad = false;
                load.label = "Quick load";
                load.invalidate();
                close();
                nativeSaveAnywhere(2);
            };
        }
        header("Tools");
        action("Settings menu", "Every setting, by page", () -> {
            close();
            TouchControls.nativeToggleMenu();
        });
        action("Trainer", null, () -> {
            close();
            TouchControls.nativeToggleTrainer();
        });
        toggle("Free camera", "Stick moves, drag looks, L2 / R2 down / up",
                () -> (TouchControls.nativeGameState() & TouchControls.STATE_FREE_CAMERA) != 0 ? 1 : 0, v -> {
                    nativeRequest(REQ_FREE_CAMERA);
                    handler.postDelayed(this::refreshValues, 120);
                });
        setting("display.skip_intro");
        String mods = modsLine(); // [mods] read-only: mods are applied when the game starts (ModsMenu, vcs_mods.h)
        if (mods != null)
            note(mods);
        header("Leave");
        Row quit = action("Quit to launcher", "Unsaved progress is lost", null);
        quit.onActivate = () -> {
            if (!confirmQuit) {
                confirmQuit = true;
                quit.label = "Press again to quit";
                quit.invalidate();
                return;
            }
            Log.i("vcs", "[panel] quit to the launcher");
            activity.finish();
        };
    }

    private void buildPerformance() {
        header("Fast forward");
        prefChoice(TouchControls.prefs(getContext()), "ff_speed", 2, "Speed", new int[] {2, 3, 4},
                new String[] {"2x", "3x", "4x"}, controls::loadPrefs);
        prefToggle(TouchControls.prefs(getContext()), "ff_hold", false, "Hold the button", "Off: a tap turns it on and off",
                controls::loadPrefs, false);
        header("Frame rate");
        setting("display.fps");
        setting("display.timing");
        setting("display.scale");
        setting("android.hot");  // cooling.cpp: drop to 30 fps when the phone is hot
        setting("android.adpf"); // power.cpp: performance hints
        setting("presets.balanced");
        setting("presets.authentic");
        header("Counters");
        setting("display.fps_counter");
        setting("graphics.perf_overlay");
        action("Record perf CSV", "Starts or stops", () -> nativeRequest(REQ_PERF_CSV));
        header("Stats overlay");
        SharedPreferences sp = StatsOverlay.prefs(getContext());
        Runnable statsChanged = () -> {
            if (stats != null)
                stats.refresh();
        };
        prefToggle(sp, "on", false, "Stats overlay", "FPS, GPU, CPU, heat", statsChanged, true);
        prefToggle(sp, "compact", true, "Compact", "One short line", statsChanged, false);
        for (String[] it : StatsOverlay.ITEMS)
            prefToggle(sp, "item_" + it[0], true, it[1], null, statsChanged, false);
        header("Frame generation");
        if (!Lsfg.installed(getContext()))
            note("Needs Lossless.dll: add it in the launcher.");
        SharedPreferences fg = prefs(getContext(), "lsfg"); // Lsfg.java's store: on, mult, flow
        // live, also with a DLL the launcher got after the game started: Vulkan asks for frame generation's features at
        // the start whenever the GPU has them, and the DLL named here is read when it is switched on (framegen_present.inl
        // fgCheckLive). Without a DLL the rows are kept for when there is one (the launcher's copy, Lsfg.privateCopy)
        // (checked at each change, not when the panel was built: the copy may arrive while the panel exists)
        Runnable fgLive = () -> {
            if (Lsfg.installed(getContext()))
                nativeFrameGenDll(Lsfg.privateCopy(getContext()).getAbsolutePath());
            nativeFrameGen(fg.getBoolean("on", false), fg.getInt("mult", 2), fg.getInt("flow", 50));
        };
        String tag = Lsfg.installed(getContext()) ? null : "needs Lossless.dll";
        prefToggle(fg, "on", false, "Frame generation", null, fgLive, false).tag = tag;
        prefChoice(fg, "mult", 2, "Multiplier", new int[] {2, 3, 4}, new String[] {"2x", "3x", "4x"}, fgLive).tag = tag;
        prefSlider(fg, "flow", 50, 25, 100, 25, "Flow scale", "%d %%", fgLive).tag = tag;
    }

    private void buildDisplay() {
        header("Picture");
        setting("display.aspect");
        setting("display.hud");
        setting("display.cutscene_bars");
        header("Quality");
        setting("display.msaa");
        setting("display.aniso");
        setting("display.lcs_filter_res"); // [lcs] registered by LCS only (src/game/lcs_settings.cpp): absent on VCS
        setting("enhance.reduce_postfx");
        // The world (the F2 menu's Enhancements > World): how far the city is drawn and how busy it is. A key the shown
        // game doesn't register is skipped (enhance.far_fog / stream_fast: LCS's src/game/lcs_drawdist.cpp)
        header("World");
        setting("enhance.draw_distance");
        setting("enhance.far_fog");
        setting("enhance.stream_fast");
        setting("enhance.traffic");
        setting("enhance.peds");
    }

    private void buildControls() {
        Context c = getContext();
        SharedPreferences tp = TouchControls.prefs(c);
        Runnable ctl = controls::loadPrefs;
        buildController();
        header("On-screen controls");
        prefChoice(tp, "mode", TouchControls.MODE_AUTO, "Show", new int[] {0, 1, 2}, TouchControls.MODE_NAMES,
                ctl).help = "Auto: hidden while a controller is in use";
        Row op = slider("Opacity", "%d %%", 10, 100, 5, () -> Math.round(tp.getFloat("opacity", 0.6f) * 100),
                v -> {
                    tp.edit().putFloat("opacity", v / 100f).apply();
                    controls.loadPrefs();
                });
        op.liveOnly = true;
        slider("Size", "%d %%", 60, 150, 5, () -> Math.round(tp.getFloat("scale", 1f) * 100), v -> {
            tp.edit().putFloat("scale", v / 100f).apply();
            controls.loadPrefs();
        });
        prefToggle(tp, "haptics", true, "Vibrate on press", null, ctl, false);
        action("Edit layout…", "Move or resize each button or group, per screen", () -> {
            close();
            controls.startEditLayout();
        });
        Row reset = action("Reset layout", "This screen's layout back to the default", null);
        reset.onActivate = () -> {
            if (!confirmReset) {
                confirmReset = true;
                reset.label = "Press again to reset";
                reset.invalidate();
                return;
            }
            confirmReset = false;
            controls.resetLayout();
            reset.label = "Layout reset";
            reset.invalidate();
        };
        prefToggle(tp, "portrait_outer", false, "Portrait mode", "Outer screen upright: picture on top, controls below",
                () -> {
                    if (activity instanceof VCSActivity)
                        ((VCSActivity) activity).applyOrientation();
                }, false);
        action("Remap buttons…", "What each on-screen button presses", () -> openPage(PAGE_REMAP_TOUCH));
        action("More settings…", "Also: hold the hide button", () -> {
            close();
            controls.openSettings();
        });
        header("Camera");
        prefChoice(tp, "camera", TouchControls.CAMERA_STICK, "Camera", new int[] {0, 1}, TouchControls.CAMERA_NAMES,
                ctl).help = "Right half: a stick under the thumb, or a drag";
        setting("mouse.camera");
        setting("mouse.sensitivity");
        setting("mouse.invert_y");
        setting("pad.invert_x");
        setting("pad.invert_y");
        setting("mouse.car_sensitivity");
        header("Gyro");
        if (!Gyro.available(c))
            note("No gyroscope on this phone.");
        else {
            Runnable g = gyro::loadPrefs;
            prefChoice(tp, "gyro", Gyro.OFF, "Gyro aiming", new int[] {0, 1, 2}, Gyro.MODE_NAMES, g).help =
                    "While aiming: free aim or R1 held";
            prefSlider(tp, "gyro_sens", 100, 10, 400, 10, "Sensitivity", "%d %%", g);
            prefChoice(tp, "gyro_axis", 0, "Left / right", new int[] {0, 1}, Gyro.AXIS_NAMES, g);
            prefToggle(tp, "gyro_invert_x", false, "Invert left / right", null, g, false);
            prefToggle(tp, "gyro_invert_y", false, "Invert up / down", null, g, false);
        }
        header("Rumble");
        if (Rumble.available(c)) {
            Runnable rr = rumble::loadPrefs;
            prefToggle(tp, "rumble", true, "Phone vibration", "When no controller is in use", rr, false);
            prefSlider(tp, "rumble_strength", 100, 10, 100, 10, "Strength", "%d %%", rr);
        }
        setting("pad.vibration");
        header("Driving and running");
        prefToggle(tp, "context_layouts", true, "Vehicle layout", "Gas and Brake pedals in vehicles", ctl, false);
        prefToggle(tp, "cruise_button", true, "Accelerator lock", "Holds Gas until Brake", ctl, false);
        prefToggle(tp, "look_latch", true, "Look latch", "Tap L2 / R2 to keep looking (drive-by), tap again to stop", ctl, false);
        prefToggle(tp, "sprint_toggle", false, "Sprint toggle", "Tap Cross to sprint, tap again to stop", ctl, false);
        header("Aim and prompts");
        setting("pad.free_aim");
        if (mouseConnected()) // only with a mouse: the overlay is a gamepad, its R1 is pad.free_aim
            setting("mouse.free_aim");
        setting("enhance.prompts");
    }

    /** A real mouse (not the touchscreen) is connected: then the mouse's free aim row shows */
    private static boolean mouseConnected() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && d.isExternal()
                    && (d.getSources() & android.view.InputDevice.SOURCE_MOUSE) == android.view.InputDevice.SOURCE_MOUSE)
                return true;
        }
        return false;
    }

    private void buildAudio() {
        header("Volume");
        setting("audio.volume");
        setting("audio.music_volume");
        setting("audio.sfx_volume");
        setting("audio.movie_volume");
        setting("audio.output");
        // [lcs] the player's own music as a station (lcs_usertracks.cpp; files in files/userdata-lcs/music): the row is
        // only there on LCS (Setting.read is null on VCS)
        if (Setting.read("audio.user_tracks") != null) {
            header("Your music");
            setting("audio.user_tracks");
        }
    }

    // ---- the controller: which one is read (Gamepads.java / gamepads.cpp) ----
    private String shownPads = "";

    private String padsSignature() {
        StringBuilder sb = new StringBuilder();
        for (Gamepads.Pad p : Gamepads.connected())
            sb.append(p.name).append('|').append(p.state).append('|').append(p.devices).append('\n');
        return sb.toString();
    }

    private void buildController() {
        Context c = getContext();
        List<Gamepads.Pad> all = Gamepads.connected();
        shownPads = padsSignature();
        List<Gamepads.Pad> usable = new ArrayList<>();
        for (Gamepads.Pad p : all)
            if (p.state != 2)
                usable.add(p);
        // Auto, each connected pad, the chosen one if it is away, touch only
        int chosenV = Gamepads.chosenVendor(c), chosenP = Gamepads.chosenProduct(c);
        String chosenN = Gamepads.chosenName(c);
        boolean chosenHere = false;
        for (Gamepads.Pad p : usable)
            chosenHere |= p.same(chosenV, chosenP, chosenN);
        List<String> names = new ArrayList<>();
        List<Gamepads.Pad> pads = new ArrayList<>();
        names.add("Auto");
        pads.add(null);
        for (Gamepads.Pad p : usable) {
            names.add(p.name);
            pads.add(p);
        }
        if (Gamepads.choice(c) == Gamepads.CHOSEN && !chosenHere && !chosenN.isEmpty()) {
            Gamepads.Pad away = new Gamepads.Pad();
            away.name = chosenN;
            away.vendor = chosenV;
            away.product = chosenP;
            names.add(chosenN + " (away)");
            pads.add(away);
        }
        names.add("Touch only");
        pads.add(null);
        int n = names.size(), touchIndex = n - 1;
        int[] values = new int[n];
        for (int i = 0; i < n; i++)
            values[i] = i;
        header("Controller");
        Row r = choice("Controller", values, names.toArray(new String[0]), () -> {
            int ch = Gamepads.choice(c);
            if (ch == Gamepads.TOUCH_ONLY)
                return touchIndex;
            if (ch == Gamepads.CHOSEN)
                for (int i = 1; i < touchIndex; i++)
                    if (pads.get(i).same(chosenV, chosenP, chosenN))
                        return i;
            return 0;
        }, v -> {
            if (v == touchIndex)
                Gamepads.choose(c, Gamepads.TOUCH_ONLY, null);
            else if (v <= 0)
                Gamepads.choose(c, Gamepads.AUTO, null);
            else
                Gamepads.choose(c, Gamepads.CHOSEN, pads.get(v));
            handler.postDelayed(() -> buildTab(false), 150);
        });
        r.help = "Auto: the one pressed last. Touch only ignores controllers";
        if (all.isEmpty())
            note("No controller connected.");
        for (Gamepads.Pad p : all)
            note(p.name + " · " + p.status());
        action("Remap controller…", "Press a button to bind it", () -> openPage(PAGE_REMAP_PAD));
        header("Shortcuts: hold Back +");
        setting("pad.shortcut_panel");
        setting("pad.shortcut_menu");
        setting("pad.shortcut_trainer");
        setting("pad.shortcut_touch");
        setting("pad.shortcut_freecam");
        setting("pad.shortcut_ff");
    }

    // ---- remap: the controller's bindings (the game's Gamepad page, pad.<id>), captured by a press ----
    private Row capturing;
    private long captureUntilMs;
    private boolean captureArmed;

    private List<String> padBindingKeys() {
        List<String> keys = new ArrayList<>();
        String s;
        try {
            s = nativePadBindings();
        } catch (UnsatisfiedLinkError e) {
            s = null;
        }
        if (s != null)
            for (String k : s.split("\n"))
                if (!k.isEmpty())
                    keys.add(k);
        return keys;
    }

    private void buildRemapPad() {
        action("‹ Back", null, () -> openPage(PAGE_MAIN));
        header("Controller buttons");
        if (!TouchControls.nativeRealGamepad())
            note("Connect a controller to bind by pressing; ◀ ▶ also picks.");
        List<String> keys = padBindingKeys();
        for (String k : keys) {
            Row r = setting(k);
            if (r != null)
                r.capture = true;
        }
        action("Reset to defaults", null, () -> {
            for (Row r : rows)
                if (r.capture && r.setting != null)
                    nativeSettingSet(r.key, r.setting.def);
            handler.postDelayed(this::refreshValues, 150);
            toast("Controller buttons reset");
        });
    }

    /** Waits for the controller's next button for `r` (Cross / A included: every button binds; Back or a tap cancels) */
    private void startCapture(Row r) {
        if (!TouchControls.nativeRealGamepad()) {
            r.step(1);
            return;
        }
        cancelCapture();
        capturing = r;
        captureArmed = false;
        captureUntilMs = android.os.SystemClock.uptimeMillis() + 6000;
        r.invalidate();
    }

    private void cancelCapture() {
        Row r = capturing;
        capturing = null;
        if (r != null)
            r.invalidate();
    }

    private void captureTick() {
        Row r = capturing;
        if (r == null)
            return;
        if (android.os.SystemClock.uptimeMillis() > captureUntilMs) {
            cancelCapture();
            return;
        }
        int b = nativeRealPad();
        if (!captureArmed) {
            captureArmed = b == 0; // the press that started it is let go first
            return;
        }
        if (b == 0)
            return;
        int bit = Integer.numberOfTrailingZeros(b);
        int source = bit < 14 ? bit : bit == 14 ? 20 : 21; // GamepadButton, or the trigger choices (vcs_input.cpp)
        int old = r.setting != null ? r.setting.value : source;
        // one source per PS2 button here: the one that had it gets this one's old source (a swap)
        for (Row o : rows)
            if (o != r && o.capture && o.setting != null && o.setting.value == source) {
                nativeSettingSet(o.key, old);
                o.setting.value = old;
                o.invalidate();
            }
        r.setter.set(source);
        Log.i("vcs", "[panel] " + r.key + " bound to source " + source);
        cancelCapture();
        padPrev = b; // the binding press is not a menu press
        dirHeld = b & (UP | DOWN | LEFT | RIGHT);
        dirNextMs = android.os.SystemClock.uptimeMillis() + 380;
    }

    // ---- remap: what each on-screen button presses (TouchControls, prefs "touch_controls" map_<bit>) ----
    private void buildRemapTouch() {
        action("‹ Back", null, () -> openPage(PAGE_MAIN));
        header("On-screen buttons");
        SharedPreferences tp = TouchControls.prefs(getContext());
        int[] values = new int[TouchControls.REMAP_BITS.length];
        for (int i = 0; i < values.length; i++)
            values[i] = TouchControls.REMAP_BITS[i];
        for (int i = 0; i < TouchControls.REMAP_BITS.length; i++) {
            int bit = TouchControls.REMAP_BITS[i];
            prefChoice(tp, "map_" + bit, bit, TouchControls.REMAP_NAMES[i], values, TouchControls.REMAP_NAMES,
                    controls::loadPrefs);
        }
        action("Reset to defaults", null, () -> {
            SharedPreferences.Editor e = tp.edit();
            for (int bit : TouchControls.REMAP_BITS)
                e.remove("map_" + bit);
            e.commit();
            controls.loadPrefs();
            buildTab(false);
            toast("On-screen buttons reset");
        });
    }

    // ---- row builders ----

    private interface Getter {
        int get();
    }

    private interface Setter {
        void set(int v);
    }

    /** [mods] "Mods: N on" from files/userdata/mods/active.txt (what this start applied), null without mods */
    private String modsLine() {
        java.io.File active = new java.io.File(GameFiles.dataDir(getContext()), "mods/active.txt") /* [mods] the shown game's */;
        if (!active.isFile())
            return null;
        int n = 0;
        try {
            for (String l : java.nio.file.Files.readAllLines(active.toPath(), java.nio.charset.StandardCharsets.UTF_8))
                if (l.startsWith("mod "))
                    n++;
        } catch (java.io.IOException e) {
            return null;
        }
        return n == 0 ? null : "Mods: " + n + " on, applied at start (launcher > Mods)";
    }

    private Row add(Row r) {
        rows.add(r);
        list.addView(r, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return r;
    }

    private void header(String text) {
        Row r = new Row(getContext(), Row.HEADER);
        r.label = text.toUpperCase(Locale.ROOT);
        add(r);
    }

    private void note(String text) {
        Row r = new Row(getContext(), Row.NOTE);
        r.label = text;
        add(r);
    }

    private Row action(String label, String detail, Runnable run) {
        Row r = new Row(getContext(), Row.ACTION);
        r.label = label;
        r.help = detail;
        r.onActivate = run;
        return add(r);
    }

    private Row toggle(String label, String help, Getter g, Setter s) {
        Row r = new Row(getContext(), Row.TOGGLE);
        r.label = label;
        r.help = help;
        r.getter = g;
        r.setter = s;
        return add(r);
    }

    private Row choice(String label, int[] values, String[] names, Getter g, Setter s) {
        Row r = new Row(getContext(), Row.CHOICE);
        r.label = label;
        r.values = values;
        r.names = names;
        r.getter = g;
        r.setter = s;
        return add(r);
    }

    private Row slider(String label, String format, int min, int max, int step, Getter g, Setter s) {
        Row r = new Row(getContext(), Row.SLIDER);
        r.label = label;
        r.format = format;
        r.min = min;
        r.max = max;
        r.stepSize = step;
        r.getter = g;
        r.setter = s;
        return add(r);
    }

    private Row prefToggle(SharedPreferences p, String key, boolean def, String label, String help, Runnable after, boolean rebuild) {
        return toggle(label, help, () -> p.getBoolean(key, def) ? 1 : 0, v -> {
            p.edit().putBoolean(key, v != 0).commit();
            if (after != null)
                after.run();
            if (rebuild)
                buildTab(false);
        });
    }

    private Row prefChoice(SharedPreferences p, String key, int def, String label, int[] values, String[] names, Runnable after) {
        return choice(label, values, names, () -> p.getInt(key, def), v -> {
            p.edit().putInt(key, v).commit();
            if (after != null)
                after.run();
        });
    }

    private Row prefSlider(SharedPreferences p, String key, int def, int min, int max, int step, String label, String format,
                           Runnable after) {
        return slider(label, format, min, max, step, () -> p.getInt(key, def), v -> {
            p.edit().putInt(key, v).apply();
            if (after != null)
                after.run();
        });
    }

    /** A setting of the game's registry (settings/settings.h) as a row, or null (absent here, or the game not ready) */
    private Row setting(String key) {
        Setting s = Setting.read(key);
        if (s == null)
            return null;
        Row r;
        switch (s.type) {
            case Setting.TOGGLE:
                r = new Row(getContext(), Row.TOGGLE);
                break;
            case Setting.CHOICE:
                r = new Row(getContext(), Row.CHOICE);
                r.values = s.values;
                r.names = s.names;
                break;
            case Setting.INTEGER:
                r = new Row(getContext(), Row.SLIDER);
                r.min = s.min;
                r.max = s.max;
                r.format = s.format;
                r.stepSize = Math.max(1, (s.max - s.min) / 20);
                break;
            case Setting.ACTION:
                r = new Row(getContext(), Row.ACTION);
                r.onActivate = () -> {
                    nativeSettingPress(key);
                    handler.postDelayed(this::refreshValues, 150);
                };
                break;
            default:
                return null; // key bindings: the settings menu
        }
        r.key = key;
        r.setting = s;
        r.label = LABEL.containsKey(key) ? LABEL.get(key) : s.label;
        r.help = helpFor(key, s);
        if (s.type == Setting.ACTION && !s.button.isEmpty())
            r.detailRight = s.button;
        r.getter = () -> r.setting.locked() ? r.setting.lockedValue : r.setting.value;
        r.setter = v -> {
            nativeSettingSet(key, v);
            r.setting.value = v; // shown at once; the registry's word comes with the next refresh
            handler.postDelayed(this::refreshValues, 150);
        };
        r.applyTags();
        return add(r);
    }

    /** The registry rows' values again (changed elsewhere, or refused / adjusted by the game), the rest redrawn */
    private void refreshValues() {
        if (!open)
            return;
        for (Row r : rows) {
            if (r.key != null) {
                Setting s = Setting.read(r.key);
                if (s != null) {
                    r.setting = s;
                    r.applyTags();
                }
            }
            r.invalidate();
        }
    }

    // ---- the game's settings as rows: upstream's labels and help (settings::Spec) are written for the PC menu's help
    // area; here a row is one line, and the focused one gets this short summary instead (upstream's help, cut to two
    // lines, for a key not in the map). Upstream's strings are never edited for this. ----
    private static final java.util.Map<String, String> LABEL = new java.util.HashMap<>(), SHORT = new java.util.HashMap<>();

    static {
        LABEL.put("enhance.reduce_postfx", "Lighter effects");
        LABEL.put("mouse.camera", "Drag camera");
        LABEL.put("mouse.sensitivity", "Drag sensitivity");
        LABEL.put("mouse.invert_y", "Invert drag up / down");
        LABEL.put("mouse.car_sensitivity", "Drag in vehicles");
        LABEL.put("pad.vibration", "Controller vibration");
        LABEL.put("pad.free_aim", "Free aim (controller / touch)");
        LABEL.put("mouse.free_aim", "Free aim (mouse)");

        SHORT.put("display.skip_intro", "Logos and intro at start-up");
        SHORT.put("display.fps", "30 as on the PS2; higher is smoother, hotter");
        SHORT.put("display.timing", "Independent: right speed at any frame rate");
        SHORT.put("display.scale", "Higher is sharper, costs GPU");
        SHORT.put("presets.balanced", "Screen's shape, 2x, 60 fps");
        SHORT.put("presets.authentic", "4:3, 1x, 30 fps, as on the PS2");
        SHORT.put("display.fps_counter", "The game's fps in a corner");
        SHORT.put("graphics.perf_overlay", "Frame times in a corner");
        SHORT.put("display.aspect", "Match window fills the screen");
        SHORT.put("display.hud", "Where the HUD sits in widescreen");
        SHORT.put("display.cutscene_bars", "Off: cutscenes fill the screen");
        SHORT.put("display.msaa", "Smoother edges, costs GPU");
        SHORT.put("display.aniso", "Sharper roads and walls at a slant");
        SHORT.put("display.lcs_filter_res", "Original is the PS2's softer colour filter");
        SHORT.put("enhance.reduce_postfx", "Trails, smoke, rain lighter above 30 fps");
        SHORT.put("enhance.draw_distance", "How far the city is drawn; higher costs CPU / GPU");
        SHORT.put("enhance.far_fog", "Fog follows the draw distance; + haze blends the far city");
        SHORT.put("enhance.stream_fast", "Loads models faster when moving quickly");
        SHORT.put("enhance.traffic", "Cars on the roads");
        SHORT.put("enhance.peds", "People on the streets");
        SHORT.put("mouse.camera", "A drag on the right half turns the camera");
        SHORT.put("mouse.sensitivity", "How far a drag turns");
        SHORT.put("mouse.invert_y", "Drag up looks down");
        LABEL.put("pad.shortcut_panel", "Quick menu (hold)");
        LABEL.put("pad.shortcut_touch", "Touch controls");
        LABEL.put("pad.shortcut_ff", "Fast forward");
        LABEL.put("pad.invert_x", "Invert stick left / right");
        LABEL.put("pad.invert_y", "Invert stick up / down");
        SHORT.put("pad.invert_x", "Right stick, touch or pad");
        SHORT.put("pad.invert_y", "Right stick, touch or pad");
        SHORT.put("mouse.car_sensitivity", "On top of drag sensitivity");
        SHORT.put("pad.vibration", "0 % is off");
        SHORT.put("pad.free_aim", "R1 aims with the right stick, no lock-on");
        SHORT.put("mouse.free_aim", "The right mouse button aims, no lock-on");
        SHORT.put("enhance.prompts", "Which buttons the game's text shows");
        SHORT.put("audio.volume", "Everything, movies too");
        SHORT.put("audio.output", "Mono or swapped left / right");
    }

    /** The focused row's line for a setting: the short summary, else upstream's help (two lines at most), else none */
    private static String helpFor(String key, Setting s) {
        String h = key != null ? SHORT.get(key) : null;
        if (h == null && s != null && !s.help.isEmpty())
            h = s.help;
        if (key != null && key.startsWith("pad.") && s != null && s.type == Setting.CHOICE && SHORT.get(key) == null)
            h = null; // the Remap screen's binding rows: their label says it
        return h;
    }

    // ---- a registry setting as nativeSettingInfo describes it ----
    static final class Setting {
        static final int TOGGLE = 0, CHOICE = 1, INTEGER = 2, KEY = 3, ACTION = 4;
        int type, value, def, min, max, flags, lockedValue;
        boolean restart;
        String format, label, help, button, lockedNote;
        int[] values;
        String[] names;

        boolean overridden() {
            return (flags & 1) != 0;
        }

        boolean changed() {
            return (flags & 2) != 0;
        }

        boolean locked() {
            return (flags & 4) != 0;
        }

        static Setting read(String key) {
            String info;
            try {
                info = nativeSettingInfo(key);
            } catch (UnsatisfiedLinkError e) {
                return null;
            }
            if (info == null)
                return null;
            String[] l = info.split("\n", -1);
            if (l.length < 13)
                return null;
            try {
                Setting s = new Setting();
                s.type = Integer.parseInt(l[0]);
                s.restart = l[1].equals("1");
                s.value = Integer.parseInt(l[2]);
                s.def = Integer.parseInt(l[3]);
                s.min = Integer.parseInt(l[4]);
                s.max = Integer.parseInt(l[5]);
                s.flags = Integer.parseInt(l[6]);
                s.lockedValue = Integer.parseInt(l[7]);
                s.format = l[8];
                s.label = l[9];
                s.help = l[10];
                s.button = l[11];
                s.lockedNote = l[12];
                List<Integer> vs = new ArrayList<>();
                List<String> ns = new ArrayList<>();
                for (int i = 13; i < l.length; i++) {
                    int tab = l[i].indexOf('\t');
                    if (tab <= 0)
                        continue;
                    vs.add(Integer.parseInt(l[i].substring(0, tab)));
                    ns.add(l[i].substring(tab + 1));
                }
                s.values = new int[vs.size()];
                for (int i = 0; i < vs.size(); i++)
                    s.values[i] = vs.get(i);
                s.names = ns.toArray(new String[0]);
                return s;
            } catch (RuntimeException e) {
                Log.e("vcs", "[panel] setting " + key + ": " + e);
                return null;
            }
        }
    }

    // ---- a row: drawn in the dusk look, its focus frame teal ----
    final class Row extends View {
        static final int HEADER = 0, NOTE = 1, ACTION = 2, TOGGLE = 3, CHOICE = 4, SLIDER = 5;
        final int type;
        String label, help, detail, detailRight, tag, key, format = "%d";
        boolean tagWarn, disabled, liveOnly, capture; // capture: a binding row of the Remap screen
        Setting setting;
        Getter getter;
        Setter setter;
        Runnable onActivate;
        int[] values;
        String[] names;
        int min, max = 1, stepSize = 1;

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final TextPaint labelPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG), smallPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private StaticLayout helpLayout;
        private int helpWidth = -1;
        private String helpText;
        private boolean pressedDown, dragging;
        private float downX, downY;

        Row(Context c, int type) {
            super(c);
            this.type = type;
            labelPaint.setColor(col(1f, Palette.INK));
            labelPaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, type == HEADER ? 11 : 15,
                    getResources().getDisplayMetrics()));
            if (type == HEADER) {
                labelPaint.setFakeBoldText(true);
                labelPaint.setLetterSpacing(0.14f);
            }
            smallPaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, type == NOTE ? 12 : 11.5f,
                    getResources().getDisplayMetrics()));
            paint.setStrokeCap(Paint.Cap.ROUND);
        }

        boolean focusable() {
            return type != HEADER && type != NOTE && !disabled;
        }

        boolean focused() {
            return focus >= 0 && focus < rows.size() && rows.get(focus) == this;
        }

        void applyTags() {
            if (setting == null)
                return;
            disabled = setting.overridden() || setting.locked();
            tag = setting.overridden() ? "set by its variable" : setting.changed() && setting.restart ? "restart to apply"
                    : setting.restart ? "next start" : null;
            tagWarn = setting.changed() && setting.restart;
            String h = setting.locked() && !setting.lockedNote.isEmpty() ? setting.lockedNote : helpFor(key, setting);
            if (!java.util.Objects.equals(h, help)) {
                help = h;
                requestLayout();
            }
        }

        void focusChanged() {
            requestLayout();
            invalidate();
        }

        private int value() {
            return getter != null ? getter.get() : 0;
        }

        private int index() {
            int v = value();
            for (int i = 0; values != null && i < values.length; i++)
                if (values[i] == v)
                    return i;
            return -1;
        }

        String valueText() {
            switch (type) {
                case TOGGLE: return value() != 0 ? "On" : "Off";
                case CHOICE: {
                    if (capturing == this)
                        return "Press a button…";
                    int i = index();
                    return i >= 0 && i < names.length ? names[i] : String.valueOf(value());
                }
                case SLIDER:
                    try {
                        return String.format(Locale.ROOT, format, value());
                    } catch (RuntimeException e) {
                        return String.valueOf(value());
                    }
                default: return detailRight;
            }
        }

        void activate() {
            if (!focusable())
                return;
            if (capture) {
                startCapture(this);
                return;
            }
            switch (type) {
                case ACTION:
                    if (onActivate != null)
                        onActivate.run();
                    break;
                case TOGGLE:
                    setter.set(value() != 0 ? 0 : 1);
                    break;
                case CHOICE:
                case SLIDER:
                    step(1);
                    return;
                default:
                    break;
            }
            invalidate();
        }

        void step(int d) {
            if (!focusable() || setter == null)
                return;
            if (type == TOGGLE)
                setter.set(d > 0 ? 1 : 0);
            else if (type == CHOICE && values != null && values.length > 0) {
                int i = index();
                i = i < 0 ? 0 : (i + d + values.length) % values.length;
                setter.set(values[i]);
            } else if (type == SLIDER)
                setter.set(Math.max(min, Math.min(max, value() + d * stepSize)));
            invalidate();
        }

        // ---- measuring and drawing ----
        private float pad() {
            return 10 * dp;
        }

        private boolean showsHelp() {
            return type == NOTE || (help != null && focused()); // a description under the focused row only
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec);
            float h;
            if (type == HEADER)
                h = 34 * dp;
            else if (type == NOTE)
                h = 0;
            else
                h = (type == SLIDER ? 58 : 46) * dp;
            if (showsHelp()) {
                String text = type == NOTE ? label : help;
                int lw = Math.max(1, Math.round(w - 2 * pad()));
                if (helpLayout == null || lw != helpWidth || !text.equals(helpText)) {
                    smallPaint.setColor(col(type == NOTE ? 0.6f : 0.62f, Palette.INK));
                    helpLayout = StaticLayout.Builder.obtain(text, 0, text.length(), smallPaint, lw)
                            .setAlignment(Layout.Alignment.ALIGN_NORMAL).setLineSpacing(0, 1.05f)
                            .setMaxLines(2).setEllipsize(android.text.TextUtils.TruncateAt.END).build();
                    helpWidth = lw;
                    helpText = text;
                }
                h += helpLayout.getHeight() + (type == NOTE ? 10 : 6) * dp;
            }
            setMeasuredDimension(w, Math.round(h));
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), p = pad();
            if (type == HEADER) {
                labelPaint.setColor(col(0.9f, Palette.TEAL));
                c.drawText(label, p * 0.4f, h - 9 * dp, labelPaint);
                paint.setColor(col(0.2f, Palette.INK));
                paint.setStrokeWidth(Math.max(1, dp));
                c.drawLine(p * 0.4f + labelPaint.measureText(label) + 8 * dp, h - 13 * dp, w, h - 13 * dp, paint);
                return;
            }
            if (type == NOTE) {
                c.save();
                c.translate(p, 4 * dp);
                helpLayout.draw(c);
                c.restore();
                return;
            }
            boolean f = focused();
            float alpha = disabled ? 0.45f : 1f;
            rect.set(2 * dp, 2 * dp, w - 2 * dp, h - 2 * dp);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(pressedDown ? col(0.22f, Palette.TEAL) : f ? col(0.12f, Palette.INK) : col(0.04f, Palette.INK));
            c.drawRoundRect(rect, 9 * dp, 9 * dp, paint);
            if (f) { // the focus frame
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2 * dp);
                paint.setColor(col(0.95f, Palette.TEAL));
                c.drawRoundRect(rect, 9 * dp, 9 * dp, paint);
                paint.setStyle(Paint.Style.FILL);
                c.drawRect(rect.left, rect.top + 10 * dp, rect.left + 3 * dp, rect.top + 36 * dp, paint);
            }
            float base = 29 * dp;
            labelPaint.setColor(col(alpha, Palette.INK));
            float right = w - p;
            // the value on the right
            String v = valueText();
            if (type == TOGGLE) {
                boolean on = value() != 0;
                float sw = 34 * dp, sh = 18 * dp, sx = right - sw, sy = base - 12 * dp - sh / 2 + 6 * dp;
                rect.set(sx, sy, sx + sw, sy + sh);
                paint.setColor(on ? col(0.85f * alpha, Palette.TEAL) : col(0.25f * alpha, Palette.INK));
                c.drawRoundRect(rect, sh / 2, sh / 2, paint);
                paint.setColor(on ? col(alpha, Palette.GLASS) : col(0.9f * alpha, Palette.INK));
                c.drawCircle(on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh * 0.36f, paint);
                right = sx - 10 * dp;
            } else if (v != null && type != SLIDER) {
                smallPaint.setColor(col(0.95f * alpha, type == ACTION ? Palette.PINK : Palette.TEAL));
                String shown = type == CHOICE ? "‹  " + v + "  ›" : v;
                float tw = smallPaint.measureText(shown);
                float maxW = (w - 2 * p) * 0.48f;
                if (tw > maxW) {
                    shown = TextUtilsEllipsize(shown, smallPaint, maxW);
                    tw = smallPaint.measureText(shown);
                }
                c.drawText(shown, right - tw, base, smallPaint);
                right -= tw + 10 * dp;
            } else if (type == SLIDER) {
                smallPaint.setColor(col(0.95f * alpha, Palette.TEAL));
                float tw = smallPaint.measureText(v);
                c.drawText(v, right - tw, base, smallPaint);
                right -= tw + 10 * dp;
            }
            // the label (and its tag) in what is left
            String lb = TextUtilsEllipsize(label, labelPaint, Math.max(10 * dp, right - p - (tag != null ? 60 * dp : 0)));
            c.drawText(lb, p + (f ? 4 * dp : 0), base, labelPaint);
            if (tag != null) {
                float lx = p + (f ? 4 * dp : 0) + labelPaint.measureText(lb) + 8 * dp;
                smallPaint.setColor(col(0.85f * alpha, tagWarn ? Palette.PINK : Palette.INK));
                float tw = smallPaint.measureText(tag);
                rect.set(lx - 4 * dp, base - 12 * dp, lx + tw + 4 * dp, base + 4 * dp);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(Math.max(1, dp));
                paint.setColor(col(0.5f * alpha, tagWarn ? Palette.PINK : Palette.INK));
                if (lx + tw < right) {
                    c.drawRoundRect(rect, 5 * dp, 5 * dp, paint);
                    c.drawText(tag, lx, base, smallPaint);
                }
                paint.setStyle(Paint.Style.FILL);
            }
            float y = 46 * dp;
            if (type == SLIDER) { // the bar
                float x0 = p + 4 * dp, x1 = w - p - 4 * dp, by = 46 * dp;
                float t = max > min ? (value() - min) / (float) (max - min) : 0;
                paint.setStrokeWidth(4 * dp);
                paint.setColor(col(0.2f * alpha, Palette.INK));
                c.drawLine(x0, by, x1, by, paint);
                paint.setColor(col(0.9f * alpha, Palette.TEAL));
                c.drawLine(x0, by, x0 + (x1 - x0) * t, by, paint);
                paint.setColor(col(alpha, Palette.INK));
                c.drawCircle(x0 + (x1 - x0) * t, by, 7 * dp, paint);
                y = 58 * dp;
            }
            if (showsHelp()) {
                c.save();
                c.translate(p + (f ? 4 * dp : 0), y - 4 * dp);
                helpLayout.draw(c);
                c.restore();
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (!focusable())
                return type != HEADER && type != NOTE;
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    pressedDown = true;
                    dragging = false;
                    downX = e.getX();
                    downY = e.getY();
                    getParent().requestDisallowInterceptTouchEvent(type == SLIDER && e.getY() > 36 * dp);
                    invalidate();
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (type == SLIDER && (dragging || Math.abs(e.getX() - downX) > 8 * dp && Math.abs(e.getX() - downX) > Math.abs(e.getY() - downY))) {
                        dragging = true;
                        getParent().requestDisallowInterceptTouchEvent(true);
                        float x0 = pad() + 4 * dp, x1 = getWidth() - pad() - 4 * dp;
                        float t = Math.max(0, Math.min(1, (e.getX() - x0) / (x1 - x0)));
                        int v = min + Math.round(t * (max - min) / stepSize) * stepSize;
                        v = Math.max(min, Math.min(max, v));
                        if (v != value())
                            setter.set(v);
                        invalidate();
                    } else if (Math.abs(e.getY() - downY) > 10 * dp && pressedDown) {
                        pressedDown = false; // a scroll
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP: {
                    boolean tap = pressedDown && !dragging;
                    pressedDown = false;
                    dragging = false;
                    int me = rows.indexOf(this);
                    setFocusRow(me, false);
                    if (tap) {
                        if (capture && TouchControls.nativeRealGamepad())
                            startCapture(this);
                        else if (type == CHOICE)
                            step(e.getX() < getWidth() * 0.35f ? -1 : 1);
                        else if (type == SLIDER)
                            step(e.getX() < getWidth() / 2f ? -1 : 1);
                        else
                            activate();
                    }
                    invalidate();
                    return true;
                }
                case MotionEvent.ACTION_CANCEL:
                    pressedDown = false;
                    dragging = false;
                    invalidate();
                    return true;
                default:
                    return true;
            }
        }
    }

    private static String TextUtilsEllipsize(String s, TextPaint p, float w) {
        return android.text.TextUtils.ellipsize(s, p, w, android.text.TextUtils.TruncateAt.END).toString();
    }

    // ---- the tab strip ----
    private final class TabStrip extends View {
        private final TextPaint tp = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        TabStrip(Context c) {
            super(c);
            tp.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 12.5f, getResources().getDisplayMetrics()));
            tp.setFakeBoldText(true);
            tp.setTextAlign(Paint.Align.CENTER);
        }

        private final float[] left = new float[TABS.length + 1]; // each tab's left edge, then the right end
        private float edge;

        /** The tabs' widths from their names (never cut: the text shrinks to fit, down to 9 sp); L1 / R1 at the ends
         *  only while a controller is connected */
        private void layoutTabs(float w) {
            edge = TouchControls.nativeRealGamepad() ? 22 * dp : 0;
            float base = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 12.5f, getResources().getDisplayMetrics());
            float min = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 9f, getResources().getDisplayMetrics());
            float room = w - 2 * edge, gap = 14 * dp;
            tp.setTextSize(base);
            float need = 0;
            for (String t : TABS)
                need += tp.measureText(t) + gap;
            if (need > room)
                tp.setTextSize(Math.max(min, base * room / need));
            float total = 0;
            for (String t : TABS)
                total += tp.measureText(t) + gap;
            float extra = Math.max(0, room - total) / TABS.length, x = edge;
            for (int i = 0; i < TABS.length; i++) {
                left[i] = x;
                x += tp.measureText(TABS[i]) + gap + extra;
            }
            left[TABS.length] = x;
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            layoutTabs(w);
            if (edge > 0) {
                smallHint(c, "L1", edge / 2, h);
                smallHint(c, "R1", w - edge / 2, h);
            }
            for (int i = 0; i < TABS.length; i++) {
                float x = (left[i] + left[i + 1]) / 2;
                boolean sel = i == tab;
                tp.setColor(col(sel ? 1f : 0.55f, Palette.INK));
                c.drawText(TABS[i], x, h / 2 + 4 * dp, tp);
                if (sel) {
                    paint.setColor(col(0.95f, Palette.PINK));
                    float uw = tp.measureText(TABS[i]) + 8 * dp;
                    c.drawRoundRect(x - uw / 2, h - 6 * dp, x + uw / 2, h - 3 * dp, 2 * dp, 2 * dp, paint);
                }
            }
            paint.setColor(col(0.15f, Palette.INK));
            c.drawRect(0, h - 1 * dp, w, h, paint);
        }

        private void smallHint(Canvas c, String s, float x, float h) {
            float size = tp.getTextSize();
            tp.setTextSize(size * 0.75f);
            tp.setColor(col(0.4f, Palette.TEAL));
            c.drawText(s, x, h / 2 + 3 * dp, tp);
            tp.setTextSize(size);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                float x = e.getX();
                if (edge > 0 && x < edge)
                    switchTab(-1);
                else if (edge > 0 && x > getWidth() - edge)
                    switchTab(1);
                else {
                    int i = TABS.length - 1;
                    for (int k = 0; k < TABS.length; k++)
                        if (x < left[k + 1]) {
                            i = k;
                            break;
                        }
                    if (i != tab)
                        switchTab(i - tab);
                }
            }
            return true;
        }
    }

    // ---- behind the card: the dim (open), or the paused veil (closed while paused) ----
    private final class Veil extends View {
        private final TextPaint tp = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();

        Veil(Context c) {
            super(c);
            tp.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 15, getResources().getDisplayMetrics()));
            tp.setTextAlign(Paint.Align.CENTER);
            tp.setFakeBoldText(true);
        }

        @Override
        protected void onDraw(Canvas c) {
            if (open) {
                c.drawColor(col(0.32f, 0x000000));
                return;
            }
            if (!paused)
                return;
            c.drawColor(col(0.38f, 0x000000));
            String s = "PAUSED", sub = "Tap, Cross or Start to resume · Back for the panel";
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            float w = Math.max(tp.measureText(sub) * 0.8f, 200 * dp) + 32 * dp;
            rect.set(cx - w / 2, cy - 38 * dp, cx + w / 2, cy + 30 * dp);
            paint.setColor(col(0.85f, Palette.GLASS));
            c.drawRoundRect(rect, 14 * dp, 14 * dp, paint);
            paint.setColor(col(0.9f, Palette.PINK));
            c.drawRect(rect.left + 14 * dp, rect.bottom - 3 * dp, rect.right - 14 * dp, rect.bottom, paint);
            tp.setColor(col(1f, Palette.INK));
            tp.setLetterSpacing(0.3f);
            c.drawText(s, cx, cy - 8 * dp, tp);
            tp.setLetterSpacing(0);
            float size = tp.getTextSize();
            tp.setTextSize(size * 0.72f);
            tp.setFakeBoldText(false);
            tp.setColor(col(0.7f, Palette.INK));
            c.drawText(sub, cx, cy + 16 * dp, tp);
            tp.setTextSize(size);
            tp.setFakeBoldText(true);
        }
    }
}
