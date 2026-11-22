#!/usr/bin/env python3
"""[android] Fetch the tools the Android game-library build needs, so a PC needs no manual SDK install
(docs/ANDROID.md, "PC builder: Windows vs macOS").

    python3 tools/android.py setup [--apk] [--yes] [--accept-android-licenses] [--dry-run]
    python3 tools/toolchain_fetch.py ...          # the same

What it fetches, only what tools/preflight.py finds missing, all pinned:
  - a JDK 17 (Eclipse Temurin 17.0.18+8, from Adoptium's GitHub releases, SHA-256 checked): sdkmanager runs on it, and
    the APK's Gradle (--apk) needs it
  - the Android command-line tools (Google's, from dl.google.com, SHA-256 as developer.android.com/studio lists it),
    then through Google's own sdkmanager: NDK 28.2.13676358 and CMake 3.31.5 (sdkmanager checks those itself). The SDK
    licences are Google's: sdkmanager shows each one and YOU accept it (in a terminal: type y; elsewhere
    --accept-android-licenses, which still prints every licence). Never accepted silently.
  - glslangValidator (KhronosGroup glslang 16.6.0 release, SHA-256 checked)
  - Windows only: git (MinGit 2.56.1 from git-for-windows' releases, SHA-256 checked). macOS: git comes with Apple's
    Command Line Tools (xcode-select --install); Linux: the distribution's package. Python itself must exist: this
    tool runs on it.

Into ONE folder per user, ~/.vcs-toolchain (Windows: %USERPROFILE%\\.vcs-toolchain), or VCS_ANDROID_TOOLCHAIN: no admin
rights, nothing written anywhere else, delete the folder to undo. tools/preflight.py and tools/android.py look there,
so after a fetch the build just works. The project ships none of these tools: this machine downloads them from their
official sources after the user said yes (listing each download and its size first).

Downloads go to <folder>/downloads/<name>.part (resumed when a run was interrupted), are checked against the pinned
SHA-256 (a mismatch: deleted and refused), then renamed and unpacked into <name>.unpack and moved into place.
Interactive: asks "[y/N]". --yes: no question (the launcher, scripts); without a terminal and without --yes it refuses.
Exit codes: 0 everything there, 1 something still missing / failed, 2 not confirmed (nothing downloaded).
"""
import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import preflight  # noqa: E402

NDK_VERSION = preflight.NDK_VERSION
CMAKE_VERSION = preflight.CMAKE_VERSION
SDK_PACKAGES = ("ndk;" + NDK_VERSION, "cmake;" + CMAKE_VERSION)
LICENSES_URL = "https://developer.android.com/studio/terms"
REPOSITORY = "https://dl.google.com/android/repository/"

