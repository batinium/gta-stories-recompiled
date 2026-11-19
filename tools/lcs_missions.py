#!/usr/bin/env python3
"""[lcs] Mission smoke test for Liberty City Stories: every mission the trainer's launcher knows, one fresh hidden game each.

    python tools/lcs_missions.py [--indices 96-117,9,48-61] [--secs 100] [--jobs 4] [--port-base 49100]
                                 [--exe lcs.exe] [--iso ISO] [--work DIR] [--card DIR --slot N] [--list]

Per mission (a fresh hidden instance, its own data folder and control port, `--jobs` of them in parallel):
  1. boot with an empty card -> the game starts a new game by itself -> Cross through the opening cutscene -> gameplay
     (Toni on foot, follow camera, VIC1 "Home Sweet Home" running). With --card/--slot instead: boot-load that save.
  2. free roam: VIC1's script is ended by writing TERMINATE (0x004E) at its IP (the game then
     clears the mission flag 0x3D9C29). The story is not advanced (a later save would restart the opening); only the
     mission launcher needs the flag clear.
  3. `trainer mission N` (retried while the flag stays 0), then watch --secs seconds: frame counter, mission flag, camera
     mode, position every ~2 s, screenshots at 25 s / 55 s / end, perf (game_fps) three times. Odd indices press Cross
     through cutscenes, even ones do not (both paths get covered); at 45 s `trainer goto` is NOT used (no blip reader).
  4. verdict: CRASH (process gone), DORMANT (`[ee] thread N -> dormant` in the log), FREEZE (frame counter still for
     (--freeze-secs, 20 s), NOSTART (flag never 1), BOOT (no gameplay reached), DARK (final screenshot almost black: stuck loading /
     black screen - look at it), ENDED (flag 1 -> 0 inside the window: passed or failed), OK (running).
Results: <work>\\missions\\results.jsonl (one line per run), <work>\\missions\\table.md, shots in <work>\\missions\\shots\\,
per-run logs in <work>\\missions\\logs\\. Standard library only. Instances are killed when their run ends (and on exit).

Addresses (SLUS-21423, src/game/lcs_addrs.cpp, lcs_trainer.cpp): frame counter 0x3D9B6C, script space pointer 0x3CF0F4,
active script list head 0x3D9C00 (CRunningScript: +0 next, +0x10 name[8], +0x18 IP offset from the space), mission
flag 0x3D9C29, player ped 0x408E40, TheCamera 0x43BBF0 (active cam u8 +0x10C, cams +0x220 x 0x2B0, mode u16 +0x40).
"""
import argparse
import atexit
import concurrent.futures
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamectl  # noqa: E402

FRAMES = 0x3D9B6C
SCRIPT_SPACE = 0x3CF0F4
ACTIVE_SCRIPTS = 0x3D9C00
MISSION = 0x3D9C29
PLAYER_PED = 0x408E40
CAMERA = 0x43BBF0
PAD0_DISABLE = 0x408C60 + 0x82
CAM_FOLLOW_PED = 4
DEFAULT_ISO_DIR = os.environ.get("VCS_ISO_DIR") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WORK = os.environ.get("LCS_MISSIONS_WORK") or os.path.join(DEFAULT_ISO_DIR, "build", "missions")

_pids = set()
_pids_lock = threading.Lock()


def kill_pid(pid):
    if not pid:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True)
    else:
        import signal
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def _cleanup():
    with _pids_lock:
        for p in list(_pids):
            kill_pid(p)


atexit.register(_cleanup)


