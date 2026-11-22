// [android] A plain-JVM check of the GPU log parsing and the tested-phones list (GpuLog), no Android or JUnit:
//
//   python3 tools/android_gpu_check.py
package io.github.batinium.vcsrecomp;

import java.util.List;

public final class GpuLogCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    public static void main(String[] args) {
        // a normal run (the real line format)
        String ok = "[android] game start, 0 argument(s)\n"
                + "[rhi] graphics cards, fastest first: Adreno (TM) 840 [1]; using Adreno (TM) 840\n"
                + "[rhi] gpu: Adreno (TM) 840, vendor 0x5143 device 0x44050001, type 1\n"
                + "[rhi] gpu: driver 'Qualcomm Technologies Inc. Adreno Vulkan Driver' info 'V@0829.0' (id 3, conformance 1.4.0.0), driverVersion 0x00829000\n"
                + "[rhi] gpu: Vulkan apiVersion 1.4.305\n";
        check("Adreno (TM) 840".equals(GpuLog.gpuName(ok)), "gpu name from the gpu line");
        check("0x00829000".equals(GpuLog.driverVersion(ok)), "driver version");
        check("1.4".equals(GpuLog.apiVersion(ok)), "api version");
        check(GpuLog.startFailure(ok) == null, "a good run has no failure");

        // Mali: the old message (from the bug report) and the current one
        String mali = "[rhi] graphics cards, fastest first: Mali-G720 MC8 [1] (not usable: needs a required feature, lacking: dual-source blend;); using none\n"
                + "Failed to initialize runtime\n[android] game exit 5\n";
        GpuLog.Failure f = GpuLog.startFailure(mali);
        check(f != null && "Mali-G720 MC8".equals(f.gpu), "mali gpu name from the cards line");
        check(f != null && f.needs.size() == 1 && f.needs.get(0).equals("a required feature, lacking: dual-source blend;"), "mali needs: " + (f == null ? null : f.needs));
        check(f != null && GpuLog.explain(f).contains("lacks: dual-source blend"), "mali text names the feature");
        check(f != null && GpuLog.explain(f).contains("Compatibility mode can't fix"), "mali text: compat cannot fix");
        String oldMali = "[rhi] graphics cards, fastest first: Mali-G720 MC8 [1] (not usable: needs a required feature (dynamic rendering, synchronization2, demote, dual-source blend, depth clamp, clip distance)); using none\nFailed to initialize runtime\n";
        f = GpuLog.startFailure(oldMali);
        check(f != null && f.needs.size() == 1 && f.needs.get(0).endsWith("clip distance)"), "old nested message: " + (f == null ? null : f.needs));

        // several features, and demote renamed
        String multi = "[rhi] graphics cards, fastest first: Foo GPU [1] (integrated) (not usable: needs a required feature, lacking: demote; depth clamp;); using none\n";
        f = GpuLog.startFailure(multi);
        check(f != null && GpuLog.explain(f).contains("lacks: demote-to-helper-invocation, depth clamp"), "multi features: " + (f == null ? null : GpuLog.explain(f)));

        // Vulkan too old: the device is in the "not in the list" part
        String old = "[rhi] graphics cards, fastest first: none; not in the list: PowerVR D-Series (not usable: needs Vulkan 1.3 - this driver has 1.1); using none\nFailed to initialize runtime\n";
        f = GpuLog.startFailure(old);
        check(f != null && GpuLog.explain(f).startsWith("Vulkan 1.3 is required, and this phone's graphics driver only offers 1.1"), "old vulkan: " + (f == null ? null : GpuLog.explain(f)));
        check(f != null && GpuLog.missingFeature(f), "old vulkan is a missing feature");

        // an extension
        f = GpuLog.startFailure("[rhi] graphics cards, fastest first: X [1] (not usable: needs VK_KHR_push_descriptor); using none\n");
        check(f != null && GpuLog.explain(f).contains("the Vulkan extension VK_KHR_push_descriptor"), "extension");

