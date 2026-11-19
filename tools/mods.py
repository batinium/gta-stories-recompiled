#!/usr/bin/env python3
"""The mod manager, for both games (docs/ANDROID.md "Mods", docs/LCS.md "Mods"). Standard library only. The desktop
launcher's Mods sheet runs it; the Android launcher does the same in ModFormats.java / ModsMenu.java with the same
storage and the same slot tables (tools/modslots/mod_slots.txt).

    python tools/mods.py --game lcs|vcs list                      the mods, what each changes, conflicts
    python tools/mods.py --game lcs import FILE [FILE...] [--name NAME] [--in-place]
                                                                  a .zip, a VC Mod Manager .vcm, or loose files
                                                                  (handling.cfg, weapon.dat, readme, .dff + .txd,
                                                                  .gxt): one mod; .rar / .7z are refused.
                                                                  --in-place: "keep where it is" (no copy: the game
                                                                  reads the originals; don't move or delete them)
    python tools/mods.py --game lcs relocate ID FILE [FILE...] [--copy]
                                                                  "find it again": the same file(s) (same contents)
                                                                  at a new place; the mod reads them there from now
                                                                  on (--copy: imports a copy of them instead)
    python tools/mods.py --game lcs store ID copy|in_place        switch: import a copy of the originals / read the
                                                                  originals where they are again (the app's copy is
                                                                  deleted; the originals must be where they were)
    python tools/mods.py --game lcs enable|disable|remove ID
    python tools/mods.py --game lcs retarget ID ENTRY SLOT        ENTRY: the entry's number (list); SLOT: a model id /
                                                                  weapon type, a name, or "auto" (match again)
    python tools/mods.py --game lcs prefer ID ENTRY               a conflict: this entry wins its slot (the other
                                                                  mods' entries on it are set aside)
    python tools/mods.py --game lcs slots vehicle|weapon          the slot picker's list
    python tools/mods.py --game lcs loader on|off                 off: the stock game (no active.txt)
    python tools/mods.py --game lcs status                        the loader and the game's last start (applied.txt)
    python tools/mods.py --game lcs sounds ISO [--find COLT45]    [lcs] the effect sounds' ids, names, rates, loops
    python tools/mods.py --game lcs sound FILE --id N [--id M] | --sound-name PREFIX --iso ISO [--mod ID] [--name N]
                                                                  [lcs] a .wav/.mp3/.ogg/.flac as effect sound(s)
                                                                  (a "sound" mod: src/game/lcs_sfxmod.cpp)
    python tools/mods.py --game lcs music                         [lcs] the user-tracks folder (<data>/music) and its
                                                                  files (Audio > User tracks picks the station)

--json prints the answer as JSON; with VCS_PROGRESS=json (the launcher) it comes as an `@vcs {"result": ...}` event.
--data DIR overrides the game's data folder (else VCS_DATA_DIR, else the game's own, as backup.py finds it).

Storage, per game, in <data>/mods/: <id>/manifest.json (the same as the Android app's), <id>/source/ (an imported
mod: one copy of every file picked, so the player can delete the originals; the game reads models and texts from it,
inside a zip or .vcm too), <id>/files/ (only what can't be read from source/: a file inside a .vcm inside a zip, and
mods imported before), <id>/converted/ (the game's converted-model cache, keyed by source hash + converter version),
active.txt (what the game applies at its next start, src/game/vcs_mods.h), applied.txt (what it did at its last
start), stock.json (the original values the game reported, for the "what is modified" view).

A mod "kept where it is" ("storage": "in_place" in its manifest; none = "copy") has no source/: each picked file's
"path" in "sources" is read where it is (the Android app's are "uri"s, read through a persisted permission). A source
that isn't there any more makes the mod "original missing": shown, skipped by the game (active.txt "needs" lines), and
"relocate" finds it again or imports a copy. Mods are other people's files: they are only parsed here, never run, and
never go into the repository.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _game_from_argv():
    for i, arg in enumerate(sys.argv):
        if arg == "--game" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if arg.startswith("--game="):
            return arg.partition("=")[2]
    return None


if __name__ == "__main__" and _game_from_argv():
    os.environ["VCS_TOOLS_GAME"] = _game_from_argv()
import toolchain as tc  # noqa: E402
import backup  # noqa: E402  (data_dir(): the one place that knows the game's data folder)

SLOTS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modslots", "mod_slots.txt")
MAX_FILES = 512
MAX_TOTAL = 256 << 20
MAX_TEXT = 1 << 20
MAX_MODEL = 32 << 20
MAX_PICKED = 128 << 20
MAX_GXT = 32 << 20


class ModError(Exception):
    def __init__(self, message, code="mod_error"):
        super().__init__(message)
        self.code = code


# ---- the slot tables (shared with Android) ------------------------------------------------------------------------

def norm(name):
    return re.sub(r"[^a-z0-9]", "", (name or "").lower())


class Slots:
    """One game's vehicles and weapons, and the name -> slot rules (mod_slots.txt)."""

    def __init__(self, game, path=SLOTS_FILE):
        self.game = game
        self.vehicles, self.weapons = [], []
        self.lookup = {"vehicle": {}, "weapon": {}}  # normalised name -> (slot, reason)
        with open(path, encoding="utf-8") as f:
            for n, raw in enumerate(f, 1):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                cols = [c.strip() for c in line.split("|")]
                head = cols[0].split()
                if len(cols) != 6 or len(head) < 3 or head[0] not in ("vcs", "lcs"):
                    raise ValueError("%s:%d: bad line" % (path, n))
                if head[0] != game:
                    continue
                kind = head[1]
                slot = {"id": int(head[2]), "model": cols[1], "label": cols[2], "class": cols[3],
                        "names": cols[4].split(), "similar": cols[5].split()}
                if kind == "weapon":
                    slot["modelId"] = int(head[3]) if len(head) > 3 and head[3] != "-" else -1
                    self.weapons.append(slot)
                elif kind == "vehicle":
                    slot["modelId"] = slot["id"]
                    self.vehicles.append(slot)
                else:
                    raise ValueError("%s:%d: kind %s" % (path, n, kind))
                table = self.lookup[kind]
                for names, reason in (([cols[1]], "name"), (slot["names"], "alias"), (slot["similar"], "similar")):
                    for name in names:
                        k = norm(name)
                        if not k:
                            continue
                        if k in table and table[k][0] is not slot:
                            raise ValueError("%s:%d: %s %s listed twice" % (path, n, kind, name))
                        table.setdefault(k, (slot, reason))

    def match(self, kind, name):
        """(slot, reason) or (None, "none")"""
        hit = self.lookup[kind].get(norm(name))
        return hit if hit else (None, "none")

    def vehicle(self, vid):
        return next((v for v in self.vehicles if v["id"] == vid), None)

    def weapon(self, wtype):
        return next((w for w in self.weapons if w["id"] == wtype), None)

    def weapon_of_model(self, model):
        return next((w for w in self.weapons if w["modelId"] == model and model >= 0), None)


# ---- what a mod brings (the same rules as ModFormats.java) ----------------------------------------------------------

# handling.cfg car lines, one column list per PC game (gtamods.com wiki "Handling.cfg"): GTA III 31 values after the
# name, VC 32 (III's plus the anti-dive multiplier before the flags), SA 34 or 35 (turn mass, drag, engine inertia,
# high-speed damping, two hex flag words, anim group; no dimensions). Every value is passed on in the PC file's own
# units; the game converts them (vcs_mods.cpp applyHandling). "-" = no equivalent in the Stories games: reported.
GTA3 = ["mass", "dimx", "dimy", "-dimension z (not used: the turn mass comes from x and y)", "comx", "comy", "comz",
        "submerged", "tractionmult", "tractionloss", "tractionbias", "gears", "maxspeed", "accel", "drive", "engine",
        "brake", "brakebias", "abs", "steer", "suspforce", "suspdamp", "seatoffset", "collision", "value", "suspupper",
        "susplower", "suspbias", "flags", "frontlights", "rearlights"]
VC = GTA3[:28] + ["antidive"] + GTA3[28:]
SA = ["mass", "turnmass", "drag", "comx", "comy", "comz", "submerged", "tractionmult", "tractionloss",
      "tractionbias", "gears", "maxspeed", "accel", "inertia", "drive", "engine", "brake", "brakebias", "abs",
      "steer", "suspforce", "suspdamp", "-high speed damping",
      # SA's suspension limits are tuned to SA's own collision (the mod's .col / embedded collision, not used: the slot's
      # stays); on LCS's collision an SA car's 0.1 / -0.05 left the wheels off the ground (it didn't move at all), so
      # the slot's limits stay
      "-suspension upper limit (SA's, for SA's collision)", "-suspension lower limit (SA's, for SA's collision)",
      "suspbias", "antidive",
      "seatoffset", "collision", "value", "modelflags", "handlingflags", "frontlights", "rearlights",
      "-anim group"]

# GTA III / VC handling flags (one hex word; the Stories games keep VC's bits, read from LCS's own 81 records: bikes
# 0x10000, helicopters 0x20000, boats 0x80000, vans 0x20, BF Injection 0x7000000 ...). Names for the report.
VC_FLAGS = ["1G_BOOST", "2G_BOOST", "REV_BONNET", "HANGING_BOOT", "NO_DOORS", "IS_VAN", "IS_BUS", "IS_LOW",
            "DBL_EXHAUST", "TAILGATE_BOOT", "NOSWING_BOOT", "NONPLAYER_STABILISER", "NEUTRALHANDLING", "HAS_NO_ROOF",
            "IS_BIG", "HALOGEN_LIGHTS", "IS_BIKE", "IS_HELI", "IS_PLANE", "IS_BOAT", "NO_EXHAUST", "REARWHEEL_1ST",
            "HANDBRAKE_TYRE", "SIT_IN_BOAT", "FAT_REARW", "NARROW_FRONTW", "GOOD_INSAND"]
# SA's two flag words, bit by bit -> the VC bit of the same meaning (None: no equivalent, reported)
SA_MODEL_FLAGS = [("IS_VAN", 5), ("IS_BUS", 6), ("IS_LOW", 7), ("IS_BIG", 14), ("REVERSE_BONNET", 2),
                  ("HANGING_BOOT", 3), ("TAILGATE_BOOT", 9), ("NOSWING_BOOT", 10), ("NO_DOORS", 4),
                  ("TANDEM_SEATS", None), ("SIT_IN_BOAT", 23), ("CONVERTIBLE", 13), ("NO_EXHAUST", 20),
                  ("DBL_EXHAUST", 8), ("NO1FPS_LOOK_BEHIND", None), ("FORCE_DOOR_CHECK", None),
                  ("AXLE_F_NOTILT", None), ("AXLE_F_SOLID", None), ("AXLE_F_MCPHERSON", None), ("AXLE_F_REVERSE", None),
                  ("AXLE_R_NOTILT", None), ("AXLE_R_SOLID", None), ("AXLE_R_MCPHERSON", None), ("AXLE_R_REVERSE", None),
                  ("IS_BIKE", 16), ("IS_HELI", 17), ("IS_PLANE", 18), ("IS_BOAT", 19), ("BOUNCE_PANELS", None),
                  ("DOUBLE_RWHEELS", None), ("FORCE_GROUND_CLEARANCE", None), ("IS_HATCHBACK", None)]
SA_HANDLING_FLAGS = [("1G_BOOST", 0), ("2G_BOOST", 1), ("NPC_ANTI_ROLL", 11), ("NPC_NEUTRAL_HANDL", 12),
                     ("NO_HANDBRAKE", None), ("STEER_REARWHEELS", None), ("HB_REARWHEEL_STEER", None),
                     ("ALT_STEER_OPT", None), ("WHEEL_F_NARROW2", 25), ("WHEEL_F_NARROW", 25), ("WHEEL_F_WIDE", None),
                     ("WHEEL_F_WIDE2", None), ("WHEEL_R_NARROW2", None), ("WHEEL_R_NARROW", None), ("WHEEL_R_WIDE", 24),
                     ("WHEEL_R_WIDE2", 24), ("HYDRAULIC_GEOM", None), ("HYDRAULIC_INST", None), ("NOS_INST", None),
                     ("HYDRAULIC_NONE", None), ("OFFROAD_ABILITY", 26), ("OFFROAD_ABILITY2", 26),
                     ("HALOGEN_LIGHTS", 15), ("PROC_REARWHEEL_1ST", 21), ("USE_MAXSP_LIMIT", None), ("LOW_RIDER", None),
                     ("STREET_RACER", None), ("UNDEFINED", None), ("SWINGING_CHASSIS", None)]
