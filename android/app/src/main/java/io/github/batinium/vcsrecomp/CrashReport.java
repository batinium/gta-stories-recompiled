// [android] Crash reports a player can send without a PC (About > Game log).
//
// The game's process copies its log (stdout / stderr: runtime, renderer, crash handler) to files/logs/game.log
// (src/android/android_log.cpp; the run before is game.prev.log), and a Java exception in that process is added to it
// here. The launcher reads it: About > Game log says whether the last run ended in a crash, and saves the report to a
// file or shares it. The report is the phone (model, chip, Android), the app's version and build tag, both logs and
// this app's own logcat (Android gives an app only its own lines: the GPU driver's messages for our process are there).
package io.github.batinium.vcsrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class CrashReport {
    private CrashReport() {
    }

    static File dir(Context c) {
        return new File(c.getFilesDir(), "logs");
    }

    static File gameLog(Context c) {
        return new File(dir(c), "game.log");
    }

    static File previousLog(Context c) {
        return new File(dir(c), "game.prev.log");
    }

    /** The game's process (VCSActivity.onCreate): a Java exception there goes into game.log, then on as before */
    static void installInGameProcess(Context context) {
        Context app = context.getApplicationContext();
        Thread.UncaughtExceptionHandler before = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, e) -> {
            try {
                dir(app).mkdirs();
                try (PrintWriter w = new PrintWriter(new FileWriter(gameLog(app), true))) {
                    w.println("[java crash] thread " + thread.getName() + ": " + e);
                    e.printStackTrace(w);
                }
            } catch (Throwable ignored) {
            }
            if (before != null)
                before.uncaughtException(thread, e);
        });
    }

    /** What the last run's log says, for About > Game log's value */
    static String lastRunSummary(Context c) {
        File f = gameLog(c);
        if (!f.isFile())
            return "No game log yet";
        String when = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date(f.lastModified()));
        String tail = tail(f, 64 * 1024);
        if (tail.contains("[crash]") || tail.contains("[java crash]"))
            return "✗ Last run crashed · " + when;
        if (tail.contains("[game-library] refused"))
            return "✗ Library not loaded · " + when;
        if (tail.contains("game exit") || tail.contains("both cities exit"))
            return "Last run ended normally · " + when;
        return "Last run ended without a word · " + when;
    }

    /** The whole report (slow: reads logcat). Off the UI thread. */
    static String build(Context c) {
        StringBuilder r = new StringBuilder(256 * 1024);
        String log = gameLog(c).isFile() ? tail(gameLog(c), 4 * 1024 * 1024) : "";
        String[] exits = processExits(c); // {the section's text, the newest exit's reason}
        r.append("GTA Stories Recompiled crash report\n");
        r.append(summary(c, log, exits[1])).append('\n');
        r.append("App: ").append(BuildConfig.VERSION_NAME).append(" (").append(BuildConfig.VCS_BUILD_TAG).append(")\n");
        r.append("Phone: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE)
                .append(")\n");
        if (Build.VERSION.SDK_INT >= 31)
            r.append("Chip: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        r.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append("), ")
                .append(Build.DISPLAY).append('\n');
        r.append("Memory: ").append(Runtime.getRuntime().maxMemory() >> 20).append(" MB Java heap; game library ")
                .append(GameFiles.gameLibrary(c).isFile() ? GameFiles.gameLibrary(c).length() / 1024 + " KB" : "none")
                .append('\n');
        r.append("Free memory now: ").append(Diag.memFreeMb(c)).append(" MB; storage: ").append(Diag.storageFree(c)).append('\n');
        int[] batt = Diag.battery(c);
        r.append("Thermal at report time: ").append(Diag.thermalName(Diag.thermalStatus(c))).append("; battery ")
                .append(batt[0] >= 0 ? batt[0] + "%" : "?").append(batt[1] > -1000 ? " / " + batt[1] / 10.0 + " C" : "").append('\n');
        r.append("Made: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date()))
                .append("\n");
        r.append(deviceLines(c));
        r.append(GpuDriver.reportLines(c)); // [gpu-driver] the custom driver, if one is selected
        r.append(CompatMode.reportLines(c)).append('\n'); // [compat] the choice and the env.txt lines the game read
        r.append("==== recent process exits (Android's own record of how the game's process ended) ====\n").append(exits[0])
                .append('\n');
        if (!log.isEmpty() && !ReportLog.cleanExit(log)) { // [diag] the run did not end by itself: where it stopped
            r.append("==== where the last run stopped (it did not end cleanly) ====\n");
            String hb = ReportLog.lastHeartbeat(log);
            r.append("last heartbeat: ").append(hb != null ? hb : "(none: it stopped in the first 10 s, or before the game started)")
                    .append("\nlast lines:\n").append(ReportLog.lastLines(log, 5)).append("\n\n");
        }
        section(r, "game.log (last run)", gameLog(c), 1024 * 1024);
        section(r, "game.prev.log (the run before)", previousLog(c), 256 * 1024);
        r.append("==== logcat (this app's own lines) ====\n");
        r.append(logcat(512 * 1024));
        return r.toString();
    }

    /** What we need to judge a phone for support: chip, board, ABI, RAM, screen, Vulkan levels Android reports. No serials, no IDs. */
    static String deviceLines(Context c) {
        StringBuilder d = new StringBuilder("[device] ");
        d.append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE).append("), Android ")
                .append(Build.VERSION.RELEASE).append(" / API ").append(Build.VERSION.SDK_INT).append(", security patch ")
                .append(Build.VERSION.SECURITY_PATCH).append('\n');
        d.append("[device] hardware ").append(Build.HARDWARE).append(", board ").append(Build.BOARD).append(", ABIs ")
                .append(java.util.Arrays.toString(Build.SUPPORTED_ABIS));
        if (Build.VERSION.SDK_INT >= 31)
            d.append(", SoC ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL);
        d.append('\n');
        try {
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            ActivityManager am = c.getSystemService(ActivityManager.class);
            if (am != null)
                am.getMemoryInfo(mi);
            android.util.DisplayMetrics dm = c.getResources().getDisplayMetrics();
            android.view.Display dp = c.getDisplay() != null ? c.getDisplay() : null;
            d.append("[device] RAM ").append(mi.totalMem >> 20).append(" MB, screen ").append(dm.widthPixels).append('x')
                    .append(dm.heightPixels).append(" @ ").append(dm.densityDpi).append(" dpi");
            if (dp != null)
                d.append(", ").append(String.format(Locale.ROOT, "%.0f", dp.getRefreshRate())).append(" Hz");
            d.append('\n');
        } catch (RuntimeException e) {
            d.append("[device] (memory/screen unreadable: ").append(e).append(")\n");
        }
        try {
            android.content.pm.PackageManager pm = c.getPackageManager();
            android.content.pm.FeatureInfo[] fi = pm.getSystemAvailableFeatures();
            StringBuilder v = new StringBuilder();
            if (fi != null)
                for (android.content.pm.FeatureInfo f : fi)
                    if (f.name != null && (f.name.startsWith("android.hardware.vulkan") || f.name.equals("android.software.vulkan.deqp.level")))
                        v.append(f.name.replace("android.", "")).append('=').append(f.version).append(' ');
            d.append("[device] vulkan features reported by Android: ").append(v.length() == 0 ? "none" : v.toString().trim()).append('\n');
        } catch (RuntimeException e) {
            d.append("[device] (vulkan features unreadable)\n");
        }
        return d.toString();
    }

    /** One line to read at a glance: phone, GPU, driver, game, compat mode, custom driver, last exit, thermal peak, fps */
    static String summary(Context c, String log, String lastExit) {
        String gpu = GpuLog.gpuName(log), hb = ReportLog.lastHeartbeat(log), peak = ReportLog.thermalPeak(log);
        double fps = ReportLog.fpsAverage(log);
        GpuDriver.Meta custom = GpuDriver.active(c);
        String game = ReportLog.value(hb, "game");
        String drv = GpuLog.driverVersion(log);
        return "Summary: " + Build.MANUFACTURER + " " + Build.MODEL + " | GPU " + (gpu != null ? gpu : "unknown")
                + " | driver " + (drv != null ? drv : "unknown") + " | game " + (game != null ? game : GameFiles.libraryGame(c))
                + " | compat " + (CompatMode.on(c) ? "on" : "off")
                + " | custom driver " + (custom != null ? custom.name : "none")
                + " | last exit " + (lastExit != null ? lastExit : cleanWord(log))
                + " | thermal peak " + (peak != null ? peak : "unknown")
                + " | fps avg " + (fps >= 0 ? String.format(Locale.ROOT, "%.1f", fps) : "n/a") + " (last run, " + ReportLog.stalls(log)
                + " stall(s)) | gpu lost " + ReportLog.deviceLost(log) + " | refused pipelines " + ReportLog.refusedPipelines(log);
    }

    private static String cleanWord(String log) {
        return log.isEmpty() ? "no run yet" : ReportLog.cleanExit(log) ? "clean" : "not clean (no exit line)";
    }

    /** Android 11+: how the game's process ended lately. {text for the report, the newest reason or null} */
    static String[] processExits(Context c) {
        if (Build.VERSION.SDK_INT < 30)
            return new String[] {"(Android 11 or newer keeps this record; this phone is on Android " + Build.VERSION.RELEASE + ")\n", null};
        try {
            ActivityManager am = c.getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> all = am != null ? am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 16) : null;
            StringBuilder t = new StringBuilder();
            String newest = null;
            if (all != null)
                for (ApplicationExitInfo e : all) {
                    if (e.getProcessName() == null || !e.getProcessName().endsWith(":game"))
                        continue;
                    String why = exitReason(e.getReason());
                    if (newest == null)
                        newest = why;
                    t.append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(e.getTimestamp()))).append("  ")
                            .append(why).append(", status ").append(e.getStatus()).append(", importance ").append(e.getImportance())
                            .append(", rss ").append(e.getRss() / 1024).append(" MB");
                    if (e.getDescription() != null && !e.getDescription().isEmpty())
                        t.append(", ").append(e.getDescription());
                    t.append('\n');
                }
            return new String[] {t.length() == 0 ? "(none recorded for the game's process)\n" : t.toString(), newest};
        } catch (RuntimeException e) {
            return new String[] {"(could not read: " + e + ")\n", null};
        }
    }

    static String exitReason(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INITIALIZATION_FAILURE";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "USER_STOPPED";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case ApplicationExitInfo.REASON_OTHER: return "OTHER";
            default: return "REASON_" + r;
        }
    }

    /** What the dialog after a failed start adds: the last heartbeat and the last 5 log lines of a run that did not end
     *  cleanly, "" when it ended cleanly or there is no log */
    static String lastRunTail(Context c) {
        File f = gameLog(c);
        if (!f.isFile())
            return "";
        String log = tail(f, 256 * 1024);
        if (ReportLog.cleanExit(log))
            return "";
        String hb = ReportLog.lastHeartbeat(log);
        return (hb != null ? "Last heartbeat: " + hb + "\n" : "") + "Last log lines:\n" + ReportLog.lastLines(log, 5);
    }

    /** Short enough for a share sheet (Android's limit is ~1 MB for the whole intent). Cut by section, not at the end:
     *  the header and the small sections stay whole, each long log keeps its newest part (game.log most) */
    static String shortened(String report, int max) {
        if (report.length() <= max)
            return report;
        List<String> parts = new java.util.ArrayList<>();
        int from = 0;
        for (int at = report.indexOf("\n==== ", 0); at >= 0; at = report.indexOf("\n==== ", at + 1)) {
            parts.add(report.substring(from, at + 1));
            from = at + 1;
        }
        parts.add(report.substring(from));
        final int small = 8 * 1024;
        int fixed = 0;
        double weights = 0;
        for (String p : parts) {
            if (p.length() <= small)
                fixed += p.length();
            else
                weights += weight(p);
        }
        int room = Math.max(0, max - fixed - 256);
        StringBuilder out = new StringBuilder(max);
        for (String p : parts) {
            if (p.length() <= small) {
                out.append(p);
                continue;
            }
            int keep = (int) (room * weight(p) / weights);
            int nl = p.indexOf('\n');
            String title = nl < 0 ? "" : p.substring(0, nl + 1);
            String body = p.substring(title.length());
            if (keep >= body.length()) {
                out.append(p);
                continue;
            }
            int cut = body.indexOf('\n', body.length() - keep);
            out.append(title).append("[... cut: save the report to a file for all of it ...]\n")
                    .append(cut < 0 ? "" : body.substring(cut + 1));
        }
        return out.toString();
    }

    private static double weight(String part) {
        return part.startsWith("==== game.log") ? 5 : part.startsWith("==== game.prev") ? 1.5 : 3;
    }

    private static void section(StringBuilder r, String title, File f, int max) {
        r.append("==== ").append(title).append(" ====\n");
        if (!f.isFile())
            r.append("(none)\n\n");
        else
            r.append(tail(f, max)).append("\n\n");
    }

    private static String tail(File f, int max) {
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            long len = in.length();
            long start = Math.max(0, len - max);
            byte[] b = new byte[(int) (len - start)];
            in.seek(start);
            in.readFully(b);
            return (start > 0 ? "[... " + start + " bytes before ...]\n" : "") + new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "(could not read: " + e + ")";
        }
    }

    private static String logcat(int max) {
        StringBuilder out = new StringBuilder();
        try {
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime").redirectErrorStream(true).start();
            try (BufferedReader in = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = in.readLine()) != null) {
                    out.append(line).append('\n');
                    if (out.length() > 2 * max) // keep the newest lines
                        out.delete(0, out.length() - max);
                }
            }
            p.waitFor();
        } catch (IOException | InterruptedException e) {
            Log.w("vcs", "[report] logcat: " + e);
            return "(logcat could not be read: " + e + ")\n";
        }
        if (out.length() > max)
            out.delete(0, out.length() - max);
        return out.toString();
    }
}