# The pinned downloads: host key -> (url, sha256, size in bytes, the archive's single top folder or "" for none).
# Host keys: windows-x64, mac-arm64, mac-x64, linux-x64, linux-arm64. Windows on Arm takes the x64 builds (emulated).
# Hashes: Temurin's own *.sha256.txt (= GitHub's asset digest), developer.android.com/studio's table for the
# command-line tools, GitHub's asset digests for glslang and MinGit (checked 2026-11-21).
_TEMURIN = "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.18%2B8/OpenJDK17U-jdk_"
JDK = {
    "windows-x64": (_TEMURIN + "x64_windows_hotspot_17.0.18_8.zip",
                    "c24e6215fa75f861d2a900ceec67c620cd2eb6dfb81541087d5981293b16f0e5", 190661454),
    "mac-arm64": (_TEMURIN + "aarch64_mac_hotspot_17.0.18_8.tar.gz",
                  "d81de06d938384fe76c4aa3c13395933aa11e2d19b0428743f810db06b05e312", 185658966),
    "mac-x64": (_TEMURIN + "x64_mac_hotspot_17.0.18_8.tar.gz",
                "47c2a8a62d6ba44bf46f2e468f9b3f26ad4b0bf6a51544e29c4520c18e709744", 180379640),
    "linux-x64": (_TEMURIN + "x64_linux_hotspot_17.0.18_8.tar.gz",
                  "0c94cbb54325c40dcf026143eb621562017db5525727f2d9131a11250f72c450", 193062224),
    "linux-arm64": (_TEMURIN + "aarch64_linux_hotspot_17.0.18_8.tar.gz",
                    "592a6702b3a07a0e0b82cb38aaab149bfce1b0c24d6b57ddb410bd9009333095", 191707249),
}
CMDLINE_TOOLS = {  # unpacks to cmdline-tools/ -> <sdk>/cmdline-tools/latest
    "windows-x64": (REPOSITORY + "commandlinetools-win-15859902_latest.zip",
                    "90ae805d20434428bffcb699c290860f19bb5f66a67e6b330067e3de801fb04a", 155655386),
    "mac-arm64": (REPOSITORY + "commandlinetools-mac_arm64-15859902_latest.zip",
                  "835b62a26162b229b441d1f6d4680383815a270809eb33522c0d480fa5002c4e", 156083281),
    "mac-x64": (REPOSITORY + "commandlinetools-mac_x86_64-15859902_latest.zip",
                "c5a6378ab5cf7e0d5701921405115befff13e9ff7417fb588389338f8bd050f3", 156281494),
    "linux-x64": (REPOSITORY + "commandlinetools-linux-15859902_latest.zip",
                  "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583", 181833628),
}
CMDLINE_TOOLS["linux-arm64"] = CMDLINE_TOOLS["linux-x64"]  # Java: the same package (the NDK has no arm64 Linux host)
_GLSLANG = "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-"
GLSLANG = {  # bin/glslang (+ glslangValidator, a symlink in the .tar.gz: the .zip would lose it)
    "windows-x64": (_GLSLANG + "windows-x86_64-release.zip",
                    "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b", 13734779),
    "mac-arm64": (_GLSLANG + "macos-universal-release.tar.gz",
                  "4f8c05f2888a73ed0b7b274bc524af9130e09ae94b43c0f3f9ed83cfae852834", 14356510),
    "linux-x64": (_GLSLANG + "linux-x86_64-release.tar.gz",
                  "2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce", 7923164),
}
GLSLANG["mac-x64"] = GLSLANG["mac-arm64"]  # universal
MINGIT = {  # Windows only: cmd/git.exe
    "windows-x64": ("https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/MinGit-2.56.1-64-bit.zip",
                    "4a92850b6e917da4fd010669b614b55dccf5b0ec333a534cc4be4700dfefdafd", 39849768),
}
# sdkmanager's own downloads (repository2-3.xml, 2026-11-21; sdkmanager checks their SHA-1 itself): for the
# size shown before asking and for --dry-run. sdk host: windows / macosx / linux.
SDK_ARCHIVES = {
    "ndk;" + NDK_VERSION: {"windows": ("android-ndk-r28c-windows.zip", 748118221),
                           "macosx": ("android-ndk-r28c-darwin.zip", 952495160),
                           "linux": ("android-ndk-r28c-linux.zip", 722261334)},
    "cmake;" + CMAKE_VERSION: {"windows": ("cmake-3.31.5-windows.zip", 20465729),
                               "macosx": ("cmake-3.31.5-darwin.zip", 42792306),
                               "linux": ("cmake-3.31.5-linux.zip", 30482790)},
}
# about what each needs on disk once unpacked (MB; the download is deleted afterwards)
UNPACKED_MB = {"jdk": 330, "cmdline-tools": 170, "ndk;" + NDK_VERSION: 2900, "cmake;" + CMAKE_VERSION: 120,
               "glslang": 60, "mingit": 110}
WIN_TOOLCHAIN_MAX = 80  # the NDK's deepest file is ~180 characters below the folder: keep the folder under 259 - 180


def host_key(host):
    arch = "arm64" if host.arm64 else "x64"
    if host.windows:
        return "windows-x64"
    if host.mac:
        return "mac-" + arch
    return "linux-" + arch


def sdk_host(host):
    return "windows" if host.windows else "macosx" if host.mac else "linux"


class FetchError(Exception):
    """A download or install step failed; the message says what to do."""


