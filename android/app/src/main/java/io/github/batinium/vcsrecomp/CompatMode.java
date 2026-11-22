// [android] Compatibility mode: conservative renderer / threading switches for GPUs the game fails or crashes on.
//
// The game's native side reads files/env.txt at its start (src/android/game_main.cpp loadEnvFile(): NAME=value lines,
// '#' comments, each line logged "[android] env.txt: ...", a name already in the environment is kept). The launcher
// (its own process) writes a managed block into that file between two marker lines and leaves every other line alone,
// so a developer's own lines (tools/android.py env) survive. The choice itself is in SharedPreferences "compat".
//
// Auto safe mode: the renderer writes files/gpu_lost.marker when Vulkan reports VK_ERROR_DEVICE_LOST (rhi_vulkan.cpp
// reportLost()). consumeMarker() runs when the launcher starts: the first time it turns the mode on, a second time
// (already on) only says so; it never loops silently.
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

final class CompatMode {
    private CompatMode() {
    }

    static final String BEGIN = "# >>> compatibility mode (managed by the launcher, do not edit) >>>";
    static final String END = "# <<< compatibility mode <<<";

    /** Flags as the native code reads them (docs/ANDROID_LAUNCHER.md "Compatibility mode" has the table). The
     *  PS2X_VU1_COMPILED=0 and PS2X_HLE=0 switches are far too slow to be a default and are not here. */
    static final String[] LINES = {
            "PS2X_HW_SPECIALISE=0", // one general shader for every draw (value check: "0" turns it off)
            "PS2X_RHI_FULL_BARRIERS=1", // whole-pipeline barriers (presence check: any value turns it on)
            "PS2X_RHI_FULL_COPY=1", // whole-buffer copies for dynamic vertex / index buffers (value check)
            "PS2X_CHAIN_ASYNC=0", // VIF1 chain walked on the render thread (value check: "0" turns it off)
            "PS2X_RENDER_THREAD=0", // no render submission worker thread
            "PS2X_GS_THREAD=0", // no GS stage thread (needs the worker anyway)
    };

    static final String MARKER = "gpu_lost.marker";

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("compat", Context.MODE_PRIVATE);
    }

    static boolean on(Context c) {
        return prefs(c).getBoolean("on", false);
    }

    /** Remembers the choice and brings files/env.txt in line with it */
    static void setOn(Context c, boolean on) {
        prefs(c).edit().putBoolean("on", on).commit();
        apply(c);
    }

    static File envFile(Context c) {
        return new File(c.getFilesDir(), "env.txt");
    }

    static File marker(Context c) {
        return new File(c.getFilesDir(), MARKER);
    }

    /** Writes (on) or removes (off) the managed block; other lines stay as they are. Idempotent. */
    static void apply(Context c) {
        File f = envFile(c);
        List<String> keep = new ArrayList<>();
        try {
            if (f.isFile()) {
                boolean inside = false;
                for (String s : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
                    if (s.trim().equals(BEGIN))
                        inside = true;
                    else if (inside && s.trim().equals(END))
                        inside = false;
                    else if (!inside)
                        keep.add(s);
                }
            }
            if (on(c)) {
                keep.add(BEGIN);
                for (String s : LINES)
                    keep.add(s);
                keep.add(END);
            }
            if (keep.isEmpty()) {
                if (f.isFile() && !f.delete())
                    Log.w("vcs", "[compat] env.txt could not be removed");
                return;
            }
            File tmp = new File(f.getPath() + ".tmp");
            Files.write(tmp.toPath(), (String.join("\n", keep) + "\n").getBytes(StandardCharsets.UTF_8));
            Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING);
            Log.i("vcs", "[compat] env.txt " + (on(c) ? "has the compatibility block" : "has no compatibility block"));
        } catch (IOException e) {
            Log.w("vcs", "[compat] env.txt: " + e);
        }
    }

    /** The launcher's start: if the game's renderer left a device-lost marker, take it away and return the notice to
     *  show once (turning the mode on the first time); null when there was none. */
    static String consumeMarker(Context c) {
        File m = marker(c);
        boolean found = m.isFile();
        if (!found)
            return null;
        if (!m.delete())
            Log.w("vcs", "[compat] marker could not be deleted");
        if (on(c))
            return "The graphics driver crashed again, even with Compatibility mode on. Send the report "
                    + "(About > Log and crash report) so it can be looked at.";
        setOn(c, true);
        return "The graphics driver crashed last time, so Compatibility mode is now on. You can turn it off in Settings.";
    }

    /** One line for the crash report: the choice and what env.txt holds now */
    static String reportLines(Context c) {
        StringBuilder r = new StringBuilder("Compatibility mode: ").append(on(c) ? "on" : "off").append('\n');
        File f = envFile(c);
        if (!f.isFile())
            return r.append("files/env.txt: (none)\n").toString();
        try {
            r.append("files/env.txt:\n");
            for (String s : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8))
                r.append("  ").append(s).append('\n');
        } catch (IOException e) {
            r.append("files/env.txt: (could not read: ").append(e).append(")\n");
        }
        return r.toString();
    }
}
