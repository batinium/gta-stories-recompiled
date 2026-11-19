#!/usr/bin/env python3
"""Headless Ghidra analysis of your SLUS_215.90, exporting the PS2Recomp function map
(ghidra/export/ghidra_export.toml + functions.csv). Only needed to redo the analysis.

    python tools/ghidra_analyze.py [build/elf/SLUS_215.90]

Another game (bring-up): its ELF, and a project / export folder outside the repository's ghidra/ (VCS's):

    python tools/ghidra_analyze.py --name lcs --project <dir> --out <dir> <SLUS_214.23>
    python tools/make_config.py --game lcs --elf SLUS_214.23 <out>/ghidra_export.toml <analyzer.toml>

Needs Ghidra 12.1.4 with the ghidra-emotionengine-reloaded extension and JDK 21: set GHIDRA_HOME and JAVA_HOME
(defaults: the scoop install locations on Windows; on macOS ~/tools/ghidra_12.1.4_PUBLIC and Homebrew's openjdk@21).
Headless only (support/analyzeHeadless): no Ghidra window.
"""
import argparse
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def default_ghidra(home):
    """GHIDRA_HOME, else (macOS / Linux) ~/tools/ghidra_<version>*, else toolchain's (scoop)."""
    if os.environ.get("GHIDRA_HOME") or tc.IS_WINDOWS:
        return tc.ghidra_home()
    found = sorted(glob.glob(os.path.join(home, "tools", "ghidra_%s*" % tc.GHIDRA_VERSION)))
    return found[-1] if found else tc.ghidra_home()


def default_java(home):
    if tc.IS_WINDOWS:
        return os.path.join(home, "scoop", "apps", "temurin21-jdk", "current")
    brew = "/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home"
    return brew if os.path.isdir(brew) else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?", default=os.path.join(tc.BUILD, "elf", "SLUS_215.90"))
    ap.add_argument("--max-cpu", type=int, default=6)
    ap.add_argument("--name", default="vcs", help="the Ghidra project's name (vcs)")
    ap.add_argument("--project", default=os.path.join(tc.ROOT, "ghidra", "project"), help="project folder (ghidra/project)")
    ap.add_argument("--out", default=os.path.join(tc.ROOT, "ghidra", "export"), help="export folder (ghidra/export)")
    a = ap.parse_args()

    home = os.path.expanduser("~")
    ghidra = default_ghidra(home)
    env = dict(os.environ)
    if not env.get("JAVA_HOME") and default_java(home):
        env["JAVA_HOME"] = default_java(home)
    headless = os.path.join(ghidra, "support", "analyzeHeadless" + (".bat" if tc.IS_WINDOWS else ""))
    if not os.path.isfile(headless):
        tc.fail(f"{headless} not found: set GHIDRA_HOME")
    if not os.path.isfile(a.elf):
        tc.fail(f"{a.elf} not found: python tools/recompile.py extracts it from your disc")
    os.makedirs(a.project, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    scripts = os.path.join(tc.ROOT, "third_party", "PS2Recomp", "ps2xRecomp", "tools", "ghidra")
    cmd = [headless, a.project, a.name, "-import", os.path.abspath(a.elf), "-overwrite", "-scriptPath", scripts,
           "-postScript", "ExportPS2Functions.java", os.path.join(a.out, "ghidra_export.toml"), os.path.join(a.out, "functions.csv"),
           "-max-cpu", str(a.max_cpu)]
    sys.exit(subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL).returncode)


if __name__ == "__main__":
    main()