class Item:
    """One thing to fetch: kind "download" (url, sha256, size, unpacked into dest) or "sdk" (sdkmanager packages)."""

    def __init__(self, key, title, size, url=None, sha256=None, dest=None, packages=(), why=""):
        self.key, self.title, self.size, self.url, self.sha256, self.dest = key, title, size, url, sha256, dest
        self.packages, self.why = tuple(packages), why

    @property
    def disk_mb(self):
        if self.packages:
            return sum(UNPACKED_MB.get(p, 200) for p in self.packages) + (self.size >> 20)
        return UNPACKED_MB.get(self.key, 200) + (self.size >> 20)

    def __repr__(self):
        return "Item(%s)" % self.key


def _download_item(key, title, table, host, dest, why):
    pin = table.get(host_key(host))
    if not pin:
        return None
    url, sha256, size = pin
    return Item(key, title, size, url=url, sha256=sha256, dest=dest, why=why)


def plan(host, apk=False, root=None):
    """(items to fetch, [(what, how)] that cannot be fetched here). Only what preflight would report missing."""
    paths = preflight.toolchain_paths(host)
    items, manual = [], []
    if tuple(host.python) < preflight.MIN_PYTHON:
        manual.append(("Python %d.%d or newer" % preflight.MIN_PYTHON, preflight._install_hint(
            host, "https://www.python.org/downloads/ or: winget install Python.Python.3.12", "brew install python",
            "apt install python3")))
    sdk = preflight.find_sdk(host)
    need_sdk = not (sdk and preflight.sdk_complete(host, sdk))
    tc_sdk = paths["sdk"]
    packages = []
    if need_sdk:
        if host.platform.startswith("linux") and host.arm64:
            manual.append(("Android NDK %s / CMake %s" % (NDK_VERSION, CMAKE_VERSION),
                           "the NDK has no arm64 Linux host build: build on an x86-64 PC or a Mac"))
            need_sdk = False
        else:
            exe = ".exe" if host.windows else ""
            if not host.exists(host.join(tc_sdk, "ndk", NDK_VERSION)):
                packages.append("ndk;" + NDK_VERSION)
            cmake_bin = host.join(tc_sdk, "cmake", CMAKE_VERSION, "bin")
            if not (host.exists(host.join(cmake_bin, "cmake" + exe)) and host.exists(host.join(cmake_bin, "ninja" + exe))):
                packages.append("cmake;" + CMAKE_VERSION)
    # sdkmanager is a Java program: a JDK 17 is needed to install SDK packages, and for the APK's Gradle
    java = preflight._find_java(host)
    java_ok = bool(java) and (host.java_major(java) or 0) >= preflight.MIN_JDK
    if (apk or packages) and not java_ok:
        item = _download_item("jdk", "JDK 17 (Eclipse Temurin 17.0.18)", JDK, host, paths["jdk"],
                              "sdkmanager and Gradle run on it")
        if item:
            items.append(item)
        else:
            manual.append(("a JDK 17", "https://adoptium.net/temurin/releases/?version=17"))
    if packages:
        sdkmanager = sdkmanager_path(host, tc_sdk)
        if not host.exists(sdkmanager):
            items.append(_download_item("cmdline-tools", "Android SDK command-line tools (sdkmanager)", CMDLINE_TOOLS,
                                        host, host.join(tc_sdk, "cmdline-tools", "latest"), "installs the NDK and CMake"))
        size = sum(SDK_ARCHIVES[p][sdk_host(host)][1] for p in packages)
        items.append(Item("sdk-packages", "Android NDK %s and CMake %s (through sdkmanager)" % (NDK_VERSION, CMAKE_VERSION)
                          if len(packages) == 2 else packages[0].replace(";", " ") + " (through sdkmanager)",
                          size, dest=tc_sdk, packages=packages, why="compile the game library for the phone"))
    if not preflight.find_glslang(host, root):
        item = _download_item("glslang", "glslangValidator (glslang 16.6.0)", GLSLANG, host,
                              host.join(paths["dir"], "glslang"), "compiles the Vulkan shaders")
        if item:
            items.append(item)
        else:
            manual.append(("glslangValidator", "apt install glslang-tools  (or VCS_GLSLANG=<path>)"))
    if not preflight.find_git(host):
        if host.windows:
            items.append(_download_item("mingit", "git (MinGit 2.56.1)", MINGIT, host,
                                        host.join(paths["dir"], "mingit"), "fetches SDL3 and the other dependencies"))
        else:
            manual.append(("git", preflight._install_hint(host, "", "xcode-select --install  (Apple's Command Line "
                                                          "Tools; or brew install git)", "apt install git")))
    if host.windows and items and len(paths["dir"]) > WIN_TOOLCHAIN_MAX:
        raise FetchError("the toolchain folder %s is too long for Windows' 260-character path limit (the NDK's files "
                         "are up to ~180 characters below it). Set %s to a short folder, for example C:\\vcs-tools, "
                         "and run setup again." % (paths["dir"], preflight.TOOLCHAIN_ENV))
    # the small, independent downloads first; the SDK packages (licences, the big NDK) last
    order = ("glslang", "mingit", "jdk", "cmdline-tools", "sdk-packages")
    items.sort(key=lambda it: order.index(it.key))
    return items, manual


