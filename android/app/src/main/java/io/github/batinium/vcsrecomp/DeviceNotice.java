// [android] The untested-phone notice and the start-up error dialog's inputs (the Android side of GpuLog.java).
//
// The launcher has no Vulkan context, so the GPU is learned from the game's own log: scan() reads
// files/logs/game.log (then game.prev.log) and keeps the GPU name and driver version in SharedPreferences "device"
// (the logs are overwritten by the next run; the choice must survive). Before the game has run once the device key is
// the chip ("Qualcomm SM8850", Build.SOC_*). assets/tested_devices.txt lists the tested GPUs (GpuLog.tested).
//
// The notice shows once per distinct device key (GPU name + driver version, or the chip) and never blocks Play.
// When the GPU becomes known after the chip's notice was shown, that notice stands: no second one for the same phone.
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.List;

final class DeviceNotice {
    private DeviceNotice() {
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("device", Context.MODE_PRIVATE);
    }

    static String chip() {
        return (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL).trim();
    }

    /** The first max bytes of a log (the GPU block and a start failure are early in a run) */
    static String head(File f, int max) {
        if (!f.isFile())
            return "";
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            byte[] b = new byte[(int) Math.min(max, in.length())];
            in.readFully(b);
            return new String(b, StandardCharsets.UTF_8);
        } catch (java.io.IOException e) {
            return "";
        }
    }

    /** The log text of the last run that got as far as choosing a GPU: game.log, else game.prev.log */
    static String[] logs(Context c) {
        return new String[] {head(CrashReport.gameLog(c), 256 * 1024), head(CrashReport.previousLog(c), 256 * 1024)};
    }

    /** Keeps the GPU name / driver from the logs (launcher start and resume; cheap) */
    static void scan(Context c) {
        for (String log : logs(c)) {
            String name = GpuLog.gpuName(log);
            if (name == null)
                continue;
            String drv = GpuLog.driverVersion(log);
            SharedPreferences.Editor e = prefs(c).edit().putString("gpu", name);
            if (drv != null)
                e.putString("driver", drv);
            e.putString("api", GpuLog.apiVersion(log));
            e.apply();
            return;
        }
    }

    static String gpuName(Context c) {
        return prefs(c).getString("gpu", null);
    }

    private static List<String> rules(Context c) {
        try (InputStream in = c.getAssets().open("tested_devices.txt")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            for (int n; (n = in.read(buf)) > 0; )
                out.write(buf, 0, n);
            return GpuLog.parseRules(out.toString("UTF-8"));
        } catch (java.io.IOException e) {
            Log.w("vcs", "[device] tested_devices.txt: " + e);
            return new java.util.ArrayList<>();
        }
    }

    /** True when the GPU (or, before a first run, the chip) is on the tested list */
    static boolean tested(Context c) {
        return GpuLog.tested(rules(c), gpuName(c) != null ? gpuName(c) : chip());
    }

    /** What identifies this phone's graphics for the once-only notice */
    static String key(Context c) {
        String gpu = gpuName(c);
        return gpu == null ? "chip:" + chip() : "gpu:" + gpu + "|" + prefs(c).getString("driver", "");
    }

    /** True when the untested notice should be shown now (not tested, and not shown for this key yet) */
    static boolean due(Context c) {
        if (tested(c))
            return false;
        String key = key(c);
        SharedPreferences p = prefs(c);
        String shown = p.getString("notified", null);
        if (key.equals(shown))
            return false;
        // the chip's notice was shown and now the GPU is known: the same phone, no second notice
        if (shown != null && shown.startsWith("chip:") && key.startsWith("gpu:") && shown.equals("chip:" + chip())) {
            p.edit().putString("notified", key).apply();
            return false;
        }
        return true;
    }

    static void markShown(Context c) {
        prefs(c).edit().putString("notified", key(c)).apply();
    }

    static final String MESSAGE = "This is an unofficial port and has only been tested on Snapdragon (Adreno) phones. Yours isn't "
            + "tested yet, so the game may not start, may glitch or may run slowly.\n\nIf something goes wrong, turn on "
            + "Compatibility mode in Settings and send the report (About > Log and crash report). That is how support "
            + "for new phones gets added.";

    /** The start-up failure the last run left, if it was not shown yet (keyed by the log's modification time) */
    static GpuLog.Failure pendingFailure(Context c) {
        File f = CrashReport.gameLog(c);
        if (!f.isFile() || f.lastModified() == prefs(c).getLong("failure_shown", 0))
            return null;
        String log = head(f, 256 * 1024);
        return GpuLog.startFailure(log);
    }

    /** The last run's log: its start and its end (a refused pipeline or a freeze may be anywhere in a long run) */
    static String headAndTail(File f) {
        if (!f.isFile())
            return "";
        long len = f.length();
        if (len <= 512 * 1024)
            return head(f, 512 * 1024);
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            byte[] t = new byte[256 * 1024];
            in.seek(len - t.length);
            in.readFully(t);
            return head(f, 256 * 1024) + "\n" + new String(t, StandardCharsets.UTF_8);
        } catch (java.io.IOException e) {
            return head(f, 256 * 1024);
        }
    }

    /** What went wrong in the last run while it kept going (black picture, freeze), if it was not shown yet */
    static ReportLog.Problem pendingProblem(Context c) {
        File f = CrashReport.gameLog(c);
        if (!f.isFile() || f.lastModified() == prefs(c).getLong("problem_shown", 0))
            return null;
        String log = headAndTail(f);
        if (GpuLog.startFailure(log) != null)
            return null; // a start that failed has its own dialog
        return ReportLog.problem(log, GpuLog.gpuName(log), CompatMode.on(c), GpuDriver.active(c) != null);
    }

    static void problemShown(Context c) {
        prefs(c).edit().putLong("problem_shown", CrashReport.gameLog(c).lastModified()).apply();
    }

    static void failureShown(Context c) {
        prefs(c).edit().putLong("failure_shown", CrashReport.gameLog(c).lastModified()).apply();
    }
}
