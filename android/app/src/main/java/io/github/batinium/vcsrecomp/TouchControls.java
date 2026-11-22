// [android] The on-screen controls (docs/ANDROID.md "Touch controls"): a PS2 pad drawn over SDL's surface. After the TouchControls of
// an earlier Android port, but the view decides what each touch hits itself and sends the result
// to native code as a virtual gamepad (src/android/touch_controls.cpp), which the platform layer merges with a real one:
// this game has no touchscreen of its own to share the touches with.
//
// - Left: the stick (anywhere on the free left half while playing: it starts under the thumb), L3, the D-pad.
// - Right: Cross / Circle / Square / Triangle, R3; a drag anywhere else on the right half turns the camera, as the mouse
//   does (the mouse camera, docs/CONTROLS.md), while the game wants the mouse (playing, no menu open).
// - Top: L1 / L2 and R1 / R2 in the corners, Select / Start beside them, three small buttons in the middle: one shows or
//   hides the controls (held: the settings: Auto / On / Off, opacity, vibration, the layout editor, the stats overlay),
//   the gear opens the game's settings menu (F2), the spark the trainer (F5).
// - A touch on nothing while a menu is open goes through to SDL (Dear ImGui's menus, the game's pause menu by mouse).
// - Auto (the default) hides everything while a physical gamepad is connected; On keeps it; Off never shows it.
// - Extras (docs/ANDROID.md "Controls extras"): a floating right stick or a drag (Camera), a layout for
//   vehicles (Cross and Square as Gas and Brake pedals, an accelerator lock, L2 / R2 latched by a tap) and for on foot (Cross as a sprint toggle),
//   chosen from the game's state (nativeGameState); while the trainer or the settings menu is open only the D-pad and the
//   face buttons (which drive them) are drawn. A fourth top button and a swipe in from the right edge open the side panel
//   (SidePanel.java).
package io.github.batinium.vcsrecomp;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.Arrays;
import java.util.Locale;

final class TouchControls extends View {
    // platform::GamepadButton as bits (src/platform/platform.h); L2 / R2 are the triggers, sent as axes
    static final int CROSS = 1, CIRCLE = 1 << 1, SQUARE = 1 << 2, TRIANGLE = 1 << 3, SELECT = 1 << 4, START = 1 << 5,
            L3 = 1 << 6, R3 = 1 << 7, L1 = 1 << 8, R1 = 1 << 9, UP = 1 << 10, DOWN = 1 << 11, LEFT = 1 << 12, RIGHT = 1 << 13,
            L2 = 1 << 14, R2 = 1 << 15;
    private static final int PAD_BUTTONS = (1 << 14) - 1;

