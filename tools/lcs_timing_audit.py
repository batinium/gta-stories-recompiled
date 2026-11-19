#!/usr/bin/env python3
"""LCS above 30 fps: measure per-frame vs per-time behaviour in one hidden run, the same scene at two time steps.

    python tools/lcs_timing_audit.py [--exe build/lcs/lcs.exe] [--iso ISO] [--port 49401] [--steps 30,144]
                                     [--env NAME=VALUE ...] [--data DIR] scroll|water|hud|bike|all

Starts LCS hidden with VCS_TIMESTEP=<first step> (exactly 1/N s a frame, unpaced), skips the new game's intro to the
first mission's control, then for each scenario switches the step with the control command `lcstimestep N` and
measures the same things per game second at every step. A per-frame thing shows the steps' ratio, a timed one doesn't.
Scenarios (docs/LCS.md, high frame rates; src/game/lcs_world_timing.cpp):
  scroll  the scrolling LED sign by Portland's (1208, -63): letters moved (+0x24) a game second (PS2: 3.75)
  water   the water's texture drift (gp-0x1998 ..): per game second, at the first mission's lake
  hud     the HUD's flash counter as the 2D pass sees it (LCS_HUD_BLINK_LOG lines in the game's log)
  bike    a PCJ-600: 4 s of gas, hard braking (heading spread: the brake wobble), a turn (lean +0x5AC, turn rate)
  run     the player running on the airport apron: metres a game second (above 100 fps: CTimer's floor lifted)
Standard library only. Writes a log per run next to --data (default build/lcs_timing_audit/).
"""
import argparse
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAME, TIME_MS, STEP = 0x3D9B6C, 0x3D9B70, 0x3D9B8C  # CTimer (src/game/lcs_addrs.cpp)
PLAYER_PED = 0x408E40
PAD0 = 0x408C60
CUTSCENE = 0x3D5AC0