def sdkmanager_path(host, sdk):
    return host.join(sdk, "cmdline-tools", "latest", "bin", "sdkmanager" + (".bat" if host.windows else ""))


def _mb(n):
    return "%d MB" % max(1, (n + (1 << 19)) >> 20)


def describe(items, manual, host, dry_run=False):
    """The text shown before asking: each download, its size and source, the total, the folder."""
    lines = []
    if items:
        lines.append("[setup] to download into %s:" % preflight.toolchain_dir(host))
        for i, it in enumerate(items, 1):
            lines.append("  %d. %s, %s (%s)" % (i, it.title, _mb(it.size), it.why))
            if it.url:
                lines.append("     from %s" % it.url)
            for p in it.packages:
                name, size = SDK_ARCHIVES[p][sdk_host(host)]
                lines.append("     %s: %s%s, %s (Google's SDK repository, through sdkmanager)" % (p, REPOSITORY, name, _mb(size)))
        total = sum(it.size for it in items)
        lines.append("  total: about %s to download, about %s on disk" % (_mb(total), _mb(sum(it.disk_mb for it in items) << 20)))
        if any(it.packages for it in items):
            lines.append("  The Android SDK packages come under Google's licences (%s): sdkmanager shows each one "
                         "and you accept it." % LICENSES_URL)
    if manual:
        lines.append("[setup] cannot be fetched here, install it yourself:")
        for what, how in manual:
            lines.append("  - %s: %s" % (what, how))
    if dry_run:
        lines.append("[setup] --dry-run: nothing downloaded")
    return "\n".join(lines)


# --------------------------------------------------------------------------------------------- downloading

def _urlopen(url, offset):
    """(stream, total size, resumed): the response from `offset` on (a Range request), or from 0 if the server ignores
    the range."""
    import urllib.request
    req = urllib.request.Request(url, headers={"User-Agent": "vcs-toolchain-fetch"})
    if offset:
        req.add_header("Range", "bytes=%d-" % offset)
    r = urllib.request.urlopen(req, timeout=120)
    resumed = offset > 0 and getattr(r, "status", 200) == 206
    length = int(r.headers.get("Content-Length") or 0)
    return r, (length + offset if resumed else length), resumed


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, sha256, target, opener=_urlopen, out=print):
    """`url` -> `target`, checked against `sha256`. Written to target.part (an interrupted run continues from it),
    renamed only when the hash matches; a mismatch deletes the part file and raises FetchError."""
    if os.path.isfile(target) and sha256_file(target) == sha256:
        return target
    part = target + ".part"
    os.makedirs(os.path.dirname(target), exist_ok=True)
    offset = os.path.getsize(part) if os.path.isfile(part) else 0
    try:
        stream, total, resumed = opener(url, offset)
    except Exception as e:  # noqa: BLE001 - urllib raises many kinds
        raise FetchError("downloading %s failed (%s): check the internet connection and run setup again (it "
                         "continues where it stopped)" % (url, e))
    if offset and not resumed:
        offset = 0
    name = url.rsplit("/", 1)[-1]
    out("[setup] downloading %s%s" % (name, " (continuing at %s)" % _mb(offset) if offset else ""))
    done, shown = offset, -1
    try:
        with stream, open(part, "ab" if offset else "wb") as f:
            while True:
                block = stream.read(1 << 20)
                if not block:
                    break
                f.write(block)
                done += len(block)
                pct = done * 100 // total if total else 0
                if total and pct // 10 != shown:
                    shown = pct // 10
                    out("[setup]   %s: %d%% of %s" % (name, pct, _mb(total)))
    except Exception as e:  # noqa: BLE001
        raise FetchError("downloading %s stopped (%s): run setup again, it continues where it stopped" % (url, e))
    got = sha256_file(part)
    if got != sha256:
        os.remove(part)
        raise FetchError("%s is not the file it should be (SHA-256 %s, expected %s): deleted and refused. Try again; a "
                         "proxy or antivirus may have changed it" % (name, got, sha256))
    os.replace(part, target)
    return target