FIRE_TYPES = ("MELEE", "INSTANT_HIT", "PROJECTILE", "AREA_EFFECT", "CAMERA", "USE")
WEAPON_COLS = ["range", "rate", "reload", "clip", "damage", "speed", "radius", "lifespan", "spread"]


def hex_word(s):
    """A handling.cfg flag word (hex, with or without 0x) -> int, None if it isn't one"""
    s = s.strip().lower()
    if s.startswith("0x"):
        s = s[2:]
    return int(s, 16) if re.fullmatch(r"[0-9a-f]{1,8}", s) else None


def sa_flags(model, handling, skipped):
    """SA's model + handling flag words -> (VC flag word, mask of the VC bits SA decides); bits without a VC meaning
    are named in `skipped`"""
    out = mask = 0
    for word, table in ((model, SA_MODEL_FLAGS), (handling, SA_HANDLING_FLAGS)):
        for bit, (name, vc) in enumerate(table):
            if vc is not None:
                mask |= 1 << vc
                if word >> bit & 1:
                    out |= 1 << vc
            elif word >> bit & 1:
                skipped.append("flag %s (no equivalent)" % name)
    return out, mask


def is_number(s):
    try:
        f = float(s)
        return f == f and f not in (float("inf"), float("-inf"))
    except ValueError:
        return False


def entry(kind, pc_name, dialect, line, source):
    return {"kind": kind, "pcName": pc_name, "dialect": dialect, "line": line, "source": source, "target": -1,
            "fields": {}, "skipped": [], "match": "none"}


def parse_handling(raw, source):
    line = raw.strip()
    if not line or not line[0].isalnum():
        return None
    t = re.split(r"[\s,]+", line)
    if len(t) < 2 or not re.fullmatch(r"[A-Za-z0-9_]{1,24}", t[0]) or is_number(t[0]):
        return None
    n = len(t) - 1
    if n == len(GTA3):
        cols, dialect = GTA3, "gta3"
    elif n == len(VC):
        cols, dialect = VC, "vc"
    elif n in (len(SA), len(SA) - 1):
        cols, dialect = SA, "sa"
    else:
        return None
    if not re.fullmatch(r"[FfRr4]", t[15]) or not re.fullmatch(r"[PpDdEe]", t[16]):
        return None
    e = entry("handling", t[0], dialect, line, source)
    words = {}
    for i in range(n):
        name, value = cols[i], t[i + 1]
        if name.startswith("-"):
            e["skipped"].append(name[1:])
            continue
        if name in ("drive", "engine"):
            e["fields"][name] = value.upper()
            continue
        if name in ("flags", "modelflags", "handlingflags"):
            w = hex_word(value)
            if w is None:
                e["skipped"].append("%s (%s is not a hex number)" % (name, value))
            else:
                words[name] = w
            continue
        if not is_number(value):
            return None
        if name == "submerged" and float(value) <= 0:
            e["skipped"].append("percent submerged (%s is not a percentage)" % value)
            continue
        if name in ("frontlights", "rearlights", "abs"):
            value = str(int(float(value)))
        e["fields"][name] = value
    # the flags as one VC-layout word plus the mask of the bits this dialect decides (the game keeps the others)
    if "flags" in words:
        mask = 0xFFFF if dialect == "gta3" else (1 << len(VC_FLAGS)) - 1
        e["fields"]["flags"] = "0x%x" % (words["flags"] & mask)
        e["fields"]["flagmask"] = "0x%x" % mask
        extra = words["flags"] & ~mask
        if extra:
            e["skipped"].append("flag bits 0x%x (no known meaning)" % extra)
    elif "modelflags" in words or "handlingflags" in words:
        f, mask = sa_flags(words.get("modelflags", 0), words.get("handlingflags", 0), e["skipped"])
        e["fields"]["flags"] = "0x%x" % f
        e["fields"]["flagmask"] = "0x%x" % mask
    return e


SA_FIRE_TYPES = FIRE_TYPES + ("AFFECT_AREA",)


def parse_weapon_sa(t, line, source):
    """SA's gun line (gtamods.com wiki "Weapon.dat"): $ NAME FIRETYPE target-range weapon-range model model2 slot
    ANIMGROUP clip damage fire-offset(3) skill required-stat accuracy move-speed anim timings... flags. One line per
    skill level (0 poor, 1 std, 2 pro): the manager keeps std (weapon_skill)."""
    if len(t) < 18 or t[2].upper() not in SA_FIRE_TYPES or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,23}", t[1]):
        return None
    # the animation group is the first word after the numbers that follow the fire type (model, model2, slot ...)
    ag = next((i for i in range(3, len(t)) if not is_number(t[i])), None)
    if ag is None or ag < 6 or ag + 7 >= len(t) or not all(is_number(x) for x in t[3:ag] + t[ag + 1:ag + 8]):
        return None
    e = entry("weapon", t[1], "sa", line, source)
    e["fields"]["range"] = t[4]  # D: the weapon's damage range (C, the aiming range, has no own field)
    e["fields"]["clip"] = str(int(float(t[ag + 1])))
    e["fields"]["damage"] = str(int(float(t[ag + 2])))
    if any(float(x) for x in t[ag + 3:ag + 6]):  # the muzzle flash position (all 0: none given)
        for k, axis in enumerate(("firex", "firey", "firez")):
            e["fields"][axis] = t[ag + 3 + k]
    e["skill"] = int(float(t[ag + 6]))
    e["skipped"] += ["fire type (kept)", "aiming range", "models, slot, animation group",
                     "skill level %s, accuracy, move speed, animation timings, flags" % t[ag + 6]]
    return e


def parse_weapon(raw, source):
    line = raw.strip()
    t = re.split(r"[\s,]+", line)
    if t and t[0] == "$":
        return parse_weapon_sa(t, line, source)
    if len(t) < 11 or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,23}", t[0]):
        return None
    if t[1].upper() not in FIRE_TYPES:
        return None
    # GTA III: 22 columns (no model2 / slot), VC: 26 (gtamods.com wiki "Weapon.dat"); the first 11 are the same
    e = entry("weapon", t[0], "gta3" if len(t) <= 22 else "vc", line, source)
    for i, col in enumerate(WEAPON_COLS):
        if not is_number(t[i + 2]):
            return None
        e["fields"][col] = t[i + 2]
    e["skipped"].append("fire type (kept: it decides animations, sounds and the HUD)")
    if len(t) > 14 and all(is_number(x) for x in t[11:14]) and any(float(x) for x in t[11:14]):  # L-N: fire offset
        for k, axis in enumerate(("firex", "firey", "firez")):
            e["fields"][axis] = t[11 + k]
    if len(t) > 11:
        e["skipped"].append("animations, models, flags, weapon slot")
    return e


def parse_stream(raw, source):
    line = raw.strip()
    if not line.startswith("index="):
        return None
    e = entry("stream", "", "vcs", line, source)
    for tok in line.split():
        k, eq, v = tok.partition("=")
        if not eq or not k:
            return None
        if k == "index":
            if not v.isdigit():
                return None
            e["target"] = int(v)
        elif k == "copy" and re.fullmatch(r"\d{1,5}", v):
            e["fields"]["copy"] = v
        elif k == "file" and re.fullmatch(r"[A-Za-z0-9_.\-]{1,64}", v) and ".." not in v:
            e["fields"]["file"] = v
        else:
            return None
    return e if e["target"] >= 0 and e["fields"] else None


def text_like(name):
    n = name.lower()
    return n.endswith((".cfg", ".dat", ".txt", ".ide", ".xml", ".ini")) or "." not in n


def model_file(name):
    return name.lower().endswith((".dff", ".txd"))


def gxt_file(name):
    return name.lower().endswith(".gxt")


def leaf(path):
    return path.replace("\\", "/").rsplit("/", 1)[-1]


class Files:
    def __init__(self):
        self.text, self.models, self.gxt, self.other = {}, {}, {}, {}
        # model / .gxt name -> its entry in the picked file ("" = the file itself; a zip's or a .vcm's own entry name):
        # where the game can read it without a copy (vcs_modfiles.h). Not set for a .vcm inside a zip, nor for a file
        # inside a .rar / .7z (the game can't read those: such files are taken out into files/).
        self.origin = {}
        # model / text name -> where it sits inside what was picked ("Replace (Banshee)/car.dff"): picks one of two
        # versions of a file (the PC one over an Android one), and its folders name the slot ("Replace (Banshee)")
        self.paths = {}
        self.notes = []  # what was left out and why (programs, pictures, another version of a file...)


# What a mod archive holds besides models and lines (gtamods.com: plugins, CLEO scripts, installers...): never run or
# copied, only named in the mod's notes
PROGRAM_EXT = (".exe", ".dll", ".asi", ".cs", ".cm", ".cleo", ".lua", ".bat", ".cmd", ".vbs", ".ps1", ".sh", ".jar",
               ".msi", ".scr", ".com", ".sa", ".fxt.dll", ".flt", ".so")
PICTURE_EXT = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga", ".dds", ".psd", ".ico")
ARCHIVE_EXT = (".rar", ".7z")
SIDE_NOTES = {".ifp": "animations (.ifp): not used (the slot's animations stay)",
              ".fxt": "a text file for a PC text plugin (.fxt): not used (the game's own names stay)",
              ".col": "PC collision (.col): not used (the slot's own collision stays)",
              ".img": "an IMG archive: not opened here (import the .dff / .txd inside it)",
              ".ide": None, ".ipl": "map placement (.ipl): not used", ".wav": "a sound (.wav): use mods.py sound to "
              "replace a game sound with it", ".mp3": "a sound (.mp3): use mods.py sound to replace a game sound "
              "with it", ".md": None}


def unused_kind(name):
    """'program' / 'picture' / a note / None (worth reading) for a file by its name"""
    lower = name.lower()
    if lower.endswith(PROGRAM_EXT):
        return "program"
    if lower.endswith(PICTURE_EXT):
        return "picture"
    for ext, text in SIDE_NOTES.items():
        if lower.endswith(ext) and text:
            return text
    return None


def depth_rank(path):
    """lower = the version to take when a mod holds one file twice: the shallower one, a phone / lite version last"""
    parts = [p.lower() for p in path.replace("\\", "/").split("/")[:-1]]
    mobile = any(re.search(r"android|mobile|\bios\b|iphone|\blite\b|\blow\b|lowpoly|\blod\b|old|backup", p)
                 for p in parts)
    return (1 if mobile else 0, len(parts))


def add_unused(f, kind, path):
    f.other[leaf(path)] = 0
    f.notes.append((kind, path))


def summarise_unused(f, p):
    """The mod's notes for the files left out, grouped"""
    programs = sorted({leaf(x) for k, x in f.notes if k == "program"}, key=str.lower)
    pictures = [x for k, x in f.notes if k == "picture"]
    if programs:
        note(p, "PC programs / plugins not used (never run here): %s" % ", ".join(programs[:12]) +
             (" and %d more" % (len(programs) - 12) if len(programs) > 12 else ""))
    if pictures:
        note(p, "%d picture(s) not used (screenshots, loose textures for other tools)" % len(pictures))
    for k, x in f.notes:
        if k not in ("program", "picture"):
            note(p, "%s: %s" % (x, k))


