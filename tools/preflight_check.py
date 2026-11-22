#!/usr/bin/env python3
"""[android] Self-test of tools/preflight.py with simulated machines and a synthetic tiny ISO 9660 image (no game data):

    python3 tools/preflight_check.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import android  # noqa: E402
import preflight  # noqa: E402

SECTOR = 2048


def _rec(name, lba, size, is_dir=False):
    nm = name.encode("ascii")
    n = 33 + len(nm)
    n += n % 2
    r = bytearray(n)
    r[0] = n
    struct.pack_into("<I", r, 2, lba)
    struct.pack_into(">I", r, 6, lba)
    struct.pack_into("<I", r, 10, size)
    struct.pack_into(">I", r, 14, size)
    r[25] = 2 if is_dir else 0
    r[32] = len(nm)
    r[33:33 + len(nm)] = nm
    return bytes(r)


def make_iso(path, boot, cnf=True):
    """A tiny ISO 9660 image: SYSTEM.CNF naming `boot`, and a dummy file of that name."""
    files = []
    if cnf:
        files.append(("SYSTEM.CNF;1", ("BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\n" % boot).encode()))
    files.append((boot + ";1", b"not a real executable"))
    img = bytearray(SECTOR * (19 + len(files)))
    root = bytearray()
    root += _rec("\x00", 18, SECTOR, True) + _rec("\x01", 18, SECTOR, True)
    for i, (n, data) in enumerate(files):
        root += _rec(n, 19 + i, len(data))
        img[(19 + i) * SECTOR:(19 + i) * SECTOR + len(data)] = data
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[40:72] = b"TESTDISC".ljust(32)
    pvd[156:190] = _rec("\x00", 18, SECTOR, True)[:34]
    img[16 * SECTOR:17 * SECTOR] = pvd
    with open(path, "wb") as f:
        f.write(img)


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def iso(self, boot, **kw):
        p = os.path.join(self.dir.name, boot.replace(".", "_") + ".iso")
        make_iso(p, boot, **kw)
        return p

    def test_vcs_lcs_detected(self):
        self.assertEqual(preflight.identify_disc(self.iso("SLUS_215.90"))[0], "vcs")
        self.assertEqual(preflight.identify_disc(self.iso("SLUS_214.23"))[0], "lcs")

    def test_auto_pick(self):
        self.assertEqual(preflight.resolve_game(self.iso("SLUS_214.23"), None), ("lcs", ""))
        self.assertEqual(preflight.resolve_game(self.iso("SLUS_215.90"), ""), ("vcs", ""))
        self.assertEqual(preflight.game_from_args([self.iso("SLUS_214.23"), "-v"]), ("lcs", ""))
        self.assertEqual(preflight.game_from_args(["--out", "x", self.iso("SLUS_214.23")]), ("lcs", ""))

    def test_matching_game_given(self):
        self.assertEqual(preflight.resolve_game(self.iso("SLUS_214.23"), "lcs"), ("lcs", ""))

    def test_contradiction(self):
        game, err = preflight.resolve_game(self.iso("SLUS_214.23"), "vcs")
        self.assertIn("--game vcs, but this disc is Liberty City Stories (SLUS_214.23)", err)
        self.assertIn("--game lcs", err)
        game, err = preflight.game_from_args([self.iso("SLUS_215.90"), "--game", "lcs"])
        self.assertIn("this disc is Vice City Stories", err)

    def test_other_game(self):
        _, err = preflight.resolve_game(self.iso("SLES_123.45"), None)
        self.assertEqual(err, "this disc is SLES_123.45: expected VCS (SLUS_215.90) or LCS (SLUS_214.23)")
        _, err = preflight.resolve_game(self.iso("SLES_546.22"), None)
        self.assertIn("European release of Vice City Stories (SLES_546.22): expected VCS", err)

    def test_not_a_disc(self):
        _, err = preflight.resolve_game(self.iso("SLUS_215.90", cnf=False), None)
        self.assertIn("no SYSTEM.CNF", err)
        junk = os.path.join(self.dir.name, "junk.iso")
        with open(junk, "wb") as f:
            f.write(b"x" * 40000)
        self.assertIn("not a readable ISO", preflight.resolve_game(junk, None)[1])
        self.assertEqual(preflight.resolve_game(os.path.join(self.dir.name, "none.iso"), None), ("", ""))
        self.assertEqual(preflight.resolve_game(None, "lcs"), ("lcs", ""))

    def test_recompile_cli(self):
        """recompile.py <iso>: the contradiction and other-game errors exit before any build starts."""
        script = os.path.join(HERE, "recompile.py")
        r = subprocess.run([sys.executable, script, self.iso("SLUS_214.23"), "--game", "vcs"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("[recompile] --game vcs, but this disc is Liberty City Stories", r.stderr)
        r = subprocess.run([sys.executable, script, self.iso("SLES_123.45")], capture_output=True, text=True)
        self.assertIn("expected VCS (SLUS_215.90) or LCS (SLUS_214.23)", r.stderr)


class PathTests(unittest.TestCase):
    def test_not_windows_never_complains(self):
        self.assertIsNone(preflight.path_problem("/" + "a" * 300, "apk", windows=False))

    def test_short_root_ok(self):
        for step in ("recompile", "build", "build-game", "apk"):
            self.assertIsNone(preflight.path_problem("C:\\gta", step, windows=True), step)

    def test_long_root_fails_with_advice(self):
        root = "D:\\work\\some\\long\\folder\\Downloads\\GTA-Stories-Recompiled-0.3.0-windows"
        msg = preflight.path_problem(root, "build-game", windows=True)
        self.assertIn("C:\\gta", msg)
        self.assertIn("260", msg)

    def test_estimate_matches_measured(self):
        """The deepest paths of real builds, measured 2026-11-21 under a 71-character root: 264 (game) / 268 (game-lcs, the template) and 291."""
        self.assertEqual(preflight.longest_path("x" * 71, "build-game")[0], 268)
        self.assertEqual(preflight.longest_path("x" * 71, "apk")[0], 291)

    def test_desktop_build_estimate(self):
        """The desktop tree's deepest file, measured 2026-11-22: 159 characters after '<build>/'; the estimate (179)
        covers it, and the repository's own folder here (51 characters, build/ inside) still fits."""
        n, _ = preflight.longest_path("x" * 10, "build", build_dir="b" * 41)
        self.assertGreaterEqual(n, 41 + 1 + 159)
        self.assertIsNone(preflight.path_problem("E:\\" + "p" * 48, "build", windows=True))

    @unittest.skipUnless(os.name == "nt", "a drive-letter build folder: os.path.abspath joins it to the current folder on macOS / Linux")
    def test_long_build_dir_names_the_build_folder(self):
        root = "C:\\gta"
        build = "D:\\" + "b" * 120
        msg = preflight.path_problem(root, "build", windows=True, build_dir=build)
        self.assertIn("VCS_BUILD_DIR", msg)
        self.assertIn("yours is %d" % len(build), msg)
        self.assertNotIn("at most about 0 ", msg)

    def test_require_exits(self):
        with self.assertRaises(SystemExit):
            preflight.require_short_path("C:\\" + "d" * 200, "apk", windows=True)

    def test_generated_object_paths_count(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "f" * 300 + ".cpp"), "w").close() if False else open(os.path.join(d, "f" * 100 + ".cpp"), "w").close()
            n1, _ = preflight.longest_path("C:\\gta", "build-game")
            n2, _ = preflight.longest_path("C:\\gta", "build-game", generated_dir=d)
            self.assertGreaterEqual(n2, n1)


