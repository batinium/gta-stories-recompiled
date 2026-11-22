// [android] What the game's log says about the GPU: its name, why the game could not start, the tested-phones list.
//
// Pure Java (no Android classes) so tools/android_gpu_check.py can test it on the PC's JVM. The launcher has no Vulkan
// context, so it learns the GPU from the last run's log (files/logs/game.log; src/rhi/vulkan/rhi_vulkan.cpp):
//   [rhi] gpu: Adreno (TM) 840, vendor 0x5143 device 0x... , type 1          (a usable GPU was picked)
//   [rhi] gpu: driver '...' info '...' (id ..., conformance ...), driverVersion 0x...
//   [rhi] graphics cards, fastest first: Mali-G720 MC8 [1] (not usable: needs a required feature, lacking: ...); using none
// and a failed start ends with "Failed to initialize runtime" / "[android] game exit 5".
package io.github.batinium.vcsrecomp;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class GpuLog {
    private GpuLog() {
    }

    private static final Pattern GPU = Pattern.compile("\\[rhi\\] gpu: (.+?), vendor 0x[0-9a-fA-F]+");
    private static final Pattern DRIVER = Pattern.compile("\\[rhi\\] gpu: driver .*driverVersion (0x[0-9a-fA-F]+)");
    private static final Pattern API = Pattern.compile("\\[rhi\\] gpu: Vulkan apiVersion (\\d+\\.\\d+)");
    private static final Pattern CARDS = Pattern.compile("\\[rhi\\] graphics cards, fastest first: (.*)");
    private static final String NEEDS = "(not usable: needs ";

    /** The GPU the game used (the last "[rhi] gpu:" name line), else the first card the "graphics cards" line names; null if none */
    static String gpuName(String log) {
        String name = null;
        Matcher m = GPU.matcher(log);
        while (m.find())
            name = m.group(1).trim();
        if (name != null)
            return name;
        String cards = lastCards(log);
        if (cards == null || cards.startsWith("none"))
            return null;
        int end = cards.indexOf(" [");
        return end > 0 ? cards.substring(0, end).replace(" (integrated)", "").trim() : null;
    }

    /** "0x..." of the driver in the last run, or null: a driver update changes it, which re-arms the untested notice */
    static String driverVersion(String log) {
        String v = null;
        Matcher m = DRIVER.matcher(log);
        while (m.find())
            v = m.group(1);
        return v;
    }

    /** "1.4" from the "Vulkan apiVersion" line, or null */
    static String apiVersion(String log) {
        String v = null;
        Matcher m = API.matcher(log);
        while (m.find())
            v = m.group(1);
        return v;
    }

    private static String lastCards(String log) {
        String c = null;
        Matcher m = CARDS.matcher(log);
        while (m.find())
            c = m.group(1);
        return c;
    }

    /** Why the last run could not start */
    static final class Failure {
        final String gpu; // may be null
        /** each "needs ..." text: "Vulkan 1.3 - this driver has 1.1", an extension, "a required feature, lacking: ..." */
        final List<String> needs = new ArrayList<>();
        final boolean noGpu; // "using none" without a reason: no Vulkan driver at all

        Failure(String gpu, boolean noGpu) {
            this.gpu = gpu;
            this.noGpu = noGpu;
        }
    }

    /** The cause the log gives for a start that failed in the graphics, or null if the run did not fail that way */
    static Failure startFailure(String log) {
        String cards = lastCards(log);
        boolean init = log.contains("Failed to initialize runtime");
        boolean none = cards != null && (cards.contains("; using none") || cards.endsWith("using none"));
        if (!init && !none)
            return null;
        Failure f = new Failure(gpuName(log), none && !cards.contains("not usable"));
        if (cards != null) {
            int at = 0;
            while ((at = cards.indexOf(NEEDS, at)) >= 0) {
                int from = at + NEEDS.length();
                int end = from;
                // the text ends at the ")" that is followed by ", " / "; " / the end (an old message nested parentheses)
                while (true) {
                    end = cards.indexOf(')', end);
                    if (end < 0) {
                        end = cards.length();
                        break;
                    }
                    if (end + 1 >= cards.length() || cards.startsWith(", ", end + 1) || cards.startsWith("; ", end + 1))
                        break;
                    end++;
                }
                f.needs.add(cards.substring(from, end).trim());
                at = from;
            }
        }
        return f;
    }

    /** True when the cause is something the phone's driver lacks (Compatibility mode cannot give it) */
    static boolean missingFeature(Failure f) {
        return !f.needs.isEmpty() || f.noGpu;
    }

    /** The dialog text: the cause in plain words, then what to try */
    static String explain(Failure f) {
        StringBuilder s = new StringBuilder();
        String gpu = f.gpu != null ? " (" + f.gpu + ")" : "";
        if (!f.needs.isEmpty()) {
            List<String> lacks = new ArrayList<>();
            String vulkan = null;
            for (String n : f.needs) {
                if (n.startsWith("Vulkan 1.3"))
                    vulkan = n;
                else if (n.contains("lacking:")) {
                    for (String part : n.substring(n.indexOf("lacking:") + 8).split(";")) {
                        String t = feature(part);
                        if (!t.isEmpty() && !lacks.contains(t))
                            lacks.add(t);
                    }
                } else if (n.startsWith("VK_"))
                    lacks.add("the Vulkan extension " + n);
                else
                    lacks.add(n);
            }
            if (vulkan != null) {
                Matcher v = Pattern.compile("has (\\d+\\.\\d+)").matcher(vulkan);
                s.append("Vulkan 1.3 is required, and this phone's graphics driver").append(gpu).append(" only offers ")
                        .append(v.find() ? v.group(1) : "an older version").append('.');
            }
            if (!lacks.isEmpty()) {
                if (s.length() > 0)
                    s.append("\n\n");
                s.append("This phone's graphics driver").append(gpu).append(" lacks: ").append(String.join(", ", lacks)).append('.');
            }
            s.append("\n\nThe game needs this and can't start here as it is. You can update the system to get a newer "
                    + "driver, but Compatibility mode can't fix a missing feature.");
        } else if (f.noGpu) {
            s.append("No Vulkan graphics driver was found on this phone, so the game can't draw.\n\nYou can update the "
                    + "system; Compatibility mode can't fix this.");
        } else {
            s.append("The game could not start").append(gpu).append(".\n\nTry again after you turn on Compatibility mode "
                    + "(Settings). If that doesn't help, update the system.");
        }
        s.append("\n\nSave the report and send it, so support for this phone can be added.");
        return s.toString();
    }

    private static String feature(String raw) {
        String t = raw.trim();
        return t.equals("demote") ? "demote-to-helper-invocation" : t;
    }

    // ---- the tested-phones list ---------------------------------------------------------------------------------------

    /** Rules from tested_devices.txt: one per line, "# comment" cut off, blanks dropped */
    static List<String> parseRules(String text) {
        List<String> rules = new ArrayList<>();
        for (String line : text.split("\\r?\\n")) {
            int hash = line.indexOf('#');
            String r = (hash >= 0 ? line.substring(0, hash) : line).trim();
            if (!r.isEmpty())
                rules.add(r);
        }
        return rules;
    }

    /** True when a rule is a (case-insensitive) substring of any of the names (the GPU, or the SoC before a first run) */
    static boolean tested(List<String> rules, String... names) {
        for (String n : names) {
            if (n == null)
                continue;
            String low = n.toLowerCase(Locale.ROOT);
            for (String r : rules)
                if (low.contains(r.toLowerCase(Locale.ROOT)))
                    return true;
        }
        return false;
    }
}
