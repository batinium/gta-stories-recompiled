#!/usr/bin/env python3
"""[lcs] Test harness for Liberty City Stories: start hidden lcs instances and drive them (Windows, Linux, macOS).

Each instance has its own control port and its own data folder (memory card, settings), so several run side by side.
Never point --data at the player's own folder (%LOCALAPPDATA%\\GTAStoriesRecomp\\LCS): use a scratch folder.

    python tools/lcs_test.py start   --port 49001 --data D:\\t\\d1 [--card DIR] [--slot N] [--seconds 3600]
                                     [--exe lcs.exe] [--iso ISO] [--env KEY=VALUE ...] [--visible]
    python tools/lcs_test.py newgame --port 49001       menus -> New Game -> skip the opening cutscene -> gameplay
    python tools/lcs_test.py vic1    --port 49001       after newgame: pass mission 1 by teleports -> free roam
    python tools/lcs_test.py ingame  --port 49001       wait for gameplay after a boot load (start --card --slot)
    python tools/lcs_test.py state   --port 49001       frame, player, position, mission flag, camera mode, ...
    python tools/lcs_test.py shot OUT.png --port 49001  screenshot (absolute path written)
    python tools/lcs_test.py info    --port 49001       the gameinfo panel's lines
    python tools/lcs_test.py goto X Y Z --port 49001    teleport (z <= -100: the ground); trainer goto
    python tools/lcs_test.py landmark N --port 49001    the trainer's landmark N (trainer list landmarks)
    python tools/lcs_test.py save [SLOT] | load [SLOT]  F9 quick save / F10 quick load (lcssave quick / load)
    python tools/lcs_test.py perf    --port 49001       the perf overlay's numbers (fps) for the last second
    python tools/lcs_test.py press KEY [MS] | hold KEY SECONDS   keys as tools/gamectl.py names them (F = Triangle:
                                                       enter / leave a car, Cross = Shift, W/A/S/D move, Space brake)
    python tools/lcs_test.py trainer ARGS... / raw CMD...  any trainer verb / any control command
    python tools/lcs_test.py quit    --port 49001       quit, then kill what is left (pid from the data folder)
    python tools/lcs_test.py smoke   --port 49001 --data DIR   start, new game, screenshot, state, quit (one shot)
    python tools/lcs_test.py savecard --data DIR --to DIR      copy a data folder's mc0 to a reusable card copy

Defaults: --exe = VCS_EXE or the lcs build of tools/toolchain.py (VCS_BUILD_DIR respected); --iso = LCS_ISO or the
'*Liberty City Stories*.iso' in the main checkout. start writes <data>\\lcs.log (stdout + stderr) and <data>\\lcs.pid
and returns once the control channel answers; the game keeps running after this tool exits, and a reaper kills it
after --seconds (default 3600) so no instance is left behind.

As a library (one script driving several instances):

    import lcs_test
    g = lcs_test.Lcs.start(port=49001, data=r"E:\\t\\d1")            # or card=r"...\\shared\\saves\\freeroam", slot=8
    g.new_game()                                                     # or g.wait_ingame()
    print(g.state()); g.goto(-700, -100, -100); g.shot(r"E:\\t\\a.png"); g.quit()

Addresses (SLUS-21423, src/game/lcs_addrs.cpp, lcs_camera.cpp): CTimer frame counter 0x3D9B6C (not monotonic above
30 fps as read from outside: use Lcs.frames(), the rig's count), script space 0x3CF0F4
(non-zero once a game runs), player ped 0x408E40, mission flag 0x3D9C29, TheCamera 0x43BBF0 (active cam u8 +0x10C,
cams +0x220 x 0x2B0, mode u16 +0x40: 4 = follow ped on foot, 17-ish = cutscene). rig.py's player / scm / scripts
commands use VCS addresses: don't use them on LCS; use trainer goto / spawn / mission instead.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("VCS_TOOLS_GAME", "lcs")
import gamectl  # noqa: E402

# ---- LCS addresses (SLUS-21423) ----
FRAMES = 0x3D9B6C        # CTimer frame counter (lcs_addrs.cpp)
SCRIPT_SPACE = 0x3CF0F4  # CTheScripts space pointer: 0 in the menus before a game starts
PLAYER_PED = 0x408E40    # CWorld::Players[0].m_pPed
MISSION = 0x3D9C29       # u8, 1 while a mission runs
CAMERA = 0x43BBF0        # TheCamera
PAD0 = 0x408C60          # CPad[0]: +0x82 disable controls
PED_STATE = 0x34C        # CPed state (0x32 driving, 0x37 dead, 0x3E arrested)
PED_HEALTH = 0x4B8       # float
PED_VEHICLE = 0x534      # vehicle pointer
PED_IN_VEHICLE = 0x538   # in-vehicle flag
WANTED_LEVEL = 0x810     # CWanted embedded at +0x7F0, level +0x20 (qa.md)
CAM_FOLLOW_PED = 4

DEFAULT_ISO_DIR = os.environ.get("VCS_ISO_DIR") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_exe():
    if os.environ.get("VCS_EXE"):
        return os.environ["VCS_EXE"]
    try:
        import toolchain as tc
        return tc.VCS_EXE
    except SystemExit:
        return ""


def default_iso():
    if os.environ.get("LCS_ISO"):
        return os.environ["LCS_ISO"]
    import glob
    for d in (os.path.dirname(HERE), DEFAULT_ISO_DIR):
        found = sorted(glob.glob(os.path.join(d, "*Liberty City Stories*.iso")))
        if found:
            return found[0]
    return ""


def copy_card(src, dst):
    """Copy a card folder (mc0) keeping file times: the card's directory order falls back to creation times."""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if os.name == "nt":
        r = subprocess.run(["robocopy", src, dst, "/E", "/COPY:DAT", "/DCOPY:T", "/NFL", "/NDL", "/NJH", "/NJS"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode >= 8:
            raise RuntimeError("robocopy failed (%d) copying %s" % (r.returncode, src))
    else:
        shutil.copytree(src, dst, copy_function=shutil.copy2)


def _kill_pid(pid, image="lcs.exe"):
    if not pid:
        return
    if os.name == "nt":  # by pid and image name: a pid from an old lcs.pid may belong to another program now
        subprocess.run(["taskkill", "/F", "/FI", "PID eq %d" % pid, "/FI", "IMAGENAME eq %s" % image], capture_output=True)
    else:
        import signal
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def _pid_alive(pid):
    if not pid:
        return False
    if os.name == "nt":
        r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/FI", "IMAGENAME eq lcs.exe", "/NH"],
                           capture_output=True, text=True)
        return str(pid) in r.stdout
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Lcs:
    def __init__(self, port, data=None, pid=None, log=None):
        self.port = port
        self.data = data
        self.pid = pid
        self.log = log

    # ---- launching ----
    @classmethod
    def start(cls, port, data, card=None, slot=None, exe=None, iso=None, seconds=3600, env=None, visible=False,
              fresh=True, timeout=90.0):
        """Start lcs.exe hidden with its own data folder and control port; return once the control channel answers.
        card: a folder holding mc0/ (copied into data); slot: VCS_LOAD_SLOT (the boot loads that save). fresh: an
        empty data folder when no card is given (a new game's card)."""
        exe = exe or default_exe()
        iso = iso or default_iso()
        if not exe or not os.path.isfile(exe):
            raise RuntimeError("no lcs executable: %r (build it, or pass --exe / VCS_EXE)" % exe)
        if not iso or not os.path.isfile(iso):
            raise RuntimeError("no LCS disc image: %r (pass --iso or set LCS_ISO)" % iso)
        if gamectl.available(port):
            raise RuntimeError("something already answers on port %d" % port)
        data = os.path.abspath(data)
        if card:
            os.makedirs(data, exist_ok=True)
            copy_card(os.path.join(card, "mc0"), os.path.join(data, "mc0"))
        elif fresh and os.path.isdir(os.path.join(data, "mc0")):
            shutil.rmtree(os.path.join(data, "mc0"))
        os.makedirs(data, exist_ok=True)
        e = dict(os.environ)
        if not visible:
            e["PS2X_HIDDEN"] = "1"
        e["PS2X_CONTROL"] = "1"
        e["PS2X_CONTROL_PORT"] = str(port)
        e["VCS_DATA_DIR"] = data
        e["VCS_KEEP_SETTINGS"] = "1"  # never rewrite a vcs.ini
        e.setdefault("PS2X_CAPTURE_SIZE", "960x672")
        e.setdefault("VCS_FULLSCREEN", "0")
        # no logos / intro movie (as tools/run.py): with them a boot load waits ~100 s on the title before the save
        # loads; without, the save is in play ~7 s after the start
        e.setdefault("VCS_SKIP_MOVIES", "1")
        e.pop("PS2X_GS", None)  # never gpu
        if slot:
            e["VCS_LOAD_SLOT"] = str(slot)
        e.update(env or {})
        log = os.path.join(data, "lcs.log")
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000008  # DETACHED_PROCESS: outlives this tool
        with open(log, "w", encoding="utf-8") as f:
            proc = subprocess.Popen([exe, iso], cwd=data, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT,
                                    env=e, creationflags=flags, start_new_session=os.name != "nt")
        with open(os.path.join(data, "lcs.pid"), "w") as f:
            f.write(str(proc.pid))
        if seconds and seconds > 0:  # reaper: no stray instance even when the driving script dies
            # Windows: only a process with that pid AND the game's image name (the pid may be reused by then)
            image = os.path.basename(exe)
            code = ("import time,subprocess,os,sys;time.sleep(%d);" % int(seconds) +
                    ("subprocess.run(['taskkill','/F','/FI','PID eq %d','/FI','IMAGENAME eq %s'],capture_output=True)"
                     % (proc.pid, image) if os.name == "nt" else "os.kill(%d,9)" % proc.pid))
            subprocess.Popen([sys.executable, "-I", "-c", code], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, creationflags=flags, start_new_session=os.name != "nt")
        g = cls(port, data, proc.pid, log)
        g._proc = proc
        t0 = time.time()
        while time.time() - t0 < timeout:
            if proc.poll() is not None:
                raise RuntimeError("lcs exited at start (code %s); log %s" % (proc.returncode, log))
            if gamectl.available(port):
                return g
            time.sleep(0.5)
        raise RuntimeError("no control channel on port %d after %.0f s" % (port, timeout))

    @classmethod
    def attach(cls, port, data=None):
        pid = None
        if data and os.path.isfile(os.path.join(data, "lcs.pid")):
            pid = int(open(os.path.join(data, "lcs.pid")).read().strip() or 0)
        return cls(port, data, pid, os.path.join(data, "lcs.log") if data else None)

    def alive(self):
        if getattr(self, "_proc", None) is not None:
            return self._proc.poll() is None
        return _pid_alive(self.pid) if self.pid else gamectl.available(self.port)

    def quit(self):
        try:
            gamectl.send("quit", port=self.port, timeout=10)
        except Exception:
            pass
        t0 = time.time()
        while time.time() - t0 < 15 and self.alive():
            time.sleep(0.5)
        if self.alive():
            _kill_pid(self.pid)

    # ---- control channel ----
    def raw(self, line, timeout=70.0):
        return gamectl.send(line, port=self.port, timeout=timeout)

    def cmd(self, line, timeout=70.0):
        reply = self.raw(line, timeout)
        if not reply.startswith("ok"):
            raise RuntimeError("%s -> %s" % (line, reply))
        return reply[3:] if len(reply) > 3 else ""

    def peek(self, addr, n):
        return bytes.fromhex(self.cmd("peek 0x%x %d" % (addr, n)))

    def u8(self, addr):
        return self.peek(addr, 1)[0]

    def u16(self, addr):
        return struct.unpack("<H", self.peek(addr, 2))[0]

    def u32(self, addr):
        return struct.unpack("<I", self.peek(addr, 4))[0]

    def f32(self, addr, n=1):
        v = struct.unpack("<%df" % n, self.peek(addr, 4 * n))
        return v[0] if n == 1 else v

    def poke32(self, addr, value):
        return self.cmd("poke 0x%x %s" % (addr, struct.pack("<I", value & 0xFFFFFFFF).hex()))

    def pokef(self, addr, *values):
        return self.cmd("pokef 0x%x %s" % (addr, " ".join("%g" % v for v in values)))

    def press(self, key, ms=150):
        return self.cmd("press %s %d" % (key, ms))

    def key(self, key, down):
        return self.cmd("key %s %s" % (key, "down" if down else "up"))

    def hold(self, key, seconds):
        self.key(key, True)
        try:
            time.sleep(seconds)
        finally:
            self.key(key, False)

    def trainer(self, args, timeout=70.0):
        return self.cmd("trainer " + args, timeout)

    # ---- game state ----
    def frames(self):
        """Game frames run, counted by the port's frame hook (rig `frames`): only ever goes up. Not CTimer's counter
        (ctimer_frames): above 30 fps lcs_framerate.cpp swaps its low 6 bits for a 30 fps count while the HUD draws,
        so a read in mid-frame can be up to 63 lower than the one before."""
        return int(self.cmd("frames"))

    def ctimer_frames(self):
        return self.u32(FRAMES)

    def ped(self):
        return self.u32(PLAYER_PED)

    def cam_mode(self):
        active = self.u8(CAMERA + 0x10C)
        if active > 2:
            return 0xFFFF
        return self.u16(CAMERA + 0x220 + active * 0x2B0 + 0x40)

    def pos(self):
        ped = self.ped()
        return self.f32(ped + 0x30, 3) if ped else None

    def state(self):
        """A dict: frame, ped, pos, health, pedstate, vehicle, wanted, mission, cam, scm, pad_disabled."""
        ped = self.ped()
        s = {"frame": self.frames(), "ped": ped, "mission": self.u8(MISSION), "cam": self.cam_mode(),
             "scm": self.u32(SCRIPT_SPACE), "pad_disabled": self.u16(PAD0 + 0x82)}
        if ped:
            s["pos"] = tuple(round(v, 1) for v in self.f32(ped + 0x30, 3))
            s["health"] = round(self.f32(ped + PED_HEALTH), 1)
            s["pedstate"] = self.u32(ped + PED_STATE)
            s["vehicle"] = self.u32(ped + PED_VEHICLE) if self.u32(ped + PED_IN_VEHICLE) else 0
            s["wanted"] = self.u32(ped + WANTED_LEVEL)
        return s

    def running(self, seconds=1.0, min_frames=5):
        """True when game frames advance over `seconds`."""
        f0 = self.frames()
        time.sleep(seconds)
        return self.frames() >= f0 + min_frames

    def in_gameplay(self):
        """Player exists, follow camera (on foot) or the player in a vehicle, controls on, frames advancing."""
        s = self.state()
        if not s["ped"] or s["pad_disabled"]:
            return False
        if s["cam"] != CAM_FOLLOW_PED and not s.get("vehicle"):
            return False
        return self.running(1.0)

    # ---- flows ----
    def new_game(self, timeout=420.0, skip=True, log=print):
        """From an empty card: Cross through the menus until the script engine starts (New Game), then Cross to skip
        the opening cutscene until gameplay (Toni outside Salvatore's, mission 1 running). Returns seconds taken."""
        t0 = time.time()
        time.sleep(8.0)
        while not self.u32(SCRIPT_SPACE):
            if time.time() - t0 > 200:
                raise RuntimeError("the game did not start from the menus")
            if not self.alive():
                raise RuntimeError("the game exited in the menus")
            self.press("Cross")
            time.sleep(3.0)
        log("[lcs_test] script engine up after %.0f s" % (time.time() - t0))
        last_press = 0.0
        while time.time() - t0 < timeout:
            if not self.alive():
                raise RuntimeError("the game exited during the opening")
            try:
                if self.in_gameplay():
                    time.sleep(2.0)
                    if self.in_gameplay():
                        log("[lcs_test] gameplay after %.0f s: %s" % (time.time() - t0, self.state()))
                        return time.time() - t0
                if skip and self.ped() and time.time() - last_press > 4.0:
                    self.press("Cross")
                    last_press = time.time()
            except (OSError, RuntimeError):
                pass
            time.sleep(1.0)
        raise RuntimeError("no gameplay after %.0f s: %s" % (timeout, self.state()))

    def wait_ingame(self, timeout=180.0, log=print, press=True):
        """After a boot load (start(card=..., slot=N)): wait for gameplay. press: Cross every 3 s until the script
        engine runs - only matters with the intro movies on (VCS_SKIP_MOVIES=0: Cross skips the intro movie, which
        otherwise plays ~100 s before the boot's save loads)."""
        t0 = time.time()
        time.sleep(4.0)
        while time.time() - t0 < timeout:
            if not self.alive():
                raise RuntimeError("the game exited while loading")
            try:
                if not self.u32(SCRIPT_SPACE):
                    if press and time.time() - t0 > 15.0:
                        self.press("Cross")
                    time.sleep(3.0)
                    continue
                # frame > 300: right after the load the mission flag is 1 for a moment (the main script restarting;
                # a mission launched then is refused - missions.md)
                if self.ped() and self.ctimer_frames() > 300 and self.in_gameplay():
                    log("[lcs_test] in game after %.0f s: %s" % (time.time() - t0, self.state()))
                    return time.time() - t0
            except (OSError, RuntimeError):
                pass
            time.sleep(1.0)
        raise RuntimeError("not in game after %.0f s" % timeout)

    def _controls_back(self, timeout=60.0, skip=True):
        """Wait until the player has control again (a cutscene / script camera over); Cross skips cutscenes."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            s = self.state()
            if not s["pad_disabled"] and (s["cam"] == CAM_FOLLOW_PED or s.get("vehicle")) and time.time() - t0 > 1.5:
                return s
            if skip and s["pad_disabled"]:
                self.press("Cross")
            time.sleep(2.0)
        raise RuntimeError("controls not back after %.0f s: %s" % (timeout, self.state()))

    def _board(self, car, tries=6):
        for _ in range(tries):
            self.press("F", 300)
            time.sleep(3.0)
            s = self.state()
            if s.get("vehicle"):
                return s
        raise RuntimeError("could not get into the car %08x: %s" % (car, self.state()))

    def pass_first_mission(self, log=print):
        """Right after new_game(): VIC1 Home Sweet Home by teleports (the extras agent's route, internal notes
        extras.md): board Vincenzo's car, the car to the first blip (1156.85, -256.23), the doorway (1147.6, -262.9),
        the clothes pickup (1137.91, -250.6, 23.1), the "rejoin Vincenzo" locate (1146.47, -246.03, 22.45), board the
        car again, the car to Atlantic Quays (1124.99, -1102.12) holding the brake -> "Mission passed! $100", free
        roam. ~2.5 min. Returns the state; raises when a step does not happen."""
        s = self._board(0)
        car = s["vehicle"]
        time.sleep(2.0)
        self.goto(1156.85, -256.23, 16.25)
        time.sleep(3.0)
        self._controls_back()
        log("[lcs_test] vic1: at the safehouse street")
        self.goto(1147.6, -262.9, 16.2)
        time.sleep(3.0)
        self._controls_back(skip=False)
        self.goto(1137.91, -250.6, 23.1)
        time.sleep(3.0)
        self._controls_back(skip=False)
        log("[lcs_test] vic1: clothes picked up")
        self.goto(1146.47, -246.03, 22.45)
        time.sleep(3.0)
        self._controls_back()
        cx, cy, _ = self.f32(car + 0x30, 3)
        self.goto(cx - 1.8, cy, -100.0)
        time.sleep(1.0)
        self._board(car)
        time.sleep(2.0)
        self.goto(1124.99, -1102.12, 10.97)
        self.key("Space", True)
        try:
            t0 = time.time()
            while self.u8(MISSION) and time.time() - t0 < 40:
                time.sleep(1.0)
        finally:
            self.key("Space", False)
        if self.u8(MISSION):
            raise RuntimeError("vic1 did not end: %s" % self.state())
        time.sleep(6.0)
        log("[lcs_test] vic1 passed: %s" % self.state())
        return self.state()

    def shot(self, path):
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return self.cmd("screenshot " + path, timeout=30)

    def info(self):
        return self.cmd("gameinfo")

    def perf(self):
        """The perf overlay's numbers as a dict (fps etc.) when they parse, else the raw reply."""
        reply = self.cmd("perf")
        out = {}
        for w in reply.split():
            if "=" in w:
                k, _, v = w.partition("=")
                try:
                    out[k] = float(v)
                except ValueError:
                    out[k] = v
        return out or reply

    def goto(self, x, y, z=-100.0):
        return self.trainer("goto %g %g %g" % (x, y, z), timeout=90)

    def landmark(self, n):
        return self.trainer("teleport %d" % n, timeout=90)

    def quick_save(self, slot=8):
        return self.cmd("lcssave quick %d" % slot)

    def quick_load(self, slot=8):
        return self.cmd("lcssave load %d" % slot)

    def save_busy(self):
        return "busy=1" in self.cmd("lcssave state")

    def wait_frames(self, n, timeout=60.0):
        start = self.frames()
        t0 = time.time()
        while self.frames() < start + n:
            if time.time() - t0 > timeout:
                raise RuntimeError("game frames stalled at %d" % self.frames())
            time.sleep(0.1)


def save_card(data, to):
    """Copy a data folder's card (mc0) to a reusable card copy folder `to` (to\\mc0)."""
    copy_card(os.path.join(data, "mc0"), os.path.join(to, "mc0"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PS2X_CONTROL_PORT", "47615")))
    ap.add_argument("--data", help="the instance's data folder (card, settings, lcs.log, lcs.pid)")
    ap.add_argument("--card", help="start: copy this card copy's mc0 into --data")
    ap.add_argument("--slot", type=int, help="start: VCS_LOAD_SLOT (the boot loads this save)")
    ap.add_argument("--exe")
    ap.add_argument("--iso")
    ap.add_argument("--seconds", type=int, default=3600, help="start: kill the game after this long (0 = never)")
    ap.add_argument("--env", action="append", default=[], help="start: KEY=VALUE for the game (repeatable)")
    ap.add_argument("--visible", action="store_true", help="start: show the window (default hidden)")
    ap.add_argument("--to", help="savecard: destination folder")
    ap.add_argument("--timeout", type=float, default=420.0)
    a = ap.parse_args()
    env = dict(kv.split("=", 1) for kv in a.env)
    c = a.command

    if c == "start":
        if not a.data:
            sys.exit("start needs --data")
        g = Lcs.start(a.port, a.data, card=a.card, slot=a.slot, exe=a.exe, iso=a.iso, seconds=a.seconds, env=env,
                      visible=a.visible)
        print("ok pid=%d port=%d log=%s" % (g.pid, g.port, g.log))
        return 0
    if c == "savecard":
        if not a.data or not a.to:
            sys.exit("savecard needs --data and --to")
        save_card(a.data, a.to)
        print("ok %s" % os.path.join(a.to, "mc0"))
        return 0
    if c == "smoke":
        if not a.data:
            sys.exit("smoke needs --data")
        g = Lcs.start(a.port, a.data, card=a.card, slot=a.slot, exe=a.exe, iso=a.iso, seconds=a.seconds, env=env)
        try:
            g.wait_ingame(a.timeout) if a.slot else g.new_game(a.timeout)
            time.sleep(3)
            print(g.state())
            print(g.perf())
            print(g.shot(os.path.join(a.data, "smoke.png")))
        finally:
            g.quit()
        return 0

    g = Lcs.attach(a.port, a.data)
    if c == "newgame":
        g.new_game(a.timeout)
    elif c == "vic1":
        g.pass_first_mission()
    elif c == "ingame":
        g.wait_ingame(a.timeout)
    elif c == "state":
        print(g.state())
    elif c == "shot":
        print(g.shot(a.args[0] if a.args else os.path.join(a.data or ".", "shot.png")))
    elif c == "info":
        print(g.info().replace(" | ", "\n"))
    elif c == "goto":
        print(g.goto(*[float(v) for v in a.args[:3]]))
    elif c == "landmark":
        print(g.landmark(int(a.args[0])))
    elif c == "save":
        print(g.quick_save(int(a.args[0]) if a.args else 8))
    elif c == "load":
        print(g.quick_load(int(a.args[0]) if a.args else 8))
    elif c == "perf":
        print(g.perf())
    elif c == "press":
        print(g.press(a.args[0], int(a.args[1]) if len(a.args) > 1 else 150))
    elif c == "hold":
        g.hold(a.args[0], float(a.args[1]) if len(a.args) > 1 else 1.0)
        print("ok")
    elif c == "trainer":
        print(g.trainer(" ".join(a.args)))
    elif c == "raw":
        print(g.raw(" ".join(a.args)))
    elif c == "quit":
        g.quit()
        print("ok")
    else:
        sys.exit("unknown command %r (see --help)" % c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
