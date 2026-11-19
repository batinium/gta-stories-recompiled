// [android] Crash reports a player can send without a PC (About > Game log).
//
// The game's process copies its log (stdout / stderr: runtime, renderer, crash handler) to files/logs/game.log
// (src/android/android_log.cpp; the run before is game.prev.log), and a Java exception in that process is added to it
// here. The launcher reads it: About > Game log says whether the last run ended in a crash, and saves the report to a
// file or shares it. The report is the phone (model, chip, Android), the app's version and build tag, both logs and
// this app's own logcat (Android gives an app only its own lines: the GPU driver's messages for our process are there).
package io.github.batinium.vcsrecomp;

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
            return "No game log yet (it starts with the next Play)";
        String when = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date(f.lastModified()));
        String tail = tail(f, 64 * 1024);
        if (tail.contains("[crash]") || tail.contains("[java crash]"))
            return "✗ The last run crashed (" + when + "): save or share the report";
        if (tail.contains("[game-library] refused"))
            return "✗ The last run could not load the code pack (" + when + ")";
        if (tail.contains("game exit") || tail.contains("both cities exit"))
            return "Last run ended normally (" + when + ")";
        return "Last run " + when + ": ended without a word (killed or crashed): send the report if it was a crash";
    }

    /** The whole report (slow: reads logcat). Off the UI thread. */
    static String build(Context c) {
        StringBuilder r = new StringBuilder(256 * 1024);
        r.append("GTA Stories Recompiled crash report\n");
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
        r.append("Made: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date()))
                .append("\n\n");
        section(r, "game.log (last run)", gameLog(c), 1024 * 1024);
        section(r, "game.prev.log (the run before)", previousLog(c), 256 * 1024);
        r.append("==== logcat (this app's own lines) ====\n");
        r.append(logcat(512 * 1024));
        return r.toString();
    }

    /** Short enough for a share sheet (Android's limit is ~1 MB for the whole intent) */
    static String shortened(String report, int max) {
        if (report.length() <= max)
            return report;
        int head = Math.min(report.indexOf("==== "), 2048);
        if (head < 0)
            head = 0;
        return report.substring(0, head) + "\n[... cut: save the report to a file for all of it ...]\n"
                + report.substring(report.length() - (max - head));
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
