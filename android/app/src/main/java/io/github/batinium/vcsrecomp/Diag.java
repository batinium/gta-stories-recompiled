// [android] The phone's side of the game's heartbeat (docs/ANDROID.md "Diagnostics"): every 5 s the game's process hands
// native code the thermal status, the battery, the free memory and whether the app is in the foreground; the native
// heartbeat thread (src/android/diagnostics.cpp) writes them into game.log every 10 s next to its frame numbers. The
// same readings, at the time of a report, are in CrashReport.
package io.github.batinium.vcsrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.StatFs;
import android.util.Log;

import java.util.Locale;

final class Diag {
    private Diag() {
    }

    // diagnostics.cpp (vcs_platform, in libmain.so; not in the smoke test's library)
    private static native void nativePush(int thermal, int battPct, int battTenthC, int memFreeMb, boolean foreground);

    private static final long EVERY_MS = 5000;
    private static Handler handler;
    private static Runnable tick;
    private static volatile boolean foreground = true;
    private static boolean linkFailed;

    /** VCSActivity.onCreate (the game only): the readings go native every 5 s until stop() */
    static void start(Context c) {
        if (tick != null)
            return;
        Context app = c.getApplicationContext();
        handler = new Handler(Looper.getMainLooper());
        tick = new Runnable() {
            @Override
            public void run() {
                push(app);
                if (!linkFailed)
                    handler.postDelayed(this, EVERY_MS);
            }
        };
        handler.post(tick);
    }

    static void stop() {
        if (handler != null && tick != null)
            handler.removeCallbacks(tick);
        tick = null;
    }

    /** onResume / onPause: the watchdog only watches while the app is in front */
    static void setForeground(Context c, boolean fg) {
        foreground = fg;
        if (tick != null)
            push(c.getApplicationContext());
    }

    private static void push(Context c) {
        int[] b = battery(c);
        try {
            nativePush(thermalStatus(c), b[0], b[1], memFreeMb(c), foreground);
        } catch (UnsatisfiedLinkError e) {
            linkFailed = true; // no native diagnostics in this library: stop asking
            Log.w("vcs", "[diag] nativePush: " + e);
        }
    }

    /** PowerManager's thermal status 0 (none) .. 6 (shutdown); -1 when the phone (below Android 10) cannot say */
    static int thermalStatus(Context c) {
        if (android.os.Build.VERSION.SDK_INT < 29)
            return -1;
        try {
            PowerManager pm = c.getSystemService(PowerManager.class);
            return pm != null ? pm.getCurrentThermalStatus() : -1;
        } catch (RuntimeException e) {
            return -1;
        }
    }

    static String thermalName(int s) {
        String[] names = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
        return s >= 0 && s < names.length ? names[s] : "?";
    }

    /** {percent or -1, temperature in tenths of a degree C or -1000} from the sticky battery broadcast */
    static int[] battery(Context c) {
        int[] r = {-1, -1000};
        try {
            Intent b = c.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b == null)
                return r;
            int l = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), s = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (l >= 0 && s > 0)
                r[0] = l * 100 / s;
            r[1] = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1000);
        } catch (RuntimeException ignored) {
        }
        return r;
    }

    /** The phone's available memory in MB, -1 if unknown */
    static int memFreeMb(Context c) {
        try {
            ActivityManager am = c.getSystemService(ActivityManager.class);
            if (am == null)
                return -1;
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            return (int) (mi.availMem >> 20);
        } catch (RuntimeException e) {
            return -1;
        }
    }

    /** Free space of the app's storage, "12.3 GB of 110.0 GB" (for the report) */
    static String storageFree(Context c) {
        try {
            StatFs fs = new StatFs(c.getFilesDir().getAbsolutePath());
            return String.format(Locale.ROOT, "%.1f GB free of %.1f GB", fs.getAvailableBytes() / 1e9, fs.getTotalBytes() / 1e9);
        } catch (RuntimeException e) {
            return "unknown";
        }
    }
}