def _extract_zip(archive, dest):
    """zipfile.extractall keeps no file modes and no symlinks: put both back (Unix archives)."""
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            mode = info.external_attr >> 16
            out = os.path.realpath(os.path.join(dest, info.filename))
            if not out.startswith(os.path.realpath(dest) + os.sep) and out != os.path.realpath(dest):
                raise FetchError("%s holds a path outside its folder: %s" % (archive, info.filename))
            if os.name != "nt" and stat.S_ISLNK(mode):
                target = z.read(info).decode("utf-8")
                if os.path.isabs(target) or ".." in target.replace("\\", "/").split("/"):
                    raise FetchError("%s holds a link outside its folder: %s" % (archive, info.filename))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                os.symlink(target, out)
                continue
            z.extract(info, dest)
            if os.name != "nt" and mode & 0o777 and not info.is_dir():
                os.chmod(out, mode & 0o777)


def unpack(archive, dest, strip_top=False):
    """`archive` into `dest`, atomically: unpacked into dest.unpack, then moved to `dest` (strip_top: the archive's
    single top folder is moved instead, e.g. jdk-17.0.18+8/ or cmdline-tools/). An old `dest` is replaced."""
    tmp = dest + ".unpack"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    if archive.endswith(".zip"):
        _extract_zip(archive, tmp)
    else:
        with tarfile.open(archive) as t:
            if hasattr(tarfile, "data_filter"):
                t.extractall(tmp, filter="data")
            else:
                for m in t.getmembers():
                    if m.name.startswith(("/", "..")) or ".." in m.name.split("/"):
                        raise FetchError("%s holds a path outside its folder: %s" % (archive, m.name))
                t.extractall(tmp)
    source = tmp
    if strip_top:
        inner = os.listdir(tmp)
        if len(inner) != 1 or not os.path.isdir(os.path.join(tmp, inner[0])):
            raise FetchError("%s does not hold the one folder it should" % os.path.basename(archive))
        source = os.path.join(tmp, inner[0])
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    os.replace(source, dest)
    shutil.rmtree(tmp, ignore_errors=True)
    return dest


def fetch_download(item, host, opener=_urlopen, out=print):
    downloads = os.path.join(preflight.toolchain_dir(host), "downloads")
    archive = download(item.url, item.sha256, os.path.join(downloads, item.url.rsplit("/", 1)[-1]), opener, out)
    out("[setup] unpacking %s" % os.path.basename(archive))
    dest = item.dest
    if item.key == "jdk" and host.mac:  # the bundle's top folder is jdk-17.0.18+8 (Contents/Home inside)
        dest = os.path.dirname(os.path.dirname(dest))
    unpack(archive, dest, strip_top=item.key in ("jdk", "cmdline-tools"))
    os.remove(archive)
    out("[setup] ok: %s in %s" % (item.title, dest))


# --------------------------------------------------------------------------------------------- the SDK packages

def _licenses_accepted(sdk):
    return os.path.isfile(os.path.join(sdk, "licenses", "android-sdk-license"))


