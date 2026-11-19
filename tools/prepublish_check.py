#!/usr/bin/env python3
"""[android] Before a push or a release: is everything git would publish safe to publish?

    python3 tools/prepublish_check.py              # the tracked files (the index) and the commits not in upstream
    python3 tools/prepublish_check.py --base REF   # the commits since REF instead (default: upstream/main, else main)
    python3 tools/prepublish_check.py --apk FILE   # also an APK about to be attached to a release: no game code in it

Exit 0 when nothing was found, 1 otherwise (each finding on its own line). It looks for:

- game code or data: anything under generated/, an ISO / ELF / SLUS_* file, a recompiled function's definition
  (`FUN_00xxxxxx_0x...(...)  {`) outside our hooks (src/), a binary file that upstream doesn't have;
- build outputs and third-party files that are never shipped: *.apk / *.aab / *.so, libvcs_game.so, Lossless.dll,
  keystores (*.jks, *.keystore);
- private files: AGENTS.md (this fork keeps it untracked), internal/;
- personal details: absolute home-folder paths (/Users/<name>/, /home/<name>/, C:\\Users\\<name>\\) and your git
  e-mail address in the files' text (not in commit authors: that is git's metadata, your choice), and the words listed
  one per line in internal/prepublish_words.txt (untracked; optional).

Paths are checked in every commit of the range too (a file deleted later is still published with the history), and
commit messages for paths and words. Standard library only.
"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FORBIDDEN_PATH = [
    (re.compile(r"(^|/)generated/"), "generated code (generated/)"),
    (re.compile(r"(^|/)generated-[^/]+/"), "another game's generated code (generated-lcs/)"),  # [lcs]
    (re.compile(r"^AGENTS\.md$"), "AGENTS.md is private: keep it untracked"),
    (re.compile(r"^internal/"), "internal/ is private: keep it untracked"),
    (re.compile(r"(?i)\.(iso|cso|chd|cue|bin|elf|img|mdf|nrg)$"), "a disc image or executable"),
    (re.compile(r"(?i)(^|/)S[LC][UEP]S_\d{3}\.\d{2}$"), "a PS2 game executable (SLUS_...)"),
    (re.compile(r"(?i)\.(apk|aab|apks|idsig)$"), "an Android package"),
    (re.compile(r"(?i)\.so(\.\d+)*$"), "a shared library (the game library or a build output)"),
    (re.compile(r"(?i)(^|/)lossless\.dll$"), "Lossless.dll (players bring their own)"),
    # [stories] the other game's assets, extracted from the player's own disc by tools/stories_pack.py
    (re.compile(r"(?i)(^|/)stories_(vcs|lcs)_[^/]+/"), "a stories_pack folder (assets from a disc)"),
    (re.compile(r"(?i)\.(dtz|mdl|xtx|xet|anm)$"), "a game data file (GAME.DTZ / model / texture chunk / animation pack)"),
    (re.compile(r"(?i)\.(jks|keystore|p12|pfx)$"), "a signing key"),
    (re.compile(r"(?i)\.(gzf|rep)$"), "a Ghidra project / export of the game"),
]

# a recompiled function's definition (the generated code's shape): name, the guest-context parameter, a body
FUN_DEF = re.compile(rb"^[ \t]*(?:static\s+|inline\s+|extern\s+\"C\"\s+)*void\s+FUN_00[0-9a-fA-F]{6}(?:_0x[0-9a-fA-F]+)?"
                     rb"\s*\([^;{)]*\)\s*\{", re.M)
HOME_PATH = re.compile(rb"(/Users/(?!Shared/)[A-Za-z0-9._-]+/|/home/(?!runner/|user/|\$)[a-z0-9._-]+/|"
                       rb"[A-Za-z]:\\\\?Users\\\\?(?!Public\\|<|%)[A-Za-z0-9._-]+\\)")
TEXT_EXT_SKIP = {".png", ".jpg", ".jpeg", ".webp", ".ico", ".icns", ".jar", ".ttf", ".otf", ".suprx"}
BINARY_OK_EXT = {".png", ".jpg", ".jpeg", ".webp", ".ico", ".icns", ".jar", ".ttf", ".otf"}


def git(*args, check=True):
    r = subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True)
    if check and r.returncode != 0:
        sys.exit("[prepublish] git %s failed: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout if r.returncode == 0 else None


def ref_exists(ref):
    return git("rev-parse", "--verify", "--quiet", ref + "^{commit}", check=False) is not None


def private_words():
    """internal/prepublish_words.txt of this checkout, else of the main checkout (a worktree shares its .git)."""
    common = git("rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip()
    for base in (ROOT, os.path.dirname(common)):
        path = os.path.join(base, "internal", "prepublish_words.txt")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                return [w.strip() for w in f if w.strip() and not w.startswith("#")]
    return []


def upstream_blobs(base):
    """{path: blob sha} of the base (upstream's tree): its files are upstream's business, not ours to vet."""
    out = git("ls-tree", "-r", "-z", base, check=False) or b""
    blobs = {}
    for entry in out.split(b"\0"):
        if b"\t" in entry:
            meta, path = entry.split(b"\t", 1)
            blobs[path.decode(errors="replace")] = meta.split()[2].decode()
    return blobs


def check_paths(paths, where, findings):
    for p in paths:
        for rx, why in FORBIDDEN_PATH:
            if rx.search(p):
                findings.append("%s: %s - %s" % (where, p, why))
                break


def check_text(path, data, email, words, findings):
    rel = path
    ext = os.path.splitext(path)[1].lower()
    if ext in TEXT_EXT_SKIP:
        return
    for m in HOME_PATH.finditer(data):
        line = data.count(b"\n", 0, m.start()) + 1
        findings.append("%s:%d: a home-folder path (%s): write it relative to the repository or with ~"
                        % (rel, line, m.group(0).decode(errors="replace")))
    low = data.lower()
    if email and email.lower().encode() in low:
        line = data.count(b"\n", 0, low.index(email.lower().encode())) + 1
        findings.append("%s:%d: your git e-mail address" % (rel, line))
    for w in words:
        i = low.find(w.lower().encode())
        if i >= 0:
            findings.append("%s:%d: a word from internal/prepublish_words.txt (%r)"
                            % (rel, data.count(b"\n", 0, i) + 1, w))
    if ext in (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl") and not path.startswith("src/"):
        m = FUN_DEF.search(data)
        if m:
            findings.append("%s:%d: a recompiled function's definition outside src/ (generated code?)"
                            % (rel, data.count(b"\n", 0, m.start()) + 1))


def check_binary(path, data, sha, upstream, findings):
    if upstream.get(path) == sha:
        return  # unchanged from upstream
    head = data[:4]
    if head == b"\x7fELF":
        findings.append("%s: an ELF file (an executable or library)" % path)
    elif len(data) > 0x8006 and data[0x8001:0x8006] == b"CD001":
        findings.append("%s: an ISO 9660 image" % path)
    elif b"\0" in data[:8192] and os.path.splitext(path)[1].lower() not in BINARY_OK_EXT:
        findings.append("%s: a binary file upstream doesn't have (%d bytes): what is it?" % (path, len(data)))


def check_apk(apk, findings):
    """A release APK: the runtime only. No game library inside, and libmain.so without recompiled functions."""
    try:
        z = zipfile.ZipFile(apk)
    except (OSError, zipfile.BadZipFile) as e:
        findings.append("%s: can't read it (%s)" % (apk, e))
        return
    with z:
        names = z.namelist()
        for n in names:
            low = n.lower()
            if low.endswith("libvcs_game.so") or low.endswith("lossless.dll") or low.endswith(".iso") \
                    or "slus_" in low or "/generated/" in low:
                findings.append("%s: %s must not be in a published APK" % (apk, n))
        for n in names:
            if n.startswith("lib/") and n.endswith("libmain.so"):
                data = z.read(n)
                # the generated functions' names (FUN_00xxxxxx_0x...): present only when the game is linked in
                hits = len(re.findall(rb"FUN_00[0-9a-f]{6}_0x[0-9a-f]+", data))
                if hits > 50:
                    findings.append("%s: %s has %d recompiled function names: a personal build (game inside), "
                                    "never publish it" % (apk, n, hits))
        if not any(n.endswith("libmain.so") for n in names):
            findings.append("%s: no libmain.so: is this the app's APK?" % apk)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="commits since this ref are checked (default: upstream/main, else main)")
    ap.add_argument("--apk", action="append", default=[], help="an APK to check as well (repeatable)")
    a = ap.parse_args()

    base = a.base or next((r for r in ("upstream/main", "main") if ref_exists(r)), None)
    if base and not ref_exists(base):
        sys.exit("[prepublish] no such ref: " + base)
    email = (git("config", "user.email", check=False) or b"").decode().strip()
    words = private_words()
    upstream = upstream_blobs(base) if base else {}
    findings = []

    # 1. the tracked files as they are in the index (what the next commit holds)
    entries = {}
    for entry in git("ls-files", "-s", "-z").split(b"\0"):
        if b"\t" in entry:
            meta, path = entry.split(b"\t", 1)
            mode, sha = meta.split()[0].decode(), meta.split()[1].decode()
            if mode != "160000":  # a submodule is a link, not content
                entries[path.decode(errors="replace")] = sha
    check_paths([p for p, sha in entries.items() if upstream.get(p) != sha], "tracked", findings)
    for path, sha in sorted(entries.items()):
        if upstream.get(path) == sha:
            continue  # upstream's file, unchanged: published by upstream already
        data = git("cat-file", "blob", sha)
        check_binary(path, data, sha, upstream, findings)
        check_text(path, data, email, words, findings)

    # 2. the history this fork adds: every path any of its commits had, and the messages
    if base:
        revs = git("rev-list", base + "..HEAD").decode().split()
        seen = set(entries.items())
        for rev in revs:
            # added or changed files with their new blob (deletions publish nothing; merges: against the first parent)
            raw = git("diff-tree", "--no-commit-id", "-r", "-z", "--root", "-m", "--first-parent",
                      "--diff-filter=d", rev).split(b"\0")
            new = []
            for meta, path in zip(raw[0::2], raw[1::2]):
                if not meta.startswith(b":"):
                    continue
                p, sha = path.decode(errors="replace"), meta.split()[3].decode()
                if (p, sha) not in seen and upstream.get(p) != sha:
                    seen.add((p, sha))
                    new.append(p)
            check_paths(new, "history (%s)" % rev[:9], findings)
            msg = git("log", "-1", "--format=%B", rev)
            for m in HOME_PATH.finditer(msg):
                findings.append("commit %s message: a home-folder path (%s)" % (rev[:9], m.group(0).decode(errors="replace")))
            for w in words:
                if w.lower().encode() in msg.lower():
                    findings.append("commit %s message: a word from internal/prepublish_words.txt (%r)" % (rev[:9], w))

    for apk in a.apk:
        check_apk(apk, findings)

    for f in findings:
        print("[prepublish] " + f)
    print("[prepublish] %s: %d tracked files, %s, %d private words" % (
        "FAILED (%d findings)" % len(findings) if findings else "ok", len(entries),
        "commits since " + base if base else "no base: history not checked", len(words)))
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