    // Remap (the side panel's "Remap buttons"): which PS2 button each on-screen button presses, prefs map_<bit> = bit.
    // The labels stay; the D-pad and the sticks are not remapped. Applied last in send(), after the sprint / lock latches.
    static final int[] REMAP_BITS = {0, 1, 2, 3, 8, 9, 14, 15, 6, 7, 4, 5};
    static final String[] REMAP_NAMES = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
            "Select", "Start"};
    private final int[] remap = new int[16]; // bit -> bit; identity when not set
    private boolean remapped;

    // native side (touch_controls.cpp)
    private static native void nativeSetPad(boolean on, int buttons, float lx, float ly, float rx, float ry, float lt, float rt);
    static native void nativeLook(float dx, float dy); // also the gyro's (Gyro.java)
    static native boolean nativeLookWanted();
    static native boolean nativeRealGamepad();
    static native void nativeToggleMenu();
    static native void nativeToggleTrainer();
    static native int nativeGameState(); // touch_controls.h GameStateBits
    // the player's HUD layout (vcs::setHudPart): the radar and the top-right cluster, moved in the picture's 640x448
    // units and scaled about their corners (side_panel.cpp)
    private static native void nativeSetHudParts(float radarDx, float radarDy, float radarScale, float clusterDx,
                                                 float clusterDy, float clusterScale);

    // GameStateBits (src/android/touch_controls.h)
    static final int STATE_IN_VEHICLE = 1, STATE_AIMING = 2, STATE_SETTINGS_MENU = 4, STATE_TRAINER = 8, STATE_FREE_CAMERA = 16,
            STATE_GAME = 32, STATE_PAUSE_MENU = 64, STATE_NEAR_VEHICLE = 128;

    // ---- settings (SharedPreferences "touch_controls", shared with the launcher's process) ----
    static final int MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2;
    static final String[] MODE_NAMES = {"Auto", "On", "Off"};

    @SuppressWarnings("deprecation") // MODE_MULTI_PROCESS: the launcher and the game (:game) are two processes; it rereads
    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("touch_controls", Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS);
    }

    static int mode(Context c) {
        int m = prefs(c).getInt("mode", MODE_AUTO);
        return m >= 0 && m < MODE_NAMES.length ? m : MODE_AUTO;
    }

    static void setMode(Context c, int m) {
        prefs(c).edit().putInt("mode", m).commit();
    }

    // the camera on the right half: the drag (the mouse camera), a right stick, or both
    static final int CAMERA_DRAG = 0, CAMERA_STICK = 1, CAMERA_BOTH = 2;
    // Floating stick: the right stick starts under the thumb anywhere free on the right half, as the left one does on the
    // left. CAMERA_BOTH (a fixed stick and the drag) is gone: a stored 2 reads as the floating stick.
    static final String[] CAMERA_NAMES = {"Drag", "Floating stick"};

    // The camera drag: mouse counts per dp of finger movement. The mouse camera turns a radian per 400 counts at 100%
    // Mouse sensitivity (docs/CONTROLS.md), so a drag across a phone's right half (~400 dp) is about half a turn.
    private static final float LOOK_COUNTS_PER_DP = 3.0f;

    // ---- the controls ----
    private static final int BUTTON = 0, STICK = 1, DPAD = 2, MAX = 24;
    private static final int ROUND = 0, PILL = 1, PEDAL = 2;
    private static final int GROUPS = 7;
    private static final String[] GROUP_NAME = {"left stick", "D-pad", "face buttons", "R3", "L1 L2 Select", "R1 R2 Start",
            "right stick"};

    private int n;
    private final int[] kind = new int[MAX], bits = new int[MAX], shape = new int[MAX], group = new int[MAX];
    private final float[] cx = new float[MAX], cy = new float[MAX], r = new float[MAX];
    private final float[] op = new float[MAX]; // each control's own opacity (a factor of the overall one)
    private final String[] label = new String[MAX];
    private final String[] name = new String[MAX]; // each control's stable name: its key in the saved layout
    private final int[] stickIndex = {-1, -1}; // the left stick, the right one (-1: none)
    private int cruiseIndex = -1, crossIndex = -1; // the accelerator lock (in a vehicle), Cross
    // [android] the AIM button (on foot): a tap latches R1 + L3 held, which is LCS's / VCS's free aim (two fingers cannot do it comfortably)
    private int aimIndex = -1;
    private boolean aimOn;
    // [android] L3 / R3 on the overlay (Controls > Extra buttons): hidden by default, see docs/ANDROID.md "L3 and R3"
    static final int EXTRA_HIDDEN = 0, EXTRA_R3_VEHICLE = 1, EXTRA_BOTH = 2;
    static final String[] EXTRA_NAMES = {"Hidden", "R3 in vehicles", "L3 and R3"};
    private int extraButtons = EXTRA_HIDDEN;
    // [android] Gestures (TouchGestures.java; Controls > Gestures): double-tap = fire, two-finger tap = free aim, a swipe
    // on the weapon box = next / previous weapon. Their pad bits are added in send() after the remap: they are the game's
    // own Circle / L2 / R2, whatever the buttons are mapped to.
    private boolean gestDtapFire = true, gestTwoFingerAim = true, gestWeaponSwipe = true;
    private TouchGestures gestures;
    private boolean gestFire;                  // a double-tap's second finger is down: Circle held
    private int gestPulse;                     // a weapon swipe's L2 / R2, held for WEAPON_PULSE_MS
    private static final long WEAPON_PULSE_MS = 90;
    private long eventTime;                    // the touch event being handled (ms, SystemClock.uptimeMillis base)
    private final float[] wsX = new float[16], wsY = new float[16]; // per slot: where a weapon-box finger landed
    private final Runnable pulseOff = () -> {
        gestPulse = 0;
        send();
    };
    private float tx, ty, tr; // the show / hide button
    private float gx, gy;     // the settings button (the game's settings menu: resolution, widescreen, frame rate...)
    private int gearId = -1;
    private float kx, ky;     // the trainer button (ui/trainer_menu.cpp: F5 on a keyboard, Back + RB on a gamepad)
    private int trainerId = -1;
    private float px, py;     // the side panel's button (SidePanel: settings, actions, the controls' extras)
    private int panelId = -1;
    private float fx, fy;     // fast forward (vcs_framerate.h setFastForward): tap to toggle, or held (ff_hold)
    private int ffId = -1;
    private int ffSpeed = 2;
    private boolean ffHold;
    static native void nativeFastForward(float speed);      // 1: off
    static native float nativeFastForwardNow();             // the game's speed now (a gamepad shortcut may change it)
    static native void nativeSetFastForwardSpeed(float speed); // the gamepad shortcut's speed
    private SidePanel panel;  // null in the smoke test
    private int edgeId = -1;  // a swipe in from the right edge opens the panel
    private float edgeX0;

    // ---- touches: one slot per pointer ----
    private static final int NONE = -1, CAMERA = -2, IGNORED = -3, WEAPON = -4; // WEAPON: a finger in the weapon box (gestures)
    private static final int SLOTS = 16;
    private final int[] slotId = new int[SLOTS], slotTarget = new int[SLOTS], slotBits = new int[SLOTS];
    private final float[] slotX = new float[SLOTS], slotY = new float[SLOTS];
    // per stick (0 left, 1 right): the finger's slot, the centre while held (it follows the thumb), the tilt
    private final int[] stickSlot = {-1, -1};
    private final float[] stickCx = new float[2], stickCy = new float[2], stickX = new float[2], stickY = new float[2];
    private final int[] pressed = new int[MAX];
    private int sentButtons = -1;
    private float sentLx = 2, sentLy = 2, sentRx = 2, sentRy = 2;
    private boolean sentOn;
    private int rawPrev; // the fingers' buttons at the last send, for the latches' edges

    // ---- state ----
    private int mode;
    private StatsOverlay stats; // its switches are in this view's settings dialog

    void setStatsOverlay(StatsOverlay s) {
        stats = s;
    }

    void setPanel(SidePanel p) {
        panel = p;
        relayout();
    }

    private boolean shown, gamepad, haptics;
    private float opacity, scale;
    // the extras (the side panel's Controls tab sets them)
    private int camera;
    private boolean contextLayouts, sprintToggle, cruiseButton, lookLatch;
    // [android] the look latch (in a vehicle): L2 or R2 kept held after a tap (0: none); each one's press time; a press
    // that let go of the latch (its release does not latch again)
    private int lookLatched;
    private final long[] lookDownAt = new long[2];
    private final boolean[] lookUnlatching = new boolean[2];
    private static final long LOOK_TAP_MS = 250;
    // the game's state (polled): the layout in a vehicle, a menu open; the latches
    private int gameState;
    private boolean vehicleLayout, relayoutPending, sprintLatched, cruiseOn;
    private int toggleId = -1;
    private boolean toggleLong;
    private final Runnable longPress = () -> {
        toggleLong = true;
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        openSettings();
    };
    private final Runnable gamepadPoll = new Runnable() {
        @Override
        public void run() {
            setGamepad(nativeRealGamepad());
            if (++settingsPoll % 5 == 0 && readGameSettings())
                relayoutPending = true; // the picture's aspect or the HUD changed (the settings menu, the panel)
            setGameState(nativeGameState());
            postDelayed(this, 200);
        }
    };

    // ---- the layout, per screen (prof: the screen's size class and orientation, profileKey): each group moved by (gdx,
    // gdy) units and scaled by gsc around its centre, then each control by its own entry in btn (name -> {dx, dy,
    // scale, opacity}); saved as "layout@<prof>" (docs/ANDROID.md "Touch controls", "Layout editor") ----
    private String prof;
    private final float[] gdx = new float[GROUPS], gdy = new float[GROUPS], gsc = new float[GROUPS];
    private final java.util.HashMap<String, float[]> btn = new java.util.HashMap<>();
    private boolean legacyLayout; // taken over from a group-only layout: the defaults as they were (no HUD nudge)
    private final RectF[] gbox = new RectF[GROUPS];
    // the game's picture on the screen (presenter.cpp fitRect: at the top on an upright phone) and its HUD
    private final RectF frame = new RectF();
    private RectF hudRadar, hudMoney; // approximate: the radar, the money / health / weapon cluster (null: unknown)
    // The player's HUD layout, per screen with the rest of the layout (btn entries "HUD0" the radar, "HUD1" the cluster:
    // dx, dy in the picture's 640x448 units, scale): the editor's HUD boxes move and resize them, the game draws them so
    private static final String[] HUD_PART = {"HUD0", "HUD1"};
    private int selHud = -1;           // the HUD part selected in the editor (-1: none)
    private String hudSent;            // the last layout handed to the game

    private float[] hudPart(int p) {
        float[] b = btn.get(HUD_PART[p]);
        return b != null ? b : new float[] {0, 0, 1, 1};
    }

    /** A HUD box where the player's layout puts it: scaled about its corner (the radar's bottom-left, the cluster's
     *  top-right), then moved */
    private RectF userHud(RectF base, int p) {
        if (base == null)
            return null;
        float[] v = hudPart(p);
        float k = v[2], cxp = p == 0 ? base.left : base.right, cyp = p == 0 ? base.bottom : base.top;
        float mx = v[0] * frame.width() / 640f, my = v[1] * frame.height() / 448f;
        return new RectF(cxp + (base.left - cxp) * k + mx, cyp + (base.top - cyp) * k + my, cxp + (base.right - cxp) * k + mx,
                cyp + (base.bottom - cyp) * k + my);
    }

    /** The HUD layout to the game when it changed (also after a screen change: each screen has its own) */
    private void sendHud() {
        float[] a = hudPart(0), b = hudPart(1);
        String key = a[0] + "," + a[1] + "," + a[2] + ";" + b[0] + "," + b[1] + "," + b[2];
        if (key.equals(hudSent))
            return;
        try {
            nativeSetHudParts(a[0], a[1], a[2], b[0], b[1], b[2]);
            hudSent = key;
        } catch (UnsatisfiedLinkError ignored) { // the smoke test, or before the game's library is loaded: next layout
        }
    }
    private int aspectCode = -1, hudMode = -1; // the game's display.aspect, display.hud (-1: unknown, e.g. the smoke test)
    private boolean portraitLayout; // an upright phone: the picture on top, the controls in the space below it
    private int settingsPoll;

    // ---- the layout editor (startEditLayout): move / resize a button or a group, per screen ----
    static final int EDIT_BUTTON = 0, EDIT_GROUP = 1;
    private boolean editing, editVehicle, showHud = true;
    private int editMode;
    private String selName;  // the selected control (Button mode)
    private int selGroup = -1; // the selected group (Group mode)
    private int dragId = -1, pinchId = -1, sliderId = -1, slider = -1;
    private boolean moved, tapEmpty;
    private float downX, downY, lastX, lastY, pinchD0, pinchS0;
    private long resetAllArmed;
    private static final int CH_BUTTON = 0, CH_GROUP = 1, CH_VEHICLE = 2, CH_HUD = 3, CH_RESET = 4, CH_RESET_ALL = 5, CH_DONE = 6;
    private static final String[] CHIP = {"Button", "Group", "Vehicle", "HUD", "Reset", "Reset all", "Done"};
    private final RectF[] chip = new RectF[CHIP.length];
    private final boolean[] chipOn = new boolean[CHIP.length];
    private final RectF toolbar = new RectF(), card = new RectF();
    private final RectF[] track = {new RectF(), new RectF()};

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path(), dpadBar = new Path();
    private final RectF rect = new RectF();

    TouchControls(Context c) {
        super(c);
        gestures = new TouchGestures(c.getResources().getDisplayMetrics().density);
        loadPrefs();
        Arrays.fill(gsc, 1f);
        for (int k = 0; k < chip.length; k++)
            chip[k] = new RectF();
        editMode = prefs(c).getInt("edit_mode", EDIT_BUTTON) == EDIT_GROUP ? EDIT_GROUP : EDIT_BUTTON;
        Arrays.fill(slotTarget, NONE);
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setHapticFeedbackEnabled(true);
        Log.i("vcs", "[touch] on-screen controls: " + MODE_NAMES[mode]);
    }

    /** The settings, again after the side panel or the dialog changed them */
    void loadPrefs() {
        SharedPreferences p = prefs(getContext());
        mode = mode(getContext());
        shown = p.getBoolean("shown", true);
        haptics = p.getBoolean("haptics", true);
        opacity = p.getFloat("opacity", 0.6f);
        float sc = p.getFloat("scale", 1f);
        int cam = p.getInt("camera", CAMERA_DRAG) == CAMERA_STICK ? CAMERA_STICK : CAMERA_DRAG;
        boolean ctx = p.getBoolean("context_layouts", true), cruise = p.getBoolean("cruise_button", true);
        int extra = Math.max(EXTRA_HIDDEN, Math.min(EXTRA_BOTH, p.getInt("extra_buttons", EXTRA_HIDDEN)));
        gestDtapFire = p.getBoolean("gesture_dtap_fire", true);
        gestTwoFingerAim = p.getBoolean("gesture_two_finger_aim", true);
        gestWeaponSwipe = p.getBoolean("gesture_weapon_swipe", true);
        if (!gestDtapFire && gestFire) {
            gestFire = false;
            if (gestures != null)
                gestures.reset();
        }
        ffSpeed = Math.max(2, Math.min(4, p.getInt("ff_speed", 2)));
        ffHold = p.getBoolean("ff_hold", false);
        try {
            nativeSetFastForwardSpeed(ffSpeed);
        } catch (UnsatisfiedLinkError ignored) { // the smoke test
        }
        sprintToggle = p.getBoolean("sprint_toggle", false);
        lookLatch = p.getBoolean("look_latch", true);
        if (!lookLatch)
            lookLatched = 0;
        remapped = false;
        for (int i = 0; i < 16; i++) {
            int to = p.getInt("map_" + i, i);
            remap[i] = to >= 0 && to < 16 ? to : i;
            remapped |= remap[i] != i;
        }
        if (!sprintToggle)
            sprintLatched = false;
        boolean gs = readGameSettings();
        boolean re = gs || sc != scale || cam != camera || ctx != contextLayouts || cruise != cruiseButton || extra != extraButtons;
        scale = sc;
        camera = cam;
        extraButtons = extra;
        contextLayouts = ctx;
        cruiseButton = cruise;
        if (!cruise)
            cruiseOn = false;
        if (re && getWidth() > 0) {
            releaseAll();
            relayout();
        }
        invalidate();
    }

    int mode() {
        return mode;
    }

    /** A menu of the game's (the trainer, the settings menu) is open: only what drives it is drawn */
    private boolean menuOpen() {
        return (gameState & (STATE_SETTINGS_MENU | STATE_TRAINER)) != 0;
    }

    /** Hidden while a menu is open: everything but the D-pad and the face buttons (the trainer's keys) */
    private boolean menuHides(int i) {
        return !editing && menuOpen() && kind[i] != DPAD && (bits[i] & (TRIANGLE | CIRCLE | CROSS | SQUARE)) == 0;
    }

    private void setGameState(int st) {
        int was = gameState;
        gameState = st;
        if (((was ^ st) & (STATE_SETTINGS_MENU | STATE_TRAINER)) != 0) {
            if (stats != null)
                stats.setSuppressed(menuOpen()); // the chips sit where the menus are
            releaseAll();
        }
        if (((was ^ st) & (STATE_NEAR_VEHICLE | STATE_IN_VEHICLE | STATE_PAUSE_MENU)) != 0)
            invalidate(); // Triangle's glyph: a door while an enterable vehicle is in reach
        boolean car = (st & STATE_IN_VEHICLE) != 0;
        if (((was ^ st) & STATE_IN_VEHICLE) != 0) {
            sprintLatched = false;
            cruiseOn = false;
            aimOn = false;
            lookLatched = 0;
            Log.i("vcs", "[touch] " + (car ? "in a vehicle" : "on foot") + (contextLayouts ? ": its layout" : ""));
        }
        if (!editing && (contextLayouts ? car != vehicleLayout : vehicleLayout))
            relayoutPending = true;
        if (relayoutPending && fingersDown() == 0) {
            relayoutPending = false;
            relayout();
        }
        if ((sprintLatched || cruiseOn || aimOn || lookLatched != 0) && !nativeLookWanted()) { // a menu or a cutscene: nothing stays latched
            sprintLatched = cruiseOn = aimOn = false;
            lookLatched = 0;
            gestFire = false;
            gestPulse = 0;
            if (gestures != null)
                gestures.reset();
            send();
        }
    }

    private int fingersDown() {
        int k = 0;
        for (int s = 0; s < SLOTS; s++)
            if (slotTarget[s] != NONE)
                k++;
        return k;
    }

    /** Anything drawn (the show / hide button at least): Off never, Auto not while a gamepad is connected */
    private boolean visible() {
        return mode == MODE_ON || (mode == MODE_AUTO && !gamepad);
    }

    // The top buttons (show / hide, settings, trainer, side panel) with a gamepad connected: hidden (the controller's
    // shortcuts reach the same, Gamepad > Shortcuts) until a touch shows them for a few seconds
    private static final long TOP_SHOWN_MS = 5000;
    private long topUntil;
    private boolean topArmed = true; // this touch may press a top button (false: it only brought them back)

    private boolean topShown() {
        return !editing && (!gamepad || android.os.SystemClock.uptimeMillis() < topUntil);
    }

    private void revealTop() {
        if (!gamepad)
            return;
        boolean was = topShown();
        topUntil = android.os.SystemClock.uptimeMillis() + TOP_SHOWN_MS;
        removeCallbacks(hideTop);
        postDelayed(hideTop, TOP_SHOWN_MS + 50);
        if (!was)
            invalidate();
    }

    private final Runnable hideTop = this::invalidate;

    /** The controls drawn and taking touches */
    private boolean active() {
        return visible() && shown && !editing;
    }

    private void setGamepad(boolean connected) {
        if (connected == gamepad)
            return;
        gamepad = connected;
        topUntil = 0; // the top buttons hide with a gamepad (a touch brings them back)
        invalidate();
        Log.i("vcs", "[touch] " + (connected ? "a gamepad is connected" : "no gamepad") + ": on-screen controls "
                + (active() ? "shown" : "hidden"));
        releaseAll();
    }

    /** Lets go of everything (a pause, a hide): nothing stays held */
    void releaseAll() {
        Arrays.fill(slotTarget, NONE);
        for (int k = 0; k < 2; k++) {
            stickSlot[k] = -1;
            stickX[k] = stickY[k] = 0;
        }
        toggleId = -1;
        if (ffId >= 0 && ffHold) // a held fast forward ends with the touch
            setFastForward(false);
        ffId = -1;
        gearId = -1;
        trainerId = -1;
        panelId = -1;
        edgeId = -1;
        removeCallbacks(longPress);
        removeCallbacks(pulseOff);
        gestPulse = 0;
        gestFire = false;
        if (gestures != null)
            gestures.reset();
        send();
        invalidate();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        post(gamepadPoll);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(gamepadPoll);
        removeCallbacks(longPress);
        nativeSetPad(false, 0, 0, 0, 0, 0, 0, 0);
        sentOn = false;
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        layout(w, h);
    }

    // ---- layout: in units of 1/135 inch (a dp on the phone the earlier port's layout was tuned on), scaled by the size
    // setting. Landscape: anchored to the sides and the bottom, centred in the 4:3 picture's black bars as far as they
    // fit, nudged off the game's HUD. An upright phone: a handheld's layout in the space below the picture. Then the
    // player's layout for this screen: each group moved / scaled, then each control ----

    private float unit() {
        DisplayMetrics dm = getResources().getDisplayMetrics();
        float u = (dm.xdpi + dm.ydpi) / 2 / 135f;
        return u > dm.density * 0.6f && u < dm.density * 1.6f ? u : dm.density;
    }

    private int add(int k, int b, int sh, int g, float x, float y, float rad, String lb, String nm) {
        if (n >= MAX)
            return -1;
        kind[n] = k; bits[n] = b; shape[n] = sh; group[n] = g; cx[n] = x; cy[n] = y; r[n] = rad; label[n] = lb;
        name[n] = nm; op[n] = 1f;
        return n++;
    }

    /** The layout's key: the screen's size in 40-unit steps, in the view's orientation. From the screen (the largest
     *  window), not the view, so the system bars coming and going don't change it; the view's size when the two disagree
     *  (just folded or unfolded: the screen's metrics may lag the view's new size). A foldable's two screens, and each
     *  held either way, are four keys. */
    private String profileKey(int w, int h, float u) {
        int sw = w, sh = h;
        try {
            android.graphics.Rect b = getContext().getSystemService(android.view.WindowManager.class)
                    .getMaximumWindowMetrics().getBounds();
            int lo = Math.min(b.width(), b.height()), hi = Math.max(b.width(), b.height());
            float va = (float) Math.max(w, h) / Math.min(w, h), sa = lo > 0 ? (float) hi / lo : 0;
            if (lo > 0 && Math.abs(sa - va) < va * 0.15f) {
                sw = w > h ? hi : lo;
                sh = w > h ? lo : hi;
            }
        } catch (RuntimeException ignored) {
        }
        return Math.round(sw / u / 40) + "x" + Math.round(sh / u / 40);
    }

    private void layout(int w, int h) {
        if (w <= 0 || h <= 0)
            return;
        float u = unit();
        String key = profileKey(w, h, u);
        if (!key.equals(prof)) {
            if (editing && prof != null)
                saveLayout();
            prof = key;
            loadLayout(Math.round(w / u / 40) + "x" + Math.round(h / u / 40));
            Log.i("vcs", "[touch] screen " + w + "x" + h + ": layout " + prof + (btn.isEmpty() && !customGroups() ? " (default)" : ""));
            selName = null;
            selGroup = -1;
        }
        vehicleLayout = contextLayouts && (editing ? editVehicle : (gameState & STATE_IN_VEHICLE) != 0);
        placeFrame(w, h);
        n = 0;
        float d = portraitLayout ? layoutPortrait(w, h, u) : layoutLandscape(w, h, u);
        // push apart what still overlaps (a large size on a small screen): the later control moves towards the middle
        for (int pass = 0; pass < 4; pass++)
            for (int j = 0; j < n; j++)
                for (int i = 0; i < j; i++) {
                    float dx = cx[j] - cx[i], dy = cy[j] - cy[i], need = r[i] + r[j] + 2 * d;
                    if (dx * dx + dy * dy >= need * need)
                        continue;
                    float move = (float) Math.sqrt(Math.max(0, need * need - dy * dy)) - Math.abs(dx);
                    cx[j] += cx[j] < w / 2f ? move : -move;
                }
        sendHud();
        if (!portraitLayout && !legacyLayout)
            avoidHud(w, h, 6 * d);
        applyGroups(w, h, u);
        applyButtons(w, h, u);
        for (int g = 0; g < GROUPS; g++)
            gbox[g] = groupBox(g);
        // the top buttons, centred: show / hide, settings, trainer, the side panel (their 48 dp touch circles side by side)
        tr = 10 * u;
        float m = 14 * u, step = 4.9f * tr, first = panel != null ? -2f : -1.5f;
        ty = gy = ky = py = fy = m + tr;
        tx = w / 2f + first * step;
        gx = w / 2f + (first + 1) * step;
        kx = w / 2f + (first + 2) * step;
        fx = w / 2f + (first + 3) * step;
        px = w / 2f + (first + 4) * step;
        for (int k = 0; k < 2; k++)
            if (stickSlot[k] < 0 && stickIndex[k] >= 0) {
                stickCx[k] = cx[stickIndex[k]];
                stickCy[k] = cy[stickIndex[k]];
            }
        send(); // the pad's state once laid out: the game sees the overlay (its PlayStation names) before a first touch
        invalidate();
    }

    /** [android] R3 is on the layout: with "L3 and R3", or in a vehicle with "R3 in vehicles" (its saved position stays
     *  in the layout file while it is hidden: the layout keeps what was set by name) */
    private boolean showR3() {
        return extraButtons == EXTRA_BOTH || (extraButtons == EXTRA_R3_VEHICLE && vehicleLayout);
    }

    /** The default layout for a landscape screen (and a near-square one either way); its unit */
    private float layoutLandscape(int w, int h, float u) {
        float fit = Math.min(1f, h / u / 340f), d = u * scale * fit, m = 14 * u;
        float y0 = Math.max(0, h - 380 * d), bh = h - y0;
        float bar = Math.max(0, (w - h * 4f / 3f) / 2); // each black bar beside the 4:3 picture
        float rStick = 60 * d, rPad = 48 * d, rFace = 26 * d, rSh = 25 * d, rSmall = 17 * d, off = 1.65f * rFace;
        // the shoulders in the top corners (L2 / R2 outside), Select / Start beside them
        float shY = y0 + m + 0.7f * rSh;
        add(BUTTON, L2, PILL, 4, m + rSh, shY, rSh, "L2", "L2");
        add(BUTTON, L1, PILL, 4, m + 3 * rSh + 10 * d, shY, rSh, "L1", "L1");
        add(BUTTON, R2, PILL, 5, w - m - rSh, shY, rSh, "R2", "R2");
        add(BUTTON, R1, PILL, 5, w - m - 3 * rSh - 10 * d, shY, rSh, "R1", "R1");
        add(BUTTON, SELECT, PILL, 4, m + 4 * rSh + 24 * d + rSmall * 1.3f, shY, rSmall * 1.3f, "SELECT", "SELECT");
        add(BUTTON, START, PILL, 5, w - m - 4 * rSh - 24 * d - rSmall * 1.3f, shY, rSmall * 1.3f, "START", "START");
        // left: the stick high, the D-pad low, L3 between
        float top = shY + rSh + 10 * d;
        float lx = Math.max(m + rStick, bar / 2), ly = Math.max(y0 + bh * 0.45f, top + rStick);
        stickIndex[0] = add(STICK, 0, ROUND, 0, lx, ly, rStick, "", "LSTICK");
        add(DPAD, UP | DOWN | LEFT | RIGHT, ROUND, 1, Math.max(m + rPad, bar / 2), h - m - rPad, rPad, "", "DPAD");
        float diag = (rStick + rSmall + 6 * d) * 0.7071f;
        if (extraButtons == EXTRA_BOTH)
            add(BUTTON, L3, ROUND, 0, lx + diag, ly + diag, rSmall, "L3", "L3");
        // right: the face buttons as on the PS2 pad, R3 low in the corner; the rest of the right half is the camera
        float fx = Math.min(w - m - rFace - off, w - bar / 2), fy = ly;
        addFace(fx, fy, rFace, off, rSmall);
        if (showR3())
            add(BUTTON, R3, ROUND, 3, Math.min(w - m - rSmall, fx + off), h - m - rSmall, rSmall, "R3", "R3");
        aimIndex = -1;
        if (!vehicleLayout)
            aimIndex = add(BUTTON, 0, ROUND, 3, Math.min(w - m - rSmall, fx + off) - 2.8f * rSmall, h - m - rSmall, rSmall * 1.15f, "AIM", "AIM");
        // the right stick (Camera: floating stick; its rest place), low on the right inside the face buttons
        stickIndex[1] = -1;
        if (camera != CAMERA_DRAG) {
            float rr = rStick * 0.8f;
            stickIndex[1] = add(STICK, 0, ROUND, 6, fx - off - rr * 1.2f, h - m - rr, rr, "", "RSTICK");
        }
        return d;
    }

    /** The face buttons around (fx, fy): the PS2's diamond on foot, Gas / Brake pedals and the lock in a vehicle */
    private void addFace(float fx, float fy, float rFace, float off, float rSmall) {
        cruiseIndex = -1;
        add(BUTTON, TRIANGLE, ROUND, 2, fx, fy - off, rFace, "△", "TRIANGLE");
        add(BUTTON, CIRCLE, ROUND, 2, fx + off, fy, rFace, "○", "CIRCLE");
        if (vehicleLayout) {
            // in a vehicle: Cross (accelerate) and Square (brake / reverse) as two pedals under the other two, the
            // accelerator lock beside Triangle (docs/ANDROID.md "Controls extras")
            float rp = rFace * 1.25f;
            crossIndex = add(BUTTON, CROSS, PEDAL, 2, fx + off * 0.6f, fy + off * 1.55f, rp, "GAS", "GAS");
            add(BUTTON, SQUARE, PEDAL, 2, fx - off * 1.2f, fy + off * 1.55f + rp * 0.2f, rp, "BRAKE", "BRAKE");
            if (cruiseButton)
                cruiseIndex = add(BUTTON, 0, ROUND, 2, fx - off * 1.1f, fy - off * 0.75f, rSmall, "LOCK", "LOCK");
        } else {
            crossIndex = add(BUTTON, CROSS, ROUND, 2, fx, fy + off, rFace, "✕", "CROSS");
            add(BUTTON, SQUARE, ROUND, 2, fx - off, fy, rFace, "□", "SQUARE");
        }
    }

    /** The default layout for an upright phone (the picture at the top, presenter.cpp): a handheld's, in the space below
     *  the picture - the shoulders along its top with Select / Start between them, the D-pad and the left stick (lower,
     *  inwards) on the left, the face buttons and the right stick on the right, L3 / R3 in the bottom corners */
    private float layoutPortrait(int w, int h, float u) {
        float top = frame.bottom, bh = h - top;
        float fit = Math.min(1f, Math.min(bh / u / 290f, w / u / 340f)), d = u * scale * fit, m = 12 * u * Math.min(1f, fit + 0.2f);
        float rStick = 52 * d, rPad = 46 * d, rFace = 25 * d, rSh = 24 * d, rSmall = 16 * d, off = 1.65f * rFace;
        float shY = top + m + 0.85f * rSh, rSel = rSmall * 1.3f;
        add(BUTTON, L2, PILL, 4, m + rSh, shY, rSh, "L2", "L2");
        add(BUTTON, L1, PILL, 4, m + 3 * rSh + 10 * d, shY, rSh, "L1", "L1");
        add(BUTTON, R2, PILL, 5, w - m - rSh, shY, rSh, "R2", "R2");
        add(BUTTON, R1, PILL, 5, w - m - 3 * rSh - 10 * d, shY, rSh, "R1", "R1");
        add(BUTTON, SELECT, PILL, 4, w / 2f - rSel - 4 * d, shY, rSel, "SELECT", "SELECT");
        add(BUTTON, START, PILL, 5, w / 2f + rSel + 4 * d, shY, rSel, "START", "START");
        float row = shY + 0.85f * rSh + 8 * d;
        float px = m + rPad + 4 * d, sx = px + rPad * 1.1f, sy = h - m - rStick;
        stickIndex[0] = add(STICK, 0, ROUND, 0, sx, sy, rStick, "", "LSTICK");
        add(DPAD, UP | DOWN | LEFT | RIGHT, ROUND, 1, px, row + rPad, rPad, "", "DPAD");
        if (extraButtons == EXTRA_BOTH)
            add(BUTTON, L3, ROUND, 0, m + rSmall, h - m - rSmall, rSmall, "L3", "L3");
        float fx = w - m - rFace - off, fy = row + off + rFace;
        addFace(fx, fy, rFace, off, rSmall);
        if (showR3())
            add(BUTTON, R3, ROUND, 3, w - m - rSmall, h - m - rSmall, rSmall, "R3", "R3");
        aimIndex = -1;
        if (!vehicleLayout)
            aimIndex = add(BUTTON, 0, ROUND, 3, w - m - rSmall - 2.8f * rSmall, h - m - rSmall, rSmall * 1.15f, "AIM", "AIM");
        stickIndex[1] = -1;
        if (camera == CAMERA_STICK) {
            float rr = rStick * 0.9f;
            stickIndex[1] = add(STICK, 0, ROUND, 6, w - sx, h - m - rr, rr, "", "RSTICK");
        }
        return d;
    }

    /** Where the game's picture is (presenter.cpp fitRect for the frame at display.aspect: centred, at the top of an
     *  upright screen), whether the portrait layout applies, and where the HUD's radar and money / health sit
     *  (display.hud; their 640x448 boxes from game-internals.md 6f, the right cluster approximate) */
    private void placeFrame(int w, int h) {
        float a;
        switch (aspectCode) {
            case 54: a = 1.25f; break;
            case 11: a = 1f; break;
            case 34: a = 0.75f; break;
            case 1610: a = 1.6f; break;
            case 169: a = 16f / 9f; break;
            case 219: a = 21f / 9f; break;
            case 329: a = 32f / 9f; break;
            case 0: a = Math.max(0.75f, Math.min(32f / 9f, (float) w / h)); break; // Match window (main.cpp's clamp)
            default: a = 4f / 3f; break; // 43, or unknown
        }
        float fw = w, fh = w / a;
        if (fh > h) {
            fh = h;
            fw = h * a;
        }
        boolean atTop = h > w && fh < h - 0.5f;
        frame.set((w - fw) / 2, atTop ? 0 : (h - fh) / 2, (w + fw) / 2, (atTop ? 0 : (h - fh) / 2) + fh);
        portraitLayout = atTop && h - fh >= 0.22f * h;
        hudRadar = hudMoney = null;
        if (hudMode < 0)
            return;
        hudRadar = userHud(hudRect(37, 336, 120, 412, true, false), 0);
        hudMoney = userHud(hudRect(355, 14, 612, 120, false, true), 1);
    }

    /** A HUD element's box (640x448 units) on the screen: Keep 4:3 in the frame's middle 4:3, Screen edges keeps its
     *  size and its distance from its edges, Stretch across the frame (main.cpp applyAspect, vcs_hud.cpp) */
    private RectF hudRect(float x0, float y0, float x1, float y1, boolean left, boolean top) {
        float fw = frame.width(), fh = frame.height(), hw, hh;
        if (fw / fh >= 4f / 3f) {
            hh = fh;
            hw = fh * 4f / 3f;
        } else {
            hw = fw;
            hh = fw * 0.75f;
        }
        float sx = hw / 640f, sy = hh / 448f, hx = frame.left + (fw - hw) / 2, hy = frame.top + (fh - hh) / 2;
        if (hudMode == 1)
            return new RectF(frame.left + x0 / 640f * fw, frame.top + y0 / 448f * fh, frame.left + x1 / 640f * fw,
                    frame.top + y1 / 448f * fh);
        if (hudMode == 2) {
            float l = left ? frame.left + x0 * sx : frame.right - (640 - x0) * sx;
            float t = top ? frame.top + y0 * sy : frame.bottom - (448 - y0) * sy;
            return new RectF(l, t, l + (x1 - x0) * sx, t + (y1 - y0) * sy);
        }
        return new RectF(hx + x0 * sx, hy + y0 * sy, hx + x1 * sx, hy + y1 * sy);
    }

    /** The game's display.aspect and display.hud (the side panel's store; nothing in the smoke test): the picture's place
     *  and the HUD's. True when either changed. */
    private boolean readGameSettings() {
        SidePanel.Setting a = SidePanel.Setting.read("display.aspect"), hd = SidePanel.Setting.read("display.hud");
        int ac = a != null ? a.value : -1, hm = hd != null ? hd.value : -1;
        if (ac == aspectCode && hm == hudMode)
            return false;
        aspectCode = ac;
        hudMode = hm;
        return true;
    }

    /** A control's half extents (its touch area): pills are wide, pedals tall */
    private float extX(int i) {
        return kind[i] == BUTTON && shape[i] == PILL ? r[i] * 1.15f : kind[i] == BUTTON && shape[i] == PEDAL ? r[i] * 0.85f : r[i];
    }

    private float extY(int i) {
        return kind[i] == BUTTON && shape[i] == PILL ? r[i] * 0.85f : kind[i] == BUTTON && shape[i] == PEDAL ? r[i] * 1.2f : r[i];
    }

    private RectF groupBox(int g) {
        RectF b = null;
        for (int i = 0; i < n; i++) {
            if (group[i] != g)
                continue;
            if (b == null)
                b = new RectF(cx[i] - extX(i), cy[i] - extY(i), cx[i] + extX(i), cy[i] + extY(i));
            else
                b.union(cx[i] - extX(i), cy[i] - extY(i), cx[i] + extX(i), cy[i] + extY(i));
        }
        return b;
    }

    private void shiftGroup(int g, float dx, float dy) {
        for (int i = 0; i < n; i++)
            if (group[i] == g) {
                cx[i] += dx;
                cy[i] += dy;
            }
    }

    /** The default layout off the game's HUD (the radar bottom left, the money / health top right: under the D-pad and
     *  R1 / R2 with display.hud at the screen edges): each group that covers one, or a group moved before it, moves by
     *  the least that clears it and stays on the screen. The shoulders first, then what is under them. */
    private void avoidHud(int w, int h, float gap) {
        java.util.ArrayList<RectF> obstacles = new java.util.ArrayList<>();
        for (RectF z : new RectF[] {hudRadar, hudMoney})
            if (z != null)
                obstacles.add(new RectF(z.left - gap, z.top - gap, z.right + gap, z.bottom + gap));
        if (obstacles.isEmpty())
            return;
        for (int g : new int[] {5, 4, 2, 6, 3, 0, 1}) {
            RectF b = groupBox(g);
            if (b == null)
                continue;
            boolean movedG = false;
            for (int pass = 0; pass < 3; pass++) {
                RectF o = null;
                for (RectF z : obstacles)
                    if (RectF.intersects(z, b)) {
                        o = z;
                        break;
                    }
                if (o == null)
                    break;
                float best = Float.MAX_VALUE, bx = 0, by = 0;
                float[][] moves = {{o.right - b.left, 0}, {o.left - b.right, 0}, {0, o.bottom - b.top}, {0, o.top - b.bottom}};
                for (float[] mv : moves) {
                    if (b.left + mv[0] < 0 || b.right + mv[0] > w || b.top + mv[1] < 0 || b.bottom + mv[1] > h)
                        continue;
                    float dd = Math.abs(mv[0]) + Math.abs(mv[1]);
                    if (dd < best) {
                        best = dd;
                        bx = mv[0];
                        by = mv[1];
                    }
                }
                if (best == Float.MAX_VALUE)
                    break;
                shiftGroup(g, bx, by);
                b.offset(bx, by);
                movedG = true;
            }
            if (movedG)
                obstacles.add(new RectF(b.left - gap, b.top - gap, b.right + gap, b.bottom + gap));
        }
    }

    /** Each group moved and scaled as the player left it (around where the default layout put it), kept on screen */
    private void applyGroups(int w, int h, float u) {
        for (int g = 0; g < GROUPS; g++) {
            float sx = 0, sy = 0;
            int k = 0;
            for (int i = 0; i < n; i++)
                if (group[i] == g) { sx += cx[i]; sy += cy[i]; k++; }
            if (k == 0)
                continue;
            float mx = sx / k, my = sy / k, sc = gsc[g];
            for (int i = 0; i < n; i++) {
                if (group[i] != g)
                    continue;
                cx[i] = mx + (cx[i] - mx) * sc + gdx[g] * u;
                cy[i] = my + (cy[i] - my) * sc + gdy[g] * u;
                r[i] *= sc;
            }
            RectF b = groupBox(g);
            float ox = b.left < 0 ? -b.left : b.right > w ? w - b.right : 0, oy = b.top < 0 ? -b.top : b.bottom > h ? h - b.bottom : 0;
            if (ox != 0 || oy != 0) {
                shiftGroup(g, ox, oy);
                if (editing) { // dragged against the edge: the offset stops there (no dead travel coming back)
                    gdx[g] += ox / u;
                    gdy[g] += oy / u;
                }
            }
        }
    }

    /** Each control's own place, size and opacity on top of its group's, kept on screen */
    private void applyButtons(int w, int h, float u) {
        for (int i = 0; i < n; i++) {
            float[] b = btn.get(name[i]);
            if (b == null)
                continue;
            cx[i] += b[0] * u;
            cy[i] += b[1] * u;
            r[i] *= b[2];
            op[i] = b[3];
            float ex = extX(i), ey = extY(i);
            float ox = cx[i] - ex < 0 ? ex - cx[i] : cx[i] + ex > w ? w - ex - cx[i] : 0;
            float oy = cy[i] - ey < 0 ? ey - cy[i] : cy[i] + ey > h ? h - ey - cy[i] : 0;
            cx[i] += ox;
            cy[i] += oy;
            if (editing) {
                b[0] += ox / u;
                b[1] += oy / u;
            }
        }
    }

    private boolean customGroups() {
        for (int g = 0; g < GROUPS; g++)
            if (gdx[g] != 0 || gdy[g] != 0 || gsc[g] != 1)
                return true;
        return false;
    }

    /** This screen's layout: "layout@<prof>" (v2), else taken over from the group-only layouts of before (group<g>@<key>,
     *  "dx dy scale", under this key or the view-size key they were saved by), kept with the defaults they were made
     *  on (legacy: no HUD nudge) and saved as v2 at once; the old keys stay as they are. */
    private void loadLayout(String viewKey) {
        Arrays.fill(gdx, 0);
        Arrays.fill(gdy, 0);
        Arrays.fill(gsc, 1);
        btn.clear();
        legacyLayout = false;
        SharedPreferences p = prefs(getContext());
        String v = p.getString("layout@" + prof, null);
        if (v != null) {
            parseLayout(v);
            return;
        }
        for (String k : new String[] {prof, viewKey}) {
            boolean any = false;
            for (int g = 0; g < GROUPS; g++) {
                String s = p.getString("group" + g + "@" + k, null);
                if (s == null)
                    continue;
                String[] f = s.split(" ");
                try {
                    gdx[g] = Float.parseFloat(f[0]);
                    gdy[g] = Float.parseFloat(f[1]);
                    gsc[g] = clampScale(Float.parseFloat(f[2]));
                    any = true;
                } catch (RuntimeException e) {
                    gdx[g] = 0; gdy[g] = 0; gsc[g] = 1;
                }
            }
            if (any) {
                legacyLayout = true;
                Log.i("vcs", "[touch] layout " + prof + ": taken over from the group layout " + k);
                saveLayout();
                return;
            }
        }
    }

    private static float clampScale(float s) {
        return Float.isNaN(s) ? 1f : Math.max(0.5f, Math.min(2f, s));
    }

    /** "v2[;legacy];g<k>:dx,dy,scale;...;<NAME>:dx,dy,scale,opacity;..." (units, factors), only what isn't the default */
    private void parseLayout(String v) {
        for (String t : v.split(";")) {
            if (t.equals("legacy")) {
                legacyLayout = true;
                continue;
            }
            int c = t.indexOf(':');
            if (c <= 0)
                continue;
            String k = t.substring(0, c);
            String[] f = t.substring(c + 1).split(",");
            try {
                if (k.length() > 1 && k.charAt(0) == 'g' && Character.isDigit(k.charAt(1))) {
                    int g = Integer.parseInt(k.substring(1));
                    if (g < GROUPS && f.length >= 3) {
                        gdx[g] = Float.parseFloat(f[0]);
                        gdy[g] = Float.parseFloat(f[1]);
                        gsc[g] = clampScale(Float.parseFloat(f[2]));
                    }
                } else if (f.length >= 4)
                    btn.put(k, new float[] {Float.parseFloat(f[0]), Float.parseFloat(f[1]), clampScale(Float.parseFloat(f[2])),
                            Math.max(0.05f, Math.min(10f, Float.parseFloat(f[3])))});
            } catch (NumberFormatException ignored) {
            }
        }
    }

    private static String num(float f) {
        return String.format(Locale.ROOT, "%.2f", f);
    }

    private void saveLayout() {
        if (prof == null)
            return;
        StringBuilder sb = new StringBuilder("v2");
        if (legacyLayout)
            sb.append(";legacy");
        for (int g = 0; g < GROUPS; g++)
            if (gdx[g] != 0 || gdy[g] != 0 || gsc[g] != 1)
                sb.append(";g").append(g).append(':').append(num(gdx[g])).append(',').append(num(gdy[g])).append(',').append(num(gsc[g]));
        for (java.util.Map.Entry<String, float[]> e : btn.entrySet()) {
            float[] b = e.getValue();
            if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 1)
                sb.append(';').append(e.getKey()).append(':').append(num(b[0])).append(',').append(num(b[1])).append(',')
                        .append(num(b[2])).append(',').append(num(b[3]));
        }
        prefs(getContext()).edit().putString("layout@" + prof, sb.toString()).commit(); // commit: the game's process ends soon after
    }

    /** This screen's layout back to the default (the side panel's Reset layout, the editor's Reset all); other screens
     *  keep theirs. Saved as an empty v2 layout, so an old group layout is not taken over again. */
    void resetLayout() {
        if (prof == null)
            return;
        Arrays.fill(gdx, 0);
        Arrays.fill(gdy, 0);
        Arrays.fill(gsc, 1);
        btn.clear();
        legacyLayout = false;
        saveLayout();
        Log.i("vcs", "[touch] layout " + prof + " reset");
        selName = null;
        selGroup = -1;
        relayout();
    }

    /** The screen may have changed (folded / unfolded, turned): laid out again once the change has settled */
    void screenChanged() {
        post(this::relayout);
    }

    private void relayout() {
        layout(getWidth(), getHeight());
    }

    // ---- touches ----

    private boolean onGear(float x, float y) {
        float dx = x - gx, dy = y - gy, g = tr * 2.4f;
        return topArmed && topShown() && dx * dx + dy * dy <= g * g;
    }

    private boolean onTrainer(float x, float y) {
        float dx = x - kx, dy = y - ky, g = tr * 2.4f;
        return topArmed && topShown() && dx * dx + dy * dy <= g * g;
    }

    private boolean onFastForward(float x, float y) {
        float dx = x - fx, dy = y - fy, g = tr * 2.4f;
        return topArmed && topShown() && dx * dx + dy * dy <= g * g;
    }

    private boolean fastForwardOn() {
        try {
            return nativeFastForwardNow() > 1.01f;
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
    }

    private void setFastForward(boolean on) {
        try {
            nativeFastForward(on ? ffSpeed : 1f);
        } catch (UnsatisfiedLinkError ignored) {
        }
        Log.i("vcs", "[touch] fast forward " + (on ? ffSpeed + "x" : "off"));
        invalidate();
    }

    private boolean onPanelButton(float x, float y) {
        float dx = x - px, dy = y - py, g = tr * 2.4f;
        return panel != null && topArmed && topShown() && dx * dx + dy * dy <= g * g;
    }

    /** Whether a touch at (x, y) is the overlay's: a drawn control or a top button (the side panel lets it through) */
    boolean hitsControl(float x, float y) {
        return onToggle(x, y) || onGear(x, y) || onTrainer(x, y) || onFastForward(x, y) || onPanelButton(x, y)
                || (active() && hit(x, y) >= 0);
    }

    private boolean onToggle(float x, float y) {
        float dx = x - tx, dy = y - ty, g = tr * 2.4f; // a 48 dp target around a small drawing
        return topArmed && topShown() && dx * dx + dy * dy <= g * g;
    }

    /** The control under (x, y), the nearest when several are near: -1 for none */
    private int hit(float x, float y) {
        int best = -1;
        float bestD = Float.MAX_VALUE;
        for (int i = 0; i < n; i++) {
            if (menuHides(i))
                continue;
            float dx = x - cx[i], dy = y - cy[i], q;
            if (kind[i] == BUTTON && shape[i] == PEDAL) {
                if (Math.abs(dx) > r[i] * 0.85f || Math.abs(dy) > r[i] * 1.2f)
                    continue;
                q = Math.abs(dy) / r[i] * 0.5f;
            } else if (kind[i] == BUTTON && shape[i] == PILL) {
                if (Math.abs(dx) > r[i] * 1.15f || Math.abs(dy) > r[i] * 0.85f)
                    continue;
                q = Math.abs(dx) / r[i];
            } else {
                float slop = kind[i] == BUTTON ? 1.2f : 1.15f;
                q = (float) Math.hypot(dx, dy) / r[i];
                if (q > slop)
                    continue;
            }
            if (q < bestD) { bestD = q; best = i; }
        }
        return best;
    }

    /** The face / shoulder button under (x, y) for a finger sliding between them; -1 for none */
    private int hitButton(float x, float y) {
        int i = hit(x, y);
        return i >= 0 && kind[i] == BUTTON ? i : -1;
    }

    private int slotOf(int id) {
        for (int s = 0; s < SLOTS; s++)
            if (slotTarget[s] != NONE && slotId[s] == id)
                return s;
        return -1;
    }

    private int freeSlot() {
        for (int s = 0; s < SLOTS; s++)
            if (slotTarget[s] == NONE)
                return s;
        return -1;
    }

    private void track(int s, float x, float y) {
        int t = slotTarget[s];
        if (t == CAMERA) {
            float k = LOOK_COUNTS_PER_DP / getResources().getDisplayMetrics().density;
            float dx = (x - slotX[s]) * k, dy = (y - slotY[s]) * k;
            if (dx != 0 || dy != 0)
                nativeLook(dx, dy);
            if (gestures != null)
                gestureCam(gestures.camMove(slotId[s], x, y, eventTime));
        } else if (t == WEAPON) {
            gestureWeaponTrack(s, x, y);
        } else if (t >= 0 && kind[t] == BUTTON) {
            // a finger slides onto a neighbour (Square to Cross), lets go off them, and presses again when back on one
            int b = hitButton(x, y);
            if (b >= 0)
                slotTarget[s] = b;
            slotBits[s] = b >= 0 ? bits[b] : 0;
        } else if (t >= 0 && kind[t] == DPAD) {
            float dx = x - cx[t], dy = y - cy[t];
            if (Math.hypot(dx, dy) < r[t] * 0.25f)
                slotBits[s] = 0;
            else {
                // eight ways (y grows downwards): 0 right, 2 down, 4 left, 6 up
                int sector = ((int) Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) + 8) % 8;
                final int[] dirs = {RIGHT, RIGHT | DOWN, DOWN, DOWN | LEFT, LEFT, LEFT | UP, UP, UP | RIGHT};
                slotBits[s] = dirs[sector];
            }
        } else if (t >= 0 && kind[t] == STICK) {
            int k = t == stickIndex[1] ? 1 : 0;
            float rr = r[t], dx = x - stickCx[k], dy = y - stickCy[k], len = (float) Math.hypot(dx, dy);
            if (len > rr) { // the stick's centre follows a thumb that goes past its edge
                stickCx[k] = x - dx / len * rr;
                stickCy[k] = y - dy / len * rr;
                dx = dx / len * rr;
                dy = dy / len * rr;
            }
            stickX[k] = dx / rr;
            stickY[k] = dy / rr;
        }
        slotX[s] = x;
        slotY[s] = y;
    }

    private void release(int s) {
        if (s < 0)
            return;
        if (gestures != null) {
            if (slotTarget[s] == CAMERA)
                gestureCam(gestures.camUp(slotId[s], slotX[s], slotY[s], eventTime));
            else
                gestures.otherUp();
        }
        for (int k = 0; k < 2; k++)
            if (s == stickSlot[k]) {
                stickSlot[k] = -1;
                stickX[k] = stickY[k] = 0;
                if (stickIndex[k] >= 0) {
                    stickCx[k] = cx[stickIndex[k]];
                    stickCy[k] = cy[stickIndex[k]];
                }
            }
        slotTarget[s] = NONE;
        slotBits[s] = 0;
    }

    /** A new finger: true when the view takes it (false: it goes on to SDL's surface underneath) */
    private boolean down(int id, float x, float y, boolean first) {
        int s = freeSlot();
        if (s < 0)
            return true;
        boolean look = nativeLookWanted(); // playing, no menu open: every touch is the controls' or the camera's
        int t = hit(x, y);
        int sk = t >= 0 && kind[t] == STICK ? (t == stickIndex[1] ? 1 : 0) : -1;
        if (sk >= 0 && stickSlot[sk] >= 0)
            t = IGNORED;
        if (t == NONE) {
            if (!look) {
                if (first)
                    return false; // a menu is open: Dear ImGui / the game's pause menu get it as a click
                t = IGNORED;
            } else if (x >= getWidth() / 2f || (portraitLayout && y < frame.bottom)) {
                // the free right half (upright: the whole picture): the floating right stick under the thumb, or the drag
                if (inWeaponBox(x, y)) {
                    t = WEAPON;
                    wsX[s] = x;
                    wsY[s] = y;
                } else if (camera != CAMERA_STICK)
                    t = CAMERA;
                else if (stickSlot[1] < 0 && stickIndex[1] >= 0) {
                    t = stickIndex[1];
                    sk = 1;
                    stickCx[1] = x;
                    stickCy[1] = y;
                } else
                    t = IGNORED;
            } else if (stickSlot[0] < 0 && stickIndex[0] >= 0) {
                t = stickIndex[0]; // the free left half: the stick, centred under the thumb
                sk = 0;
                stickCx[0] = x;
                stickCy[0] = y;
            } else
                t = IGNORED;
        } else if (sk >= 0) {
            stickCx[sk] = cx[t];
            stickCy[sk] = cy[t];
        }
        if (t >= 0 && t == cruiseIndex) {
            cruiseOn = !cruiseOn && look; // the accelerator lock: Cross held until Brake or a second tap
            Log.i("vcs", "[touch] accelerator lock " + (cruiseOn ? "on" : "off"));
        }
        if (t >= 0 && t == aimIndex)
            toggleAim(look);
        if (gestures != null) { // the camera fingers go to the gesture detector, every other finger just counts
            if (t == CAMERA)
                gestureCam(gestures.camDown(id, x, y, eventTime));
            else
                gestures.otherDown();
        }
        slotId[s] = id;
        slotTarget[s] = t;
        slotBits[s] = t >= 0 && kind[t] == BUTTON ? bits[t] : 0;
        slotX[s] = x;
        slotY[s] = y;
        if (sk >= 0 && t >= 0)
            stickSlot[sk] = s;
        if (t >= 0 && kind[t] != BUTTON)
            track(s, x, y);
        return true;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing)
            return editTouch(e);
        int act = e.getActionMasked();
        eventTime = e.getEventTime();
        switch (act) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                float x = e.getX(i), y = e.getY(i);
                float dpx = getResources().getDisplayMetrics().density;
                topArmed = topShown(); // hidden top buttons: this touch only brings them back
                revealTop();
                if (panel != null && edgeId < 0 && x >= getWidth() - 18 * dpx && !hitsControl(x, y)) {
                    edgeId = id; // perhaps a swipe in from the right edge: the side panel
                    edgeX0 = x;
                    return true;
                }
                if (panelId < 0 && onPanelButton(x, y)) {
                    panelId = id;
                    if (haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                if (toggleId < 0 && onToggle(x, y)) {
                    toggleId = id;
                    toggleLong = false;
                    postDelayed(longPress, 600);
                    if (haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                if (gearId < 0 && onGear(x, y)) {
                    gearId = id;
                    if (haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                if (ffId < 0 && onFastForward(x, y)) {
                    ffId = id;
                    if (ffHold)
                        setFastForward(true);
                    if (haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                if (trainerId < 0 && onTrainer(x, y)) {
                    trainerId = id;
                    if (haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    return true;
                }
                if (!active())
                    return act != MotionEvent.ACTION_DOWN; // hidden: everything goes through (a later finger can't)
                if (!down(id, x, y, act == MotionEvent.ACTION_DOWN))
                    return false;
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    if (e.getPointerId(i) == edgeId) {
                        if (edgeX0 - e.getX(i) > 40 * getResources().getDisplayMetrics().density) {
                            edgeId = -1;
                            Log.i("vcs", "[touch] swipe from the right edge: the side panel");
                            panel.open();
                        }
                        continue;
                    }
                    int s = slotOf(e.getPointerId(i));
                    if (s >= 0)
                        track(s, e.getX(i), e.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                if (id == edgeId) {
                    edgeId = -1;
                    return true;
                }
                if (id == panelId) {
                    panelId = -1;
                    if (onPanelButton(e.getX(i), e.getY(i))) {
                        Log.i("vcs", "[touch] panel button: the side panel");
                        panel.toggle();
                    }
                    return true;
                }
                if (id == toggleId) {
                    toggleId = -1;
                    removeCallbacks(longPress);
                    if (!toggleLong && onToggle(e.getX(i), e.getY(i)))
                        toggle();
                    return true;
                }
                if (id == gearId) {
                    gearId = -1;
                    if (onGear(e.getX(i), e.getY(i))) {
                        Log.i("vcs", "[touch] settings button: the settings menu opens / closes");
                        nativeToggleMenu();
                    }
                    return true;
                }
                if (id == ffId) {
                    ffId = -1;
                    if (ffHold)
                        setFastForward(false);
                    else if (onFastForward(e.getX(i), e.getY(i)))
                        setFastForward(!fastForwardOn());
                    return true;
                }
                if (id == trainerId) {
                    trainerId = -1;
                    if (onTrainer(e.getX(i), e.getY(i))) {
                        Log.i("vcs", "[touch] trainer button: the trainer opens / closes");
                        nativeToggleTrainer();
                    }
                    return true;
                }
                release(slotOf(id));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                return true;
            default:
                return true;
        }
        send();
        return true;
    }

    void toggle() {
        shown = !shown;
        prefs(getContext()).edit().putBoolean("shown", shown).commit();
        Log.i("vcs", "[touch] on-screen controls " + (shown ? "shown" : "hidden") + " by the player");
        releaseAll();
    }

    // ---- [android] gestures (the timing is in TouchGestures; this is what they do) ----

    /** Free aim on / off: the AIM button's and the two-finger tap's one code path */
    private void toggleAim(boolean look) {
        aimOn = !aimOn && look; // free aim: R1 + L3 held until a second tap, a vehicle or a menu
        Log.i("vcs", "[touch] free aim " + (aimOn ? "on" : "off"));
    }

    private boolean onFoot() {
        return !vehicleLayout && (gameState & STATE_IN_VEHICLE) == 0;
    }

    /** What the detector found for a camera finger: Circle held for a double-tap, free aim for a two-finger tap. Both
     *  only while the game takes the pad (no menu, no cutscene) */
    private void gestureCam(int ev) {
        if (ev == 0)
            return;
        boolean look = nativeLookWanted();
        if ((ev & TouchGestures.EV_FIRE_ON) != 0) {
            if (gestDtapFire && look) {
                gestFire = true;
                Log.i("vcs", "[touch] double-tap: fire held");
            }
        }
        if ((ev & TouchGestures.EV_FIRE_OFF) != 0)
            gestFire = false;
        if ((ev & TouchGestures.EV_TWO_FINGER_TAP) != 0 && gestTwoFingerAim && look && onFoot()) {
            Log.i("vcs", "[touch] two-finger tap");
            toggleAim(look);
        }
    }

    /** A finger on the weapon box (the right part of the HUD's top-right cluster, with a generous margin): right is the
     *  next weapon, left the previous; a clearly vertical move turns it into a camera drag. Only on foot. */
    private boolean inWeaponBox(float x, float y) {
        if (!gestWeaponSwipe || hudMoney == null || editing || !onFoot())
            return false;
        float pad = 12 * getResources().getDisplayMetrics().density;
        float l = hudMoney.left + hudMoney.width() * 0.4f - pad;
        return x >= l && x <= hudMoney.right + pad && y >= hudMoney.top - pad && y <= hudMoney.bottom + pad;
    }

    private void gestureWeaponTrack(int s, float x, float y) {
        float dp = getResources().getDisplayMetrics().density;
        int d = TouchGestures.swipeDir(x - wsX[s], y - wsY[s], dp);
        if (d == TouchGestures.SWIPE_NEXT || d == TouchGestures.SWIPE_PREV) {
            slotTarget[s] = IGNORED; // once per touch
            if (onFoot() && nativeLookWanted()) {
                // the PS2 pad's own weapon change: R2 next, L2 previous (the mouse wheel presses them: vcs_input.cpp)
                gestPulse = d == TouchGestures.SWIPE_NEXT ? R2 : L2;
                removeCallbacks(pulseOff);
                postDelayed(pulseOff, WEAPON_PULSE_MS);
                Log.i("vcs", "[touch] weapon swipe: " + (d == TouchGestures.SWIPE_NEXT ? "next" : "previous"));
            }
        } else if (d == TouchGestures.SWIPE_VERTICAL) {
            slotTarget[s] = CAMERA; // not a weapon swipe: a camera drag from here
            if (gestures != null)
                gestures.otherUp();
        }
    }

    /** [android] The look latch (docs/ANDROID.md "Controls extras"): in the vehicle layout a quick tap on L2 or R2 keeps
     *  it held (looking left / right: a drive-by is then a drag to aim and Circle to fire, two fingers instead of three);
     *  a tap on it again or a press of the other lets go. A hold works as before (L2 + R2 held: look behind). The bit
     *  to add to the pad. */
    private int lookLatch(boolean on, int raw, int pressedNow, int releasedNow) {
        if (!on || !lookLatch || !vehicleLayout || (gameState & STATE_IN_VEHICLE) == 0) {
            lookLatched = 0;
            return 0;
        }
        long t = android.os.SystemClock.uptimeMillis();
        for (int k = 0; k < 2; k++) {
            int bit = k == 0 ? L2 : R2;
            if ((pressedNow & bit) != 0) {
                lookDownAt[k] = t;
                lookUnlatching[k] = lookLatched == bit; // a tap on the latched one lets go of it
                if (lookLatched != 0) {
                    Log.i("vcs", "[touch] look latch off");
                    lookLatched = 0;
                }
            }
            if ((releasedNow & bit) != 0) {
                if (!lookUnlatching[k] && t - lookDownAt[k] < LOOK_TAP_MS && (raw & (L2 | R2)) == 0) {
                    lookLatched = bit;
                    Log.i("vcs", "[touch] look latch: " + (k == 0 ? "L2 (look left)" : "R2 (look right)"));
                }
                lookUnlatching[k] = false;
            }
        }
        return lookLatched;
    }

    /** The pad as the fingers hold it, to native code when it changed; a tick for each new press */
    private void send() {
        int b = 0;
        int[] now = new int[MAX];
        for (int s = 0; s < SLOTS; s++) {
            int t = slotTarget[s];
            if (t < 0 || t >= n)
                continue;
            b |= slotBits[s];
            now[t] |= kind[t] == STICK || t == cruiseIndex || t == aimIndex ? 1 : slotBits[s]; // what each control shows held
        }
        boolean on = active();
        if (!on) {
            b = 0;
            Arrays.fill(now, 0);
        }
        // the latches: on foot Cross as a sprint toggle, in a vehicle the accelerator lock (Brake lets go of it)
        int raw = b, pressedNow = raw & ~rawPrev, releasedNow = rawPrev & ~raw;
        rawPrev = raw;
        if (on && sprintToggle && !vehicleLayout && (gameState & STATE_IN_VEHICLE) == 0) {
            if ((pressedNow & CROSS) != 0)
                sprintLatched = !sprintLatched;
            b = (b & ~CROSS) | (sprintLatched ? CROSS : 0);
        } else
            sprintLatched = false;
        if (cruiseOn && (!on || (raw & SQUARE) != 0 || !vehicleLayout))
            cruiseOn = false;
        if (cruiseOn)
            b |= CROSS;
        if (aimOn && (!on || vehicleLayout || (gameState & STATE_IN_VEHICLE) != 0))
            aimOn = false;
        if (aimOn)
            b |= R1 | L3;
        if (on && aimIndex >= 0 && aimOn)
            now[aimIndex] |= 1;
        b |= lookLatch(on, raw, pressedNow, releasedNow);
        if (on && crossIndex >= 0 && (b & CROSS) != 0)
            now[crossIndex] |= CROSS;
        if (on && cruiseIndex >= 0 && cruiseOn)
            now[cruiseIndex] |= 1;
        if (on && lookLatched != 0)
            for (int i = 0; i < n; i++)
                if (kind[i] == BUTTON && bits[i] == lookLatched)
                    now[i] |= lookLatched; // the latched L2 / R2 shows held
        boolean tick = false, changed = false;
        for (int i = 0; i < n; i++) {
            if (kind[i] == BUTTON ? (now[i] & ~pressed[i]) != 0 : kind[i] == DPAD && pressed[i] == 0 && now[i] != 0)
                tick = true;
            if (now[i] != pressed[i])
                changed = true;
            pressed[i] = now[i];
        }
        if (remapped && !menuOpen()) { // what each button presses (the menus keep the plain pad: it drives them)
            int out = b & (UP | DOWN | LEFT | RIGHT);
            for (int bit : REMAP_BITS)
                if ((b & 1 << bit) != 0)
                    out |= 1 << remap[bit];
            b = out;
        }
        if (on) { // the gestures' own presses, after the remap: the game's real buttons
            if (gestFire)
                b |= CIRCLE;
            b |= gestPulse;
        }
        float lx = on ? stickX[0] : 0, ly = on ? stickY[0] : 0, rx = on ? stickX[1] : 0, ry = on ? stickY[1] : 0;
        if (b != sentButtons || lx != sentLx || ly != sentLy || rx != sentRx || ry != sentRy || on != sentOn) {
            nativeSetPad(on, b & PAD_BUTTONS, lx, ly, rx, ry, (b & L2) != 0 ? 1 : 0, (b & R2) != 0 ? 1 : 0);
            sentButtons = b;
            sentLx = lx;
            sentLy = ly;
            sentRx = rx;
            sentRy = ry;
            sentOn = on;
            changed = true;
        }
        if (tick && haptics)
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (changed)
            invalidate();
    }

    // ---- the settings: long press on the show / hide button ----

    void openSettings() {
        Context c = getContext();
        int pad = Math.round(16 * getResources().getDisplayMetrics().density);
        LinearLayout box = new LinearLayout(c);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, pad / 2, pad, 0);
        RadioGroup modes = new RadioGroup(c);
        modes.setOrientation(RadioGroup.HORIZONTAL);
        for (int i = 0; i < MODE_NAMES.length; i++) {
            RadioButton b = new RadioButton(c);
            b.setId(4000 + i);
            b.setText(MODE_NAMES[i]);
            modes.addView(b);
        }
        modes.check(4000 + mode);
        TextView help = new TextView(c);
        help.setText("Auto: hidden while a gamepad is connected. Off: never shown (the launcher turns it back on).");
        TextView opacityLabel = new TextView(c), sizeLabel = new TextView(c);
        SeekBar opacityBar = new SeekBar(c), sizeBar = new SeekBar(c);
        opacityBar.setMax(90);
        opacityBar.setProgress(Math.round(opacity * 100) - 10); // 10..100 %
        sizeBar.setMax(90);
        sizeBar.setProgress(Math.round(scale * 100) - 60); // 60..150 %
        Runnable labels = () -> {
            opacityLabel.setText("Opacity " + (opacityBar.getProgress() + 10) + " %");
            sizeLabel.setText("Size " + (sizeBar.getProgress() + 60) + " %");
        };
        labels.run();
        SeekBar.OnSeekBarChangeListener live = new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int v, boolean user) {
                labels.run();
                opacity = (opacityBar.getProgress() + 10) / 100f;
                float s = (sizeBar.getProgress() + 60) / 100f;
                if (s != scale) {
                    scale = s;
                    relayout();
                }
                prefs(getContext()).edit().putFloat("opacity", opacity).putFloat("scale", scale).apply();
                invalidate();
            }

            @Override
            public void onStartTrackingTouch(SeekBar bar) {}

            @Override
            public void onStopTrackingTouch(SeekBar bar) {}
        };
        opacityBar.setOnSeekBarChangeListener(live);
        sizeBar.setOnSeekBarChangeListener(live);
        CheckBox vibrate = new CheckBox(c);
        vibrate.setText("Vibrate on press");
        vibrate.setChecked(haptics);
        vibrate.setOnCheckedChangeListener((v, on) -> {
            haptics = on;
            prefs(getContext()).edit().putBoolean("haptics", on).apply();
        });
        box.addView(modes);
        box.addView(help);
        box.addView(opacityLabel);
        box.addView(opacityBar);
        box.addView(sizeLabel);
        box.addView(sizeBar);
        box.addView(vibrate);
        // the stats overlay's switches (StatsOverlay), applied at once
        View rule = new View(c);
        rule.setBackgroundColor(0x40000000 | Palette.INK);
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, pad / 16));
        rl.topMargin = rl.bottomMargin = pad / 2;
        box.addView(rule, rl);
        StatsOverlay.addSettings(c, box, () -> {
            if (stats != null)
                stats.refresh();
        });
        ScrollView scroll = new ScrollView(c); // the dialog is taller than a phone held landscape
        scroll.addView(box);
        AlertDialog dialog = new AlertDialog.Builder(c, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("On-screen controls")
                .setView(scroll)
                .setPositiveButton("Close", null)
                .setNeutralButton("Edit layout", (d, w) -> startEditLayout())
                .create();
        modes.setOnCheckedChangeListener((g, id) -> {
            mode = id - 4000;
            setMode(getContext(), mode);
            Log.i("vcs", "[touch] on-screen controls: " + MODE_NAMES[mode]);
            releaseAll();
        });
        dialog.setOnDismissListener(d -> {
            if (panel != null)
                panel.prefsChanged(); // its Controls tab shows the same settings
        });
        dialog.show();
        Dialogs.fit(dialog);
    }

    // ---- the layout editor (docs/ANDROID.md "Layout editor"): Button mode moves one control, Group mode its group (the
    // stick and L3, the D-pad, the face buttons, R3, each side's shoulders, the right stick); drag to move, pinch (or the
    // Size slider) to resize, the Opacity slider per control; Vehicle shows the in-vehicle layout to edit its pedals,
    // HUD where the game's radar and money are; Reset the selection, Reset all this screen's layout; Done or Back ----

    boolean isEditing() {
        return editing;
    }

    void startEditLayout() {
        if (prof == null)
            return;
        releaseAll();
        editing = true;
        editVehicle = false;
        selName = null;
        selGroup = -1;
        dragId = pinchId = sliderId = -1;
        resetAllArmed = 0;
        Log.i("vcs", "[touch] layout editor open (screen " + prof + ")");
        relayout();
        placeEditorUi();
        send(); // the pad is let go while editing
        invalidate();
    }

    void endEditLayout() {
        if (!editing)
            return;
        editing = false;
        editVehicle = false;
        dragId = pinchId = sliderId = -1;
        saveLayout();
        relayout();
        Log.i("vcs", "[touch] layout kept for screen " + prof);
        send();
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int rr, int b) {
        super.onLayout(changed, l, t, rr, b);
        if (editing && changed)
            placeEditorUi();
    }

    private boolean chipShown(int k) {
        return k != CH_VEHICLE || contextLayouts;
    }

    /** The toolbar where it covers no control: at the top, else the middle, else the bottom, in the widest free span
     *  between the left and the right controls at that height, in two rows if one is too tight. Placed when the editor
     *  opens (and on a size change), not while dragging, so it doesn't jump. */
    private void placeEditorUi() {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0)
            return;
        float u = unit(), m = 10 * u, ch = 34 * u, gap = 6 * u, minW = 52 * u, maxW = 84 * u;
        int k = 0;
        for (int c = 0; c < CHIP.length; c++)
            if (chipShown(c))
                k++;
        float[] tops = {m, h / 2f - ch, h - m - 2 * ch - gap};
        if (portraitLayout) {
            // upright: under the picture first, in the first band down the screen with no control in it (the
            // picture stays visible), then the usual places
            java.util.ArrayList<Float> list = new java.util.ArrayList<>();
            for (float t = frame.bottom + m; t + ch <= h - m; t += 8 * u)
                list.add(t);
            list.add(m);
            tops = new float[list.size()];
            for (int i = 0; i < tops.length; i++)
                tops[i] = list.get(i);
        }
        float bestL = 0, bestR = w, bestT = m;
        int rows = 2;
        boolean found = false;
        for (int place = 0; place < tops.length * 2 && !found; place++) {
            int rowsHere = place % 2 + 1; // each place in one row, else two
            float t = tops[place / 2];
            if (rowsHere == 2 && !portraitLayout && t == tops[1])
                t -= (ch + gap) / 2;
            {
                float bandB = t + rowsHere * ch + (rowsHere - 1) * gap, left = 0, right = w;
                for (int i = 0; i < n; i++) {
                    if (cy[i] + extY(i) < t || cy[i] - extY(i) > bandB)
                        continue;
                    if (cx[i] < w / 2f)
                        left = Math.max(left, cx[i] + extX(i));
                    else
                        right = Math.min(right, cx[i] - extX(i));
                }
                int perRow = (int) Math.ceil(k / (float) rowsHere);
                if (right - left - 2 * m >= perRow * minW + (perRow - 1) * gap) {
                    bestL = left + m;
                    bestR = right - m;
                    bestT = t;
                    rows = rowsHere;
                    found = true;
                    break;
                }
            }
        }
        if (!found) { // nowhere free: two rows at the top, over whatever is there
            bestL = m;
            bestR = w - m;
            bestT = m;
        }
        int perRow = (int) Math.ceil(k / (float) rows);
        float bw = Math.min(maxW, (bestR - bestL - (perRow - 1) * gap) / perRow);
        int idx = 0;
        for (int c = 0; c < CHIP.length; c++) {
            if (!chipShown(c)) {
                chip[c].setEmpty();
                continue;
            }
            int row = idx / perRow, col = idx % perRow;
            int inRow = Math.min(perRow, k - row * perRow);
            float x0 = (bestL + bestR) / 2 - (inRow * bw + (inRow - 1) * gap) / 2 + col * (bw + gap);
            float y0 = bestT + row * (ch + gap);
            chip[c].set(x0, y0, x0 + bw, y0 + ch);
            idx++;
        }
        toolbar.set(bestL, bestT, bestR, bestT + rows * ch + (rows - 1) * gap);
    }

    /** The selected controls (one in Button mode, a group's in Group mode); none: empty */
    private int[] selection() {
        int[] out = new int[n];
        int k = 0;
        for (int i = 0; i < n; i++)
            if (editMode == EDIT_BUTTON ? name[i].equals(selName) : group[i] == selGroup)
                out[k++] = i;
        return Arrays.copyOf(out, k);
    }

    private boolean hasSelection() {
        return selection().length > 0 || selHud >= 0;
    }

    private RectF selectionBox() {
        if (selHud >= 0)
            return selHud == 0 ? hudRadar : hudMoney;
        int[] sel = selection();
        if (sel.length == 0)
            return null;
        RectF b = null;
        for (int i : sel) {
            RectF one = new RectF(cx[i] - extX(i), cy[i] - extY(i), cx[i] + extX(i), cy[i] + extY(i));
            if (b == null)
                b = one;
            else
                b.union(one);
        }
        return b;
    }

    private float[] entry(String nm) {
        float[] b = btn.get(nm);
        if (b == null)
            btn.put(nm, b = new float[] {0, 0, 1, 1});
        return b;
    }

    private float selScale() {
        if (selHud >= 0)
            return hudPart(selHud)[2];
        if (editMode == EDIT_GROUP)
            return selGroup >= 0 ? gsc[selGroup] : 1;
        float[] b = btn.get(selName);
        return b != null ? b[2] : 1;
    }

    private void setSelScale(float s) {
        s = clampScale(s);
        if (selHud >= 0)
            entry(HUD_PART[selHud])[2] = s;
        else if (editMode == EDIT_GROUP && selGroup >= 0)
            gsc[selGroup] = s;
        else if (selName != null)
            entry(selName)[2] = s;
    }

    /** The selection's opacity as shown (the overall opacity times its own factor) */
    private float selOpacity() {
        int[] sel = selection();
        return sel.length == 0 ? opacity : Math.min(1f, opacity * op[sel[0]]);
    }

    private void setSelOpacity(float shown) {
        float f = shown / Math.max(0.05f, opacity);
        for (int i : selection())
            entry(name[i])[3] = f;
    }

    private void moveSel(float du, float dv) {
        if (selHud >= 0) { // the game's HUD: in the picture's 640x448 units
            float u = unit();
            float[] b = entry(HUD_PART[selHud]);
            b[0] += du * u * 640f / Math.max(1f, frame.width());
            b[1] += dv * u * 448f / Math.max(1f, frame.height());
            return;
        }
        if (editMode == EDIT_GROUP && selGroup >= 0) {
            gdx[selGroup] += du;
            gdy[selGroup] += dv;
        } else if (selName != null) {
            float[] b = entry(selName);
            b[0] += du;
            b[1] += dv;
        }
    }

    private void resetSel() {
        if (selHud >= 0) {
            btn.remove(HUD_PART[selHud]);
            return;
        }
        if (editMode == EDIT_GROUP && selGroup >= 0) {
            gdx[selGroup] = gdy[selGroup] = 0;
            gsc[selGroup] = 1;
            for (int i = 0; i < n; i++)
                if (group[i] == selGroup)
                    btn.remove(name[i]);
        } else if (selName != null)
            btn.remove(selName);
    }

    /** The control under a finger in the editor: the nearest within a little more than its touch area */
    private int editHit(float x, float y) {
        int t = hit(x, y);
        if (t >= 0)
            return t;
        float best = 1.5f;
        for (int i = 0; i < n; i++) {
            float q = Math.max(Math.abs(x - cx[i]) / extX(i), Math.abs(y - cy[i]) / extY(i));
            if (q < best) {
                best = q;
                t = i;
            }
        }
        return t;
    }

    /** The selection's card (its name, Size and Opacity sliders): under the toolbar, else in the middle or at the
     *  bottom, wherever it doesn't cover the selection */
    private void placeCard() {
        int w = getWidth(), h = getHeight();
        float u = unit(), cw = Math.min(Math.max(toolbar.width(), 240 * u), Math.min(w - 20 * u, 320 * u)), chh = 100 * u;
        RectF sel = selectionBox();
        float midX = toolbar.isEmpty() ? w / 2f : toolbar.centerX();
        midX = Math.max(cw / 2 + 10 * u, Math.min(w - cw / 2 - 10 * u, midX));
        float[] tops = {toolbar.bottom + 8 * u, toolbar.top - 8 * u - chh, h / 2f - chh / 2, h - 10 * u - chh};
        for (float t : tops) {
            if (t < 0 || t + chh > h)
                continue;
            card.set(midX - cw / 2, t, midX + cw / 2, t + chh);
            if (sel == null || !RectF.intersects(card, sel))
                break;
        }
        float pad = 12 * u, labelW = 104 * u;
        for (int k = 0; k < 2; k++) {
            float y = card.top + 40 * u + k * 30 * u;
            track[k].set(card.left + pad + labelW, y - 12 * u, card.right - pad, y + 12 * u);
        }
    }

    /** A slider set from a finger at x: 0 Size (50..200 %), 1 Opacity (10..100 % as shown) */
    private void setSlider(int k, float x) {
        RectF t = track[k];
        float f = Math.max(0, Math.min(1, (x - t.left) / Math.max(1, t.width())));
        if (k == 0)
            setSelScale(0.5f + f * 1.5f);
        else
            setSelOpacity(0.1f + f * 0.9f);
        relayout();
    }

    private void chipTap(int k) {
        switch (k) {
            case CH_BUTTON:
            case CH_GROUP: {
                int was = editMode;
                editMode = k == CH_BUTTON ? EDIT_BUTTON : EDIT_GROUP;
                if (was != editMode) {
                    if (editMode == EDIT_BUTTON)
                        selName = null; // a group was selected, not one of its controls
                    prefs(getContext()).edit().putInt("edit_mode", editMode).apply();
                }
                break;
            }
            case CH_VEHICLE:
                editVehicle = !editVehicle;
                selName = null;
                selGroup = -1;
                relayout();
                break;
            case CH_HUD:
                showHud = !showHud;
                break;
            case CH_RESET:
                if (hasSelection()) {
                    resetSel();
                    relayout();
                    saveLayout();
                }
                break;
            case CH_RESET_ALL:
                if (System.currentTimeMillis() - resetAllArmed < 3000) {
                    resetAllArmed = 0;
                    resetLayout();
                } else {
                    resetAllArmed = System.currentTimeMillis();
                    postDelayed(this::invalidate, 3100);
                }
                break;
            case CH_DONE:
                endEditLayout();
                return;
        }
        invalidate();
    }

    private boolean editTouch(MotionEvent e) {
        int act = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i), u = unit(), dp = getResources().getDisplayMetrics().density;
        switch (act) {
            case MotionEvent.ACTION_DOWN: {
                dragId = pinchId = sliderId = -1;
                moved = tapEmpty = false;
                for (int k = 0; k < chip.length; k++)
                    if (!chip[k].isEmpty() && chip[k].contains(x, y)) {
                        if (haptics)
                            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                        chipTap(k);
                        return true;
                    }
                if (hasSelection() && card.contains(x, y)) {
                    for (int k = 0; k < 2; k++)
                        if (y >= track[k].top - 4 * u && y <= track[k].bottom + 4 * u && x >= track[k].left - 12 * u) {
                            slider = k;
                            sliderId = id;
                            setSlider(k, x);
                            break;
                        }
                    return true;
                }
                dragId = id;
                downX = lastX = x;
                downY = lastY = y;
                // the game's HUD boxes (HUD on in the toolbar): picked when no control is right under the finger
                int exact = hit(x, y);
                int part = -1;
                if (exact < 0 && showHud)
                    for (int p = 0; p < 2; p++) {
                        RectF z = p == 0 ? hudRadar : hudMoney;
                        if (z != null && z.contains(x, y))
                            part = p;
                    }
                if (part >= 0) {
                    if (part != selHud && haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    selHud = part;
                    selName = null;
                    selGroup = -1;
                    invalidate();
                    return true;
                }
                selHud = -1;
                int t = editHit(x, y);
                if (t >= 0) {
                    boolean again = editMode == EDIT_BUTTON ? name[t].equals(selName) : group[t] == selGroup;
                    selName = name[t];
                    selGroup = group[t];
                    if (!again && haptics)
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                } else
                    tapEmpty = true; // let go without moving: nothing selected (a second finger: a pinch on the selection)
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_POINTER_DOWN:
                if (sliderId < 0 && pinchId < 0 && dragId >= 0 && hasSelection()) {
                    int j = e.findPointerIndex(dragId);
                    if (j >= 0) {
                        pinchId = id;
                        pinchD0 = (float) Math.hypot(x - e.getX(j), y - e.getY(j));
                        pinchS0 = selScale();
                        tapEmpty = false;
                        moved = true; // the drag finger goes on from where it is when the pinch ends
                    }
                }
                return true;
            case MotionEvent.ACTION_MOVE: {
                if (sliderId >= 0) {
                    int j = e.findPointerIndex(sliderId);
                    if (j >= 0)
                        setSlider(slider, e.getX(j));
                    return true;
                }
                int j = e.findPointerIndex(dragId);
                if (dragId < 0 || j < 0)
                    return true;
                float mx = e.getX(j), my = e.getY(j);
                if (pinchId >= 0) {
                    int q = e.findPointerIndex(pinchId);
                    if (q >= 0 && pinchD0 > 0)
                        setSelScale(pinchS0 * (float) Math.hypot(mx - e.getX(q), my - e.getY(q)) / pinchD0);
                } else if (!tapEmpty && hasSelection()) {
                    if (!moved && Math.hypot(mx - downX, my - downY) < 4 * dp)
                        return true; // a tap selects; a drag moves once past a few dp
                    if (!moved) {
                        lastX = downX;
                        lastY = downY;
                    }
                    moved = true;
                    moveSel((mx - lastX) / u, (my - lastY) / u);
                }
                lastX = mx;
                lastY = my;
                relayout();
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP:
                if (id == pinchId)
                    pinchId = -1;
                else if (id == dragId)
                    dragId = pinchId = -1;
                if (id == sliderId)
                    sliderId = -1;
                invalidate();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (act == MotionEvent.ACTION_UP && tapEmpty && dragId == id) {
                    selName = null;
                    selGroup = -1;
                    selHud = -1;
                }
                dragId = pinchId = sliderId = -1;
                saveLayout();
                invalidate();
                return true;
            default:
                return true;
        }
    }

    // ---- drawing: this port's own look ("dusk"): dark violet glass, teal accents on the left, pink on the right, the
    // face buttons as diamond tiles with drawn PS2 symbols, slanted shoulder tabs, a segmented stick ring, a cross D-pad ----


    private float drawOp = 1f; // the control being drawn's own opacity factor

    private int alpha(float a) {
        return Math.max(0, Math.min(255, Math.round(255 * a * Math.min(1f, opacity * drawOp))));
    }

    private int col(float a, int rgb) {
        return (alpha(a) << 24) | (rgb & 0xFFFFFF);
    }

    private int accent(int i) {
        return cx[i] < getWidth() / 2f ? Palette.TEAL : Palette.PINK;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (!visible() && !editing) {
            if (topShown()) // a gamepad hides the controls; a touch still brings the top buttons back
                drawTopButtons(c);
            return;
        }
        float dp = getResources().getDisplayMetrics().density;
        line.setStrokeWidth(Math.max(1.5f, 1.6f * dp));
        line.setStrokeCap(Paint.Cap.ROUND);
        line.setStrokeJoin(Paint.Join.ROUND);
        if (editing) {
            drawControls(c);
            drawEditor(c);
            return;
        }
        if (topShown())
            drawTopButtons(c);
        if (shown)
            drawControls(c);
    }

    /** The show / hide button (three dots in a small tab, struck through while hidden), the settings button (a gear) and
     *  the trainer button (a four-point spark), side by side at the top centre: faint over the game, their touch areas
     *  larger than their drawings */
    private void drawTopButtons(Canvas c) {
        float ta = shown ? 1f : 0.75f, hw = tr * 1.3f, hh = tr * 0.85f;
        boolean ff = fastForwardOn();
        for (int k = 0; k < (panel != null ? 5 : 4); k++) {
            float x = k == 0 ? tx : k == 1 ? gx : k == 2 ? kx : k == 3 ? fx : px, y = ty;
            rect.set(x - hw, y - hh, x + hw, y + hh);
            fill.setColor(Color.argb(Math.round(255 * 0.4f * ta), 0x1A, 0x14, 0x30));
            c.drawRoundRect(rect, hh, hh, fill);
            line.setColor(Color.argb(Math.round(255 * 0.45f * ta), 0xF4, 0xF0, 0xFF));
            c.drawRoundRect(rect, hh, hh, line);
        }
        fill.setColor(Color.argb(Math.round(255 * 0.8f * ta), 0x3F, 0xE0, 0xCF));
        for (int d = -1; d <= 1; d++)
            c.drawCircle(tx + d * tr * 0.55f, ty, tr * 0.17f, fill);
        if (!shown) {
            line.setColor(Color.argb(200, 0xF4, 0xF0, 0xFF));
            c.drawLine(tx - hw * 0.7f, ty + hh * 0.6f, tx + hw * 0.7f, ty - hh * 0.6f, line);
        }
        // the gear: a ring, eight teeth, a hub
        line.setColor(Color.argb(Math.round(255 * 0.85f * ta), 0xFF, 0x5A, 0xAE));
        c.drawCircle(gx, gy, tr * 0.38f, line);
        for (int t = 0; t < 8; t++) {
            double a = t * Math.PI / 4;
            float ca = (float) Math.cos(a), sa = (float) Math.sin(a);
            c.drawLine(gx + ca * tr * 0.42f, gy + sa * tr * 0.42f, gx + ca * tr * 0.62f, gy + sa * tr * 0.62f, line);
        }
        fill.setColor(Color.argb(Math.round(255 * 0.85f * ta), 0xFF, 0x5A, 0xAE));
        c.drawCircle(gx, gy, tr * 0.12f, fill);
        // the trainer: a teal four-point spark (concave sides) and a small ink twinkle beside it
        float so = tr * 0.56f, si = tr * 0.14f, sx = kx - tr * 0.12f, sy = ky + tr * 0.04f;
        path.reset();
        for (int p = 0; p < 8; p++) {
            double a = -Math.PI / 2 + p * Math.PI / 4;
            float rad = (p & 1) == 0 ? so : si;
            float px = sx + (float) Math.cos(a) * rad, py = sy + (float) Math.sin(a) * rad;
            if (p == 0)
                path.moveTo(px, py);
            else
                path.lineTo(px, py);
        }
        path.close();
        fill.setColor(Color.argb(Math.round(255 * 0.85f * ta), 0x3F, 0xE0, 0xCF));
        c.drawPath(path, fill);
        fill.setColor(Color.argb(Math.round(255 * 0.8f * ta), 0xF4, 0xF0, 0xFF));
        c.drawCircle(kx + tr * 0.5f, ky - tr * 0.4f, tr * 0.11f, fill);
        // fast forward: two triangles, teal and filled while on
        for (int t = 0; t < 2; t++) {
            float bx = fx - tr * 0.5f + t * tr * 0.5f;
            path.reset();
            path.moveTo(bx, fy - tr * 0.35f);
            path.lineTo(bx + tr * 0.45f, fy);
            path.lineTo(bx, fy + tr * 0.35f);
            path.close();
            fill.setColor(ff ? Color.argb(Math.round(255 * 0.95f * ta), 0x3F, 0xE0, 0xCF)
                             : Color.argb(Math.round(255 * 0.7f * ta), 0xF4, 0xF0, 0xFF));
            c.drawPath(path, fill);
        }
        if (panel == null)
            return;
        // the side panel: a pink edge on the right with a chevron pointing in, two ink rows beside it
        line.setColor(Color.argb(Math.round(255 * 0.85f * ta), 0xFF, 0x5A, 0xAE));
        c.drawLine(px + tr * 0.55f, py - tr * 0.45f, px + tr * 0.55f, py + tr * 0.45f, line);
        c.drawLine(px + tr * 0.25f, py - tr * 0.25f, px, py, line);
        c.drawLine(px, py, px + tr * 0.25f, py + tr * 0.25f, line);
        line.setColor(Color.argb(Math.round(255 * 0.8f * ta), 0xF4, 0xF0, 0xFF));
        c.drawLine(px - tr * 0.6f, py - tr * 0.22f, px - tr * 0.2f, py - tr * 0.22f, line);
        c.drawLine(px - tr * 0.6f, py + tr * 0.22f, px - tr * 0.2f, py + tr * 0.22f, line);
    }

    private void drawControls(Canvas c) {
        for (int i = 0; i < n; i++) {
            if (menuHides(i))
                continue;
            drawOp = op[i];
            if (kind[i] == STICK)
                drawStick(c, i);
            else if (shape[i] == PEDAL)
                drawPedal(c, i);
            else if (kind[i] == DPAD)
                drawDpad(c, i);
            else if ((bits[i] & (TRIANGLE | CIRCLE | CROSS | SQUARE)) != 0)
                drawFace(c, i);
            else if (shape[i] == PILL)
                drawTab(c, i);
            else
                drawHex(c, i);
        }
        drawOp = 1f;
    }

    /** Triangle shows a car door instead of its glyph: on foot, an enterable vehicle in reach, no menu over the game (the
     *  layout editor keeps the plain glyph so the button stays recognisable) */
    static boolean showDoorIcon(int gameState, boolean editing) {
        return !editing && (gameState & STATE_NEAR_VEHICLE) != 0 && (gameState & (STATE_IN_VEHICLE | STATE_PAUSE_MENU
                | STATE_SETTINGS_MENU | STATE_TRAINER)) == 0;
    }

    /** A car door seen from the side, drawn by us: the door panel, its window and its handle, inside a box of half-size g */
    private void drawDoorIcon(Canvas c, float x, float y, float g) {
        path.reset();
        path.moveTo(x - g * 0.8f, y + g * 0.85f);
        path.lineTo(x - g * 0.8f, y - g * 0.15f);
        path.lineTo(x - g * 0.45f, y - g * 0.85f);
        path.lineTo(x + g * 0.8f, y - g * 0.85f);
        path.lineTo(x + g * 0.8f, y + g * 0.85f);
        path.close();
        c.drawPath(path, line);
        path.reset(); // the window
        path.moveTo(x - g * 0.38f, y - g * 0.12f);
        path.lineTo(x - g * 0.2f, y - g * 0.55f);
        path.lineTo(x + g * 0.45f, y - g * 0.55f);
        path.lineTo(x + g * 0.45f, y - g * 0.12f);
        path.close();
        c.drawPath(path, line);
        c.drawLine(x + g * 0.05f, y + g * 0.25f, x + g * 0.5f, y + g * 0.25f, line); // the handle
    }

    /** The PS2's colours for its face buttons' symbols */
    private static int faceRgb(int b) {
        switch (b) {
            case TRIANGLE: return 0x5ADCAA;
            case CIRCLE: return 0xFF696E;
            case CROSS: return 0x7DAAFF;
            case SQUARE: return 0xF096DC;
            default: return Palette.INK;
        }
    }

    /** A face button: a diamond tile (a rounded square turned 45 degrees) with its symbol drawn, lit in its colour and
     *  haloed while held */
    private void drawFace(Canvas c, int i) {
        boolean held = pressed[i] != 0;
        float x = cx[i], y = cy[i], rr = r[i], s = rr * 0.78f;
        int rgb = faceRgb(bits[i]);
        if (held) {
            fill.setColor(col(0.22f, rgb));
            c.drawCircle(x, y, rr * 1.3f, fill);
        }
        c.save();
        c.rotate(45, x, y);
        rect.set(x - s, y - s, x + s, y + s);
        fill.setColor(held ? col(0.9f, rgb) : col(0.5f, Palette.GLASS));
        c.drawRoundRect(rect, s * 0.32f, s * 0.32f, fill);
        line.setColor(col(held ? 1f : 0.7f, rgb));
        c.drawRoundRect(rect, s * 0.32f, s * 0.32f, line);
        c.restore();
        float g = rr * 0.36f, sw = line.getStrokeWidth();
        line.setStrokeWidth(Math.max(sw, rr * 0.11f));
        line.setColor(held ? col(1f, Palette.GLASS) : col(1f, rgb));
        switch (bits[i]) {
            case TRIANGLE:
                if (showDoorIcon(gameState, editing)) {
                    drawDoorIcon(c, x, y, g);
                    break;
                }
                path.reset();
                path.moveTo(x, y - g);
                path.lineTo(x + g * 0.95f, y + g * 0.7f);
                path.lineTo(x - g * 0.95f, y + g * 0.7f);
                path.close();
                c.drawPath(path, line);
                break;
            case CIRCLE:
                c.drawCircle(x, y, g * 0.85f, line);
                break;
            case CROSS:
                c.drawLine(x - g * 0.8f, y - g * 0.8f, x + g * 0.8f, y + g * 0.8f, line);
                c.drawLine(x - g * 0.8f, y + g * 0.8f, x + g * 0.8f, y - g * 0.8f, line);
                break;
            default:
                rect.set(x - g * 0.75f, y - g * 0.75f, x + g * 0.75f, y + g * 0.75f);
                c.drawRect(rect, line);
                break;
        }
        line.setStrokeWidth(sw);
    }

    /** A shoulder / Select / Start button: a tab slanted outwards, an accent bar along its bottom edge */
    private void drawTab(Canvas c, int i) {
        boolean held = pressed[i] != 0;
        float x = cx[i], y = cy[i], rr = r[i], h = rr * 0.6f, sl = rr * 0.32f;
        float dir = cx[i] < getWidth() / 2f ? 1 : -1; // the slant leans away from the middle
        int ac = accent(i);
        path.reset();
        path.moveTo(x - rr + (dir > 0 ? sl : 0), y - h);
        path.lineTo(x + rr - (dir < 0 ? sl : 0), y - h);
        path.lineTo(x + rr - (dir > 0 ? sl : 0), y + h);
        path.lineTo(x - rr + (dir < 0 ? sl : 0), y + h);
        path.close();
        fill.setColor(held ? col(0.85f, ac) : col(0.5f, Palette.GLASS));
        c.drawPath(path, fill);
        line.setColor(col(0.45f, Palette.INK));
        c.drawPath(path, line);
        float sw = line.getStrokeWidth();
        line.setStrokeWidth(sw * 2.2f);
        line.setColor(col(held ? 1f : 0.9f, held ? Palette.INK : ac));
        float bx0 = x - rr + (dir < 0 ? sl : 0) + rr * 0.25f, bx1 = x + rr - (dir > 0 ? sl : 0) - rr * 0.25f;
        c.drawLine(bx0, y + h - sw * 1.6f, bx1, y + h - sw * 1.6f, line);
        line.setStrokeWidth(sw);
        int len = label[i].length();
        text.setTextSize(rr * (len > 4 ? 0.3f : 0.62f));
        text.setLetterSpacing(len > 4 ? 0.08f : 0.04f);
        text.setColor(held ? col(1f, Palette.GLASS) : col(0.95f, Palette.INK));
        c.drawText(label[i], x, y - h * 0.12f - (text.descent() + text.ascent()) / 2, text);
        text.setLetterSpacing(0);
    }

    /** L3 / R3 (and the accelerator lock): a small hexagon */
    private void drawHex(Canvas c, int i) {
        boolean held = pressed[i] != 0;
        float x = cx[i], y = cy[i], rr = r[i];
        int ac = accent(i);
        path.reset();
        for (int k = 0; k < 6; k++) {
            double a = Math.PI / 6 + k * Math.PI / 3;
            float px = x + (float) Math.cos(a) * rr, py = y + (float) Math.sin(a) * rr;
            if (k == 0)
                path.moveTo(px, py);
            else
                path.lineTo(px, py);
        }
        path.close();
        fill.setColor(held ? col(0.85f, ac) : col(0.5f, Palette.GLASS));
        c.drawPath(path, fill);
        line.setColor(col(0.8f, ac));
        c.drawPath(path, line);
        text.setTextSize(rr * (label[i].length() > 2 ? 0.42f : 0.7f));
        text.setColor(held ? col(1f, Palette.GLASS) : col(1f, ac));
        c.drawText(label[i], x, y - (text.descent() + text.ascent()) / 2, text);
    }

    /** Gas / Brake in a vehicle: a tall tile with ridges, its label, lit while held (Gas also while the lock holds it) */
    private void drawPedal(Canvas c, int i) {
        boolean held = pressed[i] != 0;
        float x = cx[i], y = cy[i], hw = r[i] * 0.72f, hh = r[i] * 1.08f;
        int ac = bits[i] == CROSS ? Palette.TEAL : Palette.PINK;
        rect.set(x - hw, y - hh, x + hw, y + hh);
        fill.setColor(held ? col(0.85f, ac) : col(0.5f, Palette.GLASS));
        c.drawRoundRect(rect, hw * 0.35f, hw * 0.35f, fill);
        line.setColor(col(held ? 1f : 0.75f, ac));
        c.drawRoundRect(rect, hw * 0.35f, hw * 0.35f, line);
        line.setColor(held ? col(0.6f, Palette.GLASS) : col(0.35f, Palette.INK));
        for (int k = -2; k <= 2; k++)
            c.drawLine(x - hw * 0.55f, y + hh * 0.18f * k + hh * 0.25f, x + hw * 0.55f, y + hh * 0.18f * k + hh * 0.25f, line);
        text.setTextSize(r[i] * 0.34f);
        text.setLetterSpacing(0.08f);
        text.setColor(held ? col(1f, Palette.GLASS) : col(0.95f, Palette.INK));
        c.drawText(label[i], x, y - hh * 0.55f - (text.descent() + text.ascent()) / 2, text);
        text.setLetterSpacing(0);
    }

    /** The stick: a ring of twelve segments around a faint well, the knob tethered to the centre */
    private void drawStick(Canvas c, int i) {
        int sk = i == stickIndex[1] ? 1 : 0;
        boolean held = stickSlot[sk] >= 0;
        float x = held ? stickCx[sk] : cx[i], y = held ? stickCy[sk] : cy[i], rr = r[i];
        float stickX = this.stickX[sk], stickY = this.stickY[sk];
        int ac = accent(i);
        fill.setColor(col(0.3f, Palette.GLASS));
        c.drawCircle(x, y, rr, fill);
        float sw = line.getStrokeWidth();
        line.setStrokeWidth(sw * 1.8f);
        line.setColor(col(held ? 0.95f : 0.65f, ac));
        rect.set(x - rr, y - rr, x + rr, y + rr);
        for (int k = 0; k < 12; k++)
            c.drawArc(rect, k * 30 + 5, 20, false, line);
        line.setStrokeWidth(sw);
        line.setColor(col(0.25f, Palette.INK));
        c.drawCircle(x, y, rr * 0.55f, line);
        float k = rr * 0.55f, kx = x + stickX * k, ky = y + stickY * k;
        if (held) {
            line.setColor(col(0.6f, ac));
            c.drawLine(x, y, kx, ky, line);
        }
        fill.setColor(col(held ? 0.9f : 0.6f, ac));
        c.drawCircle(kx, ky, rr * 0.4f, fill);
        line.setColor(col(0.8f, Palette.GLASS));
        c.drawCircle(kx, ky, rr * 0.24f, line);
    }

    /** The D-pad: a cross of four rounded arms, each lit while held, a chevron in each */
    private void drawDpad(Canvas c, int i) {
        float x = cx[i], y = cy[i], rr = r[i], a = rr * 0.32f, len = rr * 0.95f;
        int ac = accent(i);
        final int[] dirs = {UP, RIGHT, DOWN, LEFT};
        // the cross's outline (both bars as one shape), the hub, then each arm turned a quarter per direction
        path.reset();
        path.addRoundRect(x - a, y - len, x + a, y + len, a * 0.45f, a * 0.45f, Path.Direction.CW);
        dpadBar.reset();
        dpadBar.addRoundRect(x - len, y - a, x + len, y + a, a * 0.45f, a * 0.45f, Path.Direction.CW);
        path.op(dpadBar, Path.Op.UNION);
        rect.set(x - a, y - a, x + a, y + a);
        fill.setColor(col(0.5f, Palette.GLASS));
        c.drawRect(rect, fill);
        for (int k = 0; k < 4; k++) {
            boolean held = (pressed[i] & dirs[k]) != 0;
            c.save();
            c.rotate(90 * k, x, y);
            c.clipRect(x - a, y - len, x + a, y - a);
            rect.set(x - a, y - len, x + a, y + a); // square at the hub end: the clip cuts it there
            fill.setColor(held ? col(0.9f, ac) : col(0.5f, Palette.GLASS));
            c.drawRoundRect(rect, a * 0.45f, a * 0.45f, fill);
            line.setColor(held ? col(1f, Palette.GLASS) : col(0.9f, Palette.INK));
            float cy0 = y - len * 0.62f, w = a * 0.45f;
            c.drawLine(x - w, cy0 + w * 0.5f, x, cy0 - w * 0.5f, line);
            c.drawLine(x, cy0 - w * 0.5f, x + w, cy0 + w * 0.5f, line);
            c.restore();
        }
        line.setColor(col(0.7f, ac));
        c.drawPath(path, line);
    }


    private static String displayName(String nm) {
        switch (nm) {
            case "LSTICK": return "Left stick";
            case "RSTICK": return "Right stick";
            case "DPAD": return "D-pad";
            case "TRIANGLE": return "Triangle";
            case "CIRCLE": return "Circle";
            case "CROSS": return "Cross";
            case "SQUARE": return "Square";
            case "GAS": return "Gas";
            case "BRAKE": return "Brake";
            case "LOCK": return "Accelerator lock";
            case "SELECT": return "Select";
            case "START": return "Start";
            default: return nm;
        }
    }

    /** Text at (x, baseline y), shrunk to fit maxW */
    private void fitText(Canvas c, String s, float x, float y, float maxW, float size) {
        text.setTextSize(size);
        float tw = text.measureText(s);
        if (tw > maxW && tw > 0)
            text.setTextSize(size * maxW / tw);
        c.drawText(s, x, y, text);
    }

    private void drawEditor(Canvas c) {
        int w = getWidth(), h = getHeight();
        float u = unit();
        // the picture's edges (where the letterbox starts) and the game's HUD, approximately
        line.setColor(Color.argb(110, 255, 255, 255));
        if (frame.top > 1 || frame.bottom < h - 1 || frame.left > 1 || frame.right < w - 1)
            c.drawRect(frame, line);
        if (showHud)
            for (RectF z : new RectF[] {hudRadar, hudMoney}) {
                if (z == null)
                    continue;
                boolean picked = (z == hudRadar ? 0 : 1) == selHud;
                fill.setColor(picked ? Color.argb(70, 0x3F, 0xE0, 0xCF) : Color.argb(70, 0xFF, 0x9A, 0x3C));
                c.drawRoundRect(z, 6 * u, 6 * u, fill);
                line.setColor(picked ? 0xFF000000 | Palette.TEAL : (200 << 24) | Palette.EDIT);
                c.drawRoundRect(z, 6 * u, 6 * u, line);
                text.setColor(Color.argb(230, 0xFF, 0xC8, 0x96));
                fitText(c, z == hudRadar ? "HUD: radar" : "HUD: money, health", z.centerX(), z.centerY() + 5 * u, z.width() - 4 * u, 12 * u);
            }
        // what can be picked: each control (Button mode) or each group (Group mode), the selection in yellow
        int[] sel = selection();
        if (editMode == EDIT_GROUP) {
            for (int g = 0; g < GROUPS; g++) {
                if (gbox[g] == null)
                    continue;
                rect.set(gbox[g]);
                rect.inset(-4 * u, -4 * u);
                if (g == selGroup && sel.length > 0) {
                    fill.setColor(Color.argb(60, 0x3F, 0xE0, 0xCF));
                    c.drawRoundRect(rect, 8 * u, 8 * u, fill);
                    line.setColor(0xFF000000 | Palette.TEAL);
                } else
                    line.setColor(Color.argb(150, 255, 255, 255));
                c.drawRoundRect(rect, 8 * u, 8 * u, line);
            }
        } else
            for (int i = 0; i < n; i++) {
                boolean on = name[i].equals(selName);
                rect.set(cx[i] - extX(i) - 3 * u, cy[i] - extY(i) - 3 * u, cx[i] + extX(i) + 3 * u, cy[i] + extY(i) + 3 * u);
                if (on) {
                    fill.setColor(Color.argb(60, 0x3F, 0xE0, 0xCF));
                    c.drawRoundRect(rect, 8 * u, 8 * u, fill);
                }
                line.setColor(on ? 0xFF000000 | Palette.TEAL : Color.argb(110, 255, 255, 255));
                c.drawRoundRect(rect, 8 * u, 8 * u, line);
            }
        // the toolbar
        chipOn[CH_BUTTON] = editMode == EDIT_BUTTON;
        chipOn[CH_GROUP] = editMode == EDIT_GROUP;
        chipOn[CH_VEHICLE] = editVehicle;
        chipOn[CH_HUD] = showHud && (hudRadar != null || hudMoney != null);
        chipOn[CH_RESET] = false;
        chipOn[CH_RESET_ALL] = false;
        boolean armed = System.currentTimeMillis() - resetAllArmed < 3000;
        for (int k = 0; k < chip.length; k++) {
            if (chip[k].isEmpty())
                continue;
            boolean done = k == CH_DONE, dim = (k == CH_RESET && sel.length == 0) || (k == CH_HUD && hudRadar == null && hudMoney == null);
            int bg = done ? Color.argb(235, 0xFF, 0x5A, 0xAE) : k == CH_RESET_ALL && armed ? Color.argb(235, 0xFF, 0x5A, 0xAE)
                    : chipOn[k] ? Color.argb(230, 0x3F, 0xE0, 0xCF) : Color.argb(210, 0x1A, 0x14, 0x30);
            fill.setColor(bg);
            c.drawRoundRect(chip[k], 8 * u, 8 * u, fill);
            line.setColor(Color.argb(dim ? 90 : 220, 255, 255, 255));
            c.drawRoundRect(chip[k], 8 * u, 8 * u, line);
            text.setColor(done || chipOn[k] || (k == CH_RESET_ALL && armed) ? Color.BLACK : Color.argb(dim ? 110 : 255, 255, 255, 255));
            String s = k == CH_RESET_ALL && armed ? "Sure?" : CHIP[k];
            fitText(c, s, chip[k].centerX(), chip[k].centerY() + 5 * u, chip[k].width() - 8 * u, 14 * u);
        }
        // the hint, or the selection's card with its sliders
        text.setColor(Color.argb(235, 255, 255, 255));
        if (sel.length == 0 && selHud < 0) {
            String hint = editMode == EDIT_BUTTON ? "Tap a button, drag to move it, pinch to resize"
                                                  : "Tap a group, drag to move it, pinch to resize";
            if (showHud && (hudRadar != null || hudMoney != null))
                hint += "; the orange HUD boxes too";
            String hud = showHud && hudMode >= 0 ? "Orange: the game's HUD (Screen > HUD: " + (hudMode == 2 ? "Screen edges" : hudMode == 1 ? "Stretch" : "Keep 4:3") + ")" : null;
            float y = toolbar.bottom + 20 * u;
            if (y + (hud != null ? 18 * u : 0) > h - 4 * u)
                y = toolbar.top - (hud != null ? 24 * u : 8 * u);
            drawShadowed(c, hint, toolbar.centerX(), y, Math.max(toolbar.width(), 200 * u), 13 * u);
            if (hud != null)
                drawShadowed(c, hud, toolbar.centerX(), y + 18 * u, Math.max(toolbar.width(), 200 * u), 12 * u);
            return;
        }
        placeCard();
        fill.setColor(Color.argb(225, 0x1A, 0x14, 0x30));
        c.drawRoundRect(card, 10 * u, 10 * u, fill);
        line.setColor(0xC0000000 | Palette.TEAL);
        c.drawRoundRect(card, 10 * u, 10 * u, line);
        String title = (selHud == 0 ? "HUD: radar" : selHud == 1 ? "HUD: money, health" : editMode == EDIT_GROUP ? GROUP_NAME[selGroup]
                : displayName(selName)) + " · drag to move, pinch to resize";
        text.setColor(0xFF000000 | Palette.TEAL);
        fitText(c, title, card.centerX(), card.top + 20 * u, card.width() - 20 * u, 13 * u);
        float[] value = {selScale(), selOpacity()};
        String[] lab = {"Size " + Math.round(value[0] * 100) + " %", "Opacity " + Math.round(value[1] * 100) + " %"};
        float[] frac = {(value[0] - 0.5f) / 1.5f, (value[1] - 0.1f) / 0.9f};
        for (int k = 0; k < 2; k++) {
            RectF t = track[k];
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(Color.argb(235, 255, 255, 255));
            fitText(c, lab[k], card.left + 12 * u, t.centerY() + 5 * u, t.left - card.left - 16 * u, 13 * u);
            text.setTextAlign(Paint.Align.CENTER);
            float f = Math.max(0, Math.min(1, frac[k])), kx0 = t.left + f * t.width();
            float sw = line.getStrokeWidth();
            line.setStrokeWidth(4 * u);
            line.setColor(Color.argb(120, 255, 255, 255));
            c.drawLine(t.left, t.centerY(), t.right, t.centerY(), line);
            line.setColor(Color.argb(255, 0x3F, 0xE0, 0xCF));
            c.drawLine(t.left, t.centerY(), kx0, t.centerY(), line);
            line.setStrokeWidth(sw);
            fill.setColor(k == slider && sliderId >= 0 ? 0xFF000000 | Palette.TEAL : Color.WHITE);
            c.drawCircle(kx0, t.centerY(), 9 * u, fill);
        }
    }

    private void drawShadowed(Canvas c, String s, float x, float y, float maxW, float size) {
        int was = text.getColor();
        text.setColor(Color.argb(200, 0, 0, 0));
        fitText(c, s, x + 1.5f, y + 1.5f, maxW, size);
        text.setColor(was);
        fitText(c, s, x, y, maxW, size);
    }
}