def fake_host(platform="win32", have=(), env=None, which=(), python=(3, 12), java=None):
    have = set(have)
    env = dict({"USERPROFILE": "C:\\Users\\Public"}, **(env or {}))  # the toolchain folder: never this machine's home
    return preflight.Host(platform=platform, env=env, exists=lambda p: p in have,
                          which=lambda n: ("C:\\bin\\" + n) if n in which else None, python=python,
                          java_major=lambda j: java)


class ToolTests(unittest.TestCase):
    SDK = "D:\\work\\AppData\\Local\\Android\\Sdk"

    def full(self):
        n = preflight.NDK_VERSION
        c = preflight.CMAKE_VERSION
        return [self.SDK, self.SDK + "\\ndk\\" + n, self.SDK + "\\cmake\\%s\\bin\\cmake.exe" % c,
                self.SDK + "\\cmake\\%s\\bin\\ninja.exe" % c, "C:\\VulkanSDK\\Bin\\glslangValidator.exe",
                self.SDK + "\\cmdline-tools\\latest\\bin\\sdkmanager.bat"]

    def host(self, drop=(), **kw):
        have = [h for h in self.full() if h not in drop] + kw.pop("have", [])
        env = {"LOCALAPPDATA": "D:\\work\\AppData\\Local", "VULKAN_SDK": "C:\\VulkanSDK"}
        env.update(kw.pop("env", {}))
        return fake_host(have=have, env=env, which=kw.pop("which", ("git",)), **kw)

    def problems(self, host, **kw):
        # find_sdk uses os.path.isdir: patch it for the fake machine
        with unittest.mock.patch("os.path.isdir", lambda p: host.exists(p)):
            return preflight.missing_tools(host, **kw)

    def test_all_present(self):
        self.assertEqual(self.problems(self.host()), [])

    def test_everything_missing_one_message(self):
        h = fake_host(env={}, which=(), python=(3, 6))
        probs = self.problems(h, apk=True)
        text = preflight.format_problems(probs)
        for needle in ("Python 3.8", "Android SDK", "glslangValidator", "git", "JDK 17", "winget", "vulkan.lunarg.com"):
            self.assertIn(needle, text)
        self.assertEqual(len(probs), 5)  # python, sdk, glslang, git, jdk (NDK / CMake are judged inside an SDK)

    def test_missing_ndk_and_cmake_name_sdkmanager(self):
        h = self.host(drop=[self.SDK + "\\ndk\\" + preflight.NDK_VERSION,
                            self.SDK + "\\cmake\\%s\\bin\\ninja.exe" % preflight.CMAKE_VERSION])
        probs = self.problems(h)
        self.assertEqual(len(probs), 2)
        self.assertIn('sdkmanager.bat" "ndk;28.2.13676358" "cmake;3.31.5"', probs[0][1])

    def test_android_home_wins(self):
        h = fake_host(have=["D:\\sdk"], env={"ANDROID_HOME": "D:\\sdk"})
        with unittest.mock.patch("os.path.isdir", lambda p: h.exists(p)):
            self.assertEqual(preflight.find_sdk(h), "D:\\sdk")

    def test_jdk(self):
        java = "C:\\jdk\\bin\\java.exe"
        h = self.host(env={"JAVA_HOME": "C:\\jdk"}, java=11, have=[java])
        self.assertIn("found Java 11", self.problems(h, apk=True)[0][0])
        self.assertEqual(self.problems(h, apk=False), [])  # build-game needs no JDK
        h = self.host(env={"JAVA_HOME": "C:\\jdk"}, java=21, have=[java])
        self.assertEqual(self.problems(h, apk=True), [])

    def test_glslang_via_path(self):
        h = self.host(drop=["C:\\VulkanSDK\\Bin\\glslangValidator.exe"])
        self.assertEqual(len(self.problems(h)), 1)
        h = self.host(drop=["C:\\VulkanSDK\\Bin\\glslangValidator.exe"], which=("git", "glslangValidator"))
        self.assertEqual(self.problems(h), [])

    def test_version_constants_match_android_py(self):
        self.assertEqual((preflight.NDK_VERSION, preflight.CMAKE_VERSION), (android.NDK_VERSION, android.CMAKE_VERSION))

    def test_require_exits_nonzero_once(self):
        with self.assertRaises(SystemExit) as cm:
            with unittest.mock.patch("os.path.isdir", lambda p: False):
                preflight.require_tools(fake_host(env={}, which=()))
        self.assertEqual(cm.exception.code, 1)