def unvcm(d):
    """VC Mod Manager's .vcm: "vcm\\0" + 32 hex digits, then !NAME!0sf<bytes>!NAME!ef (stored) / !NAME!1sf<zlib>..."""
    if len(d) < 36 or d[:4] != b"vcm\0":
        raise ModError("Not a VC Mod Manager file (no \"vcm\" header).")
    f = Files()
    at, count = 36, 0
    while at < len(d) and d[at:at + 1] == b"!":
        name_end = d.find(b"!", at + 1, min(len(d), at + 200))
        if name_end < 0 or name_end + 3 >= len(d) or d[name_end + 2:name_end + 4] != b"sf":
            raise ModError("The .vcm file is damaged near byte %d." % at)
        name = d[at + 1:name_end].decode("latin-1")
        packed = d[name_end + 1:name_end + 2] == b"1"
        end = ("!" + name + "!ef").encode("latin-1")
        body = name_end + 4
        stop = d.find(end, body)
        if stop < 0:
            raise ModError("The .vcm entry %s has no end." % name)
        count += 1
        if count > MAX_FILES:
            raise ModError("The .vcm holds too many files.")
        data = d[body:stop]
        if packed:
            try:
                data = zlib.decompressobj().decompress(data, MAX_MODEL + 1)
            except zlib.error:
                data = None
        lname = leaf(name)
        if data is None:
            pass
        elif text_like(lname) and len(data) <= MAX_TEXT:
            f.text[lname] = data
        elif model_file(lname) and len(data) <= MAX_MODEL:
            f.models[lname] = data
            f.origin[lname] = name
        else:
            f.other[lname] = len(data)
        at = stop + len(end)
    if count == 0:
        raise ModError("The .vcm file holds nothing.")
    return f


def merge(into, f, origins=True, base=""):
    """f's files into `into`; `base`: where f sits in the pick (its paths are under it). A model / .gxt held twice
    (another version in another folder) keeps the better version (depth_rank) and notes the other."""
    def at(name):
        return (base + "/" if base else "") + f.paths.get(name, name)
    for name, data in f.text.items():
        into.text[at(name)] = data
    into.other.update(f.other)
    into.notes += [(k, (base + "/" if base else "") + x) for k, x in f.notes]
    for attr in ("models", "gxt"):
        mine, theirs = getattr(into, attr), getattr(f, attr)
        for name, data in theirs.items():
            path = at(name)
            if name in mine and into.paths.get(name) and mine[name] != data:
                old = into.paths[name]
                if depth_rank(path) >= depth_rank(old):
                    into.notes.append(("another version of %s, not used (%s is)" % (leaf(name), old), path))
                    continue
                into.notes.append(("another version of %s, not used (%s is)" % (leaf(name), path), old))
            mine[name] = data
            into.paths[name] = path
            if origins and name in f.origin:
                into.origin[name] = f.origin[name]
            else:
                into.origin.pop(name, None)


def zip_entry(info):
    """The entry's name as the zip stores it (the game compares the bytes; vcs_modfiles.cpp)"""
    if info.flag_bits & 0x800:
        return info.filename
    try:
        return info.filename.encode("cp437").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return info.filename  # not UTF-8 bytes: the game falls back to the file's name in any folder


def classify(f, path, data, origin=None, depth=0):
    """One file of a mod (loose, or inside an archive at `path`) into f; origin: where the game can read it (None:
    it can't, the manager takes a copy out)"""
    name = leaf(path)
    lower = name.lower()
    kind = unused_kind(name)
    if kind:
        add_unused(f, kind, path)
    elif lower.endswith(".vcm"):
        sub = unvcm(data)
        merge(f, sub, origins=origin == "", base=path.rsplit("/", 1)[0] if "/" in path else "")
    elif lower.endswith(ARCHIVE_EXT) or (lower.endswith(".zip") and depth > 0):
        if depth >= 2:
            add_unused(f, "an archive three levels deep: not opened (unpack it and import its files)", path)
        else:
            sub = unpack(data, name, depth + 1) if not lower.endswith(".zip") else unzip(data, depth + 1, origins=False)
            folder = path[:-len(name)] + re.sub(r"\.[^.]+$", "", name)
            merge(f, sub, origins=False, base=folder.rstrip("/"))
    elif model_file(name) and len(data) <= MAX_MODEL:
        f.models[name] = data
        f.paths[name] = path
        if origin is not None:
            f.origin[name] = origin
    elif model_file(name):
        raise ModError("%s is over %d MB." % (name, MAX_MODEL >> 20))
    elif gxt_file(name) and len(data) <= MAX_GXT:
        f.gxt[name] = data
        f.paths[name] = path
        if origin is not None:
            f.origin[name] = origin
    elif text_like(name) and len(data) <= MAX_TEXT and b"\0" not in data[:4096]:
        f.text[path] = data
    else:
        f.other[name] = len(data)


def unzip(data, depth=0, origins=True):
    import io
    f = Files()
    total = 0
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as e:
        raise ModError("The zip can't be read (%s)." % e)
    infos = [i for i in z.infolist() if not i.is_dir()]
    if len(infos) > MAX_FILES:
        raise ModError("The zip holds more than %d files." % MAX_FILES)
    for info in infos:
        name = leaf(info.filename)
        if not name or name.startswith("."):
            continue
        if unused_kind(name):  # a program or a picture: not even read
            add_unused(f, unused_kind(name), info.filename)
            continue
        total += info.file_size
        if total > MAX_TOTAL:
            raise ModError("The zip unpacks to more than %d MB." % (MAX_TOTAL >> 20))
        if info.flag_bits & 1:
            raise ModError("The zip is encrypted (%s): unpack it with its password and import the files." % name)
        lower = name.lower()
        if text_like(name) and info.file_size > MAX_TEXT and not model_file(name):
            f.other[name] = info.file_size
            continue
        if model_file(name) and info.file_size > MAX_MODEL:
            raise ModError("%s is over %d MB." % (name, MAX_MODEL >> 20))
        if not (text_like(name) or model_file(name) or gxt_file(name) or lower.endswith((".vcm", ".zip") + ARCHIVE_EXT)):
            f.other[name] = info.file_size
            continue
        classify(f, info.filename.replace("\\", "/"), z.read(info), zip_entry(info) if origins else None, depth)
    return f


# ---- .rar / .7z: unpacked by 7-Zip or bsdtar (libarchive) when the computer has one; never run, only read ----------

def archive_tool():
    """(kind, path) of a program that unpacks .rar / .7z here, or None: 7-Zip (7z / 7zz / 7za, or its usual install
    folder), else bsdtar (Windows 10+'s tar.exe and macOS's tar are bsdtar, which reads both). VCS_UNPACKER overrides."""
    env = os.environ.get("VCS_UNPACKER")
    if env:
        return ("7z" if "7z" in os.path.basename(env).lower() else "tar", env)
    for n in ("7z", "7zz", "7za"):
        p = shutil.which(n)
        if p:
            return ("7z", p)
    for p in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe",
              "/opt/homebrew/bin/7zz", "/usr/local/bin/7zz"):
        if os.path.isfile(p):
            return ("7z", p)
    import subprocess
    for p in (shutil.which("bsdtar"), os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
              if os.name == "nt" else None, "/usr/bin/tar" if sys.platform == "darwin" else None):
        if p and os.path.isfile(p):
            try:
                out = subprocess.run([p, "--version"], capture_output=True, timeout=10).stdout.decode("latin-1")
            except (OSError, subprocess.SubprocessError):
                continue
            if "bsdtar" in out or "libarchive" in out:
                return ("tar", p)
    return None


def unpack(data, name, depth=1):
    """A .rar / .7z (bytes) -> Files: unpacked into a temporary folder by archive_tool() and read from there. Nothing
    in it is run; programs and pictures are only named. Its files have no origin: the game can't read inside these
    archives, so the manager keeps the models / texts it needs in files/."""
    import subprocess
    import tempfile
    ext = name[name.rfind("."):].lower()
    tool = archive_tool()
    if tool is None:
        raise ModError("A %s archive can't be opened here: no 7-Zip or bsdtar on this computer. Unpack it (7-Zip, "
                       "WinRAR, the Mac's Archive Utility) and import the folder, or zip its files." % ext, "mod_archive")
    with tempfile.TemporaryDirectory(prefix="vcsmod") as tmp:
        arc = os.path.join(tmp, "archive" + ext)
        out = os.path.join(tmp, "x")
        os.makedirs(out)
        with open(arc, "wb") as fh:
            fh.write(data)
        if tool[0] == "7z":
            # -p-: a password-protected archive fails instead of asking; -snl: links stored as links are not followed
            cmd = [tool[1], "x", "-y", "-bso0", "-bsp0", "-p-", "-snl-", "-o" + out, arc]
        else:
            cmd = [tool[1], "-x", "-f", arc, "-C", out]  # bsdtar refuses absolute paths and ".." by default
        try:
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=300)
        except (OSError, subprocess.SubprocessError) as e:
            raise ModError("%s could not be unpacked (%s)." % (name, e), "mod_archive")
        f = Files()
        count, total = 0, 0
        for root, dirs, names in os.walk(out):
            dirs.sort()
            for n in sorted(names):
                full = os.path.join(root, n)
                rel = os.path.relpath(full, out).replace("\\", "/")
                if os.path.islink(full) or n.startswith("."):
                    continue
                count += 1
                if count > MAX_FILES:
                    raise ModError("%s holds more than %d files." % (name, MAX_FILES))
                if unused_kind(n):
                    add_unused(f, unused_kind(n), rel)
                    continue
                size = os.path.getsize(full)
                total += size
                if total > MAX_TOTAL:
                    raise ModError("%s unpacks to more than %d MB." % (name, MAX_TOTAL >> 20))
                if text_like(n) and size > MAX_TEXT and not model_file(n):
                    f.other[n] = size
                    continue
                with open(full, "rb") as fh:
                    classify(f, rel, fh.read(), None, depth)
        if count == 0:
            why = r.stderr.decode("latin-1", "replace").strip().splitlines()
            raise ModError("%s could not be unpacked%s." % (name, " (%s)" % why[-1][:160] if why else
                                                            " (damaged, or it needs a password)"), "mod_archive")
        return f


def read_one(path, data, rel=None):
    """One picked file -> Files (a zip, a .vcm, a .rar / .7z, a loose file); rel: its path inside the picked folder"""
    name = os.path.basename(path)
    rel = rel or name
    lower = name.lower()
    if data[:2] == b"PK":
        f = unzip(data)
        return f
    if data[:4] == b"vcm\0":
        return unvcm(data)
    if lower.endswith(ARCHIVE_EXT) or data[:6] in (b"Rar!\x1a\x07", b"7z\xbc\xaf\x27\x1c"):
        return unpack(data, name if lower.endswith(ARCHIVE_EXT) else name + (".rar" if data[:3] == b"Rar" else ".7z"))
    f = Files()
    if model_file(name) and len(data) <= MAX_MODEL:
        f.models[name] = data
        f.origin[name] = ""
        f.paths[name] = rel
    elif gxt_file(name) and len(data) <= MAX_GXT:
        f.gxt[name] = data
        f.origin[name] = ""
        f.paths[name] = rel
    elif unused_kind(name):
        add_unused(f, unused_kind(name), rel)
    elif len(data) <= MAX_TEXT:
        f.text[rel] = data
    else:
        f.other[name] = len(data)
    return f


def note(p, text):
    if text not in p["notes"]:
        p["notes"].append(text)


def carcols(line):
    return re.fullmatch(r"[A-Za-z0-9_%]{2,24}\s*,\s*\d{1,3}\s*,\s*\d{1,3}(\s*,.*)?", line.strip()) is not None


def duplicate(p, e):
    for i, x in enumerate(p["entries"]):
        if x["kind"] == e["kind"] and x["fields"] == e["fields"]:
            if x["pcName"] == "VCMCAR" and e["pcName"] != "VCMCAR":
                p["entries"][i] = e
            return True
    return False


def weapon_skill(p):
    """SA's weapon.dat has one gun line per skill level: the std line (1) is kept, else the first; the others are
    noted"""
    sa = [e for e in p["entries"] if "skill" in e]
    for e in sa:
        same = [x for x in sa if x["pcName"].lower() == e["pcName"].lower()]
        keep = next((x for x in same if x["skill"] == 1), same[0])
        if e is not keep and e in p["entries"]:
            p["entries"].remove(e)
            note(p, "%s: SA skill level %d line not used (the std line is)" % (e["pcName"], e["skill"]))
    for e in sa:
        e.pop("skill", None)


def carcols_palette(files):
    """The mods' carcols.dat colour list (the "col" section: one "r,g,b" per line, index = its place), [] if none.
    GTA III / VC / SA (gtamods.com wiki "Carcols.dat"): a car line's numbers are indices into this list; without the
    list they mean the PC game's own palette, which isn't here, so they can't be converted."""
    for name, raw in files.text.items():
        section, cols = None, []
        for line in re.split(r"\r?\n|\r", raw.decode("latin-1")):
            line = line.split("#")[0].strip()
            if not line:
                continue
            low = line.lower()
            if low in ("col", "car", "car4", "end"):
                section = low if low != "end" else None
                continue
            if section == "col":
                m = re.fullmatch(r"(\d{1,3})\s*[,\s]\s*(\d{1,3})\s*[,\s]\s*(\d{1,3})(\s.*)?", line)
                if m:
                    cols.append(tuple(min(255, int(m.group(k))) for k in (1, 2, 3)))
        if len(cols) >= 2:
            return cols
    return []


