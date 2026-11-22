#!/usr/bin/env python3
"""[android] Digest a game log or a crash report (About > Log and crash report) into one screen: the phone, GPU and driver,
what went wrong (start failure, device lost, refused pipelines, freezes, last exit), how it ran (fps, thermals) and which log
prefixes take the room. Accepts .txt / .log / .rtf (TextEdit) / raw `adb logcat -s vcs` output. Reads only; changes nothing.

    python3 tools/log_digest.py report.txt [more.txt ...]
    python3 tools/log_digest.py --noise report.txt      # also list the chattiest log prefixes
"""
import collections
import re
import subprocess
import sys


def load(path):
    raw = open(path, "rb").read()
    text = raw.decode("utf-8", "replace")
    if text.lstrip().startswith("{\\rtf"):
        try:  # macOS: textutil is exact
            text = subprocess.run(["textutil", "-convert", "txt", "-stdout", path], capture_output=True, check=True).stdout.decode("utf-8", "replace")
        except (OSError, subprocess.CalledProcessError):
            text = re.sub(r"\\'[0-9a-f]{2}|\\[a-z]+-?\d* ?|[{}]", "", text)
    # logcat lines: "11-22 09:05:07.927 15425 15487 I vcs     : [rhi] ..." -> the part after the tag
    return [re.sub(r"^\d\d-\d\d [\d:.]+ +\d+ +\d+ [VDIWEF] vcs +: ", "", l.rstrip("\n")) for l in text.splitlines()]


def first(lines, pat, group=1):
    for l in lines:
        m = re.search(pat, l)
        if m:
            return m.group(group)
    return None


def digest(path, noise):
    L = load(path)
    out = ["== " + path + " (%d lines)" % len(L)]
    summary = first(L, r"^(Summary: .*)")
    if summary:
        out.append(summary)
    for key, pat in (("app", r"^(?:App|Version)[: ]+(.+)"), ("phone", r"^(?:Device|Phone)[: ]+(.+)"), ("android", r"^(Android [^\n]+)")):
        v = first(L, pat)
        if v:
            out.append("%-8s %s" % (key, v.strip()[:150]))
    gpu = first(L, r"\[rhi\] gpu: (.+?), vendor") or first(L, r"; using (.+)$")  # older builds: only the cards line
    out.append("gpu      %s | %s | api %s | loader %s" % (gpu or "?", first(L, r"\[rhi\] gpu: driver '([^']*)'") or "?",
                                                         first(L, r"\[rhi\] gpu: Vulkan apiVersion (\S+)") or "?",
                                                         first(L, r"^\[rhi\] gpu: loader: (.+)") or "?"))
    problems = []
    cards = first(L, r"\[rhi\] graphics cards, fastest first: (.*)")
    if cards and ("using none" in cards or "not usable" in cards):
        problems.append("START FAILED: " + cards[:200])
    if any("Failed to initialize runtime" in l for l in L):
        problems.append("Failed to initialize runtime")
    lost = [l for l in L if l.startswith("[rhi]") and "VK_ERROR_DEVICE_LOST" in l]
    if lost:
        problems.append("DEVICE LOST x%d (%s)" % (len(lost), lost[0][:80]))
    ref = [l for l in L if l.startswith("[rhi] pipeline '") and "' failed (" in l]
    if ref:
        names = collections.Counter(re.match(r"\[rhi\] pipeline '([^']*)'", l).group(1) for l in ref)
        code = first(ref, r"failed \((\w+, -?\d+)\)")
        sizes = sorted({(int(a), int(b)) for a, b in (re.findall(r"ps (\d+) B vs (\d+) B", l)[0] for l in ref if re.search(r"ps \d+ B vs", l))})
        problems.append("REFUSED PIPELINES x%d %s: %s; ps/vs sizes %s" % (len(ref), dict(names), code, sizes[:6]))
    stalls = [l for l in L if l.startswith("[watchdog] no frame for")]
    if stalls:
        problems.append("FREEZES x%d (%s)" % (len(stalls), stalls[0][:90]))
    exits = [l for l in L if l.startswith("[android] game exit")]
    if exits and not exits[-1].endswith(" 0"):
        problems.append("last exit: " + exits[-1])
    for l in L:
        if re.search(r"signal \d+ \(SIG|Fatal signal|SIGSEGV|SIGABRT|backtrace:", l):
            problems.append("NATIVE CRASH: " + l[:120])
            break
    out.append("problems " + ("none found" if not problems else ""))
    out += ["  - " + p for p in problems]
    hb = [l for l in L if l.startswith("[hb] ")]
    if hb:
        fps = [float(m.group(1)) for l in hb if "bg=1" not in l for m in [re.search(r" fps=([0-9.]+)", l)] if m]
        th = [m.group(1) for l in hb for m in [re.search(r" thermal=(\w+)", l)] if m]
        drv = first(hb, r" driver=(\S+)")
        out.append("ran      %d heartbeats (%d s), fps avg %.1f min %.1f, thermal peak %s, driver %s" % (
            len(hb), 10 * len(hb), sum(fps) / len(fps) if fps else -1, min(fps) if fps else -1,
            max(th, key=["none", "light", "moderate", "severe", "critical", "emergency", "shutdown", "?"].index) if th else "?", drv))
    else:
        out.append("ran      no heartbeat lines (a build before 0.3.1-2, or the game never started)")
    if noise:
        c = collections.Counter()
        b = collections.Counter()
        for l in L:
            m = re.match(r"(\[[^\]]+\])", l)
            k = m.group(1) if m else "(other)"
            c[k] += 1
            b[k] += len(l) + 1
        tot = sum(b.values())
        out.append("noise    %d bytes; chattiest prefixes:" % tot)
        for k, n in c.most_common(6):
            out.append("  %5d lines %5.1f%%  %s" % (n, 100.0 * b[k] / tot, k))
    return "\n".join(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    for p in args:
        print(digest(p, "--noise" in sys.argv))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