def install_sdk_packages(item, host, interactive, accept_licenses, java_home, runner=subprocess.run, out=print):
    """Google's sdkmanager installs the NDK and CMake into the toolchain SDK. The licences: in a terminal sdkmanager
    asks for each one; with --accept-android-licenses its texts are printed and answered y; otherwise refused."""
    sdk = item.dest
    sdkmanager = sdkmanager_path(host, sdk)
    env = dict(os.environ)
    if java_home:
        env["JAVA_HOME"] = java_home
    # sdkmanager keeps its cache and settings in ~/.android by default: inside the toolchain folder instead
    env["ANDROID_USER_HOME"] = os.path.join(preflight.toolchain_dir(host), "android-user-home")
    if not _licenses_accepted(sdk):
        if interactive and not accept_licenses:
            out("[setup] sdkmanager now shows the Android SDK licences (%s). Read each one and type y to accept it; "
                "nothing is installed if you decline." % LICENSES_URL)
            runner([sdkmanager, "--sdk_root=" + sdk, "--licenses"], env=env)
        elif accept_licenses:
            out("[setup] --accept-android-licenses: the Android SDK licences follow (%s); they are accepted on your "
                "behalf as you asked." % LICENSES_URL)
            runner([sdkmanager, "--sdk_root=" + sdk, "--licenses"], env=env, input="y\n" * 30, text=True)
        else:
            raise FetchError("the Android SDK licences have to be accepted by you: run %s in a terminal, or read them "
                             "(%s) and add --accept-android-licenses" % (preflight.setup_command(host), LICENSES_URL))
        if not _licenses_accepted(sdk):
            raise FetchError("the Android SDK licence was not accepted: nothing installed (the NDK needs it)")
    out("[setup] sdkmanager: installing %s (it downloads and checks them itself)" % " ".join(item.packages))
    r = runner([sdkmanager, "--sdk_root=" + sdk] + list(item.packages), env=env, stdin=subprocess.DEVNULL)
    if getattr(r, "returncode", 0) != 0:
        raise FetchError("sdkmanager failed (%s): see its message above, then run setup again" % r.returncode)
    if not preflight.sdk_complete(host, sdk):
        raise FetchError("sdkmanager finished but %s has no NDK %s / CMake %s" % (sdk, NDK_VERSION, CMAKE_VERSION))
    out("[setup] ok: NDK %s and CMake %s in %s" % (NDK_VERSION, CMAKE_VERSION, sdk))


# --------------------------------------------------------------------------------------------- setup

def confirm(question, yes, interactive, ask=input):
    """True to go on. --yes: yes. A terminal: asks (only y / yes is yes). Neither: no."""
    if yes:
        return True
    if not interactive:
        return False
    try:
        return ask(question).strip().lower() in ("y", "yes")
    except EOFError:
        return False


def is_interactive():
    return sys.stdin is not None and sys.stdin.isatty() and os.environ.get("VCS_PROGRESS") != "json"