# ---- a tiny PNG reader: mean brightness of a screenshot (stdlib only) ----
def png_stats(path):
    """(mean luma 0..255, fraction of pixels brighter than 24) of an 8-bit RGB/RGBA PNG, or None."""
    try:
        d = open(path, "rb").read()
        if d[:8] != b"\x89PNG\r\n\x1a\n":
            return None
        pos, idat, w, h, ct = 8, b"", 0, 0, 2
        while pos < len(d):
            ln, typ = struct.unpack(">I4s", d[pos:pos + 8])
            body = d[pos + 8:pos + 8 + ln]
            if typ == b"IHDR":
                w, h, bd, ct = struct.unpack(">IIBB", body[:10])
                if bd != 8 or ct not in (2, 6):
                    return None
            elif typ == b"IDAT":
                idat += body
            pos += 12 + ln
        bpp = 3 if ct == 2 else 4
        raw = zlib.decompress(idat)
        stride = w * bpp
        prev = bytearray(stride)
        tot = bright = n = 0
        p = 0
        for _ in range(h):
            f = raw[p]
            line = bytearray(raw[p + 1:p + 1 + stride])
            p += 1 + stride
            if f == 1:
                for i in range(bpp, stride):
                    line[i] = (line[i] + line[i - bpp]) & 255
            elif f == 2:
                for i in range(stride):
                    line[i] = (line[i] + prev[i]) & 255
            elif f == 3:
                for i in range(stride):
                    a = line[i - bpp] if i >= bpp else 0
                    line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
            elif f == 4:
                for i in range(stride):
                    a = line[i - bpp] if i >= bpp else 0
                    b = prev[i]
                    c = prev[i - bpp] if i >= bpp else 0
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                    line[i] = (line[i] + pr) & 255
            prev = line
            for x in range(0, w, 4):  # every 4th pixel of every 4th row is plenty
                pass
            if _ % 4 == 0:
                for x in range(0, stride, bpp * 4):
                    y = (line[x] * 30 + line[x + 1] * 59 + line[x + 2] * 11) // 100
                    tot += y
                    n += 1
                    if y > 24:
                        bright += 1
        return (tot / n, bright / n) if n else None
    except Exception:
        return None


# ---- launching ----
def default_exe():
    if os.environ.get("VCS_EXE"):
        return os.environ["VCS_EXE"]
    try:
        import toolchain as tc
        return tc.VCS_EXE
    except BaseException:
        return ""


def default_iso():
    if os.environ.get("LCS_ISO"):
        return os.environ["LCS_ISO"]
    for d in (os.path.dirname(HERE), DEFAULT_ISO_DIR):
        found = sorted(glob.glob(os.path.join(d, "*Liberty City Stories*.iso")))
        if found:
            return found[0]
    return ""


