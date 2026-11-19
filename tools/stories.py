#!/usr/bin/env python3
"""Stories: both cities (EXPERIMENTAL) - Liberty City with Vice City on the horizon (docs/STORIES.md).

    python tools/stories.py                 play LCS (your window, your saves); VCS runs hidden as the far city
    python tools/stories.py --check         only say whether both games are built and have a disc
    python tools/stories.py --lcs-iso X --vcs-iso Y --port 48310

Needs both games built (python tools/build.py and python tools/build.py --game lcs) from your own two discs.
Liberty City Stories is started as usual, shown, with your settings and saves. Vice City Stories is started hidden,
muted, without input, from a card of its own (<build>/stories/vcs-background: a copy of your newest Vice City save, else
a new game without its opening mission - free roam either way; your own card is only read), with
STORIES_DUALWORLD=back: its camera follows Liberty City's camera moved by the offset (Vice City east of Portland) and
its picture is drawn behind Liberty City's where nothing of Liberty City is (src/game/dualworld.h). Closing Liberty
City (or Ctrl+C here) closes both. Nothing of this runs in a normal start.

Starter saves (STORIES_STARTER_SAVES=1, the default; 0 off): a city with no save to start from gets its card from a
starter save made right after its first mission, so the story goes on from mission 2 (docs/STORIES.md "A new game
starts after the first mission"). `--set-starter lcs=<data folder>` / `vcs=<data folder>` installs one.

Settings: Liberty City's (its vcs.ini, the one its F2 menu writes) drive both. Vice City is started with a vcs.ini made
for it (<build>/stories/vcs-background.ini: your Vice City vcs.ini with every shared setting taken from Liberty City's;
VCS_SETTINGS_INI), never your own Vice City one; while playing, each F2 change in the game in front reaches the other
live (src/game/dualworld.cpp syncSettings). If Vice City stops, Liberty City goes on alone and Vice City is started again
(at most --restarts times).
"""
import argparse
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

ASPECTS = {43: 4 / 3, 1610: 16 / 10, 169: 16 / 9, 219: 21 / 9, 329: 32 / 9, 54: 5 / 4, 11: 1.0, 34: 3 / 4}