# --------------------------------------------------------------------------------------------- Windows vs macOS (vs Linux)
# The PC builder must behave the same on every desktop: the same checks, the same setup offer, only the install hints
# and the path-length limit (Windows) differ. Each test runs on fake Windows, macOS and Linux machines.

PLATFORMS = ("win32", "darwin", "linux")


def platform_host(platform, tools=(), toolchain=(), python=(3, 12), java=17, machine="x86_64", env=None):
    """A fake machine. tools: what is installed the usual way ("sdk" = Android Studio's SDK with NDK + CMake,
    "glslang", "git", "java" on PATH). toolchain: what tools/toolchain_fetch.py put in the toolchain folder
    ("sdk", "glslang", "git", "jdk")."""
    windows = platform == "win32"
    home = {"win32": "C:\\Users\\Public", "darwin": "/Users/Shared", "linux": "/home/user"}[platform]
    base_env = {"USERPROFILE": home, "LOCALAPPDATA": home + "\\AppData\\Local"} if windows else {"HOME": home}
    base_env.update(env or {})
    have = set()
    on_path = {n for n in ("glslang", "git", "java") if n in tools}
    host = preflight.Host(platform=platform, env=base_env, exists=lambda p: p in have, python=python,
                          which=lambda n: (host.join(home, "bin", n + (".exe" if windows else "")) if
                                           (n == "glslangValidator" and "glslang" in on_path) or n in on_path else None),
                          java_major=lambda j: java, machine=machine)
    exe = ".exe" if windows else ""

    def add_sdk(sdk):
        cmake_bin = host.join(sdk, "cmake", preflight.CMAKE_VERSION, "bin")
        have.update([sdk, host.join(sdk, "ndk", preflight.NDK_VERSION), host.join(cmake_bin, "cmake" + exe),
                     host.join(cmake_bin, "ninja" + exe)])

    if "sdk" in tools:
        add_sdk(host.join(home + "\\AppData\\Local", "Android", "Sdk") if windows else
                host.join(home, "Library", "Android", "sdk") if platform == "darwin" else host.join(home, "Android", "Sdk"))
    paths = preflight.toolchain_paths(host)
    if "sdk" in toolchain:
        add_sdk(paths["sdk"])
    if "glslang" in toolchain:
        have.add(paths["glslang"][0])
    if "git" in toolchain and paths["git"]:
        have.add(paths["git"])
    if "jdk" in toolchain:
        have.add(host.join(paths["jdk"], "bin", "java" + exe))
    host.have = have
    return host