        // no cause in the log: generic, compat mode may help
        f = GpuLog.startFailure(ok + "Failed to initialize runtime\n");
        check(f != null && !GpuLog.missingFeature(f) && GpuLog.explain(f).contains("turn on Compatibility mode"), "generic init failure");
        check(GpuLog.startFailure("[rhi] graphics cards, fastest first: none; using none\n") != null
                && GpuLog.explain(GpuLog.startFailure("[rhi] graphics cards, fastest first: none; using none\n")).contains("No Vulkan graphics driver"), "no gpu at all");

        // the tested list
        List<String> rules = GpuLog.parseRules("# header\nAdreno (TM) 840   # Honor PNM-N49\n\n# Adreno (TM) 830\n");
        check(rules.size() == 1 && rules.get(0).equals("Adreno (TM) 840"), "rules: " + rules);
        check(GpuLog.tested(rules, "Adreno (TM) 840"), "840 tested");
        check(GpuLog.tested(rules, "adreno (tm) 840 (Qualcomm 1.2)"), "case and suffix");
        check(!GpuLog.tested(rules, "Adreno (TM) 750"), "750 untested");
        check(!GpuLog.tested(rules, "Mali-G720 MC8"), "mali untested");
        check(!GpuLog.tested(rules, (String) null), "unknown untested");

        // [diag] heartbeat / watchdog lines (src/android/diagnostics.cpp) and the report's summary pieces (ReportLog)
        String hb1 = "[hb] t=10 frames=590 fps=59.0 frame_ms_max=31.2 thermal=light battery=71/36.5 mem_free=1830 game=vcs driver=system";
        String hb2 = "[hb] t=20 frames=1180 fps=59.0 frame_ms_max=18.0 thermal=moderate battery=70/38.0 mem_free=1790 game=vcs driver=system";
        String hbBg = "[hb] t=30 frames=1180 fps=0.0 frame_ms_max=0.0 thermal=severe battery=70/38.0 mem_free=1790 game=vcs driver=system bg=1";
        String hb3 = "[hb] t=40 frames=1500 fps=32.0 frame_ms_max=60.0 thermal=light battery=69/37.0 mem_free=1700 game=vcs driver=turnip-v25";
        String run = "[log] game process 1 started 2026-11-21 10:00:00\n" + hb1 + "\nsome line\n" + hb2 + "\n" + hbBg + "\n" + hb3 + "\n";
        check(hb3.equals(ReportLog.lastHeartbeat(run)), "last heartbeat");
        check(ReportLog.lastHeartbeat("no heartbeat here\n") == null, "no heartbeat");
        check("vcs".equals(ReportLog.value(hb3, "game")) && "turnip-v25".equals(ReportLog.value(hb3, "driver")), "value()");
        check("moderate".equals(ReportLog.thermalPeak(run)) == false && "severe".equals(ReportLog.thermalPeak(run)), "thermal peak: " + ReportLog.thermalPeak(run));
        check(Math.abs(ReportLog.fpsAverage(run) - (59.0 + 59.0 + 32.0) / 3) < 0.01, "fps average skips background: " + ReportLog.fpsAverage(run));
        check(ReportLog.fpsAverage("nothing") < 0, "no fps");
        check(!ReportLog.cleanExit(run) && ReportLog.cleanExit(run + "[android] game exit 0\n") && !ReportLog.cleanExit(run + "[android] game exit 5\n"), "clean exit");
        check("a\nb\nc".equals(ReportLog.lastLines("x\na\n\nb\nc\n", 3)), "last lines: " + ReportLog.lastLines("x\na\n\nb\nc\n", 3));
        String stall = run + "[watchdog] no frame for 8 s (frames so far 1500; last hb: " + hb3 + ")\n[watchdog]   tid 1 main: S (sleeping) wchan=futex_wait_queue\n";
        check(ReportLog.stalls(stall) == 1 && hb3.equals(ReportLog.lastHeartbeat(stall)), "stall count; the hb inside the watchdog line is not the last hb");

        System.out.println(checks + " checks, " + failures + " failures");
        if (failures > 0)
            System.exit(1);
    }
}
