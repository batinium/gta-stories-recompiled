#!/usr/bin/env python3
"""Scripted screenshots and video clips of LCS for a release page (one command re-captures everything).

    python tools/media_capture.py --exe <build>\\lcs.exe --iso <disc.iso> --save <folder holding mc0/> --out <folder>
        [--port-base 49700] [--only portland_sunset,clip_portland] [--no-video] [--list]

How it works
  * Starts hidden instances of the game (PS2X_HIDDEN=1, VCS_KEEP_SETTINGS=1, own memory card copy in <out>/data<k>),
    each at a chosen resolution: PS2X_HIDDEN_WINDOW=WxH (the window the picture is laid out for: Hor+ widescreen follows
    its aspect) + PS2X_CAPTURE_SIZE=WxH (the size of control-channel screenshots and of `burst` frames).
  * Screenshots: gamectl `screenshot` (the picture plus whatever overlay / menu is open).
  * Video: the runtime's `burst DIR N` command writes every presented frame as a PNG. With VCS_TIMESTEP=60 the game
    clock steps exactly 1/60 s per frame and is not paced, so a slow capture (PNG writing) never changes the game's
    speed: the clip is frame exact, 60 fps. The frames are then encoded with ffmpeg (H.264, yuv420p, CRF 18). The clips
    carry no sound: the game's audio runs on the host clock, which an unpaced capture does not follow.
  * The world is set up with the trainer's control command (`trainer teleport|time|weather|spawn|setwanted|...`, see
    src/game/lcs_trainer.cpp) and `gamectl press/key`.
Output: <out>/shots/*.png, <out>/clips/*.mp4, <out>/index.md (file + caption) and <out>/contact_sheet.png (ffmpeg tile).
Everything is data in SHOTS / CLIPS below: add a row to add a picture.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402

FFMPEG = shutil.which("ffmpeg")

# ---------------------------------------------------------------- the list of pictures
# name, caption, setup (applied in order), resolution (key of RES), ui (what to open before the picture)
# setup items: ("goto", landmark-name-substring) | ("time", h) | ("weather", n) | ("vehicle", [names tried]) |
#              ("wanted", n) | ("wait", s) | ("keys", [(key, ms)]) | ("cmd", raw trainer command)
RES = {"fhd": "1920x1080", "uw": "2560x1080", "43": "1440x1080", "qhd": "2560x1440"}

# Coordinates (x, y) are the trainer's own landmark table (src/game/lcs_trainer.cpp); ("xy", x, y) = `trainer goto x y 3`
# (z 3 over water: the player falls in and the spawned boat is beside him).
SEA = ("xy", 1281, -1320)  # the bay south of Atlantic Quays
CAR = ["Kuruma", "Leone Sentinel", "Sentinel", "Stallion"]
BIKE = ["Sanchez", "PCJ", "Freeway"]
BOAT = ["Speeder", "Reefer", "Predator"]
SHOTS = [
    dict(name="portland_sunset", res="fhd", caption="Portland at sunset, driving",
         setup=[("goto", "Hepburn Heights"), ("time", 19), ("weather", 0), ("vehicle", CAR), ("drive", 3), ("time", 19), ("wait", 7)]),
    dict(name="staunton_skyline_day", res="fhd", caption="Staunton Island in daylight",
         setup=[("goto", "Belleville Park"), ("time", 12), ("weather", 4), ("wait", 8)]),
    dict(name="shoreside_vale", res="fhd", caption="Shoreside Vale",
         setup=[("goto", "Wichita Gardens"), ("time", 15), ("weather", 0), ("wait", 12)]),
    dict(name="bike_ride", res="fhd", caption="Riding a motorbike in Portland",
         setup=[("goto", "Hepburn Heights"), ("time", 16), ("weather", 0), ("vehicle", BIKE), ("drive", 3), ("wait", 14)]),
    dict(name="boat", res="fhd", caption="A speedboat in Portland's bay",
         setup=[SEA, ("time", 14), ("weather", 0), ("vehicle", BOAT), ("drive", 3), ("wait", 14)]),
    dict(name="chase_wanted", res="fhd", caption="A chase with wanted stars",
         setup=[("goto", "Hepburn Heights"), ("time", 21), ("weather", 0), ("vehicle", CAR), ("wanted", 4), ("drive", 6), ("wait", 5)]),
    dict(name="pause_map", res="fhd", caption="The pause menu map",
         setup=[("goto", "Hepburn Heights"), ("time", 12), ("wait", 6)], ui="pause_map"),
    dict(name="settings_f2", res="fhd", caption="The F2 settings menu", setup=[("goto", "Hepburn Heights"), ("time", 12), ("wait", 7)], ui="menu"),
    dict(name="trainer_f5", res="fhd", caption="The F5 trainer", setup=[("goto", "Hepburn Heights"), ("time", 12), ("wait", 7)], ui="trainer"),
    dict(name="perf_overlay", res="fhd", caption="The F3 performance overlay",
         setup=[("goto", "Hepburn Heights"), ("time", 12), ("weather", 0), ("wait", 8)], ui="perf"),
    dict(name="ratio_21x9", res="uw", caption="21:9 (2560x1080): Hor+ widescreen shows more of the street",
         setup=[("goto", "Belleville Park"), ("time", 12), ("weather", 4), ("wait", 14)]),
    dict(name="ratio_4x3", res="43", caption="4:3 (1440x1080): the same place at the PS2's ratio",
         setup=[("goto", "Belleville Park"), ("time", 12), ("weather", 4), ("wait", 8)]),
]

# 15-30 s clips (60 fps, frame exact): keys held for the whole clip
CLIPS = [
    dict(name="clip_portland_drive", res="fhd", caption="Driving through Portland", seconds=20,
         setup=[("goto", "Hepburn Heights"), ("time", 12), ("weather", 0), ("vehicle", CAR), ("wait", 4)], hold=["W"]),
    dict(name="clip_chase", res="fhd", caption="A police chase", seconds=20,
         setup=[("goto", "Hepburn Heights"), ("time", 21), ("weather", 0), ("vehicle", CAR), ("wanted", 4), ("wait", 3)], hold=["W"]),
    dict(name="clip_boat", res="fhd", caption="A boat ride", seconds=20,
         setup=[SEA, ("time", 14), ("weather", 0), ("vehicle", BOAT), ("wait", 12)], hold=["W"]),
    dict(name="clip_mission_start", res="fhd", caption="A mission start (cutscene)", seconds=25,
         setup=[("goto", "Hepburn Heights"), ("time", 12), ("cmd", "mission 48"), ("wait", 2)], hold=[]),
]


# ---------------------------------------------------------------- one running game
class Game:
    def __init__(self, exe, iso, save, out, port, res, extra_env=None, tag="g"):
        self.port, self.res = port, res
        self.data = os.path.join(out, "data%d" % port)
        if os.path.isdir(self.data):
            shutil.rmtree(self.data)
        os.makedirs(self.data)
        if save:
            shutil.copytree(os.path.join(save, "mc0"), os.path.join(self.data, "mc0"))
        env = dict(os.environ, PS2X_HIDDEN="1", VCS_KEEP_SETTINGS="1", VCS_DATA_DIR=self.data, PS2X_CONTROL_PORT=str(port),
                   PS2X_HIDDEN_WINDOW=res, PS2X_CAPTURE_SIZE=res, VCS_SKIP_MOVIES="1", PS2X_AUDIO="0")
        env.update(extra_env or {})
        self.log = open(os.path.join(out, "%s_%d.log" % (tag, port)), "wb")
        self.proc = subprocess.Popen([exe, iso], cwd=os.path.dirname(os.path.abspath(exe)), env=env, stdout=self.log,
                                     stderr=self.log, stdin=subprocess.DEVNULL)
        if not gamectl.wait(port=port, timeout=180):
            self.kill()
            raise RuntimeError("the game did not open its control channel on port %d (see the log; Windows may reserve "
                               "the port: netsh int ipv4 show excludedportrange protocol=tcp)" % port)

    def cmd(self, line, timeout=90):
        return gamectl.send(line, port=self.port, timeout=timeout)

    def wait_ingame(self, timeout=240):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                f0 = int(self.cmd("frames").split()[-1])
                time.sleep(1.0)
                if int(self.cmd("frames").split()[-1]) > f0 + 5 and "ready=1" in self.cmd("trainer state"):
                    return
            except Exception:
                time.sleep(1.0)
        raise RuntimeError("not in game after %d s" % timeout)

    def kill(self):
        try:
            self.cmd("quit", timeout=5)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=8)
        except Exception:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True)
        self.log.close()


def trainer(g, line):
    reply = g.cmd("trainer " + line)
    if not reply.startswith("ok"):
        print("  [warn] trainer %s -> %s" % (line, reply))
    return reply


def find_index(g, what, names):
    """The id of the first `trainer list <what>` entry ("ok 12:Name; 13:Name;") whose name contains one of `names`."""
    reply = g.cmd("trainer list " + what)
    items = []
    for part in reply.split(";"):
        part = part.strip()
        if part.startswith("ok "):
            part = part[3:]
        if ":" in part:
            i, name = part.split(":", 1)
            try:
                items.append((int(i), name))
            except ValueError:
                pass
    for n in names:
        for i, name in items:
            if n.lower() in name.lower():
                return i
    return None


def apply_setup(g, setup):
    trainer(g, "toggle freeze 0")
    trainer(g, "setwanted 0")
    for item in setup:
        kind, arg = item[0], (item[1] if len(item) > 1 else None)
        if kind == "goto":
            i = find_index(g, "landmarks", [arg])
            if i is None:
                print("  [warn] no landmark like %r" % arg)
            else:
                trainer(g, "teleport %d" % i)
        elif kind == "xy":
            trainer(g, "goto %d %d 3" % (item[1], item[2]))
            time.sleep(1.0)
        elif kind == "drive":  # W held for `arg` s, then released (the car rolls on)
            g.cmd("key W down")
            time.sleep(arg)
            g.cmd("key W up")
        elif kind == "time":
            trainer(g, "time %d" % arg)
            trainer(g, "toggle freeze 1")  # the clock stays where it was put
        elif kind == "weather":
            trainer(g, "weather %d" % arg)
        elif kind == "vehicle":
            i = find_index(g, "vehicles", arg)
            if i is None:
                print("  [warn] none of the vehicles %s" % arg)
            else:
                trainer(g, "spawn %d" % i)
        elif kind == "wanted":
            trainer(g, "setwanted %d" % arg)
        elif kind == "cmd":
            trainer(g, arg)
        elif kind == "keys":
            for key, ms in arg:
                g.cmd("press %s %d" % (key, ms))
        elif kind == "wait":
            time.sleep(arg)


def open_ui(g, ui):
    if ui == "menu":
        g.cmd("menu")  # the settings menu on its first page
        time.sleep(1.5)
    elif ui == "trainer":
        g.cmd("press F5 150")
        time.sleep(1.5)
    elif ui == "perf":
        g.cmd("press F3 150")
        time.sleep(2.0)
    elif ui == "pause_map":
        g.cmd("press Escape 150")  # Start: the pause menu opens on its Map page
        time.sleep(2.0)


def close_ui(g, ui):
    if ui == "menu":
        g.cmd("menu close")
    elif ui == "trainer":
        g.cmd("press F5 150")
    elif ui == "perf":
        g.cmd("press F3 150")
    elif ui == "pause_map":
        g.cmd("press Escape 150")
    time.sleep(0.5)


# ---------------------------------------------------------------- outputs
def encode(frames_dir, out_mp4, res, fps=60):
    """frames_dir/frames.rgb (RGB24, `res` = WxH) -> H.264 yuv420p CRF 18."""
    if not FFMPEG:
        raise RuntimeError("ffmpeg not found (winget install Gyan.FFmpeg)")
    subprocess.run([FFMPEG, "-y", "-loglevel", "error", "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", res,
                    "-framerate", str(fps), "-i", os.path.join(frames_dir, "frames.rgb"),
                    "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                    out_mp4], check=True)


def record(g, frames_dir, seconds, fps=60):
    """`burst DIR N raw`: the runtime appends every presented frame to DIR/frames.rgb; burst.txt gets a line per frame."""
    os.makedirs(frames_dir, exist_ok=True)
    n = int(seconds * fps)
    reply = g.cmd("burst %s %d raw" % (frames_dir.replace("\\", "/"), n))
    if not reply.startswith("ok"):
        raise RuntimeError("burst: " + reply)
    last, stall = -1, 0
    while True:
        try:
            with open(os.path.join(frames_dir, "burst.txt"), "rb") as f:
                have = f.read().count(b"\n")
        except OSError:
            have = 0
        if have >= n:
            break
        stall = stall + 1 if have == last else 0
        if stall > 180:
            raise RuntimeError("burst stalled at %d / %d frames" % (have, n))
        last = have
        time.sleep(1.0)
    time.sleep(1.5)


def contact_sheet(shots_dir, out_png):
    if not FFMPEG:
        return
    files = sorted(glob.glob(os.path.join(shots_dir, "*.png")))
    if not files:
        return
    inputs = []
    for f in files:
        inputs += ["-i", f]
    cols = 4
    rows = (len(files) + cols - 1) // cols
    fc = "".join("[%d:v]scale=480:270:force_original_aspect_ratio=decrease,pad=480:270:(ow-iw)/2:(oh-ih)/2[v%d];" % (i, i)
                 for i in range(len(files)))
    fc += "".join("[v%d]" % i for i in range(len(files))) + "xstack=inputs=%d:layout=%s[o]" % (
        len(files), "|".join("%d_%d" % ((i % cols) * 480, (i // cols) * 270) for i in range(len(files))))
    subprocess.run([FFMPEG, "-y", "-loglevel", "error"] + inputs + ["-filter_complex", fc, "-map", "[o]", "-frames:v", "1", out_png])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="lcs.exe")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--save", required=True, help="a folder holding mc0/ with a free-roam save")
    ap.add_argument("--out", required=True, help="output folder (not inside the repository)")
    ap.add_argument("--port-base", type=int, default=49700, help="control port of the first instance")
    ap.add_argument("--only", default="", help="comma separated names to (re)capture")
    ap.add_argument("--no-video", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for s in SHOTS + CLIPS:
            print(s["name"], "-", s["caption"])
        return
    only = {x for x in a.only.split(",") if x}
    shots_dir, clips_dir = os.path.join(a.out, "shots"), os.path.join(a.out, "clips")
    os.makedirs(shots_dir, exist_ok=True)
    os.makedirs(clips_dir, exist_ok=True)
    rows, k = [], 0
    jobs = [("shot", s) for s in SHOTS] + ([] if a.no_video else [("clip", c) for c in CLIPS])
    games = {}
    try:
        for kind, job in jobs:
            if only and job["name"] not in only:
                continue
            res = RES[job["res"]]
            extra = {"VCS_TIMESTEP": "60"} if kind == "clip" else {}
            key = (res, kind)
            if key not in games:
                games[key] = Game(a.exe, a.iso, a.save, a.out, a.port_base + k, res, extra, tag=kind)
                k += 1
                games[key].wait_ingame()
                games[key].cmd("setting presets.balanced")  # Hor+ at the window's shape, HUD at the edges, 2x, AA, 60 fps
                time.sleep(3.0)
                games[key].wait_ingame()
                trainer(games[key], "toggle god 1")
                trainer(games[key], "toggle vehicle 1")
            g = games[key]
            print("[%s] %s" % (kind, job["name"]))
            apply_setup(g, job["setup"])
            if kind == "shot":
                path = os.path.join(shots_dir, job["name"] + ".png")
                if job.get("ui"):
                    open_ui(g, job["ui"])
                print("  ", g.cmd("screenshot " + path.replace("\\", "/")))
                if job.get("ui"):
                    close_ui(g, job["ui"])
                rows.append((os.path.relpath(path, a.out), job["caption"], res))
            else:
                frames = os.path.join(a.out, "frames_" + job["name"])
                if os.path.isdir(frames):
                    shutil.rmtree(frames)
                for key_ in job.get("hold", []):
                    g.cmd("key %s down" % key_)
                record(g, frames, job["seconds"])
                for key_ in job.get("hold", []):
                    g.cmd("key %s up" % key_)
                mp4 = os.path.join(clips_dir, job["name"] + ".mp4")
                encode(frames, mp4, res)
                shutil.rmtree(frames)
                rows.append((os.path.relpath(mp4, a.out), job["caption"], res))
    finally:
        for g in games.values():
            g.kill()
    contact_sheet(shots_dir, os.path.join(a.out, "contact_sheet.png"))
    rows = []
    for job in SHOTS + ([] if a.no_video else CLIPS):
        sub, ext = ("shots", ".png") if job in SHOTS else ("clips", ".mp4")
        path = os.path.join(a.out, sub, job["name"] + ext)
        if os.path.isfile(path):
            rows.append((os.path.join(sub, job["name"] + ext), job["caption"], RES[job["res"]]))
    with open(os.path.join(a.out, "index.md"), "w", encoding="utf-8") as f:
        f.write("# LCS media\n\n| File | Caption | Resolution |\n|---|---|---|\n")
        for rel, cap, res in rows:
            f.write("| %s | %s | %s |\n" % (rel.replace("\\", "/"), cap, res))
    print("done: %d items, index %s" % (len(rows), os.path.join(a.out, "index.md")))


if __name__ == "__main__":
    main()