def parse_carcols(line, source, palette, four=False):
    """A carcols.dat car line (name, primary, secondary, ...; SA car4: four per set, the first two used) -> a "colours"
    entry with each pair as RGB (the game picks its nearest palette entries), None if it isn't one"""
    t = [x for x in re.split(r"[\s,]+", line.split("#")[0].strip()) if x]
    if len(t) < 3 or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{1,23}", t[0]) or not all(x.isdigit() for x in t[1:]):
        return None
    nums = [int(x) for x in t[1:]]
    step = 4 if four else 2
    e = entry("colours", t[0], "carcols", line.strip(), source)
    pairs = [nums[i:i + 2] for i in range(0, len(nums) - 1, step)]
    bad = [n for pr in pairs for n in pr if n >= len(palette)]
    if bad:
        return None
    for i, (a, b) in enumerate(pairs[:8]):
        e["fields"]["pair%d" % (i + 1)] = "%d,%d,%d,%d,%d,%d" % (palette[a] + palette[b])
    if len(pairs) > 8:
        e["skipped"].append("colour pairs after the 8th (the game keeps 8)")
    return e


def read_vcm_script(xml, source, p):
    t = re.search(r"<title>([^<]{1,80})</title>", xml)
    if t:
        p["title"] = t.group(1).strip()
    for m in re.finditer(r"param2=\"([^\"]*)\"\s*>\s*car_updatehandling", xml):
        e = parse_handling("VCMCAR " + m.group(1), source)
        if e and not duplicate(p, e):
            p["entries"].append(e)
    for m in re.finditer(r"param3=\"([^\"]*)\"\s*>\s*config_replaceline", xml):
        note(p, "Colours (%s): not applied yet (the PC and PS2 colour palettes differ)"
             % m.group(1).replace("%%carfilename%%", "car").strip())


VEHICLE_IDE_TYPES = ("car", "bike", "bmx", "boat", "heli", "plane", "train", "mtruck", "quad", "trailer")


def parse_ide(line):
    """A vehicles.ide / default.ide "cars" line (GTA III / VC / SA: id, model, txd, type, handling id, game name,
    ...) -> (model name, handling id, type), None if it isn't one. The handling id is the handling.cfg line's name
    (VC's sanchez -> DIRTBIKE), so the manager knows which car a handling line is for."""
    t = [x.strip() for x in line.split("#")[0].split(",")]
    if len(t) < 6 or not t[0].isdigit() or t[3].lower() not in VEHICLE_IDE_TYPES:
        return None
    if not all(re.fullmatch(r"[A-Za-z0-9_]{1,24}", x) for x in (t[1], t[4])):
        return None
    return t[1], t[4], t[3].lower()


# VC's weapon types (gtamods.com wiki "Weapon.dat" / VC's eWeaponType), by name: an id-only weapon file (a weapon
# loader's .xml <logicalid>) names its slot with them
VC_WEAPON_TYPES = ["unarmed", "brassknuckle", "screwdriver", "golfclub", "nitestick", "knife", "baseballbat",
                   "hammer", "cleaver", "machete", "katana", "chainsaw", "grenade", "detonategrenade", "teargas",
                   "molotov", "rocket", "colt45", "python", "shotgun", "spas12shotgun", "stubbyshotgun", "tec9", "uzi",
                   "silencedingram", "mp5", "m4", "ruger", "sniperrifle", "laserscope", "rocketlauncher",
                   "flamethrower", "m60", "minigun", "detonator", "helicannon", "camera"]


def parse_weapon_xml(xml, source):
    """A weapon loader's .xml (<weapon><basic><name>, <logicalid> = VC's weapon type, <properties> range, firerate,
    reload, clipsize, damage, speed, radius, lifespan, spread, offset x / y / z) -> a weapon entry, None if it isn't
    one"""
    if "<weapon" not in xml or "<properties" not in xml:
        return None

    def tag(name):
        m = re.search(r"<%s>\s*([^<]*?)\s*</%s>" % (name, name), xml)
        return m.group(1) if m else None
    lid = tag("logicalid")
    if lid is None or not lid.isdigit() or int(lid) >= len(VC_WEAPON_TYPES):
        return None
    e = entry("weapon", VC_WEAPON_TYPES[int(lid)], "xml", "%s (weapon id %s)" % (tag("name") or "?", lid), source)
    for col, name in (("range", "range"), ("rate", "firerate"), ("reload", "reload"), ("clip", "clipsize"),
                      ("damage", "damage"), ("speed", "speed"), ("radius", "radius"), ("lifespan", "lifespan"),
                      ("spread", "spread")):
        v = tag(name)
        if v is not None and is_number(v):
            e["fields"][col] = str(int(float(v))) if col in ("rate", "reload", "clip", "damage") else v
    off = re.search(r"<offset>(.*?)</offset>", xml, re.S)
    if off:
        xyz = [re.search(r"<%s>\s*([-0-9.eE]+)\s*</%s>" % (a, a), off.group(1)) for a in "xyz"]
        if all(xyz) and any(float(m.group(1)) for m in xyz):
            for axis, m in zip(("firex", "firey", "firez"), xyz):
                e["fields"][axis] = m.group(1)
    e["skipped"] += ["fire type (kept)", "animations, models, flags, weapon slot, spawn places"]
    e["byId"] = True
    return e if e["fields"] else None


def rw_chunks(b, off, end):
    import struct
    while off + 12 <= end:
        t, s, _ = struct.unpack_from("<III", b, off)
        yield t, off + 12, min(off + 12 + s, end)
        off += 12 + s


def dff_atomics(b):
    """A .dff's frame names and, per atomic, its frame's name and its ancestors' (lower case): [] if unreadable"""
    import struct
    try:
        clump = next((c for c in rw_chunks(b, 0, len(b)) if c[0] == 0x10), None)
        if not clump:
            return []
        frames, parents, atoms = [], [], []
        for t, d, e in rw_chunks(b, clump[1], clump[2]):
            if t == 0x0E:
                st = next(rw_chunks(b, d, e))
                n = min(struct.unpack_from("<I", b, st[1])[0], 4096)
                parents = [struct.unpack_from("<i", b, st[1] + 4 + i * 56 + 48)[0] for i in range(n)]
                frames = [""] * n
                i = 0
                for x in rw_chunks(b, st[2], e):
                    if x[0] != 3 or i >= n:
                        break
                    for y in rw_chunks(b, x[1], x[2]):
                        if y[0] == 0x253F2FE:
                            frames[i] = b[y[1]:y[2]].split(b"\0")[0].decode("latin-1").lower()
                    i += 1
            elif t == 0x14:
                st = next(rw_chunks(b, d, e))
                atoms.append(struct.unpack_from("<I", b, st[1])[0])
        out = []
        for fr in atoms:
            chain, k = [], fr
            while 0 <= k < len(frames) and len(chain) < 64:
                chain.append(frames[k])
                k = parents[k]
            out.append(chain)
        return out
    except (StopIteration, struct.error, IndexError):
        return []


def weapon_parts(dff, slots):
    """A clump holding several weapons, each its own frame (a GTA III-style pack: colt45_l0, uzi_l0, m16_l0 ... in
    one weapons.dff) -> [(frame, weapon name, slot)] for every frame that names a weapon of this game; else []"""
    found, seen = [], set()
    for chain in dff_atomics(dff):
        for frame in chain:
            base = re.sub(r"_l\d$", "", frame)
            slot, reason = slots.match("weapon", base)
            if slot and reason in ("name", "alias") and frame not in seen:
                seen.add(frame)
                found.append((frame, base, slot))
                break
    return found if len({s["id"] for _, _, s in found}) >= 2 else []


def wheel_pack(dff):
    """A .dff with wheels only (the PC games' models/generic/wheels.dff, or a mod's replacement of it)"""
    chains = dff_atomics(dff)
    return bool(chains) and all(any(f.startswith("wheel") for f in c) for c in chains) and b"chassis" not in dff


MATCH_RANK = {"name": 0, "alias": 1, "id": 2, "readme": 3, "mod": 4, "similar": 5, "picked": 0, "none": 9}


def slot_hints(text, slots, family="vehicle"):
    """Slot names in a readme's "replace(s) X" phrases or a "Replace (X)" folder: [(slot, reason)] in order"""
    out = []
    for m in re.finditer(r"replac\w*\W+((?:[\w'-]+\W+){0,5}[\w'-]+)", text, re.I):
        words = re.findall(r"[A-Za-z][A-Za-z0-9_]*", m.group(1))
        for w in words:
            w = re.sub(r"\.(dff|txd)$", "", w, flags=re.I)
            if w.lower() in ("the", "and", "or", "with", "dff", "txd", "type", "by", "default", "in", "file", "files",
                             "model", "car", "vehicle", "it", "a", "an", "of", "to", "your"):
                continue
            slot, reason = slots.match(family, w)
            if slot:
                out.append((slot, reason))
    out.sort(key=lambda sr: MATCH_RANK.get(sr[1], 9))  # an exact name before a nearest equivalent
    return out


def parse(files, title, slots):
    p = {"title": re.sub(r"\.[A-Za-z0-9]{1,4}$", "", title), "entries": [], "notes": []}
    ide = {}  # handling id -> (model name, type)
    for name, raw in files.text.items():
        for line in re.split(r"\r?\n|\r", raw.decode("latin-1")):
            hit = parse_ide(line)
            if hit:
                ide.setdefault(norm(hit[1]), (hit[0], hit[2]))
    for name, raw in files.text.items():
        text = raw.decode("latin-1")
        if leaf(name).upper() == "VCM.XML":
            read_vcm_script(text, name, p)
            continue
        if leaf(name).lower().endswith(".xml"):
            e = parse_weapon_xml(text, name)
            if e and not duplicate(p, e):
                p["entries"].append(e)
            continue
        stream = leaf(name).lower() == "vcs_stream.txt"
        palette = carcols_palette(files) if not stream else []
        section = None
        for line in re.split(r"\r?\n|\r", text):
            low = line.split("#")[0].strip().lower()
            if low in ("col", "car", "car4", "end"):
                section = low if low != "end" else None
            e = parse_stream(line, name) if stream else (parse_handling(line, name) or parse_weapon(line, name))
            if e is None and not stream and palette and section in ("car", "car4"):
                e = parse_carcols(line, name, palette, section == "car4")
            if e and e["kind"] == "handling" and norm(e["pcName"]) in ide:
                e["ideName"] = ide[norm(e["pcName"])][0]  # the IDE line's model for this handling id
            if e and not duplicate(p, e):
                p["entries"].append(e)
            elif not e and not stream and carcols(line) and section != "col" and not parse_ide(line):
                note(p, "Colours (%s): not applied (its numbers are the PC game's palette and the mod has no "
                        "colour list)" % line.strip())
    weapon_skill(p)
    txds = [n for n in files.models if n.lower().endswith(".txd")]
    used_txd = set()
    for n, dff in files.models.items():
        if not n.lower().endswith(".dff"):
            continue
        base = n[:-4]
        txd = next((t for t in txds if t[:-4].lower() == base.lower()), None)
        if txd is None and len(txds) == 1:
            txd = txds[0]
        if txd is None:  # a texture file in the same folder (an III pack's weapons.dff next to its MISC.TXD)
            folder = files.paths.get(n, n).rsplit("/", 1)[0] if "/" in files.paths.get(n, n) else ""
            near = [t for t in txds if (files.paths.get(t, t).rsplit("/", 1)[0] if "/" in files.paths.get(t, t)
                                        else "") == folder]
            txd = near[0] if len(near) == 1 else None
        if len(dff) < 12 or dff[:4] != b"\x10\0\0\0":
            note(p, "%s: not a RenderWare model (no clump at its start: damaged or protected), not used" % n)
            continue
        if wheel_pack(dff):
            note(p, "%s: a wheel pack (the PC games' generic wheels): not used - the cars keep the game's wheels"
                    % files.paths.get(n, n))
            if txd:
                used_txd.add(txd)
            continue
        parts = [] if b"chassis" in dff else weapon_parts(dff, slots)
        if parts:
            # several weapons in one model file: one entry each, the game converts that frame alone (part=)
            for frame, wname, slot in parts:
                e = entry("model", wname, "weapon", "%s (%s) + %s" % (n, frame, txd or "no textures"), n)
                e["fields"]["dff"] = n
                if txd:
                    e["fields"]["txd"] = txd
                e["fields"]["part"] = frame
                e["path"] = files.paths.get(n, n)
                p["entries"].append(e)
            if txd:
                used_txd.add(txd)
            names = [c[0] for c in dff_atomics(dff)]
            left = sorted({re.sub(r"_l\d$", "", f) for f in names} - {w for _, w, _ in parts})
            note(p, "%s holds %d weapons: %s%s" % (n, len(parts), ", ".join(w for _, w, _ in parts),
                                                   "; not used (no such weapon here): " + ", ".join(left) if left else ""))
            continue
        car = b"chassis" in dff or slots.match("vehicle", base)[0] is not None
        e = entry("model", base, "car" if car else "weapon", n + (" + " + txd if txd else ""), n)
        e["fields"]["dff"] = n
        e["path"] = files.paths.get(n, n)
        if txd:
            e["fields"]["txd"] = txd
            used_txd.add(txd)
        else:
            e["skipped"].append("textures (no .txd)")
        if car:
            e["skipped"].append("extras, collision")
        p["entries"].append(e)
    for t in txds:
        if t not in used_txd:
            note(p, "%s: textures without a model of that name (HUD / icons / paint jobs for the PC game): not used"
                    % files.paths.get(t, t))
    for n in files.gxt:
        e = entry("replace", leaf(n), "gxt", n, n)
        e["fields"]["disc"] = "TEXT/" + leaf(n).upper()
        e["fields"]["file"] = n
        e["target"] = 0
        e["match"] = "name"
        p["entries"].append(e)
    summarise_unused(files, p)
    for e in p["entries"]:
        if e["kind"] in ("handling", "weapon", "model", "colours"):
            auto_target(e, slots)
    link(p, files, slots)
    for e in p["entries"]:
        s = target_slot(e, slots)
        if s and s["class"] in ("heli", "plane") and slots.game == "lcs" and not e.get("off"):
            # [lcs] LCS has no flyable aircraft: its Dodo, Hunter and Mavericks are VC's leftovers, class 0 (cars) in
            # their model infos (read in a run, 2026-11-18): spawned by the trainer they stand and drive as cars
            note(p, "%s -> %s: Liberty City Stories has no flyable aircraft. Its %s is a leftover the game treats as a "
                    "car (only the trainer spawns it, and it drives, it doesn't fly): the mod changes its %s only"
                 % (e["pcName"], s["label"], s["label"], "look" if e["kind"] == "model" else "values, not flight"))
    return p