def names(problems):
    """The tool each problem is about, platform-neutral."""
    out = []
    for what, _ in problems:
        for key in ("Python", "Android SDK CMake", "Android SDK", "Android NDK", "glslangValidator", "git", "JDK"):
            if key in what:
                out.append(key)
                break
    return out


class ParityTests(unittest.TestCase):
    def test_same_missing_list_logic(self):
        """The same machine state gives the same list on every platform."""
        for tools in ((), ("sdk",), ("sdk", "git"), ("glslang", "git", "java"), ("sdk", "glslang", "git", "java")):
            for apk in (False, True):
                got = {p: names(preflight.missing_tools(platform_host(p, tools, java=17 if "java" in tools else None),
                                                        apk=apk)) for p in PLATFORMS}
                self.assertEqual(got["win32"], got["darwin"], (tools, apk))
                self.assertEqual(got["darwin"], got["linux"], (tools, apk))
        everything = names(preflight.missing_tools(platform_host("darwin", (), java=None, python=(3, 6)), apk=True))
        self.assertEqual(everything, ["Python", "Android SDK", "glslangValidator", "git", "JDK"])

    def test_install_hints_per_platform(self):
        hints = {}
        for p in PLATFORMS:
            probs = preflight.missing_tools(platform_host(p, (), java=None, python=(3, 6)), apk=True)
            hints[p] = " ".join(how for _, how in probs)
        self.assertIn("winget", hints["win32"])
        self.assertIn("vulkan.lunarg.com", hints["win32"])
        self.assertIn("brew install glslang", hints["darwin"])
        self.assertIn("xcode-select --install", hints["darwin"])
        self.assertIn("brew install --cask temurin@17", hints["darwin"])
        self.assertIn("apt install glslang-tools", hints["linux"])
        for p in ("darwin", "linux"):
            self.assertNotIn("winget", hints[p])

    def test_setup_is_the_first_hint(self):
        for p, cmd in (("win32", "python tools\\android.py setup"), ("darwin", "python3 tools/android.py setup"),
                       ("linux", "python3 tools/android.py setup")):
            h = platform_host(p)
            text = preflight.format_problems(preflight.missing_tools(h), h)
            self.assertEqual(text.splitlines()[1], "  Quickest: run: " + cmd, p)
            self.assertIn(preflight.toolchain_dir(h), text)

    def test_path_length_only_on_windows(self):
        root = {"win32": "D:\\" + "x" * 120, "darwin": "/Users/" + "x" * 120, "linux": "/home/" + "x" * 120}
        for p in PLATFORMS:
            problem = preflight.path_problem(root[p], "build-game", windows=platform_host(p).windows)
            if p == "win32":
                self.assertIn("260-character", problem)
            else:
                self.assertIsNone(problem, p)

    def test_toolchain_folder_picked_up(self):
        for p in PLATFORMS:
            h = platform_host(p, tools=() if p == "win32" else ("git",), toolchain=("sdk", "glslang", "git", "jdk"),
                              java=17)
            self.assertEqual(preflight.missing_tools(h, apk=True), [], p)
            paths = preflight.toolchain_paths(h)
            self.assertEqual(preflight.find_sdk(h), paths["sdk"])
            self.assertEqual(preflight.find_glslang(h), paths["glslang"][0])
            self.assertTrue(preflight._find_java(h).startswith(paths["jdk"]))
            if p == "win32":
                self.assertEqual(preflight.find_git(h), paths["git"])

    def test_toolchain_dir_default_and_override(self):
        self.assertEqual(preflight.toolchain_dir(platform_host("win32")), "C:\\Users\\Public\\.vcs-toolchain")
        self.assertEqual(preflight.toolchain_dir(platform_host("darwin")), "/Users/Shared/.vcs-toolchain")
        self.assertEqual(preflight.toolchain_dir(platform_host("linux")), "/home/user/.vcs-toolchain")
        h = platform_host("win32", env={"VCS_ANDROID_TOOLCHAIN": "C:\\vt"})
        self.assertEqual(preflight.toolchain_paths(h)["sdk"], "C:\\vt\\android-sdk")

    def test_complete_sdk_wins_over_incomplete_android_home(self):
        for p in PLATFORMS:
            h = platform_host(p, toolchain=("sdk",))
            incomplete = h.join(h.env.get("USERPROFILE") or h.env["HOME"], "old-sdk")
            h.env["ANDROID_HOME"] = incomplete
            h.have.add(incomplete)
            self.assertEqual(preflight.find_sdk(h), preflight.toolchain_paths(h)["sdk"], p)
            h.have.difference_update(x for x in list(h.have) if x.startswith(preflight.toolchain_dir(h)))
            self.assertEqual(preflight.find_sdk(h), incomplete, p)  # none complete: the first that exists

    def test_old_java_home_loses_to_toolchain_jdk(self):
        h = platform_host("win32", toolchain=("jdk",), env={"JAVA_HOME": "C:\\jdk11"})
        h.have.add("C:\\jdk11\\bin\\java.exe")
        h._java_major = lambda j: 11 if j.startswith("C:\\jdk11") else 17
        self.assertTrue(preflight._find_java(h).startswith(preflight.toolchain_paths(h)["jdk"]))


