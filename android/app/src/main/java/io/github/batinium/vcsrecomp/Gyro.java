// [android] Gyro aiming (docs/ANDROID.md "Controls extras"): the gyroscope turned into mouse-camera counts, the same
// path as the touch overlay's camera drag (TouchControls.nativeLook -> takeMouseMotion), so the mouse camera, the scopes
// and free aim all follow the phone, with Keyboard > Mouse sensitivity / Invert applied on top. A rate sensor: the angle
// turned since the last reading is the camera's turn, 1:1 at 100 % (400 counts a radian at 100 % Mouse sensitivity,
// docs/CONTROLS.md); nothing to recentre or drift.
//
// Settings (SharedPreferences "touch_controls", the side panel's Controls tab): gyro (0 off, 1 while aiming, 2 always),
// gyro_sens (percent), gyro_axis (0: turn the phone, 1: lean it like a wheel), gyro_invert_x, gyro_invert_y.
// "While aiming" is the game's free aim or R1 (the lock-on) held, on the overlay or a controller (nativeGameState); on LCS
// also its drive-by look (L2 / R2 with a drive-by weapon, lcs_driveby.h).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.View;

final class Gyro implements SensorEventListener {
    static final int OFF = 0, AIMING = 1, ALWAYS = 2;
    static final String[] MODE_NAMES = {"Off", "While aiming", "Always"};
    static final String[] AXIS_NAMES = {"Turn the phone", "Lean it (wheel)"};
    private static final float COUNTS_PER_RADIAN = 400f;

    private final View host; // for its display's rotation
    private final SensorManager sensors;
    private final Sensor gyro;
    private boolean listening, resumed, blocked;
    private int mode, axis;
    private float sens;
    private boolean invertX, invertY;
    private long lastNs;

    Gyro(View host) {
        this.host = host;
        sensors = host.getContext().getSystemService(SensorManager.class);
        gyro = sensors != null ? sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) : null;
        loadPrefs();
    }

    static boolean available(Context c) {
        SensorManager s = c.getSystemService(SensorManager.class);
        return s != null && s.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null;
    }

    void loadPrefs() {
        SharedPreferences p = TouchControls.prefs(host.getContext());
        mode = Math.max(OFF, Math.min(ALWAYS, p.getInt("gyro", OFF)));
        sens = Math.max(10, Math.min(400, p.getInt("gyro_sens", 100))) / 100f;
        axis = p.getInt("gyro_axis", 0) == 1 ? 1 : 0;
        invertX = p.getBoolean("gyro_invert_x", false);
        invertY = p.getBoolean("gyro_invert_y", false);
        update();
    }

    void setResumed(boolean r) {
        resumed = r;
        update();
    }

    /** The side panel is open: no camera from the phone meanwhile */
    void setBlocked(boolean b) {
        blocked = b;
        lastNs = 0;
    }

    private void update() {
        boolean want = resumed && mode != OFF && gyro != null;
        if (want == listening)
            return;
        listening = want;
        lastNs = 0;
        if (want)
            sensors.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
        else
            sensors.unregisterListener(this);
        Log.i("vcs", "[gyro] " + (want ? "on (" + MODE_NAMES[mode] + ")" : "off") + (gyro == null ? ": no gyroscope" : ""));
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        long t = e.timestamp;
        float dt = lastNs != 0 ? (t - lastNs) * 1e-9f : 0;
        lastNs = t;
        if (dt <= 0 || dt > 0.1f || blocked || !TouchControls.nativeLookWanted())
            return;
        if (mode == AIMING && (TouchControls.nativeGameState() & TouchControls.STATE_AIMING) == 0)
            return;
        // the device's axes (x right, y up, z out of the screen in its natural portrait) to the screen's as held:
        // turning about the screen's up axis is yaw, about its right axis pitch
        Display d = host.getDisplay();
        int rot = d != null ? d.getRotation() : Surface.ROTATION_90;
        float gx = e.values[0], gy = e.values[1], gz = e.values[2];
        float up, right; // rad/s about the screen's up and right axes
        switch (rot) {
            case Surface.ROTATION_90: up = gx; right = -gy; break;
            case Surface.ROTATION_270: up = -gx; right = gy; break;
            case Surface.ROTATION_180: up = -gy; right = -gx; break;
            default: up = gy; right = gx; break;
        }
        float yawRate = axis == 1 ? gz : up; // lean like a wheel: about the axis out of the screen
        // turning left (counter-clockwise from above, positive about up) is mouse left (negative counts); tilting the
        // top edge towards you (positive about right) looks up (negative counts, Invert off)
        float k = COUNTS_PER_RADIAN * sens * dt;
        float dx = -yawRate * k * (invertX ? -1 : 1), dy = -right * k * (invertY ? -1 : 1);
        if (dx != 0 || dy != 0)
            TouchControls.nativeLook(dx, dy);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }
}