def link(p, files, slots):
    """What the mod's own files say about slots, for entries the names alone didn't place (generic rules, in order):
    1. a handling line named by an IDE line (DIRTBIKE -> the IDE's sanchez) takes that vehicle's slot;
    2. a car model named nowhere: a "Replace (Banshee)" folder or a readme's "replaces banshee.dff" names it;
    3. a weapon line / model by id (a weapon loader's .xml) or in the same folder as a placed weapon line / model;
    4. a vehicle mod with one car model: its unplaced handling / colour lines go with the model, and the other way;
    5. two entries of one mod on one slot and kind (three versions of one gun): the best placed is used, the others
       are set aside (mods.py prefer picks another)."""
    readme = "\n".join(raw.decode("latin-1") for n, raw in files.text.items()
                       if re.search(r"read|install|info|txt$", leaf(n), re.I))
    ents = p["entries"]
    for e in ents:
        if e["target"] < 0 and e["kind"] in ("handling", "colours") and e.get("ideName"):
            slot, reason = slots.match("vehicle", e["ideName"])
            if slot:
                e["target"], e["match"] = slot["id"], "mod"
        if e["kind"] == "weapon" and e.get("byId") and e["target"] >= 0:
            e["match"] = "id"
    for e in ents:
        if e["target"] >= 0 or entry_family(e) != "vehicle" or e["kind"] != "model":
            continue
        folders = e.get("path", "").split("/")[:-1]
        hints = []
        for folder in reversed(folders):
            if re.search(r"replac", folder, re.I):
                hints += slot_hints(folder, slots)
        hints += slot_hints(readme, slots)
        if hints:
            e["target"], e["match"] = hints[0][0]["id"], "readme"
    # weapons: by the folder of a placed weapon entry
    placed_w = [x for x in ents if entry_family(x) == "weapon" and x["target"] >= 0]
    for e in ents:
        if e["target"] >= 0 or e["kind"] != "model" or entry_family(e) != "weapon":
            continue  # (a weapon.dat line names its own weapon: never moved by its neighbours)
        here = (e.get("path") or e["source"]).rsplit("/", 1)[0]
        for x in placed_w:
            there = (x.get("path") or x["source"]).rsplit("/", 1)[0]
            if there != here or x is e:
                continue
            slot = slots.weapon_of_model(x["target"]) if x["kind"] == "model" else slots.weapon(x["target"])
            if not slot:
                continue
            if e["kind"] == "model" and slot["modelId"] >= 0:
                e["target"], e["match"] = slot["modelId"], "mod"
            elif e["kind"] == "weapon":
                e["target"], e["match"] = slot["id"], "mod"
            break
    cars = [x for x in ents if x["kind"] == "model" and entry_family(x) == "vehicle"]
    if len(cars) == 1:
        car = cars[0]
        lines = [x for x in ents if x["kind"] in ("handling", "colours")]
        placed = {x["target"] for x in lines if x["target"] >= 0}
        if car["target"] >= 0:
            for x in lines:
                if x["target"] < 0:
                    x["target"], x["match"] = car["target"], "mod"
        elif len(placed) == 1:
            car["target"], car["match"] = placed.pop(), "mod"
    # one slot, one entry of each kind per mod
    best = {}
    for i, e in enumerate(ents):
        key = slot_key(e)
        if key is None:
            continue
        rank = (MATCH_RANK.get(e["match"], 9), depth_rank(e.get("path") or e["source"]), i)
        if key not in best or rank < best[key][0]:
            best[key] = (rank, e)
    for e in ents:
        key = slot_key(e)
        if key and best[key][1] is not e:
            e["off"] = True
            note(p, "%s (%s): another version for %s, set aside (the mod's %s is used; pick it with Use this one)"
                 % (e["pcName"], e.get("path") or e["source"], target_name(e, slots), best[key][1]["pcName"]))


def entry_family(e):
    """vehicle / weapon / file / stream: what kind of slot the entry goes into"""
    if e["kind"] in ("handling", "colours") or (e["kind"] == "model" and e["dialect"] == "car"):
        return "vehicle"
    if e["kind"] in ("weapon", "model"):
        return "weapon"
    return "file" if e["kind"] == "replace" else "stream"


def auto_target(e, slots):
    family = entry_family(e)
    if family not in ("vehicle", "weapon"):
        return
    slot, reason = slots.match(family, e["pcName"])
    if slot is None:
        e["target"], e["match"] = -1, "none"
    elif e["kind"] == "model" and family == "weapon":
        e["target"], e["match"] = (slot["modelId"], reason) if slot["modelId"] >= 0 else (-1, "none")
    else:
        e["target"], e["match"] = slot["id"], reason


def target_slot(e, slots):
    family = entry_family(e)
    if e["target"] < 0:
        return None
    if family == "vehicle":
        return slots.vehicle(e["target"])
    if family == "weapon":
        return slots.weapon_of_model(e["target"]) if e["kind"] == "model" else slots.weapon(e["target"])
    return None


def target_name(e, slots):
    if e["kind"] == "replace":
        return e["fields"].get("disc", "a disc file")
    if e["kind"] == "stream":
        return "streaming entry %d" % e["target"]
    if e["kind"] == "sound":
        return "sound %d%s" % (e["target"], " (%s)" % e["pcName"] if e.get("pcName") else "")
    if e["kind"] == "anims":  # [lcs] an animation set (tools/stories_pack.py)
        return "the %s animations" % e["fields"].get("set", "?")
    if e["kind"] == "hero":  # [stories] the other game's player model (tools/stories_hero.py)
        return "the player (%s)" % e["fields"].get("name", "?")
    if e["target"] < 0:
        return "no slot chosen"
    s = target_slot(e, slots)
    if s:
        return s["label"]
    return ("model %d" if entry_family(e) == "vehicle" or e["kind"] == "model" else "weapon %d") % e["target"]


MATCH_TEXT = {"name": "matched by name", "alias": "alias", "similar": "nearest equivalent", "picked": "picked by you",
              "none": "no match: pick a slot", "readme": "named in the mod's readme / folder",
              "mod": "the mod's other files say so", "id": "the mod's weapon id"}
CHANGES = {"handling": "how it drives (handling.cfg line)", "weapon": "how it fires (weapon.dat line)",
           "colours": "its paint colours (carcols.dat line)",
           "model": "how it looks (.dff model + .txd textures)", "replace": "a whole game file", "stream": "model data",
           "sound": "how it sounds (an effect sound from an audio file)",
           "anims": "how he moves (an animation set made from your other Stories disc)",
           "hero": "who the player is (the other Stories game's hero, made from your disc)"}


def directive(e, mod_id, where=None):
    """The active.txt line; where(name) -> the path the game reads a model / text file from (default: files/)"""
    if e["target"] < 0:
        return None
    k, f = e["kind"], e["fields"]
    where = where or (lambda name: "%s/files/%s" % (mod_id, safe_name(name)))
    if k == "handling":
        return "handling id=%d " % e["target"] + " ".join("%s %s" % kv for kv in f.items())
    if k == "colours":
        return "colours id=%d " % e["target"] + " ".join("%s %s" % kv for kv in f.items())
    if k == "weapon":
        return "weapon type=%d " % e["target"] + " ".join("%s %s" % kv for kv in f.items())
    if k == "model":
        # dff / txd are files; part= (one weapon of a clump that holds several) is a frame name
        return "model id=%d " % e["target"] + " ".join("%s=%s" % (key, where(v) if key in ("dff", "txd") else enc(v))
                                                       for key, v in f.items())
    if k == "stream":
        parts = []
        for key, v in f.items():
            parts.append("%s=%s" % (key, mod_id + "/files/" + v if key == "file" else v))
        return "stream index=%d " % e["target"] + " ".join(parts)
    if k == "replace":
        return "replace disc=%s file=%s" % (f["disc"], where(f["file"]))
    if k == "sound":  # [lcs] an effect sound from an audio file (src/game/lcs_sfxmod.cpp)
        return "sound id=%d file=%s" % (e["target"], where(f["file"]))
    if k == "anims":  # [lcs] an animation set from the other Stories disc (tools/stories_pack.py, src/game/lcs_swim.cpp)
        return "anims set=%s file=%s" % (f["set"], where(f["file"]))
    if k == "hero":  # [stories] the other game's player model (tools/stories_hero.py, src/game/stories_hero.cpp)
        return "hero name=%s mdl=%s xtx=%s" % (f["name"], where(f["mdl"]), where(f["xtx"])) + (
            " outfits=%s" % where(f["outfits"]) if f.get("outfits") else "")
    return None


def enc(text):
    """A path as one active.txt token: space, %, ! and non-ASCII bytes as %XX (vcs_modfiles.h)"""
    return "".join(chr(b) if 0x20 < b < 0x7F and b not in (0x25, 0x21) else "%%%02X" % b for b in text.encode("utf-8"))


def in_place(m):
    return m.get("storage") == "in_place"


def source_path(s):
    """Where a kept-in-place source is on this computer (None: an Android uri, or never recorded)"""
    p = s.get("path")
    return os.path.abspath(p) if p else None


def file_value(m, name):
    """The path the game reads one of the mod's model / text files from"""
    sname = safe_name(name)
    f = next((x for x in m.get("files", []) if x.get("name") == sname), None)
    origin = f.get("from") if f else None
    src = next((s for s in m.get("sources", []) if origin and s.get("name") == origin.get("source")), None)
    if src is None:
        return "%s/files/%s" % (m["id"], sname)  # a copy taken out (a .vcm inside a zip), or an older mod
    if in_place(m):
        p = source_path(src)
        base = enc(p.replace("\\", "/")) if p else "place:%s/%d" % (m["id"], m["sources"].index(src))
    else:
        base = enc("%s/source/%s" % (m["id"], src["name"]))
    return base + ("!" + enc(origin["entry"]) if origin.get("entry") else "")


