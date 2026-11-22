// [android] A plain-JVM check of what the launcher says for phones nobody has tested (GpuLog / ReportLog), from logs
// in tools/fixtures/device_logs/ (synthetic: real line formats, no game data). Each file's first line says what to expect:
//
//   # expect: tested=yes|no failure=none|missing|generic explain=<text> problem=none|<title text> lost=<n> refused=<n> stalls=<n> report=<text>
//
// (explain= may repeat; every key is optional.) tested= is the tested-phones rule list (assets/tested_devices.txt) against
// the log's GPU; failure= the start-up error the log leaves; explain= a piece of the dialog text; lost= / refused= / stalls=
// problem= the dialog the launcher shows for a run that started but went wrong (black picture, freeze), lost= / refused= /
// stalls= the counts in the report summary. Run: python3 tools/android_gpu_check.py <fixtures> <tested_devices.txt>
package io.github.batinium.vcsrecomp;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class DeviceProfilesCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]);
        List<String> rules = GpuLog.parseRules(new String(Files.readAllBytes(new File(args[1]).toPath()), StandardCharsets.UTF_8));
        File[] files = dir.listFiles((d, n) -> n.endsWith(".log"));
        Arrays.sort(files);
        check(files.length >= 10, "fixtures found: " + files.length);
        for (File f : files) {
            String text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
            String first = text.substring(0, text.indexOf('\n'));
            check(first.startsWith("# expect:"), f.getName() + ": first line is '# expect: ...'");
            String log = text.substring(text.indexOf('\n') + 1);
            String who = f.getName();
            Matcher m = Pattern.compile("(\\w+)=(.*?)(?= \\w+=|$)").matcher(first.substring("# expect:".length()).trim());
            List<String> explains = new ArrayList<>();
            GpuLog.Failure fail = GpuLog.startFailure(log);
            while (m.find()) {
                String k = m.group(1), v = m.group(2).trim();
                switch (k) {
                    case "tested":
                        check(GpuLog.tested(rules, GpuLog.gpuName(log) != null ? GpuLog.gpuName(log) : "?") == v.equals("yes"),
                                who + ": tested=" + v + " (gpu " + GpuLog.gpuName(log) + ")");
                        break;
                    case "failure":
                        if (v.equals("none"))
                            check(fail == null, who + ": no start failure");
                        else if (v.equals("missing"))
                            check(fail != null && GpuLog.missingFeature(fail), who + ": a missing-feature failure");
                        else
                            check(fail != null && !GpuLog.missingFeature(fail), who + ": a generic failure");
                        break;
                    case "explain":
                        explains.add(v);
                        break;
                    case "problem": {
                        ReportLog.Problem pr = ReportLog.problem(log, GpuLog.gpuName(log), false, false);
                        if (v.equals("none"))
                            check(pr == null || fail != null, who + ": no problem dialog");
                        else {
                            check(pr != null && pr.title.contains(v), who + ": problem dialog '" + v + "' got " + (pr == null ? null : pr.title));
                            check(pr != null && pr.message.contains("send it"), who + ": the problem dialog asks for the report");
                            check(pr == null || !pr.message.contains("Compatibility mode doesn't help") || !pr.compatMayHelp, who + ": no compat advice that cannot work");
                        }
                        break;
                    }
                    case "lost":
                        check(ReportLog.deviceLost(log) == Integer.parseInt(v), who + ": lost=" + v + " got " + ReportLog.deviceLost(log));
                        break;
                    case "refused":
                        check(ReportLog.refusedPipelines(log) == Integer.parseInt(v), who + ": refused=" + v + " got " + ReportLog.refusedPipelines(log));
                        break;
                    case "stalls":
                        check(ReportLog.stalls(log) == Integer.parseInt(v), who + ": stalls=" + v + " got " + ReportLog.stalls(log));
                        break;
                    case "report":
                        // the text the report's summary line holds for these counts (CrashReport.summary)
                        String line = "gpu lost " + ReportLog.deviceLost(log) + " | refused pipelines " + ReportLog.refusedPipelines(log);
                        check(line.contains(v), who + ": report line '" + line + "' has '" + v + "'");
                        break;
                    default:
                        check(false, who + ": unknown key " + k);
                }
            }
            for (String e : explains) {
                String text2 = fail == null ? "" : GpuLog.explain(fail);
                check(text2.contains(e), who + ": explain has '" + e + "' in: " + text2.replace('\n', ' '));
            }
            if (fail != null) { // every start failure asks for the report, and never promises what Compatibility mode cannot do
                String t = GpuLog.explain(fail);
                check(t.contains("send it"), who + ": the dialog asks for the report");
                check(!GpuLog.missingFeature(fail) || t.contains("can't fix"), who + ": a missing feature says compat mode cannot fix it");
            }
        }
        System.out.println(checks + " checks, " + failures + " failures (" + files.length + " device logs)");
        if (failures > 0)
            System.exit(1);
    }
}