import toolchain_fetch  # noqa: E402


def keys(items):
    return [it.key for it in items]


class FetchPlanTests(unittest.TestCase):
    def test_same_plan_on_windows_and_macos(self):
        for p, machine in (("win32", "AMD64"), ("darwin", "arm64"), ("darwin", "x86_64"), ("linux", "x86_64")):
            h = platform_host(p, tools=("git",), java=None, machine=machine)
            items, manual = toolchain_fetch.plan(h)
            self.assertEqual(keys(items), ["glslang", "jdk", "cmdline-tools", "sdk-packages"], p)
            self.assertEqual(manual, [], p)
            for it in items:
                if it.url:
                    self.assertTrue(it.url.startswith(("https://dl.google.com/android/repository/",
                                                       "https://github.com/adoptium/", "https://github.com/KhronosGroup/")))
                    self.assertEqual(len(it.sha256), 64)
            self.assertEqual(items[-1].packages, ("ndk;" + preflight.NDK_VERSION, "cmake;" + preflight.CMAKE_VERSION))
            self.assertGreater(items[-1].size, 700 << 20)  # the NDK: the size is shown before asking

    def test_per_os_archives(self):
        win = toolchain_fetch.plan(platform_host("win32", tools=("git",), java=None, machine="AMD64"))[0]
        mac = toolchain_fetch.plan(platform_host("darwin", tools=("git",), java=None, machine="arm64"))[0]
        self.assertIn("windows", win[0].url)
        self.assertTrue(win[1].url.endswith("x64_windows_hotspot_17.0.18_8.zip"))
        self.assertTrue(mac[0].url.endswith("macos-universal-release.tar.gz"))
        self.assertTrue(mac[1].url.endswith("aarch64_mac_hotspot_17.0.18_8.tar.gz"))
        self.assertIn("mac_arm64", mac[2].url)

    def test_nothing_when_complete(self):
        for p in PLATFORMS:
            self.assertEqual(toolchain_fetch.plan(platform_host(p, ("sdk", "glslang", "git", "java")), apk=True), ([], []))

    def test_jdk_only_when_needed(self):
        for p in PLATFORMS:
            h = platform_host(p, ("sdk", "glslang", "git"), java=None)
            self.assertEqual(toolchain_fetch.plan(h)[0], [])  # build-game with a complete SDK needs no Java
            self.assertEqual(keys(toolchain_fetch.plan(h, apk=True)[0]), ["jdk"])

    def test_git(self):
        items, manual = toolchain_fetch.plan(platform_host("win32", ("sdk", "glslang")))
        self.assertEqual(keys(items), ["mingit"])
        self.assertIn("MinGit", items[0].url)
        for p, hint in (("darwin", "xcode-select --install"), ("linux", "apt install git")):
            items, manual = toolchain_fetch.plan(platform_host(p, ("sdk", "glslang")))
            self.assertEqual(items, [])
            self.assertIn(hint, manual[0][1])

    def test_linux_arm64_has_no_ndk_host(self):
        items, manual = toolchain_fetch.plan(platform_host("linux", ("git",), machine="aarch64"))
        self.assertNotIn("sdk-packages", keys(items))
        self.assertIn("no arm64 Linux host", manual[0][1])

    def test_windows_long_toolchain_folder_refused(self):
        h = platform_host("win32", ("git",), env={"VCS_ANDROID_TOOLCHAIN": "C:\\" + "t" * 90})
        with self.assertRaises(toolchain_fetch.FetchError):
            toolchain_fetch.plan(h)

    def test_describe_lists_sizes_and_sources(self):
        h = platform_host("darwin", ("git",), java=None, machine="arm64")
        text = toolchain_fetch.describe(*toolchain_fetch.plan(h), host=h, dry_run=True)
        for needle in ("android-ndk-r28c-darwin.zip", "908 MB", "cmake-3.31.5-darwin.zip", "commandlinetools-mac_arm64",
                       "developer.android.com/studio/terms", "--dry-run: nothing downloaded", "total: about"):
            self.assertIn(needle, text)


