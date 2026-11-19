// [android] Rumble (docs/ANDROID.md "Controls extras"): the game's vibration (the pad's motors as the game drives them,
// game/vcs_input.cpp -> platform::gamepadRumble, scaled by Gamepad > Vibration) on the phone's own vibrator while no real
// gamepad is connected (SDL rumbles that one). Polled 30 times a second from native code (Rumble.nativeLevel, in
// touch_controls.cpp); the stronger motor is the amplitude, renewed while it lasts, cancelled at 0.
//
// Settings (SharedPreferences "touch_controls"): rumble (on by default), rumble_strength (percent, 100).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

final class Rumble {
    private static native int nativeLevel(); // low << 16 | high, 0..65535 each

    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Vibrator vibrator;
    private final boolean amplitude;
    private boolean on, resumed, running, paused;
    private float strength = 1f;
    private int sentAmp;
    private long sentMs;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running)
                return;
            poll();
            handler.postDelayed(this, 33);
        }
    };

    Rumble(Context c) {
        context = c;
        VibratorManager vm = c.getSystemService(VibratorManager.class);
        vibrator = vm != null ? vm.getDefaultVibrator() : null;
        amplitude = vibrator != null && vibrator.hasAmplitudeControl();
        loadPrefs();
    }

    static boolean available(Context c) {
        VibratorManager vm = c.getSystemService(VibratorManager.class);
        return vm != null && vm.getDefaultVibrator().hasVibrator();
    }

    void loadPrefs() {
        SharedPreferences p = TouchControls.prefs(context);
        on = p.getBoolean("rumble", true);
        strength = Math.max(0, Math.min(100, p.getInt("rumble_strength", 100))) / 100f;
        update();
    }

    void setResumed(boolean r) {
        resumed = r;
        update();
    }

    /** The game is paused by the side panel: no vibration meanwhile */
    void setPaused(boolean p) {
        paused = p;
    }

    private void update() {
        boolean run = on && resumed && vibrator != null && vibrator.hasVibrator();
        if (run == running)
            return;
        running = run;
        if (run)
            handler.post(tick);
        else {
            handler.removeCallbacks(tick);
            stop();
        }
        Log.i("vcs", "[rumble] phone vibration " + (run ? "on" : "off") + (vibrator != null && !amplitude ? " (no amplitude control)" : ""));
    }

    private void stop() {
        if (sentAmp != 0 && vibrator != null)
            vibrator.cancel();
        sentAmp = 0;
    }

    private void poll() {
        int level = nativeLevel();
        float v = Math.max(level >>> 16, level & 0xFFFF) / 65535f * strength;
        if (paused || TouchControls.nativeRealGamepad()) // a real pad rumbles itself (platform_sdl3.cpp)
            v = 0;
        int amp = v < 0.04f ? 0 : amplitude ? Math.max(1, Math.min(255, Math.round(v * 255))) : 255;
        long now = android.os.SystemClock.uptimeMillis();
        if (amp == 0) {
            stop();
            return;
        }
        // a new level, or the last one about to run out: a fresh half second
        if (Math.abs(amp - sentAmp) > 12 || now - sentMs > 400) {
            vibrator.vibrate(VibrationEffect.createOneShot(500, amp));
            sentAmp = amp;
            sentMs = now;
        }
    }
}