def send(port, line, timeout=70.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.sendall((line + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            c = s.recv(65536)
            if not c:
                break
            data += c
    return data.decode("utf-8", "replace").strip()


class Game:
    def __init__(self, args, name, env):
        self.port = args.port
        data = os.path.abspath(args.data)
        card = os.path.join(data, "card%d" % self.port)
        shutil.rmtree(card, ignore_errors=True)
        os.makedirs(card)
        e = dict(os.environ)
        e.update({"PS2X_HIDDEN": "1", "VCS_KEEP_SETTINGS": "1", "VCS_DATA_DIR": card, "PS2X_CONTROL_PORT": str(self.port),
                  "VCS_SKIP_MOVIES": "1", "VCS_TIMESTEP": str(args.steps[0])})
        e.update(env)
        self.log_path = os.path.join(data, name + ".log")
        log = open(self.log_path, "w")
        self.proc = subprocess.Popen([args.exe, args.iso], cwd=data, stdout=log, stderr=subprocess.STDOUT, env=e,
                                     stdin=subprocess.DEVNULL)
        t0 = time.time()
        while time.time() - t0 < 90:
            try:
                if send(self.port, "ping", 2) == "ok":
                    return
            except OSError:
                pass
            if self.proc.poll() is not None:
                raise RuntimeError("the game exited")
            time.sleep(0.5)
        raise RuntimeError("no control channel")

    def cmd(self, line, timeout=70.0):
        r = send(self.port, line, timeout)
        if not r.startswith("ok"):
            raise RuntimeError("%s -> %s" % (line, r))
        return r[3:]

    def peek(self, a, n):
        return bytes.fromhex(self.cmd("peek 0x%x %d" % (a, n)))

    def u32(self, a):
        return struct.unpack("<I", self.peek(a, 4))[0]

    def f32(self, a):
        return struct.unpack("<f", self.peek(a, 4))[0]

    def clock(self):
        return struct.unpack("<II", self.peek(FRAME, 8))

    def wait_ms(self, ms, timeout=180):
        _, m0 = self.clock()
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.clock()[1] - m0 >= ms:
                return
            time.sleep(0.02)
        raise RuntimeError("the game clock stands still")

    def new_game(self, timeout=300):
        t0, ok = time.time(), 0
        while time.time() - t0 < timeout:
            try:
                st = self.cmd("trainer state")
            except RuntimeError as e:
                st = str(e)
            dis = struct.unpack("<h", self.peek(PAD0 + 0x82, 2))[0]
            if "ready=1" in st and self.peek(CUTSCENE, 1)[0] == 0 and dis == 0:
                ok += 1
                if ok >= 3:
                    return
            else:
                ok = 0
                if time.time() - t0 > 6:
                    self.cmd("press Shift 100")
            time.sleep(2.0)
        raise RuntimeError("never got control")

    def step(self, n):
        self.cmd("lcstimestep %d" % n)
        self.wait_ms(300)

    def quit(self):
        try:
            send(self.port, "quit", 3)
        except OSError:
            pass
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def sample(g, ms, read, every=0.0):
    """(game ms, value) pairs over `ms` of game time"""
    out = []
    _, m0 = g.clock()
    while True:
        _, m = g.clock()
        out.append((m, read()))
        if m - m0 >= ms:
            return out
        if every:
            time.sleep(every)


# ---- scenarios ---------------------------------------------------------------------------------------------------
SCROLL_BARS, SCROLL_SIZE = 0x3DABE0, 0x80


def scroll(g, steps, out):
    """letters per game second of the nearest visible scroll bar"""
    g.cmd("trainer goto 1196 -63 -100")
    g.wait_ms(4000)
    vis = [i for i in range(12) if g.u32(SCROLL_BARS + i * SCROLL_SIZE + 0x68)]
    if not vis:
        out("scroll: no bar visible")
        return
    bar = SCROLL_BARS + vis[0] * SCROLL_SIZE
    for n in steps:
        g.step(n)
        _, m0 = g.clock()
        c0 = g.u32(bar)
        f0 = g.clock()[0]
        g.wait_ms(4000)
        f1, m1 = g.clock()
        c1 = g.u32(bar)
        secs = (m1 - m0) / 1000.0
        out("scroll step 1/%d: bar %d, %.2f updates a game s (PS2 30), %.2f letters a game s (PS2 3.75), %.1f frames a game s"
            % (n, vis[0], (c1 - c0) / secs, (c1 - c0) / 8.0 / secs, (f1 - f0) / secs))


WATER = 0x3D5558


def water(g, steps, out):
    """the first drift float per game second (wrapping at 1 to 0)"""
    for n in steps:
        g.step(n)
        total, last, _ = 0.0, g.f32(WATER), None
        _, m0 = g.clock()
        m = m0
        while m - m0 < 4000:
            v = g.f32(WATER)
            d = v - last
            if d >= 0:
                total += d
            last = v
            _, m = g.clock()
            time.sleep(0.01)
        out("water step 1/%d: drift %.4f a game s (wind %.2f)" % (n, total / ((m - m0) / 1000.0), g.f32(0x3D9E2C)))


def bike_state(g):
    """(speed m/s, lean deg, heading deg) of the player's bike (matrix: forward +0x10; move speed +0x70 in units of
    1/50 s; CBike's lean +0x5AC, radians, smoothed by pow(handling, step) in ProcessControl 0x15BCB8)"""
    veh = g.u32(g.u32(PLAYER_PED) + 0x534)
    m = struct.unpack("<8f", g.peek(veh, 32))
    v = struct.unpack("<3f", g.peek(veh + 0x70, 12))
    speed = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) * 50.0
    lean = math.degrees(g.f32(veh + 0x5AC))
    heading = math.degrees(math.atan2(-m[4], m[5]))
    return speed, lean, heading


def hold(g, keys, ms, every=0.0):
    for k in keys:
        g.cmd("key %s down" % k)
    try:
        return sample(g, ms, lambda: bike_state(g), every)
    finally:
        for k in keys:
            g.cmd("key %s up" % k)


def unwrap(hs):
    base = hs[0]
    return [((h - base + 540.0) % 360.0) - 180.0 for h in hs]


def bike(g, steps, out, model=205):
    """PCJ-600 at the airport: 4 s of gas then 2.5 s of hard braking (the heading's spread: the brake wobble), then
    2 s of gas and 1 s of gas + left (lean and turn rate)"""
    for n in steps:
        g.step(n)
        g.cmd("trainer teleport 19")
        g.wait_ms(2500)
        g.cmd("trainer spawn %d" % model)
        g.wait_ms(5000)
        if not g.u32(g.u32(PLAYER_PED) + 0x538):
            out("bike step 1/%d: not on the bike" % n)
            continue
        gas = hold(g, ["W"], 4000)
        brake = hold(g, ["S"], 2500)
        hold(g, ["W"], 2000)
        turn = hold(g, ["W", "A"], 1000)
        leans = [s[1][1] for s in turn]
        hd = unwrap([s[1][2] for s in turn])
        turn_rate = hd[-1] / ((turn[-1][0] - turn[0][0]) / 1000.0) if len(hd) > 1 else 0.0
        spread = unwrap([s[1][2] for s in brake])
        out("bike step 1/%d: speed %.1f m/s after 4 s of gas; braking from it: heading %.1f..%.1f deg, %.1f m/s after"
            " 2.5 s; 1 s turning at %.1f m/s: lean up to %.1f deg, heading %.1f deg/s"
            % (n, gas[-1][1][0], min(spread), max(spread), brake[-1][1][0], turn[0][1][0],
               max(leans, key=abs), turn_rate))
        g.cmd("trainer teleport 19")  # off the bike
        g.wait_ms(1500)

def player_pos(g):
    ped = g.u32(PLAYER_PED)
    return struct.unpack("<3f", g.peek(ped + 0x30, 12))


def run(g, steps, out):
    """the player running (W) on the airport apron: metres a game second (the ped's animation moves him)"""
    for n in steps:
        g.step(n)
        g.cmd("trainer teleport 19")
        g.wait_ms(3000)
        g.cmd("key W down")
        try:
            g.wait_ms(1500)  # up to speed
            p0, (_, m0) = player_pos(g), g.clock()
            g.wait_ms(3000)
            p1, (_, m1) = player_pos(g), g.clock()
        finally:
            g.cmd("key W up")
        dist = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
        out("run step 1/%d: %.2f m a game second" % (n, dist / ((m1 - m0) / 1000.0)))


def hud(g, steps, out):
    for n in steps:
        g.step(n)
        size0 = os.path.getsize(g.log_path)
        g.wait_ms(8000)
        with open(g.log_path, encoding="utf-8", errors="replace") as f:
            f.seek(size0)
            lines = [l.strip() for l in f if "HUD flash" in l]
        out("hud step 1/%d: %s" % (n, lines[-1] if lines else "(no LCS_HUD_BLINK_LOG line)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=["scroll", "water", "hud", "bike", "run", "all"])
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "lcs", "lcs.exe"))
    ap.add_argument("--iso", required=True)
    ap.add_argument("--port", type=int, default=49401)
    ap.add_argument("--steps", default="30,144")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--data", default=os.path.join(ROOT, "build", "lcs_timing_audit"))
    ap.add_argument("--name", default="audit")
    a = ap.parse_args()
    a.steps = [int(x) for x in a.steps.split(",")]
    os.makedirs(a.data, exist_ok=True)
    env = dict(kv.split("=", 1) for kv in a.env)
    if a.scenario in ("hud", "all"):
        env.setdefault("LCS_HUD_BLINK_LOG", "1")
    g = Game(a, a.name, env)
    report = []

    def out(line):
        print(line, flush=True)
        report.append(line)

    try:
        g.new_game()
        out("env: %s" % (" ".join("%s=%s" % kv for kv in sorted(env.items())) or "(defaults)"))
        if a.scenario in ("water", "all"):
            water(g, a.steps, out)
        if a.scenario in ("hud", "all"):
            hud(g, a.steps, out)
        if a.scenario in ("scroll", "all"):
            scroll(g, a.steps, out)
        if a.scenario in ("bike", "all"):
            bike(g, a.steps, out)
        if a.scenario in ("run", "all"):
            run(g, a.steps, out)
    finally:
        g.quit()


if __name__ == "__main__":
    main()