def fake_server(blobs):
    """An opener for toolchain_fetch.download: serves `blobs` {url: bytes}, honours the resume offset; records calls."""
    calls = []

    def opener(url, offset):
        import io
        calls.append((url, offset))
        data = blobs[url]
        return io.BytesIO(data[offset:]), len(data), offset > 0
    opener.calls = calls
    return opener


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.data = os.urandom(3 << 20)
        self.sha = __import__("hashlib").sha256(self.data).hexdigest()
        self.url = "https://example.invalid/tool.zip"
        self.target = os.path.join(self.dir.name, "tool.zip")

    def test_ok_and_atomic(self):
        toolchain_fetch.download(self.url, self.sha, self.target, fake_server({self.url: self.data}), out=lambda *a: None)
        with open(self.target, "rb") as f:
            self.assertEqual(f.read(), self.data)
        self.assertFalse(os.path.exists(self.target + ".part"))

    def test_sha_mismatch_rejected(self):
        with self.assertRaises(toolchain_fetch.FetchError) as cm:
            toolchain_fetch.download(self.url, "0" * 64, self.target, fake_server({self.url: self.data}),
                                     out=lambda *a: None)
        self.assertIn("refused", str(cm.exception))
        self.assertFalse(os.path.exists(self.target))
        self.assertFalse(os.path.exists(self.target + ".part"))

    def test_resume(self):
        with open(self.target + ".part", "wb") as f:
            f.write(self.data[:1 << 20])
        server = fake_server({self.url: self.data})
        toolchain_fetch.download(self.url, self.sha, self.target, server, out=lambda *a: None)
        self.assertEqual(server.calls, [(self.url, 1 << 20)])
        with open(self.target, "rb") as f:
            self.assertEqual(f.read(), self.data)


def _glslang_archive(windows):
    """A stand-in for the glslang release: bin/glslang(.exe) (a shell script on Unix)."""
    import io
    import tarfile
    import zipfile
    buf = io.BytesIO()
    if windows:
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("bin/glslang.exe", b"MZ")
            z.writestr("bin/glslangValidator.exe", b"MZ")
    else:
        with tarfile.open(fileobj=buf, mode="w:gz") as t:
            body = b"#!/bin/sh\necho fake glslang\n"
            info = tarfile.TarInfo("bin/glslang")
            info.size, info.mode = len(body), 0o755
            t.addfile(info, io.BytesIO(body))
            link = tarfile.TarInfo("bin/glslangValidator")
            link.type, link.linkname = tarfile.SYMTYPE, "glslang"
            t.addfile(link)
            inc = tarfile.TarInfo("include/glslang.h")
            inc.size = 0
            t.addfile(inc, io.BytesIO(b""))
    return buf.getvalue()