# ---- storage -------------------------------------------------------------------------------------------------------

def safe_name(n):
    s = re.sub(r"[^A-Za-z0-9_.\-]+", "_", n)
    return "mod" + s if not s or s.startswith(".") else s


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class Store:
    def __init__(self, game, data=None):
        self.game = game
        self.data = data or backup.data_dir()
        self.dir = os.path.join(self.data, "mods")
        self.slots = Slots(game)

    def mods(self):
        out = []
        if os.path.isdir(self.dir):
            for name in os.listdir(self.dir):
                path = os.path.join(self.dir, name, "manifest.json")
                if os.path.isfile(path):
                    try:
                        with open(path, encoding="utf-8") as f:
                            out.append(json.load(f))
                    except (OSError, ValueError) as e:
                        print("[mods] skipping %s: %s" % (name, e), file=sys.stderr)
        out.sort(key=lambda m: (m.get("imported", 0), m.get("id", "")))  # load order: first imported first
        return out

    def get(self, mod_id):
        m = next((m for m in self.mods() if m["id"] == mod_id), None)
        if not m:
            raise ModError("No mod %r (mods.py list names them)." % mod_id, "mod_unknown")
        return m

    def save(self, m):
        d = os.path.join(self.dir, m["id"])
        os.makedirs(d, exist_ok=True)
        for e in m["entries"]:
            e["targetName"] = target_name(e, self.slots)
        tmp = os.path.join(d, "manifest.json.part")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(m, f, indent=2)
        os.replace(tmp, os.path.join(d, "manifest.json"))

    def loader_on(self):
        return not os.path.exists(os.path.join(self.dir, "loader_off"))

    def write_active(self):
        active = os.path.join(self.dir, "active.txt")
        os.makedirs(self.dir, exist_ok=True)
        if not self.loader_on():
            if os.path.exists(active):
                os.remove(active)
            return
        lines = ["# written by the mod manager (tools/mods.py / the Android launcher); the game reads it at start "
                 "(src/game/vcs_mods.h)"]
        count = 0
        for m in self.mods():
            if not m.get("enabled", True):
                continue
            part = [d for d in (directive(e, m["id"], lambda n, m=m: file_value(m, n))
                                for e in m["entries"] if not e.get("off")) if d]
            if part and in_place(m):
                # kept where it is: the game skips the whole mod when one of its originals is missing
                part = ["needs file=%s" % (enc(source_path(s).replace("\\", "/")) if source_path(s)
                                           else "place:%s/%d" % (m["id"], i))
                        for i, s in enumerate(m.get("sources", []))] + part
            if part:
                lines.append("mod %s %s" % (m["id"], re.sub(r"[\r\n]", " ", m["name"])))
                lines += part
                count += len(part)
        if count == 0:
            if os.path.exists(active):
                os.remove(active)
            return
        tmp = active + ".part"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(tmp, active)

    def new_id(self, title):
        base = re.sub(r"^_+|_+$", "", re.sub(r"[^a-z0-9]+", "_", title.lower()))[:32] or "mod"
        mod_id, i = base, 2
        while os.path.exists(os.path.join(self.dir, mod_id)):
            mod_id, i = "%s_%d" % (base, i), i + 1
        return mod_id

    # ---- the game's last start ----
    def applied(self):
        path = os.path.join(self.dir, "applied.txt")
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                return f.read().splitlines(), os.path.getmtime(path)
        except OSError:
            return [], 0

    def stock(self, lines):
        """The original values the game reported ("(was X)"), kept in stock.json across starts: the first report of a
        slot's field in a start is the stock value (later mods on the same field report the earlier mod's)."""
        path = os.path.join(self.dir, "stock.json")
        try:
            with open(path, encoding="utf-8") as f:
                stock = json.load(f)
        except (OSError, ValueError):
            stock = {}
        seen = set()
        changed = False
        for line in lines:
            m = re.match(r"^\S+ -> weapon (\d+): (\w+) (\S+) \(was (\S+)\)", line)
            if m:
                key, field, was = "weapon %s" % m.group(1), m.group(2), m.group(4)
            else:
                m = re.match(r"^\S+ -> .+? \((\d+)\): (.+?) (\S+) \(was (\S+)\)$", line)
                if not m:
                    continue
                key, field, was = "vehicle %s" % m.group(1), m.group(2), m.group(4)
            if (key, field) in seen:
                continue
            seen.add((key, field))
            if stock.setdefault(key, {}).get(field) != was:
                stock[key][field] = was
                changed = True
        if changed:
            os.makedirs(self.dir, exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(stock, f, indent=1, sort_keys=True)
        return stock


def dir_size(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


# ---- commands ------------------------------------------------------------------------------------------------------

def pick_list(paths, with_rel=False):
    """Picked paths (files or folders) -> the files in them (with_rel: (file, its path from the picked folder, the
    folder's own name first))"""
    singles = []
    for path in paths:
        if os.path.isdir(path):
            top = os.path.dirname(os.path.abspath(path).rstrip("\\/"))
            for root, dirs, names in os.walk(path):
                dirs.sort()
                for n in sorted(names):
                    full = os.path.join(root, n)
                    singles.append((full, os.path.relpath(os.path.abspath(full), top).replace("\\", "/")))
        elif os.path.isfile(path):
            singles.append((path, os.path.basename(path)))
        else:
            raise ModError("%s: no such file." % path, "mod_missing")
    singles = [s for s in singles if not os.path.basename(s[0]).startswith(".")]
    if len(singles) > MAX_FILES:
        raise ModError("More than %d files picked." % MAX_FILES)
    return singles if with_rel else [s[0] for s in singles]


def read_picked(path, total=0):
    size = os.path.getsize(path)
    if size > MAX_PICKED or total + size > MAX_TOTAL:
        raise ModError("%s is too big (the limit is %d MB)." % (os.path.basename(path), MAX_PICKED >> 20))
    with open(path, "rb") as f:
        return f.read()


def gather(paths):
    """Picked paths (files or folders) -> Files (origins: name -> (source key, entry)), the sources
    {key: (bytes, path)}. Programs and pictures in a picked folder are named in the notes, not read or kept."""
    files, sources = Files(), {}
    total = 0
    for path, rel in pick_list(paths, with_rel=True):
        kind = unused_kind(os.path.basename(path))
        if kind:
            add_unused(files, kind, rel)
            continue
        data = read_picked(path, total)
        total += len(data)
        one = read_one(path, data, rel)
        key = safe_name(os.path.basename(path))
        while key in sources and sources[key][0] != data:
            key = "_" + key
        packed = data[:2] == b"PK" or data[:4] == b"vcm\0" or data[:3] == b"Rar" or data[:2] == b"7z"
        merge(files, one, base=re.sub(r"\.[^./]+$", "", rel) if packed else "")
        def mine(n):  # this file's model / text is the one kept (not another version's)
            return (n in one.models and files.models.get(n) is one.models[n]) or (n in one.gxt and files.gxt.get(n) is one.gxt[n])
        used = any(mine(n) for n in set(one.models) | set(one.gxt))
        if used or one.text:
            sources[key] = (data, os.path.abspath(path), list(one.text), packed)
        for n, entry in one.origin.items():
            if mine(n):
                files.origin[n] = (key, entry)
    return files, sources


def source_info(key, data, path):
    return {"name": key, "size": len(data), "sha256": sha256(data), "path": path, "mtime": os.path.getmtime(path)}


def mod_title(paths):
    """The mod's name from what was picked: a folder holding one folder only -> that one's name (an unpacked
    download's "x/My Car/..."); a download site's id prefix ("1622253381_", "TP2vAIUE_") and underscores dropped"""
    one = os.path.normpath(paths[0])
    if len(paths) > 1:
        one = os.path.dirname(os.path.abspath(paths[0]))
    for _ in range(4):
        if not os.path.isdir(one):
            break
        inside = [n for n in os.listdir(one) if not n.startswith(".")]
        if len(inside) != 1 or not os.path.isdir(os.path.join(one, inside[0])):
            break
        one = os.path.join(one, inside[0])
    t = re.sub(r"\.(zip|rar|7z|vcm)$", "", os.path.basename(one), flags=re.I)
    t = re.sub(r"^\d{9,}_", "", t)
    m = re.match(r"([A-Za-z0-9]{8})_(?=.)", t)
    if m and any(c.islower() for c in m.group(1)) and sum(c.isupper() for c in m.group(1)[1:]) >= 2:
        # a download site's random id ("TP2vAIUE_", "AQFGDGfM_"), not a word ("Cheetah2_", "Admiral_")
        t = t[9:]
    return re.sub(r"\s+", " ", t.replace("_", " ")).strip() or "mod"


def cmd_import(store, paths, name=None, keep_in_place=False):
    files, sources = gather(paths)
    title = name or mod_title(paths)
    p = parse(files, title, store.slots)
    if name:
        p["title"] = name
    if not p["entries"]:
        raise ModError("Nothing in %s that the game can use: no handling.cfg or weapon.dat line, no .dff model, no "
                       ".gxt text.%s" % (title, "".join("\n- " + n for n in p["notes"])), "mod_empty")
    m = {"loader": 2, "game": store.game, "id": store.new_id(p["title"]), "name": p["title"],
         "source": ", ".join(os.path.basename(os.path.normpath(x)) for x in paths), "imported": int(time.time() * 1000),
         "enabled": True, "storage": "in_place" if keep_in_place else "copy",
         "entries": p["entries"], "notes": p["notes"], "sources": [], "files": []}
    # the picked files the mod needs: what the game reads (models, texts), the texts a line came from and readmes;
    # other texts (licences, plugin settings) are neither copied nor needed in place
    used_text = {e["source"] for e in p["entries"]}
    wanted = {v for e in p["entries"] for k, v in e["fields"].items() if k in ("dff", "txd", "file")}
    needed = {v[0] for n, v in files.origin.items() if n in wanted}
    sources = {k: v[:2] for k, v in sources.items()
               if v[3] or k in needed or any(t in used_text or re.search(r"read|install|info", leaf(t), re.I) for t in v[2])}
    d = os.path.join(store.dir, m["id"])
    os.makedirs(d, exist_ok=True)
    for key, (data, path) in sources.items():
        if not keep_in_place:
            # "Import": the manager's own copy of everything picked, once: the player may delete the originals
            os.makedirs(os.path.join(d, "source"), exist_ok=True)
            with open(os.path.join(d, "source", key), "wb") as f:
                f.write(data)
        m["sources"].append(source_info(key, data, path))
    # what the game reads: from the picked file itself (a loose file, a zip's or a .vcm's entry), no second copy; only a
    # file it can't reach (a .vcm inside a zip) is taken out into files/
    keep = dict(files.models)
    keep.update(files.gxt)
    for n, data in keep.items():
        info = {"name": safe_name(n), "size": len(data), "sha256": sha256(data)}
        if n in files.origin:
            info["from"] = {"source": files.origin[n][0], "entry": files.origin[n][1]}
        else:
            os.makedirs(os.path.join(d, "files"), exist_ok=True)
            with open(os.path.join(d, "files", safe_name(n)), "wb") as f:
                f.write(data)
        m["files"].append(info)
    store.save(m)
    store.write_active()
    print("[mods] %s %s as %s: %d entries" % ("added (kept where it is)" if keep_in_place else "imported", m["source"],
                                             m["id"], len(m["entries"])), file=sys.stderr)
    return {"imported": m["id"], "storage": m["storage"], "mod": describe(store, m, *store_context(store))}


# ---- kept where it is / imported: where the originals are, switching -------------------------------------------------

def source_state(s, m=None):
    """ok / missing / changed for one kept-in-place source (its recorded size, then its hash when its time moved)"""
    p = source_path(s)
    if not p or not os.path.isfile(p):
        return "missing"
    try:
        if os.path.getsize(p) != s.get("size"):
            return "changed"
        if abs(os.path.getmtime(p) - s.get("mtime", 0)) > 1e-3:
            with open(p, "rb") as f:
                if sha256(f.read()) != s.get("sha256"):
                    return "changed"
            s["mtime"] = os.path.getmtime(p)  # the same bytes: remembered (saved with the next change)
    except OSError:
        return "missing"
    return "ok"


def originals(m):
    """{"state": ok | missing | changed, "missing": [names], "changed": [names]} of a mod kept where it is"""
    if not in_place(m):
        return {"state": "ok", "missing": [], "changed": []}
    states = [(s.get("name"), source_state(s)) for s in m.get("sources", [])]
    missing = [n for n, st in states if st == "missing"]
    changed = [n for n, st in states if st == "changed"]
    return {"state": "missing" if missing else "changed" if changed else "ok", "missing": missing, "changed": changed}


def copy_dir(store, m):
    return os.path.join(store.dir, m["id"], "source")


def cmd_relocate(store, mod_id, paths, as_copy=False):
    """Find it again: the picked files are matched to the mod's sources by their contents (sha256)"""
    m = store.get(mod_id)
    by_hash = {s["sha256"]: s for s in m.get("sources", [])}
    found = {}
    for path in pick_list(paths):
        with open(path, "rb") as f:
            data = f.read()
        s = by_hash.get(sha256(data))
        if s is not None:
            found[s["name"]] = (s, data, os.path.abspath(path))
    if not found:
        raise ModError("None of the picked files is this mod's original (%s): their contents differ. To use a new "
                       "version, add it as a new mod." % ", ".join(s["name"] for s in m.get("sources", [])), "mod_other")
    # every source must be reachable afterwards: found now, or still where it was (kept in place), or in the copy
    for s in m.get("sources", []):
        if s["name"] in found:
            continue
        copied = os.path.isfile(os.path.join(copy_dir(store, m), s["name"]))
        if not (copied or (in_place(m) and source_state(s) == "ok")):
            raise ModError("Also pick %s (this mod has %d files)." % (s["name"], len(m["sources"])), "mod_pick")
    for name, (s, data, path) in found.items():
        s["path"], s["mtime"] = path, os.path.getmtime(path)
    return switch(store, m, "copy" if as_copy else "in_place", found)


def switch(store, m, storage, found=None):
    """copy -> in_place: the app's copy deleted (the originals checked first); in_place -> copy: copied in"""
    found = found or {}
    d = copy_dir(store, m)
    if storage == "copy":
        for s in m.get("sources", []):
            target = os.path.join(d, s["name"])
            if os.path.isfile(target):
                continue
            data = found[s["name"]][1] if s["name"] in found else None
            if data is None:
                if source_state(s) != "ok":
                    raise ModError("%s is not where it was (%s): find it again first." % (s["name"], s.get("path")),
                                   "mod_missing")
                with open(source_path(s), "rb") as f:
                    data = f.read()
            os.makedirs(d, exist_ok=True)
            with open(target + ".part", "wb") as f:
                f.write(data)
            os.replace(target + ".part", target)
    else:
        for s in m.get("sources", []):
            if s["name"] not in found and source_state(s) != "ok":
                raise ModError("%s is not where it was (%s): pick it (Find it again)." % (s["name"], s.get("path")),
                               "mod_pick")
    for name, (s, data, path) in found.items():
        derive_from(m, name, data, path)
    m["storage"] = storage
    store.save(m)
    store.write_active()
    if storage == "in_place":
        # the app's copy only: the originals are read where they are from now on (files/ too, where they hold it)
        if os.path.isdir(d):
            shutil.rmtree(d)
        drop_derived(store, m)
    return {"mod": m["id"], "name": m["name"], "storage": storage}


def derive_from(m, key, data, path):
    """A files/ copy (an older import) that is the same bytes as an entry of source `key`: read from there instead"""
    try:
        one = read_one(path, data)
    except ModError:
        return False
    changed = False
    for n, entry in one.origin.items():
        blob = one.models.get(n, one.gxt.get(n))
        for f in m.get("files", []):
            if f.get("name") == safe_name(n) and "from" not in f and blob is not None and f.get("sha256") == sha256(blob):
                f["from"] = {"source": key, "entry": entry}
                changed = True
    return changed


def drop_derived(store, m):
    """files/<name> of every file the game now reads from its source"""
    d = os.path.join(store.dir, m["id"], "files")
    for f in m.get("files", []):
        p = os.path.join(d, f.get("name", ""))
        if f.get("from") and os.path.isfile(p):
            os.remove(p)
    if os.path.isdir(d) and not os.listdir(d):
        os.rmdir(d)


def migrate(store, m):
    """An imported mod from before: source/ and files/ held the same bytes twice. The game reads them from source/
    now (the same bytes: the converted-model cache stays valid) and files/ keeps only what can't be read there."""
    if in_place(m) or not any("from" not in f for f in m.get("files", [])):
        return False
    changed = False
    for s in m.get("sources", []):
        p = os.path.join(copy_dir(store, m), s.get("name", ""))
        if os.path.isfile(p):
            with open(p, "rb") as fh:
                changed |= derive_from(m, s["name"], fh.read(), p)
    if changed:
        store.save(m)
        store.write_active()  # (before the copies go: active.txt never names a deleted file)
        drop_derived(store, m)
    return changed


def entry_at(m, index):
    try:
        i = int(index)
    except ValueError:
        i = -1
    if not 0 <= i < len(m["entries"]):
        raise ModError("%s has no entry %s (0..%d)." % (m["id"], index, len(m["entries"]) - 1), "mod_entry")
    return m["entries"][i]


def resolve_slot(store, e, slot_text):
    family = entry_family(e)
    if family not in ("vehicle", "weapon"):
        raise ModError("This entry has no slot to choose.", "mod_entry")
    table = store.slots.vehicles if family == "vehicle" else store.slots.weapons
    s = None
    if re.fullmatch(r"-?\d+", slot_text):
        n = int(slot_text)
        s = next((x for x in table if x["id"] == n), None)
    if s is None:
        s, _ = store.slots.match(family, slot_text)
    if s is None:
        s = next((x for x in table if norm(x["label"]) == norm(slot_text)), None)
    if s is None:
        raise ModError("No %s %r in %s's list (mods.py slots %s)." % (family, slot_text, store.game.upper(), family),
                       "mod_slot")
    if e["kind"] == "model" and family == "weapon":
        if s["modelId"] < 0:
            raise ModError("%s has no model of its own to replace." % s["label"], "mod_slot")
        return s["modelId"]
    return s["id"]


def cmd_retarget(store, mod_id, index, slot_text):
    m = store.get(mod_id)
    e = entry_at(m, index)
    if slot_text == "auto":
        auto_target(e, store.slots)
    else:
        e["target"] = resolve_slot(store, e, slot_text)
        e["match"] = "picked"
    store.save(m)
    store.write_active()
    return {"mod": m["id"], "entry": int(index), "target": e["target"], "targetName": target_name(e, store.slots),
            "match": e["match"]}


def slot_key(e):
    family = entry_family(e)
    if e["target"] < 0:
        return None
    if family in ("vehicle", "weapon"):
        # the same kind into the same slot: a conflict (a car's handling and its model are not: one drives, one looks)
        part = {"model": "looks", "colours": "paint"}.get(e["kind"], "behaviour")
        return "%s %s %d" % (family, part, e["target"])
    return "%s %s" % (e["kind"], e["fields"].get("disc", e["target"]))


def cmd_prefer(store, mod_id, index):
    mods = store.mods()
    m = next((x for x in mods if x["id"] == mod_id), None)
    if not m:
        raise ModError("No mod %r." % mod_id, "mod_unknown")
    e = entry_at(m, index)
    key = slot_key(e)
    if key is None:
        raise ModError("The entry has no slot yet.", "mod_entry")
    e["off"] = False
    set_aside = []
    for other in mods:
        for x in other["entries"]:
            if x is not e and slot_key(x) == key and not x.get("off"):
                x["off"] = True
                set_aside.append(other["id"])
        store.save(other)
    store.write_active()
    return {"mod": mod_id, "entry": int(index), "setAside": set_aside}


def store_context(store):
    lines, when = store.applied()
    return lines, when, store.stock(lines)


def entry_lines(e, mod_id, lines):
    """applied.txt lines about this entry"""
    if e["target"] < 0:
        return []
    out = []
    t = e["target"]
    pats = {"handling": [r"\(%d\):" % t, r"handling for model %d\b" % t],
            "weapon": [r"weapon %d\b" % t, r"weapon type %d\b" % t],
            "model": [r"model %d\b" % t], "stream": [r"(stream entry|streaming index) %d\b" % t],
            "replace": [re.escape(e["fields"].get("disc", "?"))], "colours": [r"colours for model %d\b" % t],
            "sound": [r": sound .*%s" % re.escape(e["fields"].get("file", "?"))],
            "anims": [r": anims "], "hero": [r": hero "]}.get(e["kind"], [])
    for line in lines:
        if not (line.startswith(mod_id + " ") or line.startswith(mod_id + ":")):
            continue
        if any(re.search(p, line) for p in pats):
            out.append(line[len(mod_id):].lstrip(": "))
    return out


def describe(store, m, lines, when, stock):
    entries = []
    orig = originals(m)
    for i, e in enumerate(m["entries"]):
        s = target_slot(e, store.slots)
        family = entry_family(e)
        stock_key = "%s %d" % ("weapon" if family == "weapon" else "vehicle",
                               (s["id"] if s else e["target"]))
        before = stock.get(stock_key, {}) if e["kind"] == "weapon" else {}
        fields = [{"name": k, "value": v, "stock": before.get(k)} for k, v in e["fields"].items()]
        applied = entry_lines(e, m["id"], lines)
        state = "pending"
        if applied:
            # a value written / an entry served: applied (a field the weapon doesn't use is "skipped" on its own)
            done = any(re.search(r"\(was |values written|now served|^-> model \d+: streaming", a) for a in applied)
            state = "applied" if done else "skipped"
        if orig["state"] == "missing":
            state = "missing"  # the game skips the whole mod until its original is found
        entries.append({
            "index": i, "kind": e["kind"], "family": family, "pcName": e["pcName"], "dialect": e["dialect"],
            "source": e["source"], "line": e["line"][:300], "target": e["target"],
            "targetName": target_name(e, store.slots), "slotClass": s["class"] if s else "",
            "match": e.get("match", "picked" if e["target"] >= 0 else "none"),
            "matchText": MATCH_TEXT.get(e.get("match", "none"), ""), "changes": CHANGES.get(e["kind"], ""),
            "fields": fields, "skipped": e["skipped"], "off": bool(e.get("off")), "applied": applied,
            "state": state, "slot": slot_key(e)})
    d = os.path.join(store.dir, m["id"])
    conv = os.path.join(d, "converted")
    return {"id": m["id"], "name": m["name"], "source": m.get("source", ""), "imported": m.get("imported", 0),
            "enabled": m.get("enabled", True), "notes": m.get("notes", []), "entries": entries,
            "stored": dir_size(d), "converted": dir_size(conv) if os.path.isdir(conv) else 0,
            "sources": m.get("sources", []), "folder": d,
            # [mods] "copy" (imported: the app's own copy) or "in_place" (kept where it is: read from "places");
            # "original": ok / missing (renamed, moved, deleted: skipped by the game) / changed (other contents)
            "storage": "in_place" if in_place(m) else "copy", "original": orig["state"],
            "missing": orig["missing"], "changed": orig["changed"],
            "places": [source_path(s) or s.get("uri", "") for s in m.get("sources", [])]}


def cmd_list(store):
    lines, when, stock = store_context(store)
    mods = store.mods()
    for m in mods:
        migrate(store, m)  # an older import's double copy: once
    described = [describe(store, m, lines, when, stock) for m in mods]
    # every slot changed, overall: who changes it, in load order (the last one the game applies wins)
    slots = {}
    for md in described:
        for e in md["entries"]:
            if not e["slot"]:
                continue
            s = slots.setdefault(e["slot"], {"slot": e["slot"], "family": e["family"], "target": e["target"],
                                             "targetName": e["targetName"], "kind": e["kind"], "by": []})
            s["by"].append({"mod": md["id"], "name": md["name"], "entry": e["index"],
                            # (a mod whose original is missing is skipped by the game: it wins nothing)
                            "active": md["enabled"] and not e["off"] and md["original"] != "missing",
                            "off": e["off"], "enabled": md["enabled"]})
    conflicts = []
    for s in slots.values():
        active = [b for b in s["by"] if b["active"]]
        s["winner"] = active[-1]["mod"] if active else ""
        # two mods on one slot: a conflict while both apply (the later one wins, field by field); "resolved" once the
        # player chose (prefer: the others' entries set aside)
        s["conflict"] = len(active) > 1
        s["resolved"] = not s["conflict"] and len([b for b in s["by"] if b["enabled"]]) > 1
        if s["conflict"]:
            conflicts.append(s["slot"])
    # each entry knows its slot's conflict (the mod card says it, next to the overview)
    for md in described:
        for e in md["entries"]:
            s = slots.get(e["slot"]) if e["slot"] else None
            e["conflict"] = bool(s and s["conflict"])
            e["others"] = [b["name"] for b in s["by"] if b["mod"] != md["id"] and b["enabled"]] if s else []
            e["wins"] = bool(s and s["winner"] == md["id"])
    order = {"vehicle": 0, "weapon": 1}
    overview = sorted(slots.values(), key=lambda s: (order.get(s["family"], 2), s["targetName"], s["slot"]))
    return {"game": store.game, "folder": store.dir, "loader": store.loader_on(), "mods": described,
            "slots": overview, "conflicts": conflicts, "applied": lines[-1] if lines else "",
            "appliedTime": int(when), "count": len(described),
            "enabled": sum(1 for m in described if m["enabled"]),
            # the slot picker's lists (the launcher has them without another run)
            "vehicleSlots": cmd_slots(store, "vehicle")["slots"], "weaponSlots": cmd_slots(store, "weapon")["slots"]}


def cmd_slots(store, family):
    table = store.slots.vehicles if family == "vehicle" else store.slots.weapons
    table = sorted(table, key=lambda s: s["label"].lower())  # the picker: by name
    return {"family": family, "slots": [{"id": s["id"], "label": s["label"], "class": s["class"],
                                         "model": s["model"], "modelId": s["modelId"]} for s in table]}


# ---- [lcs] sounds and user tracks (src/game/lcs_sfxmod.cpp, lcs_usertracks.cpp) ------------------------------------

AUDIO_EXT = (".wav", ".mp3", ".ogg", ".flac")


def lcs_sounds(iso):
    """Every effect sound on the player's LCS disc: (id, bank, index, rate, bytes, loop, VAG name), the ids the game's
    sound table uses (its .SDT files in bank order; bank 2, the speech, without names)"""
    import vcs_disc
    disc = vcs_disc.IsoImage(iso)
    out, sid = [], 0
    for bank in range(67):
        stem = "AUDIO/SET%d/SFX%d_PS" % (bank // 10, bank)
        sdt, raw = disc.find(stem + ".SDT"), disc.find(stem + ".RAW")
        if not sdt or not raw:
            raise ModError("%s: not LCS's disc (no %s.SDT / .RAW)" % (iso, stem), "mod_disc")
        table = disc.read_file(sdt)
        for i in range(len(table) // 12):
            off, size, rate = (int.from_bytes(table[i * 12 + k:i * 12 + k + 4], "little") for k in (0, 4, 8))
            name, loop = "", False
            if bank != 2:
                head = disc.read_sectors(raw["lba"] + off // 2048, 2)[off % 2048:]
                if head[:4] == b"VAGp":
                    name = head[0x20:0x30].split(b"\0")[0].decode("ascii", "replace")
                end = off + size - 16
                last = disc.read_sectors(raw["lba"] + end // 2048, 1)[end % 2048:end % 2048 + 16]
                loop = len(last) > 1 and last[1] == 3
            out.append({"id": sid, "bank": bank, "index": i, "rate": rate, "seconds": round((size - 48) / 16 * 28 / max(rate, 1), 3),
                        "loop": loop, "name": name})
            sid += 1
    return out


def cmd_sounds(iso, find=None):
    sounds = [s for s in lcs_sounds(iso) if s["bank"] != 2 and (not find or find.upper() in s["name"].upper())]
    return {"sounds": sounds, "note": "ids for `mods.py sound FILE --id N`; bank 2 (speech) can't be replaced"}


def cmd_sound(store, path, ids, mod_id=None, name=None):
    """An audio file as the new sound of effect id(s): a new mod (or one more entry of mod `mod_id`)"""
    if store.game != "lcs":
        raise ModError("Sounds can be replaced on LCS only.", "mod_game")
    if os.path.splitext(path)[1].lower() not in AUDIO_EXT:
        raise ModError("%s: not a .wav / .mp3 / .ogg / .flac file." % path, "mod_audio")
    with open(path, "rb") as f:
        data = f.read(MAX_PICKED + 1)
    if len(data) > MAX_PICKED:
        raise ModError("%s is too large for an effect sound." % path, "mod_audio")
    if mod_id:
        m = store.get(mod_id)
    else:
        title = name or os.path.splitext(os.path.basename(path))[0]
        m = {"loader": 2, "game": store.game, "id": store.new_id(title), "name": title, "source": os.path.basename(path),
             "imported": int(time.time() * 1000), "enabled": True, "storage": "copy", "entries": [], "notes": [],
             "sources": [], "files": []}
    fname = safe_name(os.path.basename(path))
    d = os.path.join(store.dir, m["id"], "files")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, fname), "wb") as f:
        f.write(data)
    if not any(x.get("name") == fname for x in m["files"]):
        m["files"].append({"name": fname, "size": len(data), "sha256": sha256(data)})
    for sid, label in ids:
        e = entry("sound", label, "sound", "sound id=%d file=%s" % (sid, fname), os.path.basename(path))
        e["target"], e["match"], e["fields"] = sid, "picked", {"file": fname}
        m["entries"].append(e)
    store.save(m)
    store.write_active()
    return {"mod": m["id"], "sounds": [i for i, _ in ids], "file": fname}


def cmd_music(store):
    """The user-tracks folder (Audio > User tracks): <data>/music, made if missing, and the playable files in it"""
    d = os.path.join(store.data, "music")
    os.makedirs(d, exist_ok=True)
    files = []
    for root, _, names in os.walk(d):
        files += [os.path.relpath(os.path.join(root, n), d) for n in names if os.path.splitext(n)[1].lower() in AUDIO_EXT]
    return {"folder": d, "files": sorted(files),
            "note": "the game plays these as a radio station (Audio > User tracks: which station, or Off)"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", choices=("vcs", "lcs"), default=tc.GAME if tc.GAME in ("vcs", "lcs") else "lcs")
    ap.add_argument("--data", help="the game's data folder (default: VCS_DATA_DIR, else the game's own)")
    ap.add_argument("--json", action="store_true", help="print the answer as JSON")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("status")
    p = sub.add_parser("import")
    p.add_argument("paths", nargs="+")
    p.add_argument("--name")
    p.add_argument("--in-place", action="store_true",
                   help="keep where it is: no copy, the game reads the files there (don't move or delete them)")
    p = sub.add_parser("relocate")
    p.add_argument("id")
    p.add_argument("paths", nargs="+")
    p.add_argument("--copy", action="store_true", help="import a copy of the found files instead")
    p = sub.add_parser("store")
    p.add_argument("id")
    p.add_argument("storage", choices=("copy", "in_place"))
    for c in ("enable", "disable", "remove"):
        sub.add_parser(c).add_argument("id")
    p = sub.add_parser("retarget")
    p.add_argument("id")
    p.add_argument("entry")
    p.add_argument("slot")
    p = sub.add_parser("prefer")
    p.add_argument("id")
    p.add_argument("entry")
    sub.add_parser("slots").add_argument("family", choices=("vehicle", "weapon"))
    sub.add_parser("loader").add_argument("state", choices=("on", "off"))
    p = sub.add_parser("sounds", help="[lcs] the effect sounds' ids, from your disc")
    p.add_argument("iso")
    p.add_argument("--find", help="only names containing this (e.g. COLT45, SHOTGUN, HORN)")
    p = sub.add_parser("sound", help="[lcs] an audio file as the new sound of effect id(s)")
    p.add_argument("file")
    p.add_argument("--id", type=int, action="append", default=[], help="a sound id (mods.py sounds); repeatable")
    p.add_argument("--sound-name", help="every sound whose disc name starts so (needs --iso)")
    p.add_argument("--iso", help="your LCS disc, for --sound-name")
    p.add_argument("--mod", help="add to this mod instead of making a new one")
    p.add_argument("--name", help="the new mod's name")
    sub.add_parser("music", help="[lcs] the user-tracks folder and its files")
    a = ap.parse_args()

    try:
        store = Store(a.game, a.data)
        if a.cmd == "list":
            out = cmd_list(store)
        elif a.cmd == "status":
            lines, when = store.applied()
            out = {"loader": store.loader_on(), "applied": lines, "appliedTime": int(when),
                   "active": os.path.isfile(os.path.join(store.dir, "active.txt"))}
        elif a.cmd == "import":
            out = cmd_import(store, a.paths, a.name, a.in_place)
        elif a.cmd == "relocate":
            out = cmd_relocate(store, a.id, a.paths, a.copy)
        elif a.cmd == "store":
            out = switch(store, store.get(a.id), a.storage)
        elif a.cmd in ("enable", "disable"):
            m = store.get(a.id)
            m["enabled"] = a.cmd == "enable"
            store.save(m)
            store.write_active()
            out = {"mod": m["id"], "enabled": m["enabled"]}
        elif a.cmd == "remove":
            m = store.get(a.id)
            shutil.rmtree(os.path.join(store.dir, m["id"]))
            store.write_active()
            out = {"removed": m["id"], "name": m["name"]}
        elif a.cmd == "retarget":
            out = cmd_retarget(store, a.id, a.entry, a.slot)
        elif a.cmd == "prefer":
            out = cmd_prefer(store, a.id, a.entry)
        elif a.cmd == "slots":
            out = cmd_slots(store, a.family)
        elif a.cmd == "loader":
            os.makedirs(store.dir, exist_ok=True)
            off = os.path.join(store.dir, "loader_off")
            if a.state == "off":
                open(off, "w").close()
            elif os.path.exists(off):
                os.remove(off)
            store.write_active()
            out = {"loader": store.loader_on()}
        elif a.cmd == "sounds":
            out = cmd_sounds(a.iso, a.find)
        elif a.cmd == "sound":
            ids = [(i, "") for i in a.id]
            if a.sound_name:
                if not a.iso:
                    raise ModError("--sound-name needs --iso (your disc).", "mod_usage")
                ids += [(s["id"], s["name"]) for s in lcs_sounds(a.iso)
                        if s["bank"] != 2 and s["name"].upper().startswith(a.sound_name.upper())]
            if not ids:
                raise ModError("No sound chosen: --id N (mods.py sounds lists them) or --sound-name with --iso.", "mod_usage")
            out = cmd_sound(store, a.file, ids, a.mod, a.name)
        elif a.cmd == "music":
            out = cmd_music(store)
    except ModError as e:
        tc.fail(str(e), e.code)
    except (OSError, ValueError) as e:
        tc.fail(str(e), "mod_error")
    out["command"] = a.cmd
    if tc.json_progress():
        tc.event(result="ok", **out)
    elif a.json:
        print(json.dumps(out, indent=1))
    elif a.cmd == "list":
        print_list(out)
    else:
        print(json.dumps(out, indent=1))
    return 0


def print_list(out):
    print("%s mods in %s (loader %s)" % (out["game"].upper(), out["folder"], "on" if out["loader"] else "off"))
    for m in out["mods"]:
        print("%s  %s [%s]  from %s  (%s%s)" % ("on " if m["enabled"] else "off", m["id"], m["name"], m["source"],
                                                "kept where it is: " + ", ".join(m["places"]) if m["storage"] == "in_place"
                                                else "imported copy",
                                                "" if m["original"] == "ok" else "; ORIGINAL " + m["original"].upper()))
        for e in m["entries"]:
            print("   %d. %s %s -> %s (%s)%s  %s" % (e["index"], e["kind"], e["pcName"], e["targetName"], e["matchText"],
                                                   " SET ASIDE" if e["off"] else "", e["state"]))
    for s in out["slots"]:
        if s["conflict"]:
            print("conflict on %s: %s; wins: %s" % (s["targetName"], ", ".join(b["mod"] for b in s["by"]), s["winner"]))


if __name__ == "__main__":
    sys.exit(main())
