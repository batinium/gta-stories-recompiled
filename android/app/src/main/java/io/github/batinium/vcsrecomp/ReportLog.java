// [android] What the heartbeat lines in game.log say (docs/ANDROID.md "Diagnostics"): the last one, the thermal peak, the
// frame rate of the run, whether the run ended cleanly. Pure Java (no Android classes) so tools/android_gpu_check.py
// tests it on the PC's JVM. The lines are written by src/android/diagnostics.cpp:
//   [hb] t=40 frames=2351 fps=58.9 frame_ms_max=31.2 thermal=light battery=71/36.5 mem_free=1830 game=vcs driver=system
//   [watchdog] no frame for 8 s (frames so far 2351; last hb: ...)
package io.github.batinium.vcsrecomp;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ReportLog {
    private ReportLog() {
    }

    private static final Pattern FPS = Pattern.compile("^\\[hb\\] .*? fps=([0-9.]+)");
    private static final Pattern THERMAL = Pattern.compile("^\\[hb\\] .*? thermal=(\\w+|\\?)");
    private static final String[] LEVELS = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};

    /** True when the run's log ends with the game's own exit line (anything else: a crash, a kill, a freeze) */
    static boolean cleanExit(String log) {
        return log.contains("[android] game exit 0") || log.contains("[android] both cities exit 0");
    }

    /** The last "[hb]" line, or null */
    static String lastHeartbeat(String log) {
        int at = log.lastIndexOf("[hb] ");
        while (at > 0 && log.charAt(at - 1) != '\n')
            at = log.lastIndexOf("[hb] ", at - 1);
        if (at < 0)
            return null;
        int end = log.indexOf('\n', at);
        return (end < 0 ? log.substring(at) : log.substring(at, end)).trim();
    }

    /** The last n lines (non-empty), oldest first, joined with newlines */
    static String lastLines(String log, int n) {
        List<String> out = new ArrayList<>();
        int end = log.length();
        while (end > 0 && out.size() < n) {
            int start = log.lastIndexOf('\n', end - 1);
            String line = log.substring(start + 1, end).trim();
            if (!line.isEmpty())
                out.add(0, line);
            end = start < 0 ? 0 : start;
        }
        return String.join("\n", out);
    }

    /** The highest thermal status the heartbeats name ("none" .. "shutdown"), null when there are no heartbeats / none known */
    static String thermalPeak(String log) {
        int peak = -1;
        for (String line : log.split("\n")) {
            Matcher m = THERMAL.matcher(line);
            if (!m.find())
                continue;
            for (int i = 0; i < LEVELS.length; i++)
                if (LEVELS[i].equals(m.group(1)))
                    peak = Math.max(peak, i);
        }
        return peak < 0 ? null : LEVELS[peak];
    }

    /** The average of the heartbeats' frame rates while the app was in front and presenting; -1 when none */
    static double fpsAverage(String log) {
        double sum = 0;
        int n = 0;
        for (String line : log.split("\n")) {
            Matcher m = FPS.matcher(line);
            if (!m.find() || line.contains(" bg=1"))
                continue;
            double v = Double.parseDouble(m.group(1));
            if (v > 0.05) { // a loading screen or a freeze presents nothing: not a frame rate
                sum += v;
                n++;
            }
        }
        return n == 0 ? -1 : sum / n;
    }

    /** How many stalls the watchdog logged */
    static int stalls(String log) {
        int n = 0;
        for (String line : log.split("\n"))
            if (line.startsWith("[watchdog] no frame for"))
                n++;
        return n;
    }

    /** How many times the graphics device was lost ("[rhi] vkQueueSubmit: VK_ERROR_DEVICE_LOST ..."): once per run at most */
    static int deviceLost(String log) {
        int n = 0;
        for (String line : log.split("\n"))
            if (line.startsWith("[rhi] ") && line.contains("VK_ERROR_DEVICE_LOST"))
                n++;
        return n;
    }

    /** How many pipelines the driver refused ("[rhi] pipeline 'gs draw' failed (VK_ERROR_UNKNOWN, -13): ..."): the picture is
     *  black or missing parts then (the stock Adreno 840 driver, 2026-11-22) */
    static int refusedPipelines(String log) {
        int n = 0;
        for (String line : log.split("\n"))
            if (line.startsWith("[rhi] pipeline '") && line.contains("' failed ("))
                n++;
        return n;
    }

    /** A problem the last run showed while the game itself kept going (the launcher says it when you come back) */
    static final class Problem {
        final String title, message;
        final boolean compatMayHelp;

        Problem(String title, String message, boolean compatMayHelp) {
            this.title = title;
            this.message = message;
            this.compatMayHelp = compatMayHelp;
        }
    }

    /** What went wrong in the last run, in plain words, or null: the driver refused drawing steps (black or missing
     *  picture), or the game froze (the watchdog). A start that failed has its own dialog (GpuLog.explain) and is not
     *  repeated here. */
    static Problem problem(String log, String gpu, boolean compatOn, boolean customDriver) {
        int refused = refusedPipelines(log), stalls = stalls(log);
        String on = gpu != null ? " (" + gpu + ")" : "";
        String compat = compatOn ? "Compatibility mode is already on. Update the system and the GPU driver."
                : "Turn on Compatibility mode in Settings and try again. If it keeps happening, update the system and the GPU driver.";
        String send = "\n\nSave the report and send it, so the cause can be found.";
        if (deviceLost(log) > 0)
            return new Problem("The graphics driver crashed",
                    "The graphics driver" + on + " stopped responding and the game closed.\n\n" + compat + send, !compatOn);
        if (refused > 0)
            return new Problem("Black or broken picture",
                    "The graphics driver" + on + " refused " + refused + " drawing step" + (refused == 1 ? "" : "s")
                            + ", so the screen was black or partly missing. Your game files are fine; this is the driver.\n\n"
                            + "Update the system and the GPU driver" + (customDriver ? " (or try without the custom driver)" : "")
                            + ". Compatibility mode doesn't help here." + send, false);
        if (stalls > 0)
            return new Problem("The game froze",
                    "The game stopped drawing for several seconds" + on + ".\n\n" + compat + send, !compatOn);
        return null;
    }

    /** A line after "key=" up to the next space, e.g. value("... game=vcs driver=system", "game") = "vcs"; null if absent */
    static String value(String line, String key) {
        if (line == null)
            return null;
        int at = line.indexOf(" " + key + "=");
        if (at < 0)
            return null;
        int s = at + key.length() + 2, e = line.indexOf(' ', s);
        return e < 0 ? line.substring(s) : line.substring(s, e);
    }
}