class SetupTests(unittest.TestCase):
    """setup() end to end on this machine's real disk (the toolchain folder is a temporary one), with a fake server."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.windows = os.name == "nt"
        self.blob = _glslang_archive(self.windows)
        self.sha = __import__("hashlib").sha256(self.blob).hexdigest()
        key = toolchain_fetch.host_key(preflight.Host())
        self.url = toolchain_fetch.GLSLANG[key][0]
        self.patch = unittest.mock.patch.dict(toolchain_fetch.GLSLANG, {key: (self.url, self.sha, len(self.blob))})
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def host(self):
        """Only glslang missing: a real-disk host with everything else faked present."""
        tc = os.path.join(self.dir.name, "tc")
        h = preflight.Host(env={"VCS_ANDROID_TOOLCHAIN": tc, "HOME": self.dir.name, "PATH": ""},
                           which=lambda n: "/bin/" + n if n in ("git", "java") else None, java_major=lambda j: 17)
        real = os.path.exists
        h.exists = lambda p: real(p) or "Android" in p or p.endswith("sdk")
        h.env["ANDROID_HOME"] = os.path.join(self.dir.name, "Android", "sdk")
        return h

    def run_setup(self, **kw):
        lines = []
        server = fake_server({self.url: self.blob})
        code = toolchain_fetch.setup(self.host(), opener=server, out=lines.append, **kw)
        return code, server, "\n".join(lines)

    def test_interactive_no_downloads_nothing(self):
        code, server, text = self.run_setup(interactive=True, ask=lambda q: "n")
        self.assertEqual((code, server.calls), (2, []))
        self.assertIn("nothing downloaded", text)

    def test_interactive_empty_answer_is_no(self):
        code, server, _ = self.run_setup(interactive=True, ask=lambda q: "")
        self.assertEqual((code, server.calls), (2, []))

    def test_not_a_terminal_without_yes_refuses(self):
        code, server, text = self.run_setup(interactive=False, ask=lambda q: self.fail("must not ask"))
        self.assertEqual((code, server.calls), (2, []))
        self.assertIn("add --yes", text)

    def test_dry_run_downloads_nothing(self):
        code, server, text = self.run_setup(interactive=False, dry_run=True)
        self.assertEqual((code, server.calls), (0, []))
        self.assertIn(self.url, text)

    def test_yes_fetches_and_preflight_finds_it(self):
        asked = []
        code, server, text = self.run_setup(interactive=False, yes=True, ask=asked.append)
        self.assertEqual(code, 0, text)
        self.assertEqual(asked, [])
        h = self.host()
        found = preflight.find_glslang(h)
        self.assertEqual(found, preflight.toolchain_paths(h)["glslang"][0])
        self.assertTrue(os.path.isfile(found))
        if not self.windows:
            self.assertTrue(os.access(found, os.X_OK))
        self.assertFalse(os.listdir(os.path.join(preflight.toolchain_dir(h), "downloads")))  # archive removed
        self.assertEqual(self.run_setup(interactive=False)[0], 0)  # now nothing to fetch

    def test_interactive_yes_fetches(self):
        code, server, _ = self.run_setup(interactive=True, ask=lambda q: "y")
        self.assertEqual(code, 0)
        self.assertEqual(len(server.calls), 1)

    def test_sha_mismatch_fails_setup(self):
        with unittest.mock.patch.dict(toolchain_fetch.GLSLANG,
                                      {toolchain_fetch.host_key(preflight.Host()): (self.url, "f" * 64, 1)}):
            code, _, text = self.run_setup(yes=True, interactive=False)
        self.assertEqual(code, 1)
        self.assertIn("refused", text)
        self.assertIsNone(preflight.find_glslang(self.host()))


class LicenceTests(unittest.TestCase):
    """The Android SDK licences: accepted by the user (a terminal, or --accept-android-licenses), never silently."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.sdk = os.path.join(self.dir.name, "android-sdk")
        self.item = toolchain_fetch.Item("sdk-packages", "NDK", 1, dest=self.sdk, packages=toolchain_fetch.SDK_PACKAGES)
        self.host = preflight.Host(env={"VCS_ANDROID_TOOLCHAIN": self.dir.name})

    def runner(self, accept):
        calls = []

        def run(cmd, **kw):
            calls.append((cmd, kw))
            if "--licenses" in cmd and accept:
                os.makedirs(os.path.join(self.sdk, "licenses"), exist_ok=True)
                open(os.path.join(self.sdk, "licenses", "android-sdk-license"), "w").close()
            if "--licenses" not in cmd:  # the install: the packages appear
                for rel in (("ndk", preflight.NDK_VERSION), ("cmake", preflight.CMAKE_VERSION, "bin")):
                    os.makedirs(os.path.join(self.sdk, *rel), exist_ok=True)
                for t in ("cmake", "ninja"):
                    open(os.path.join(self.sdk, "cmake", preflight.CMAKE_VERSION, "bin", t + (".exe" if os.name == "nt" else "")), "w").close()
            return subprocess.CompletedProcess(cmd, 0)
        run.calls = calls
        return run

    def install(self, interactive, accept_flag, user_accepts=True):
        run = self.runner(user_accepts)
        toolchain_fetch.install_sdk_packages(self.item, self.host, interactive, accept_flag, None, run, out=lambda *a: None)
        return run.calls

    def test_terminal_user_answers(self):
        calls = self.install(interactive=True, accept_flag=False)
        self.assertIn("--licenses", calls[0][0])
        self.assertNotIn("input", calls[0][1])  # sdkmanager asks the person, nothing typed for them
        self.assertEqual(calls[1][0][-2:], list(toolchain_fetch.SDK_PACKAGES))
        self.assertTrue(calls[1][1]["env"]["ANDROID_USER_HOME"].startswith(self.dir.name))  # no ~/.android

    def test_flag_answers_and_prints(self):
        calls = self.install(interactive=False, accept_flag=True)
        self.assertTrue(calls[0][1]["input"].startswith("y\n"))
        self.assertNotIn("stdout", calls[0][1])  # the licence texts go to the output, not swallowed

    def test_no_terminal_no_flag_refused(self):
        with self.assertRaises(toolchain_fetch.FetchError) as cm:
            self.install(interactive=False, accept_flag=False)
        self.assertIn("--accept-android-licenses", str(cm.exception))

    def test_declined_installs_nothing(self):
        with self.assertRaises(toolchain_fetch.FetchError):
            self.install(interactive=True, accept_flag=False, user_accepts=False)

    def test_setup_refuses_before_downloading(self):
        """--yes without a terminal is consent to download, not to Google's licences: refused before any download."""
        h = platform_host("darwin", ("git", "glslang"), env={"VCS_ANDROID_TOOLCHAIN": self.dir.name}, machine="arm64")
        server = fake_server({})
        lines = []
        code = toolchain_fetch.setup(h, yes=True, interactive=False, opener=server, out=lines.append)
        self.assertEqual((code, server.calls), (2, []))
        self.assertIn("--accept-android-licenses", "\n".join(lines))