def norm(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def load_vehicles():
    """tools/modslots/mod_slots.txt: per game, model id -> name, names (aliases), class, similar."""
    t = {"lcs": {}, "vcs": {}}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modslots", "mod_slots.txt")
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^(lcs|vcs) vehicle (\d+) \| (.*)$", line.strip())
            if not m:
                continue
            cols = [c.strip() for c in m.group(3).split("|")]
            t[m.group(1)][int(m.group(2))] = {
                "name": norm(cols[0]), "class": cols[2] if len(cols) > 2 else "",
                "names": [norm(x) for x in (cols[3].split() if len(cols) > 3 else [])],
                "similar": [norm(x) for x in (cols[4].split() if len(cols) > 4 else [])]}
    return t


AIR = ("heli", "plane")


def nearest_map(src_game, dst_game):
    """{src model id: dst model id}: car_map's nearest vehicles, without the packs (tools/stories_pack.py: the donor a
    packed vehicle is converted on)"""
    return {int(a): int(b) for a, b in (p.split(":") for p in car_map(src_game, dst_game, packs=False).split(",") if p)}


def car_map(src_game, dst_game, packs=True, vcs_data=None):
    """"mine:theirs,..." for STORIES_DW_CARMAP: each vehicle of src_game -> the nearest of dst_game - the same name, an
    alias, the 'similar' list (kept to the same class), else the first of the same class (bike -> bike, boat -> boat)."""
    t = load_vehicles()
    src, dst = t[src_game], t[dst_game]
    pairs = []
    for mid, v in sorted(src.items()):
        keys = [v["name"]] + v["names"]
        pick = next((d for d, w in dst.items() if w["name"] in keys), None)
        pick = pick or next((d for d, w in dst.items() if any(k in w["names"] for k in keys)), None)
        if pick is None:
            for sname in v["similar"]:
                pick = next((d for d, w in dst.items() if (w["name"] == sname or sname in w["names"]) and
                             w["class"] == v["class"]), None)
                if pick:
                    break
        pick = pick or next((d for d, w in sorted(dst.items()) if w["class"] == v["class"]), None)
        # [parity] an aircraft never becomes a car or a boat in mid-air: a plane with no plane there takes the nearest
        # helicopter (and a helicopter the nearest plane), a match of another class is dropped for that
        if v["class"] in AIR and (pick is None or dst[pick]["class"] not in AIR):
            pick = next((d for d, w in sorted(dst.items()) if w["class"] == "heli"), None) or                 next((d for d, w in sorted(dst.items()) if w["class"] in AIR), None)
        if pick:
            pairs.append("%d:%d" % (mid, pick))
    # [crossover] the other game's real cars packed into Liberty City's mods (tools/stories_pack.py): a packed car
    # crosses as itself (VCS's Deluxo into an LCS slot bound for it on arrival, and back as the Deluxo)
    import stories_pack
    bound = stories_pack.bound_map("lcs") if packs else {}  # VCS model id -> LCS slot
    if bound:
        # (LCS -> VCS: the fixed slots here; a slot bound on demand is mapped back by the game, dualworld.cpp mapCar)
        over = bound if src_game == "vcs" else {slot: vid for vid, slot in bound.items() if slot < stories_pack.ID_BASE}
        if src_game == "vcs":  # + the nearest LCS model x 1000: what arrives when no slot is free (lcs_trainer arrive)
            nearest = {int(a): int(b) for a, b in (p.split(":") for p in pairs)}
            over = {k: v + 1000 * nearest.get(k, 0) if v >= stories_pack.ID_BASE else v for k, v in over.items()}
        # [parity] aircraft stay aircraft: a packed VCS helicopter is bound by LCS into a model that keeps its helicopter
        # handling (crossover2: new ids 217-236 copying the nearest model info) and flies; when nothing is free it
        # arrives as the nearest LCS aircraft (x 1000 above: `pairs` maps aircraft to aircraft only)
        pairs = [p for p in pairs if int(p.split(":")[0]) not in over] + ["%d:%d" % kv for kv in sorted(over.items())]
    # [crossover] LCS's real vehicles packed into Vice City's mods (stories_pack.py build <LCS iso> --game vcs): an LCS
    # vehicle crosses into VCS as itself, 100000 + its id (+ 1000 x its nearest VCS model: what arrives when VCS can't
    # bind it; vcs_trainer arrive); one on a VCS new id goes back as its LCS self (dualworld.cpp mapCar)
    if packs and src_game == "lcs" and dst_game == "vcs":
        # [robust] Vice City's packs as the background Vice City has them (vcs_data: <build>/stories/vcs-background, where
        # stories_auto.py --for stories puts them), not the player's own Vice City folder: read there, a Kuruma crossed
        # as VCS's nearest model (171) in a hidden pair, though Vice City had the pack
        packed = {k: v for k, v in stories_pack.bound_map("vcs", vcs_data).items() if v >= stories_pack.ID_BASE}
        if packed:
            nearest = {int(a): int(b) for a, b in (p.split(":") for p in pairs)}
            over = {k: v + 1000 * nearest.get(k, 0) for k, v in packed.items() if nearest.get(k, 0) < 1000}
            pairs = [p for p in pairs if int(p.split(":")[0]) not in over] + ["%d:%d" % kv for kv in sorted(over.items())]
    return ",".join(pairs)


def send(port, line, timeout=10.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.sendall((line + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    return data.decode(errors="replace").strip()


def wait_port(port, proc, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            if send(port, "ping", 2.0) == "ok":
                return True
        except OSError:
            time.sleep(0.5)
    return False


def field(reply, name):
    for word in reply.split():
        if word.startswith(name + "="):
            return word.split("=", 1)[1]
    return None


def dump_settings(exe, out):
    """exe --dump-settings-json: its settings registry with the values its own vcs.ini holds (nothing else starts)."""
    env = dict(os.environ)
    env.pop("VCS_SETTINGS_INI", None)
    try:
        subprocess.run([exe, "--dump-settings-json", out], cwd=os.path.dirname(exe), env=env, timeout=60,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(out, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def background_ini(lcs_exe, vcs_exe, out):
    """The background Vice City's vcs.ini: the player's own Vice City vcs.ini (its iso=, window place, Vice City-only
    settings), every setting both games have taken from Liberty City's (same keys and meaning; the range clamped:
    LCS's draw distance goes to 1000%, VCS's to 300%; Liberty City at its default -> Vice City at its own default, as
    the effects volume's differ). Returns the number of settings taken, or None (no dump: Vice City keeps its own)."""
    tmp = out + ".json"
    lj = dump_settings(lcs_exe, tmp + ".lcs")
    vj = dump_settings(vcs_exe, tmp + ".vcs")
    if not lj or not vj:
        return None
    vspec = {x["key"]: x for x in vj["settings"]}
    taken, drop = {}, set()
    for ls in lj["settings"]:
        k = ls["key"]
        vs = vspec.get(k)
        if not vs or ls["type"] == "action" or ls.get("otherGame") or vs.get("otherGame"):
            continue
        drop.add(k)
        if vs.get("alias") or k == "display.scale":  # older keys read when the key is absent (src/main.cpp: "scale")
            drop.add(vs.get("alias") or "scale")
        v = ls["value"]
        if v == ls["default"]:
            continue  # Vice City's own default
        if vs["type"] == "integer":
            v = max(vs["min"], min(vs["max"], v))
        elif vs["type"] == "choice" and v not in [c["value"] for c in vs.get("choices", [])]:
            continue
        if v != vs["default"]:
            taken[k] = v
    lines = []
    own = vj.get("ini") or os.path.join(os.path.dirname(vcs_exe), "vcs.ini")
    if os.path.isfile(own):
        with open(own, encoding="utf-8", errors="replace") as f:
            for line in f:
                key = line.split("=", 1)[0].strip()
                if "=" in line and key in drop:
                    continue
                lines.append(line.rstrip("\r\n"))
    lines.append("# tools/stories.py: Liberty City's settings for the background Vice City (made at each start)")
    lines += ["%s=%d" % kv for kv in sorted(taken.items())]
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    for p in (tmp + ".lcs", tmp + ".vcs"):
        try:
            os.remove(p)
        except OSError:
            pass
    return len(drop), len(taken)


def stop(proc, port, name):
    if proc is None or proc.poll() is not None:
        return
    try:
        send(port, "quit", 5.0)
    except OSError:
        pass
    try:
        proc.wait(timeout=8)  # [launcherfix] the game's own shutdown guard ends it after ~3 s
    except subprocess.TimeoutExpired:
        print("[stories] %s did not quit: ended" % name)
        proc.kill()
        proc.wait()


VCS_CARD_FOLDER = "BASLUS-21590GTA40001"  # Vice City's save folder on mc0 (vcs_disc.KNOWN_GAMES)
VCS_QUICK_SLOT = 8  # save-anywhere's quick save slot (vcs_save_anywhere.cpp kQuickSlot)
SLOT_FILE = re.compile(r"^[1-8] ")  # a save slot file: "<slot> <mission name>" (game-internals.md 10c)


LCS_CARD_FOLDER = "BASLUS-21423GTA30001"  # Liberty City's save folder on mc0
LCS_SLOT_FILE = re.compile(r"^[1-8](%3A|:| )")  # "8%3A Home Sweet Home": the card layer's %XX for "8: ..."


def data_base():
    """src/main.cpp defaultDataDirectory: the folder holding each game's data folder (VCS, LCS)."""
    if tc.IS_WINDOWS:
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "GTAStoriesRecomp")
    if tc.IS_MACOS:
        return os.path.expanduser("~/Library/Application Support/GTAStoriesRecomp")
    xdg = os.environ.get("XDG_DATA_HOME", "")
    return os.path.join(xdg if xdg.startswith("/") else os.path.expanduser("~/.local/share"), "GTAStoriesRecomp")


def player_vcs_data():
    """[robust] The player's own Vice City data folder (src/main.cpp dataDirectory; VCS_DATA_DIR is not read here: in
    this tool it is Liberty City's). STORIES_VCS_DATA: another one (tests)."""
    if os.environ.get("STORIES_VCS_DATA"):
        return os.environ["STORIES_VCS_DATA"]
    return os.path.join(data_base(), "VCS")


def player_lcs_data():
    """[firstmission] Liberty City's data folder in this run: VCS_DATA_DIR when set (Liberty City inherits this tool's
    environment), else its default one (src/main.cpp dataDirectory)."""
    return os.environ.get("VCS_DATA_DIR") or os.path.join(data_base(), "LCS")


def newest_save(data, game="vcs"):
    """The newest save slot file's time on a data folder's card (0: none)."""
    folder = os.path.join(data, "mc0", LCS_CARD_FOLDER if game == "lcs" else VCS_CARD_FOLDER)
    pattern = LCS_SLOT_FILE if game == "lcs" else SLOT_FILE
    try:
        return max([os.path.getmtime(os.path.join(folder, n)) for n in os.listdir(folder)
                    if pattern.match(n) and os.path.getsize(os.path.join(folder, n)) >= 4096] or [0])
    except OSError:
        return 0


# [firstmission] Starter saves (docs/STORIES.md "A new game starts after the first mission"): a card with one save made
# right after each city's first story mission (Liberty City: Home Sweet Home; Vice City: Soldier). When a city has no
# save of the player's to start from, its card is made from the starter, so the game's boot loads it and the story goes
# on from the second mission. Never copied over a card that has a save. Saves are game data: never in the repository;
# the player makes them (play the first mission, save) and `--set-starter` takes them.
def starter_saves_on():
    return os.environ.get("STORIES_STARTER_SAVES", "1") != "0"


def starter_card(game):
    """The starter card folder (holding mc0/) for "lcs" / "vcs", or None: <build>/stories/starter/<game>, else
    STORIES_STARTER_DIR/starter_<game>."""
    found = [os.path.join(tc.BUILD, "stories", "starter", game)]
    if os.environ.get("STORIES_STARTER_DIR"):
        found.append(os.path.join(os.environ["STORIES_STARTER_DIR"], "starter_" + game))
    for path in found:
        if newest_save(path, game):
            return path
    return None


def put_card(src, data):
    """Copy the card src/mc0 onto data/mc0; a card already there (it has no save: the callers check) is kept as
    mc0.old. copy2 keeps the files' times; the card's own .ps2x-order files keep its directory order."""
    import shutil
    dst = os.path.join(data, "mc0")
    os.makedirs(data, exist_ok=True)
    if os.path.isdir(dst):
        old = os.path.join(data, "mc0.old")
        shutil.rmtree(old, ignore_errors=True)
        os.replace(dst, old)
    shutil.copytree(os.path.join(src, "mc0"), dst, copy_function=shutil.copy2)


def lcs_starter():
    """[firstmission] Liberty City's card on a fresh start: when the card it plays (player_lcs_data) has no save, the
    starter's is copied onto it (its boot then loads it). A card with any save is never touched."""
    data = player_lcs_data()
    if newest_save(data, "lcs"):
        return "save"
    src = starter_card("lcs") if starter_saves_on() else None
    if not src:
        return "new"
    put_card(src, data)
    print("[stories] Liberty City: no save on your card - it starts from the starter save (after its first mission: %s)"
          % src)
    return "starter"


def set_starter(spec):
    """--set-starter lcs|vcs=<data folder>: that folder's card (its mc0/, with a save made after the first mission)
    becomes the starter, copied to <build>/stories/starter/<game>."""
    import shutil
    game, sep, src = spec.partition("=")
    if game not in ("lcs", "vcs") or not sep or not newest_save(src, game):
        print("[stories] --set-starter lcs=<folder> or vcs=<folder>: a data folder whose mc0/ holds a %s save"
              % (game.upper() if game in ("lcs", "vcs") else "game's"))
        sys.exit(1)
    dst = os.path.join(tc.BUILD, "stories", "starter", game)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(os.path.join(src, "mc0"), os.path.join(dst, "mc0"), copy_function=shutil.copy2)
    print("[stories] starter save for %s: %s" % (game.upper(), dst))


def background_card(data):
    """[robust] The background Vice City's card (docs/STORIES.md "Vice City behind: free roam"). The player's own Vice
    City saves, when there are any, are copied onto it (only read: their card is never written) when it has none or
    theirs are newer - the old card kept as mc0.old - and its boot loads the newest save, as Vice City alone does.
    Without a save: the starter save when there is one ([firstmission] starter_card: after Soldier), else a new game,
    and dualworld.cpp (STORIES_DW_OPENING=0) keeps the opening mission from starting: free roam at Fort Baxter.
    Returns "save", "starter" or "new"."""
    import shutil
    mine, src = newest_save(data), player_vcs_data()
    theirs = newest_save(src)
    # [savesync] what the card sync back (sync_back_card) compares with: the background card's newest save and the
    # player's card as they were when this session started
    SYNC_BASE.clear()
    SYNC_BASE.update(src=src, fp=card_fingerprint(src), start=theirs if (theirs and theirs > mine) else mine)
    if theirs and theirs > mine:
        dst = os.path.join(data, "mc0")
        if os.path.isdir(dst):
            old = os.path.join(data, "mc0.old")
            shutil.rmtree(old, ignore_errors=True)
            os.replace(dst, old)
        # copy2 keeps the files' times; the card's own .ps2x-order file keeps its directory order
        shutil.copytree(os.path.join(src, "mc0"), dst, copy_function=shutil.copy2)
        print("[stories] Vice City behind loads your newest Vice City save (a copy of your card: %s)" % src)
        SYNC_BASE["start"] = newest_save(data)  # copy2 keeps the times: the same as theirs
        return "save"
    if mine:
        print("[stories] Vice City behind loads its card's newest save")
        return "save"
    starter = starter_card("vcs") if starter_saves_on() else None
    if starter:  # [firstmission] no save anywhere: the starter (after Soldier); the sync takes only newer saves back
        put_card(starter, data)
        # [starterfix] only a save point's save is a starter: the quick save slot (8: save-anywhere) loads as a NEW game
        # (docs/STORIES.md), so it is not installed; what is dropped is only the copy on the background card (the starter
        # folder itself is untouched). Its slot files are dated 2000-01-01: any save of the player's, whenever made, counts
        # as newer than the starter (the copy above); the game picks the boot's save by the date inside the file
        folder = os.path.join(data, "mc0", VCS_CARD_FOLDER)
        for n in os.listdir(folder):
            if SLOT_FILE.match(n):
                if n.startswith(str(VCS_QUICK_SLOT) + " "):
                    print("[stories] the starter's quick save (slot %d) is not used: it would load as a new game" % VCS_QUICK_SLOT)
                    os.remove(os.path.join(folder, n))
                else:
                    os.utime(os.path.join(folder, n), (946684800, 946684800))
        if not newest_save(data):  # nothing but a quick save: no starter after all
            print("[stories] the starter card has no save point save: Vice City behind starts a new game")
            starter = None
    if starter:
        SYNC_BASE["start"] = newest_save(data)
        print("[stories] Vice City behind starts from the starter save (after its first mission: %s)" % starter)
        return "starter"
    print("[stories] Vice City behind: a new game in free roam (no Vice City save; its opening mission is skipped)")
    return "new"


SYNC_BASE = {}  # [savesync] set by background_card: src (the player's data folder), fp (its card), start (newest save time)
SYNC_KEEP = 3   # backups of the player's card kept next to it


def card_fingerprint(data):
    """[savesync] The player's card (whole mc0 folder) as (relative path, size, mtime_ns) triples: any change differs."""
    out = []
    root = os.path.join(data, "mc0")
    for d, _, files in os.walk(root):
        for n in files:
            f = os.path.join(d, n)
            try:
                st = os.stat(f)
            except OSError:
                continue
            out.append((os.path.relpath(f, root), st.st_size, st.st_mtime_ns))
    return sorted(out)


def vcs_running(exclude=()):
    """[savesync] True when a Vice City process (vcs.exe / vcs) other than `exclude` pids runs: its card may be open."""
    try:
        if tc.IS_WINDOWS:
            out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=20).stdout
            rows = [l.replace('"', "").split(",") for l in out.splitlines()]
            return any(r[0].lower() == "vcs.exe" and int(r[1]) not in exclude for r in rows if len(r) > 1 and r[1].isdigit())
        out = subprocess.run(["pgrep", "-x", "vcs"], capture_output=True, text=True, timeout=20).stdout.split()
        return any(int(x) not in exclude for x in out)
    except (OSError, subprocess.SubprocessError, ValueError):
        return True  # cannot tell: do not write


def sync_back_card(data, exclude=()):
    """[savesync] Saves made in Vice City during both-cities mode live on the background card; copy them back to the
    player's own Vice City card (docs/STORIES.md "Saves go back to your card"). STORIES_SAVE_SYNC=0 turns it off.
    Safe by rule: only when the background card has a save newer than the session's start; never when the player's
    card changed since the session began (played alone meanwhile) or a Vice City runs; their card is backed up first
    (mc0.bak-<time>, the last SYNC_KEEP kept). The card is a folder of slot files: copied whole, swapped in by rename.
    Returns "synced", "none", "off", "skipped" or "failed"."""
    import shutil
    if os.environ.get("STORIES_SAVE_SYNC", "1") == "0":
        return "off"
    if not SYNC_BASE:
        return "none"
    src = SYNC_BASE["src"]
    new = newest_save(data)
    if not new or new <= SYNC_BASE["start"]:
        return "none"
    if os.path.abspath(src) == os.path.abspath(data):
        return "none"
    try:
        if card_fingerprint(src) != SYNC_BASE["fp"]:
            print("[stories] save sync: your Vice City card changed during this session (Vice City alone?): NOT overwritten. "
                  "The session's saves stay on the background card (%s); both are kept" % os.path.join(data, "mc0"))
            return "skipped"
        if vcs_running(exclude):
            print("[stories] save sync: a Vice City is running (it may use your card): NOT written. The session's saves stay "
                  "on the background card (%s)" % os.path.join(data, "mc0"))
            return "skipped"
        card, bg = os.path.join(src, "mc0"), os.path.join(data, "mc0")
        tmp = card + ".sync-new"
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.copytree(bg, tmp, copy_function=shutil.copy2)
        if os.path.isdir(card):
            bak = card + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
            shutil.copytree(card, bak, copy_function=shutil.copy2)
            olds = sorted(d for d in os.listdir(src) if d.startswith("mc0.bak-"))
            for d in olds[:-SYNC_KEEP]:
                shutil.rmtree(os.path.join(src, d), ignore_errors=True)
            print("[stories] save sync: your card backed up as %s" % bak)
            gone = card + ".sync-old"
            shutil.rmtree(gone, ignore_errors=True)
            os.replace(card, gone)
            os.replace(tmp, card)
            shutil.rmtree(gone, ignore_errors=True)
        else:
            os.makedirs(src, exist_ok=True)
            os.replace(tmp, card)
        SYNC_BASE.update(fp=card_fingerprint(src), start=new)
        print("[stories] save sync: your Vice City card now has this session's saves (%s)" % card)
        return "synced"
    except OSError as e:
        print("[stories] save sync failed (%s): your card is left as it was; the saves stay on %s"
              % (e, os.path.join(data, "mc0")))
        return "failed"


def vcs_skip_intro(vp, alive, seconds=150):
    """The background Vice City until it plays: then its picture is handed over. A loaded save or the new game without
    its opening mission (dualworld.cpp opening_skipped) plays as soon as the trainer says ready; a new game that still
    shows the intro cutscene (STORIES_DW_OPENING=1) has it skipped with Cross, as before."""
    t0 = time.time()
    ready = False
    t_ready = 0.0
    while time.time() - t0 < seconds and alive():
        try:
            state = send(vp, "trainer state", 5.0)
            skipped = (field(send(vp, "dualworld", 5.0), "opening_skipped") or "0") != "0"
            if "ready=1" in state and (skipped or os.environ.get("STORIES_DW_OPENING") != "1") and time.time() - t0 > 10:
                if not ready:
                    ready, t_ready = True, time.time()
                elif time.time() - t_ready > 4:
                    break
            elif "ready=1" in state and time.time() - t0 > 40:
                send(vp, "press Shift 300")
                if not ready:
                    ready, t_ready = True, time.time()
                elif time.time() - t_ready > 20:
                    break
            elif time.time() - t0 > 30 and os.environ.get("STORIES_DW_OPENING") == "1":
                send(vp, "press Shift 300")
        except OSError:
            pass
        time.sleep(2)
    try:
        send(vp, "dualworld resume", 5.0)
    except OSError:
        pass
    return ready


def oneproc(a):
    """[stories] Both games in one process: stories.exe loads lcs_game.dll and vcs_game.dll (each built with
    tools/build.py --dll in a build folder of its own) and shows the game in front in its one window; the border
    switches which game's picture the window shows, without a window swap. Each game gets the same switches as the two
    processes below, through the host's per-game environment (--env lcs:NAME=value / vcs:NAME=value)."""
    lcs_dll = os.environ.get("STORIES_LCS_DLL") or os.path.join(tc.BUILD, "oneproc", "lcs", "lcs_game.dll")
    vcs_dll = os.environ.get("STORIES_VCS_DLL") or os.path.join(tc.BUILD, "oneproc", "vcs", "vcs_game.dll")
    host = os.environ.get("STORIES_HOST_EXE") or os.path.join(os.path.dirname(lcs_dll), "stories.exe")
    missing = [p for p in (lcs_dll, vcs_dll, host) if not os.path.isfile(p)]
    if missing:
        print("[stories] --oneproc needs both games built as one program: python tools/build.py --oneproc")
        print("  missing: " + ", ".join(missing))
        sys.exit(1)
    if a.check:
        print("[stories] ok: %s with %s and %s" % (host, lcs_dll, vcs_dll))
        return
    # the normal games' folders: Liberty City's settings (vcs.ini) and both discs (iso=) are the player's usual ones
    lcs_dir = os.path.join(tc.BUILD, "lcs")
    lcs_exe = os.environ.get("STORIES_LCS_EXE") or tc.exe(os.path.join(lcs_dir, "lcs"))
    vcs_exe = os.environ.get("STORIES_VCS_EXE") or tc.exe(os.path.join(tc.BUILD, "vcs"))
    lcs_ini = os.path.join(os.path.dirname(lcs_exe), "vcs.ini")
    vcs_ini = os.path.join(os.path.dirname(vcs_exe), "vcs.ini")

    def iso_in(ini):
        try:
            with open(ini, encoding="utf-8", errors="replace") as f:
                for line in f:
                    k, sep, v = line.partition("=")
                    if sep and k.strip() == "iso" and v.strip():
                        return v.strip()
        except OSError:
            pass
        return None
    lcs_iso = a.lcs_iso or iso_in(lcs_ini)
    vcs_iso = a.vcs_iso or iso_in(vcs_ini)
    if not lcs_iso or not vcs_iso:
        print("[stories] --oneproc needs both discs: --lcs-iso / --vcs-iso (or iso= in %s and %s)" % (lcs_ini, vcs_ini))
        sys.exit(1)
    lp, vp = a.port, a.port + 1
    lcs_starter()  # [firstmission] a fresh Liberty City card starts after its first mission
    name = "GTAStoriesOneProc-%d" % os.getpid()
    common = {"STORIES_DW_NAME": name, "STORIES_DW_OFFSET": a.offset, "STORIES_DW_BORDER": str(a.border),
              "PS2X_CONTROL": "1", "STORIES_HERO": a.hero,
              "STORIES_DW_MISSION_BORDER": a.mission_border}  # [hero] one hero in both cities
    data = os.path.join(tc.BUILD, "stories", "vcs-background")
    os.makedirs(data, exist_ok=True)
    logs = os.path.join(tc.BUILD, "stories")
    lenv = dict(common, PS2X_CONTROL_PORT=str(lp), STORIES_DW_CARMAP=car_map("lcs", "vcs", vcs_data=data))
    if os.path.isfile(lcs_ini):
        lenv["VCS_SETTINGS_INI"] = lcs_ini  # Liberty City's own settings (its DLL's folder has no vcs.ini)
    # the far city's picture: 1280 wide at 16:9 until the front's aspect arrives (dualworld syncAspect resizes it)
    venv = dict(common, PS2X_CONTROL_PORT=str(vp), STORIES_DW_SIZE=a.size or "1280x720", VCS_KEEP_SETTINGS="1",
                VCS_DATA_DIR=data, VCS_SKIP_MOVIES="1", VCS_FAR_CLIP="6000",
                STORIES_DW_WAIT="1", STORIES_DW_CARMAP=car_map("vcs", "lcs"),
                VCS_LOAD_SLOT="",  # "": LCS's slot is not Vice City's (the background plays its own card)
                STORIES_DW_OPENING=os.environ.get("STORIES_DW_OPENING", "0"))  # [robust] never its opening mission
    background_card(data)
    # [parity] the background Vice City takes Liberty City's settings (a file made for it; needs the normal exes'
    # --dump-settings-json: without them Vice City keeps its own vcs.ini's)
    if os.path.isfile(lcs_exe) and os.path.isfile(vcs_exe):
        bg_ini = os.path.join(logs, "vcs-background.ini")
        made = background_ini(lcs_exe, vcs_exe, bg_ini)
        if made:
            venv["VCS_SETTINGS_INI"] = bg_ini
            print("[stories] Vice City takes Liberty City's settings (%d shared, %d not at their default)" % made)
    args = [host, "--lcs", lcs_dll, "--vcs", vcs_dll, "--lcs-iso", lcs_iso, "--vcs-iso", vcs_iso]
    for who, env in (("lcs", lenv), ("vcs", venv)):
        for k, v in env.items():
            args += ["--env", "%s:%s=%s" % (who, k, v)]
    log = open(os.path.join(logs, "oneproc.log"), "w", encoding="utf-8")
    proc = subprocess.Popen(args, cwd=os.path.dirname(host), stdout=log, stderr=subprocess.STDOUT)
    try:
        if not wait_port(lp, proc, 90) or not wait_port(vp, proc, 60):
            print("[stories] the games did not start (see %s)" % os.path.join(logs, "oneproc.log"))
            if proc.poll() is None:
                proc.kill()
            sys.exit(1)
        print("[stories] one process: Vice City is loading in the background (about a minute)...")
        vcs_skip_intro(vp, lambda: proc.poll() is None)
        print("[stories] Vice City is on the horizon, east of Portland")
        proc.wait()
        print("[stories] closed")
        sync_back_card(data)
    except KeyboardInterrupt:
        print("[stories] stopping")
        try:
            send(lp, "quit", 5.0)
        except OSError:
            pass
        try:
            proc.wait(timeout=8)  # [launcherfix] the game's own shutdown guard ends it after ~3 s
        except subprocess.TimeoutExpired:
            proc.kill()
        sync_back_card(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only check that both games are there")
    ap.add_argument("--lcs-iso", default=os.environ.get("LCS_ISO"))
    ap.add_argument("--vcs-iso", default=os.environ.get("VCS_ISO"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("STORIES_PORT", "48310")),
                    help="control ports: this (LCS) and the next (VCS) (STORIES_PORT: tests started through the launcher)")
    ap.add_argument("--offset", default="-3800,0,0", help="Vice City = Liberty City + this (metres x,y,z)")
    ap.add_argument("--size", default="", help="the far city's picture WxH (default: 1280 wide, LCS's aspect)")
    ap.add_argument("--hero", default=os.environ.get("STORIES_HERO", "vic"), choices=("city", "toni", "vic"),
                    help="who plays in both cities: Vic (default), Toni, or each city's own hero (city) (STORIES_HERO; the other "
                         "game's hero from tools/stories_pack.py hero; src/game/stories_hero.h)")
    ap.add_argument("--mission-border", default=os.environ.get("STORIES_DW_MISSION_BORDER", "fail"), choices=("block", "fail"),
                    help="crossing the border during a mission: fail (default; the crossing fails the mission) "
                         "or block (the border is a wall until the mission is over) (STORIES_DW_MISSION_BORDER)")
    ap.add_argument("--border", type=float, default=1800.0,
                    help="the border: an LCS x east of Portland (Vice City's side: it + the offset's x)")
    ap.add_argument("--restarts", type=int, default=3, help="how many times a stopped Vice City is started again")
    ap.add_argument("--oneproc", action="store_true",
                    help="both games in ONE process and one window (stories.exe + the games' DLLs, built with "
                         "tools/build.py --dll; docs/STORIES.md 'One process')")
    ap.add_argument("--set-starter", metavar="GAME=FOLDER",
                    help="lcs=<data folder> or vcs=<data folder>: its card (a save made after the first mission) becomes "
                         "that city's starter save (<build>/stories/starter/<game>; docs/STORIES.md)")
    a = ap.parse_args()
    if a.set_starter:
        return set_starter(a.set_starter)
    if a.oneproc:
        return oneproc(a)

    # STORIES_LCS_EXE / STORIES_VCS_EXE: other builds (tests)
    lcs_exe = os.environ.get("STORIES_LCS_EXE") or tc.exe(os.path.join(tc.BUILD, "lcs", "lcs"))
    vcs_exe = os.environ.get("STORIES_VCS_EXE") or tc.exe(os.path.join(tc.BUILD, "vcs"))
    missing = []
    if not os.path.isfile(lcs_exe):
        missing.append("Liberty City Stories is not built (%s): python tools/build.py --game lcs --recompile <your LCS .iso>" % lcs_exe)
    if not os.path.isfile(vcs_exe):
        missing.append("Vice City Stories is not built (%s): python tools/build.py --recompile <your VCS .iso>" % vcs_exe)
    for game, iso in (("LCS", a.lcs_iso), ("VCS", a.vcs_iso)):
        if iso and not os.path.isfile(iso):
            missing.append("%s disc not found: %s" % (game, iso))
    if missing:
        print("[stories] both cities need both games, each built from your own disc:")
        for m in missing:
            print("  - " + m)
        sys.exit(1)
    if a.check:
        print("[stories] ok: %s and %s" % (lcs_exe, vcs_exe))
        return

    lp, vp = a.port, a.port + 1
    name = "GTAStoriesDualWorld-%d" % os.getpid()
    common = {"STORIES_DW_NAME": name, "STORIES_DW_OFFSET": a.offset, "STORIES_DW_BORDER": str(a.border),
              "STORIES_HERO": a.hero, "STORIES_DW_MISSION_BORDER": a.mission_border}  # [hero] both games play the same hero: the crossing keeps him
    data = os.path.join(tc.BUILD, "stories", "vcs-background")
    os.makedirs(data, exist_ok=True)
    logs = os.path.join(tc.BUILD, "stories")

    lcs_starter()  # [firstmission] a fresh Liberty City card starts after its first mission
    env = dict(os.environ, STORIES_DUALWORLD="front", PS2X_CONTROL="1", PS2X_CONTROL_PORT=str(lp),
               STORIES_DW_CARMAP=car_map("lcs", "vcs", vcs_data=data), **common)
    largs = [lcs_exe] + ([a.lcs_iso] if a.lcs_iso else [])
    llog = open(os.path.join(logs, "lcs.log"), "w", encoding="utf-8")
    lcs = subprocess.Popen(largs, cwd=os.path.dirname(lcs_exe), env=env, stdout=llog, stderr=subprocess.STDOUT)
    vcs = None
    try:
        if not wait_port(lp, lcs, 60):
            print("[stories] Liberty City Stories did not start (see %s)" % os.path.join(logs, "lcs.log"))
            sys.exit(1)
        aspect_id = 43
        try:
            aspect_id = int(field(send(lp, "dualworld"), "aspect") or 43)
        except (OSError, ValueError):
            pass
        ratio = ASPECTS.get(aspect_id, 16 / 9)  # "Match window" (0): 16:9 (docs/STORIES.md, known limits)
        w = 1280
        h = int(round(w / ratio / 2)) * 2
        size = a.size or "%dx%d" % (w, min(h, 1344))
        # [parity] the background Vice City's settings: Liberty City's (one set for both cities; never your own Vice City
        # vcs.ini, which is only read). No VCS_ASPECT / VCS_DRAW_DISTANCE overrides any more: a variable would make the
        # setting read-only, and the F2 changes made in front could not reach it (dualworld.cpp syncSettings).
        bg_ini = os.path.join(logs, "vcs-background.ini")
        made = background_ini(lcs_exe, vcs_exe, bg_ini)
        if made:
            print("[stories] Vice City takes Liberty City's settings (%d shared, %d not at their default)" % made)
        venv = dict(os.environ, STORIES_DUALWORLD="back", STORIES_DW_SIZE=size, PS2X_CONTROL="1", PS2X_CONTROL_PORT=str(vp),
                    VCS_KEEP_SETTINGS="1", VCS_DATA_DIR=data, VCS_SKIP_MOVIES="1", VCS_FAR_CLIP="6000",
                    STORIES_DW_WAIT="1", PS2X_HIDDEN_WINDOW=size, STORIES_DW_CARMAP=car_map("vcs", "lcs"), **common)
        if made:
            venv["VCS_SETTINGS_INI"] = bg_ini
        venv.pop("VCS_LOAD_SLOT", None)  # LCS's slot: Vice City plays its own card
        venv["STORIES_DW_OPENING"] = os.environ.get("STORIES_DW_OPENING", "0")  # [robust] never its opening mission
        background_card(data)
        if os.environ.get("PS2X_HIDDEN") != "1":
            # the window made hidden, shown when the player crosses over (sound and input as usual, muted while behind)
            venv["PS2X_WINDOW_HIDDEN"] = "1"
        elif os.environ.get("PS2X_HIDDEN_WINDOW"):
            venv["PS2X_HIDDEN_WINDOW"] = os.environ["PS2X_HIDDEN_WINDOW"]  # a hidden test: Liberty City's window size
        vargs = [vcs_exe] + ([a.vcs_iso] if a.vcs_iso else [])

        def start_vcs(nth):
            vlog = open(os.path.join(logs, "vcs-background.log" if nth == 0 else "vcs-background-%d.log" % nth), "w",
                        encoding="utf-8")
            proc = subprocess.Popen(vargs, cwd=os.path.dirname(vcs_exe), env=venv, stdout=vlog, stderr=subprocess.STDOUT)
            if not wait_port(vp, proc, 60):
                print("[stories] Vice City Stories did not start (see %s): Liberty City runs alone" % vlog.name)
                return proc
            print("[stories] Vice City is loading in the background (about a minute)...")
            vcs_skip_intro(vp, lambda: lcs.poll() is None and proc.poll() is None)
            print("[stories] Vice City is on the horizon, east of Portland")
            return proc

        def role(port):
            try:
                return field(send(port, "dualworld", 3.0), "role")
            except OSError:
                return None

        vcs = start_vcs(0)
        restarts = 0
        silent = 0.0  # since when Vice City's control port has not answered (a game thread that died: it is ended)
        front = "lcs"  # the game last seen in front (shown, played)
        while True:
            lr = role(lp) if lcs.poll() is None else None
            if lr == "front":
                front = "lcs"
            if vcs is not None and vcs.poll() is None:
                r = role(vp)
                silent = 0.0 if r else (silent or time.time())
                if r == "front" and lr != "front":
                    front = "vcs"
                if silent and time.time() - silent >= 30:
                    print("[stories] Vice City does not answer (30 s): ended")
                    vcs.kill()
                    vcs.wait()
                    silent = 0.0
            if lcs.poll() is not None:
                # [launcherfix] who was in front is asked again now, not read from the last look (up to a second old: a
                # window closed just after a crossing left Vice City running hidden behind nothing, the launcher "playing")
                if front != "lcs" and vcs is not None and vcs.poll() is None and role(vp) != "front":
                    front = "lcs"
                if front == "lcs" or vcs is None or vcs.poll() is not None:
                    print("[stories] Liberty City closed (code %s)" % lcs.returncode)
                    break
                # Liberty City stopped behind Vice City: Vice City goes on alone (closing it ends both)
                if lcs.returncode is not None and not getattr(lcs, "noted", False):
                    lcs.noted = True
                    print("[stories] Liberty City stopped (code %s) while Vice City was in front: Vice City goes on alone"
                          % lcs.returncode)
            if vcs is not None and vcs.poll() is not None:
                if front == "vcs" and vcs.returncode == 0:
                    print("[stories] Vice City closed: stopping both")
                    break
                if lcs.poll() is not None:
                    print("[stories] both games stopped")
                    break
                print("[stories] Vice City stopped (code %s): Liberty City goes on alone" % vcs.returncode)
                vcs = None
                sync_back_card(data, exclude=[lcs.pid])  # [savesync] the crash path: keep what it saved
                if restarts < a.restarts:
                    restarts += 1
                    time.sleep(6)  # Liberty City sees it gone (5 s) and takes the front back if Vice City had it
                    print("[stories] starting Vice City again (%d of %d)" % (restarts, a.restarts))
                    vcs = start_vcs(restarts)
                    front = "lcs"
            time.sleep(1)
    except KeyboardInterrupt:
        print("[stories] stopping both")
    finally:
        stop(vcs, vp, "Vice City Stories")
        stop(lcs, lp, "Liberty City Stories")
        sync_back_card(data)  # [savesync] this session's Vice City saves to the player's own card


if __name__ == "__main__":
    main()