def copy_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if os.name == "nt":
        r = subprocess.run(["robocopy", src, dst, "/E", "/COPY:DAT", "/DCOPY:T", "/NFL", "/NDL", "/NJH", "/NJS"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode >= 8:
            raise RuntimeError("robocopy failed copying %s" % src)
    else:
        shutil.copytree(src, dst, copy_function=shutil.copy2)


class Game:
    def __init__(self, port, data, proc, log):
        self.port, self.data, self.proc, self.log = port, data, proc, log

    @classmethod
    def start(cls, exe, iso, port, data, log, card=None, slot=None, hard_limit=1500):
        if gamectl.available(port):
            raise RuntimeError("something already answers on port %d" % port)
        if os.path.isdir(data):
            shutil.rmtree(data, ignore_errors=True)
        os.makedirs(data, exist_ok=True)
        if card:
            copy_tree(os.path.join(card, "mc0"), os.path.join(data, "mc0"))
        e = dict(os.environ)
        e.update({"PS2X_HIDDEN": "1", "PS2X_CONTROL": "1", "PS2X_CONTROL_PORT": str(port), "VCS_DATA_DIR": data,
                  "VCS_KEEP_SETTINGS": "1", "VCS_WATCHDOG": "10", "PS2X_CAPTURE_SIZE": "960x672", "VCS_FULLSCREEN": "0"})
        e.pop("PS2X_GS", None)  # never the GPU path
        if slot:
            e["VCS_LOAD_SLOT"] = str(slot)
        os.makedirs(os.path.dirname(log), exist_ok=True)
        f = open(log, "w", encoding="utf-8", errors="replace")
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        proc = subprocess.Popen([exe, iso], cwd=data, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT,
                                env=e, creationflags=flags)
        with _pids_lock:
            _pids.add(proc.pid)
        g = cls(port, data, proc, log)
        g._logf = f
        g.t_hard = time.time() + hard_limit
        t0 = time.time()
        while time.time() - t0 < 90:
            if proc.poll() is not None:
                raise RuntimeError("lcs exited at start (code %s)" % proc.returncode)
            if gamectl.available(port):
                return g
            time.sleep(0.5)
        g.kill()
        raise RuntimeError("no control channel on port %d" % port)

    def alive(self):
        return self.proc.poll() is None

    def kill(self):
        if self.alive():
            try:
                gamectl.send("quit", port=self.port, timeout=5)
            except Exception:
                pass
            t0 = time.time()
            while time.time() - t0 < 6 and self.alive():
                time.sleep(0.3)
        if self.alive():
            kill_pid(self.proc.pid)
        with _pids_lock:
            _pids.discard(self.proc.pid)
        try:
            self._logf.close()
        except Exception:
            pass

    def raw(self, line, timeout=30.0):
        return gamectl.send(line, port=self.port, timeout=timeout)

    def cmd(self, line, timeout=30.0):
        r = self.raw(line, timeout)
        if not r.startswith("ok"):
            raise RuntimeError("%s -> %s" % (line, r))
        return r[3:] if len(r) > 3 else ""

    def peek(self, addr, n):
        return bytes.fromhex(self.cmd("peek 0x%x %d" % (addr, n)))

    def u8(self, a):
        return self.peek(a, 1)[0]

    def u16(self, a):
        return struct.unpack("<H", self.peek(a, 2))[0]

    def u32(self, a):
        return struct.unpack("<I", self.peek(a, 4))[0]

    def press(self, key, ms=150):
        return self.cmd("press %s %d" % (key, ms))

    def key(self, key, down):
        return self.cmd("key %s %s" % (key, "down" if down else "up"))

    def frames(self):
        return self.u32(FRAMES)

    def cam_mode(self):
        active = self.u8(CAMERA + 0x10C)
        if active > 2:
            return 0xFFFF
        return self.u16(CAMERA + 0x220 + active * 0x2B0 + 0x40)

    def pos(self):
        ped = self.u32(PLAYER_PED)
        if not ped:
            return None
        return tuple(round(v, 1) for v in struct.unpack("<3f", self.peek(ped + 0x30, 12)))

    def shot(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return self.cmd("screenshot " + path)

    def perf(self):
        m = re.search(r"game_fps=([\d.]+)", self.cmd("perf"))
        return float(m.group(1)) if m else None

    def log_text(self):
        try:
            return open(self.log, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            return ""

    # ---- flows ----
    def gameplay(self):
        """Player exists, follow camera, controls on, frames advance."""
        ped = self.u32(PLAYER_PED)
        if not ped or self.u16(PAD0_DISABLE) or self.cam_mode() != CAM_FOLLOW_PED:
            return False
        f0 = self.frames()
        time.sleep(1.0)
        return self.frames() > f0 + 5

    def new_game(self, timeout=300.0):
        """Empty card: the game starts a new game by itself after the intro movies; Cross skips movies and the opening
        cutscene. Returns True once in gameplay (VIC1 running)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not self.alive():
                return False
            try:
                if self.u32(SCRIPT_SPACE) and self.gameplay():
                    time.sleep(3.0)
                    return True
                self.press("Cross", 200)
            except Exception:
                pass
            time.sleep(2.5)
        return False

    def free_roam(self):
        """End the running opening mission: MISSION_HAS_FINISHED + TERMINATE_THIS_SCRIPT at its script's IP (the game
        clears the mission flag).

        MISSION_HAS_FINISHED (LCS 0x00DD: case 0x10FFFC of the 0xDB.. handler FUN_0010FF30, mission scripts only) runs
        CTheScripts::MissionCleanUp.Process (0x1089B0): the mission's peds and cars become random ones again and its
        special characters are released. A bare TERMINATE skipped that: VIC1's Vincenzo (special character slot 0 =
        model 109, waiting in his Kuruma next to Toni) stayed a MISSION_CHAR. The next mission that loads a different
        special character into slot 0 (LOAD_SPECIAL_CHARACTER -> CStreaming::RequestSpecialModel 0x18FF80) removes the
        model's peds that CanBeDeleted (0x1E0DC8: not mission chars) and frees model 109 anyway (0x18C428): Vincenzo was
        drawn from freed memory - the 60 / 106 / 55 hang (an element's material count read from reused memory,
        0x2E87F0) and the 91 corruption (the fade path 0x2D6918 writes alpha bytes through that material list:
        0xFF at random addresses, VU0 micro memory 0xD91 and the world sector lists among them). docs/game-internals.md."""
        space = self.u32(SCRIPT_SPACE)
        s, n = self.u32(ACTIVE_SCRIPTS), 0
        while s and n < 256:
            name = self.peek(s + 0x10, 8).split(b"\0")[0]
            if name.lower() == b"vic1":
                ip = self.u32(s + 0x18)
                self.cmd("poke 0x%x dd004e00" % ((space + ip) & 0x1FFFFFF))
                break
            s, n = self.u32(s), n + 1
        t0 = time.time()
        while time.time() - t0 < 10:
            time.sleep(0.5)
            if self.u8(MISSION) == 0:
                time.sleep(2.0)
                return True
        return False


def parse_indices(spec, all_ids):
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if part == "all":
            out += all_ids
        elif "-" in part:
            a, b = part.split("-")
            out += [i for i in all_ids if int(a) <= i <= int(b)]
        else:
            out.append(int(part))
    seen, res = set(), []
    for i in out:
        if i not in seen:
            seen.add(i)
            res.append(i)
    return res


def parse_list(reply):
    out = {}
    for item in reply.split(";"):
        item = item.strip()
        if ":" in item:
            k, v = item.split(":", 1)
            if k.strip().isdigit():
                out[int(k)] = v.strip()
    return out


LOG_BAD = re.compile(r"dormant|unlisted-resume|\berror\b|exception|access violation|fault|abort|unhandled", re.I)
LOG_IGNORE = re.compile(r"MCMAN|module arena exhausted|sceCdSearchFile", re.I)


def run_one(a, idx, name, worker):
    port = a.port_base + worker
    data = os.path.join(a.work, "data%d" % worker)
    base = os.path.join(a.out, "shots")
    log = os.path.join(a.out, "logs", "%03d.log" % idx)
    res = {"index": idx, "name": name, "result": "?", "fps": None, "notes": []}
    notes = res["notes"]
    t_start = time.time()
    g = None
    try:
        g = Game.start(a.exe, a.iso, port, data, log, card=a.card, slot=a.slot)
        if a.card:
            ok = False
            t0 = time.time()
            while time.time() - t0 < 240:
                try:
                    if g.u32(SCRIPT_SPACE) and g.gameplay():
                        ok = True
                        break
                except Exception:
                    pass
                if not g.alive():
                    break
                time.sleep(2)
            time.sleep(4)
        else:
            ok = g.new_game()
            if ok:
                ok = g.free_roam()
                if not ok:
                    notes.append("could not end VIC1")
        if not g.alive():
            res["result"] = "CRASH"
            notes.append("died before the launch (exit %s)" % g.proc.returncode)
            return res
        if not ok:
            res["result"] = "BOOT"
            notes.append("no gameplay after the boot")
            g.shot(os.path.join(base, "%03d_boot.png" % idx))
            return res
        res["boot_s"] = round(time.time() - t_start)
        try:
            lst = parse_list(g.cmd("trainer list missions")); lst.update(parse_list(g.cmd("trainer list jobs")))
            res["name"] = lst.get(idx, res["name"])
        except Exception:
            pass
        # launch
        started = False
        for attempt in range(4):
            r = g.raw("trainer mission %d" % idx, timeout=40)
            time.sleep(3.0)
            if g.u8(MISSION):
                started = True
                break
            notes.append("launch try %d: %s" % (attempt + 1, r[:60]))
            time.sleep(3.0)
        if not started:
            res["result"] = "NOSTART"
            g.shot(os.path.join(base, "%03d_nostart.png" % idx))
            return res
        t0 = time.time()
        f_last, t_last_adv = g.frames(), time.time()
        res["frame0"] = f_last
        flag_hist = [1]
        cams, fps, shots = set(), [], {}
        freeze = False
        ended_at = None
        cross = (idx % 2 == 1) if a.cutscenes == "odd" else (a.cutscenes == "all")
        last_pos = None
        while time.time() - t0 < a.secs:
            el = time.time() - t0
            if not g.alive():
                res["result"] = "CRASH"
                notes.append("process exited (code %s) at %.0f s" % (g.proc.returncode, el))
                break
            try:
                f, flag, cam = g.frames(), g.u8(MISSION), g.cam_mode()
                last_pos = g.pos() or last_pos
            except Exception as e:
                notes.append("control error at %.0f s: %s" % (el, str(e)[:60]))
                time.sleep(2)
                continue
            cams.add(cam)
            if flag != flag_hist[-1]:
                flag_hist.append(flag)
                if flag == 0 and ended_at is None:
                    ended_at = el
            if f != f_last:
                f_last, t_last_adv = f, time.time()
            elif time.time() - t_last_adv >= a.freeze_secs:
                freeze = True
                notes.append("frame counter still %d for %.0f s at %.0f s" % (f, time.time() - t_last_adv, el))
                try:
                    g.shot(os.path.join(base, "%03d_freeze.png" % idx))
                except Exception:
                    pass
                break
            if cross and cam != CAM_FOLLOW_PED:
                try:
                    g.press("Cross", 150)
                except Exception:
                    pass
            for tag, at in (("a", 25), ("b", 55), ("c", a.secs - 8)):
                if tag not in shots and el >= at:
                    try:
                        p = os.path.join(base, "%03d_%s.png" % (idx, tag))
                        g.shot(p)
                        shots[tag] = p
                    except Exception:
                        pass
            if len(fps) < 3 and el >= 15 * (len(fps) + 1):
                try:
                    v = g.perf()
                    if v:
                        fps.append(v)
                except Exception:
                    pass
            time.sleep(2.0)
        res["frames"] = (f_last - res["frame0"]) if f_last >= res["frame0"] else None
        res["secs"] = round(time.time() - t0)
        res["fps"] = round(sum(fps) / len(fps), 1) if fps else None
        res["flag"] = "..".join(str(x) for x in flag_hist)
        res["cams"] = sorted(cams)
        res["pos"] = last_pos
        # log scan
        text = g.log_text()
        bad = [l.strip()[:140] for l in text.splitlines() if LOG_BAD.search(l) and not LOG_IGNORE.search(l)]
        dormant = [l for l in bad if "dormant" in l]
        if res["result"] == "?":
            if dormant:
                res["result"] = "DORMANT"
            elif freeze:
                res["result"] = "FREEZE"
            elif not g.alive():
                res["result"] = "CRASH"
            else:
                res["result"] = "ENDED" if flag_hist[-1] == 0 else "OK"
        if dormant and res["result"] not in ("DORMANT",):
            notes.append("dormant in log: " + dormant[0])
        for l in bad[:3]:
            if l not in notes:
                notes.append("log: " + l)
        # darkness of the last screenshot
        last = shots.get("c") or shots.get("b")
        if last:
            st = png_stats(last)
            if st:
                res["luma"] = round(st[0], 1)
                if st[0] < 6 and res["result"] in ("OK", "ENDED"):
                    res["result"] = "DARK"
                    notes.append("final screenshot nearly black (mean %.1f)" % st[0])
        if res["result"] == "ENDED":
            notes.append("mission flag 1->0 after %.0f s (passed / failed)" % (ended_at or 0))
        if res["result"] == "OK" and not (CAM_FOLLOW_PED in cams or any(c != 17 for c in cams)):
            notes.append("camera stayed in mode 17 (cutscene)")
        res["cutscene_skip"] = cross
    except Exception as e:
        res["result"] = "ERROR"
        notes.append(str(e)[:160])
    finally:
        if g is not None:
            if res["result"] in ("CRASH", "FREEZE", "DORMANT"):
                try:
                    tail = g.log_text().splitlines()[-12:]
                    res["log_tail"] = [l[:160] for l in tail]
                except Exception:
                    pass
            g.kill()
    return res


def table_row(r):
    return "| %s | %s | %s | %s | %s |" % (r["index"], r["name"], r["result"],
                                          ("%.0f" % r["fps"]) if r.get("fps") else "-", "; ".join(r["notes"])[:160] or "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--indices", default="96-117,9,48-61,63-95,10,11,12,14,21,22",
                    help="mission indices (ranges, 'all'); default: 96-117 first, then the rest")
    ap.add_argument("--secs", type=int, default=100)
    ap.add_argument("--freeze-secs", dest="freeze_secs", type=int, default=20,
                    help="frame counter still this long = FREEZE (a mission load can stall ~10 s under load)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--port-base", dest="port_base", type=int, default=49100)
    ap.add_argument("--exe", default=default_exe())
    ap.add_argument("--iso", default=default_iso())
    ap.add_argument("--work", default=DEFAULT_WORK, help="data folders live here (data0..); results in <work>\\missions")
    ap.add_argument("--card", help="a folder holding mc0/ (boot-load --slot from it instead of a new game)")
    ap.add_argument("--slot", type=int)
    ap.add_argument("--cutscenes", choices=("odd", "all", "none"), default="odd",
                    help="press Cross through cutscenes: on odd indices only (default), always, never")
    ap.add_argument("--list", action="store_true", help="boot once, print the launcher's mission list and exit")
    a = ap.parse_args()
    if not a.exe or not os.path.isfile(a.exe):
        sys.exit("no lcs executable (--exe / VCS_EXE)")
    if not a.iso or not os.path.isfile(a.iso):
        sys.exit("no LCS disc image (--iso / LCS_ISO)")
    a.out = os.path.join(a.work, "missions")
    os.makedirs(a.out, exist_ok=True)
    all_ids = [9] + list(range(48, 62)) + list(range(63, 118)) + [10, 11, 12, 14, 21, 22]  # trainer list missions / jobs
    names = {}
    if a.list:
        g = Game.start(a.exe, a.iso, a.port_base + 99, os.path.join(a.work, "data_list"),
                       os.path.join(a.out, "logs", "list.log"))
        try:
            t0 = time.time()
            while time.time() - t0 < 120 and len(names) < 50:
                names = parse_list(g.cmd("trainer list missions"))
                time.sleep(2)
            names.update(parse_list(g.cmd("trainer list jobs")))
        finally:
            g.kill()
        for k in sorted(names):
            print(k, names[k])
        return
    indices = parse_indices(a.indices, sorted(all_ids))
    print("[missions] %d missions, %d s each, %d at a time" % (len(indices), a.secs, a.jobs), flush=True)
    results_path = os.path.join(a.out, "results.jsonl")
    lock = threading.Lock()
    results = {}
    free = list(range(a.jobs))

    def job(idx):
        with lock:
            w = free.pop(0)
        try:
            r = run_one(a, idx, names.get(idx, ""), w)
        finally:
            with lock:
                free.append(w)
        with lock:
            results[idx] = r
            with open(results_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(r) + "\n")
            print(table_row(r), flush=True)
        return r

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        list(ex.map(job, indices))
    rows = ["| index | name | result | fps | notes |", "|---|---|---|---|---|"] + [table_row(results[i]) for i in sorted(results)]
    with open(os.path.join(a.out, "table.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    print("\n".join(rows))
    bad = [r for r in results.values() if r["result"] not in ("OK", "ENDED")]
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