class ApplyEnvTests(unittest.TestCase):
    def test_path_and_java_home(self):
        for p in PLATFORMS:
            h = platform_host(p, toolchain=("jdk", "glslang", "git"))
            paths = preflight.toolchain_paths(h)
            h.have.add(h.join(paths["dir"], "glslang", "bin"))
            h.have.add(h.join(paths["dir"], "mingit", "cmd"))
            env = h.env
            env["PATH"] = "C:\\Windows" if h.windows else "/usr/bin"
            toolchain_fetch.apply_env(env, h)
            sep = ";" if h.windows else ":"
            parts = env["PATH"].split(sep)
            self.assertEqual(parts[0], "C:\\Windows" if h.windows else "/usr/bin")  # the user's own tools first
            self.assertIn(h.join(paths["dir"], "glslang", "bin"), parts)
            self.assertEqual(h.join(paths["dir"], "mingit", "cmd") in parts, h.windows)
            self.assertEqual(env["JAVA_HOME"], paths["jdk"])

    def test_good_java_home_kept(self):
        h = platform_host("darwin", toolchain=("jdk",), env={"JAVA_HOME": "/opt/jdk21"})
        h.have.add("/opt/jdk21/bin/java")
        toolchain_fetch.apply_env(h.env, h)
        self.assertEqual(h.env["JAVA_HOME"], "/opt/jdk21")


class ScriptParityTests(unittest.TestCase):
    """tools/build-android-pack.bat and .command: the same steps and the same messages."""

    def messages(self, name):
        with open(os.path.join(HERE, name), encoding="utf-8") as f:
            text = f.read()
        import re
        return [m.replace("^", "") for m in re.findall(r"\[pack\] ([^\"\r\n]*)", text)]

    def test_same_messages(self):
        bat, sh = self.messages("build-android-pack.bat"), self.messages("build-android-pack.command")
        norm = lambda ms: [m.replace("\\", "/").replace("%ISO%", "$ISO").replace("%PY%", "$PY") for m in ms]  # noqa: E731
        self.assertEqual(sorted(set(norm(bat))), sorted(set(norm(sh))))
        self.assertGreater(len(set(bat)), 8)

    def test_command_is_executable_in_git(self):
        r = subprocess.run(["git", "ls-files", "-s", "tools/build-android-pack.command"], cwd=os.path.dirname(HERE),
                           capture_output=True, text=True)
        if r.returncode == 0 and r.stdout:
            self.assertTrue(r.stdout.startswith("100755"), r.stdout)


if __name__ == "__main__":
    unittest.main()