def setup(host=None, apk=False, root=None, yes=False, accept_licenses=False, dry_run=False, interactive=None,
          ask=input, out=print, opener=_urlopen, runner=subprocess.run):
    """Fetch what is missing. Returns 0 (all there), 1 (something still missing or failed), 2 (not confirmed)."""
    host = host or preflight.Host()
    interactive = is_interactive() if interactive is None else interactive
    try:
        items, manual = plan(host, apk, root)
    except FetchError as e:
        out("[setup] " + str(e))
        return 1
    if not items and not manual:
        out("[setup] nothing to fetch: everything the build needs is here")
        return 0
    out(describe(items, manual, host, dry_run))
    if dry_run or not items:
        return 0 if dry_run else 1
    sdk_items = [it for it in items if it.packages]
    if sdk_items and not interactive and not accept_licenses and not _licenses_accepted(sdk_items[0].dest):
        # refused before anything is downloaded: the licences need a person (a terminal) or the explicit flag
        out("[setup] the Android SDK licences have to be accepted by you: run %s in a terminal, or read them (%s) and "
            "add --accept-android-licenses. Nothing downloaded." % (preflight.setup_command(host), LICENSES_URL))
        return 2
    if not confirm("Download these into %s? [y/N] " % preflight.toolchain_dir(host), yes, interactive, ask):
        if not interactive and not yes:
            out("[setup] not run in a terminal: add --yes to download without the question. Nothing downloaded.")
        else:
            out("[setup] nothing downloaded.")
        return 2
    try:
        free_mb = _free_mb(preflight.toolchain_dir(host))
        need_mb = sum(it.disk_mb for it in items) + max(it.size for it in items) // (1 << 20)
        if free_mb is not None and free_mb < need_mb:
            raise FetchError("not enough free space for %s: about %d MB needed, %d MB free" % (
                preflight.toolchain_dir(host), need_mb, free_mb))
        for it in items:
            if it.packages:
                install_sdk_packages(it, host, interactive, accept_licenses, java_home_for(host), runner, out)
            else:
                fetch_download(it, host, opener, out)
    except FetchError as e:
        out("[setup] " + str(e))
        return 1
    if manual:
        out("[setup] fetched; still missing (install it yourself): " + ", ".join(w for w, _ in manual))
        return 1
    out("[setup] done: the build finds these in %s by itself" % preflight.toolchain_dir(host))
    return 0


def java_home_for(host):
    """JAVA_HOME for sdkmanager: the folder of the java preflight picks when it is JAVA_HOME's or the toolchain's; None
    (keep the environment's) for a java found on PATH (macOS' /usr/bin/java is a stub, not a JDK folder)."""
    java = preflight._find_java(host)
    if not java:
        return None
    exe = "java.exe" if host.windows else "java"
    for home in (host.env.get("JAVA_HOME"), preflight.toolchain_paths(host)["jdk"]):
        if home and java == host.join(home, "bin", exe):
            return home
    return None


def _free_mb(path):
    while path and not os.path.isdir(path):
        parent = os.path.dirname(path)
        if parent == path:
            return None
        path = parent
    try:
        return shutil.disk_usage(path).free >> 20
    except OSError:
        return None


def apply_env(env=None, host=None):
    """Point this process (and the tools it starts) at the toolchain folder: its bin folders appended to PATH (the
    user's own tools still win), JAVA_HOME at its JDK when no JDK 17 is found otherwise. The SDK needs nothing here:
    preflight.find_sdk prefers a complete one, and tools/android.py takes its SDK from there."""
    env = os.environ if env is None else env
    host = host or preflight.Host(env=env)
    paths = preflight.toolchain_paths(host)
    extra = [host.join(paths["dir"], "glslang", "bin")]
    if host.windows:
        extra.append(host.join(paths["dir"], "mingit", "cmd"))
    sep = ";" if host.windows else ":"
    current = env.get("PATH", "").split(sep) if env.get("PATH") else []
    for d in extra:
        if host.exists(d) and d not in current:
            current.append(d)
    env["PATH"] = sep.join(current)
    jdk_java = host.join(paths["jdk"], "bin", "java.exe" if host.windows else "java")
    if host.exists(jdk_java):
        java = preflight._find_java(host)
        if java == jdk_java or not java or (host.java_major(java) or 0) < preflight.MIN_JDK:
            env["JAVA_HOME"] = paths["jdk"]
    return env


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    a = ap.parse_args(argv)
    sys.exit(run_from_args(a))


def add_arguments(ap):
    ap.add_argument("--apk", action="store_true", help="also what the APK build needs (a JDK 17)")
    ap.add_argument("--yes", action="store_true", help="download without asking (the launcher, scripts)")
    ap.add_argument("--accept-android-licenses", action="store_true",
                    help="accept Google's Android SDK licences (printed in full) without the question; read them first: "
                         + LICENSES_URL)
    ap.add_argument("--dry-run", action="store_true", help="only list what would be downloaded (URLs and sizes)")


def run_from_args(a):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return setup(apk=a.apk, root=root, yes=a.yes, accept_licenses=a.accept_android_licenses, dry_run=a.dry_run)


if __name__ == "__main__":
    main()
