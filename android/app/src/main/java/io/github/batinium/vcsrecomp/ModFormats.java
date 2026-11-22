// [android] The mod loader's importer (docs/ANDROID.md "Mods"): reads what a PC mod for GTA III / Vice City / San
// Andreas brings that VCS can use - handling.cfg lines, weapon.dat lines - from a .zip, a VC Mod Manager .vcm, or a
// loose .cfg / .dat / .txt, and says what it cannot use (models, textures, colours). Plain Java, no Android classes:
// tools/android_mods_check.py runs it on the PC's JVM. ModsMenu stores the result; src/game/vcs_mods.cpp applies it.
//
// Mod files are other people's data: they are only ever parsed here, never run, and never committed.
package io.github.batinium.vcsrecomp;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.Inflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ModFormats {
    private ModFormats() {}

    static final class FormatException extends Exception {
        FormatException(String m) {
            super(m);
        }
    }

    // ---- the game's slots -------------------------------------------------------------------------------------------
    //
    // [mods] One table for both launchers: tools/modslots/mod_slots.txt (the APK's assets/mod_slots.txt; the desktop's
    // tools/mods.py reads the same file). Per game: vehicles (model id, model name, label, class) and weapons (type,
    // model id, ...), with the names a PC mod may use for each: the slot's own model name ("matched by name"), aliases
    // (the same vehicle / weapon in GTA III / VC / SA), and "similar" names (a vehicle / weapon the game doesn't have,
    // put into the nearest one of the same class). ModsMenu calls loadSlots() once and useGame() for the game shown.

    /** A vehicle slot (id = model id) or a weapon slot (id = weapon type, modelId its model, -1 none) */
    static final class Slot {
        final int id, modelId;
        final String model, label, cls;

        Slot(int id, int modelId, String model, String label, String cls) {
            this.id = id;
            this.modelId = modelId;
            this.model = model;
            this.label = label;
            this.cls = cls;
        }
    }

    /** One game's slots and the name rules */
    static final class Slots {
        final List<Slot> vehicles = new ArrayList<>(), weapons = new ArrayList<>();
        final Map<String, Object[]> vehicleNames = new java.util.HashMap<>(), weaponNames = new java.util.HashMap<>(); // name -> {Slot, reason}
    }

    private static final Map<String, Slots> SLOTS = new java.util.HashMap<>();
    private static volatile String game = "vcs";

    static String norm(String name) {
        return name == null ? "" : name.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]", "");
    }

    /** mod_slots.txt's text (format in the file) */
    static synchronized void loadSlots(String text) {
        SLOTS.clear();
        int n = 0;
        for (String raw : text.split("\r?\n")) {
            n++;
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#"))
                continue;
            String[] cols = line.split("\\|", -1);
            String[] head = cols[0].trim().split("\\s+");
            if (cols.length != 6 || head.length < 3)
                throw new IllegalArgumentException("mod_slots.txt:" + n + ": bad line");
            Slots s = SLOTS.computeIfAbsent(head[0], k -> new Slots());
            boolean vehicle = head[1].equals("vehicle");
            int id = Integer.parseInt(head[2]);
            int modelId = vehicle ? id : head.length > 3 && !head[3].equals("-") ? Integer.parseInt(head[3]) : -1;
            Slot slot = new Slot(id, modelId, cols[1].trim(), cols[2].trim(), cols[3].trim());
            (vehicle ? s.vehicles : s.weapons).add(slot);
            Map<String, Object[]> names = vehicle ? s.vehicleNames : s.weaponNames;
            String[][] groups = {{cols[1].trim()}, cols[4].trim().split("\\s+"), cols[5].trim().split("\\s+")};
            String[] reasons = {"name", "alias", "similar"};
            for (int g = 0; g < 3; g++)
                for (String name : groups[g]) {
                    String k = norm(name);
                    if (!k.isEmpty() && !names.containsKey(k))
                        names.put(k, new Object[] {slot, reasons[g]});
                }
        }
    }

    /** The game the lists and matching are for: "vcs" / "lcs" */
    static void useGame(String g) {
        game = g;
    }

    static String game() {
        return game;
    }

    static synchronized Slots slots() {
        Slots s = SLOTS.get(game);
        if (s == null)
            throw new IllegalStateException("mod_slots.txt not loaded for " + game);
        return s;
    }

    /** {Slot, reason} or null: reason "name" / "alias" / "similar" */
    static Object[] match(boolean vehicle, String pcName) {
        Slots s = slots();
        return (vehicle ? s.vehicleNames : s.weaponNames).get(norm(pcName));
    }

    /** The vehicle slot a PC name matches, -1 when none (the player chooses then) */
    static int matchVehicle(String pcName) {
        Object[] m = match(true, pcName);
        return m == null ? -1 : ((Slot) m[0]).id;
    }

    static int matchWeapon(String pcName) {
        Object[] m = match(false, pcName);
        return m == null ? -1 : ((Slot) m[0]).id;
    }

    static Slot vehicle(int id) {
        for (Slot v : slots().vehicles)
            if (v.id == id)
                return v;
        return null;
    }

    static Slot weapon(int type) {
        for (Slot w : slots().weapons)
            if (w.id == type)
                return w;
        return null;
    }

    static int weaponModel(int type) {
        Slot w = weapon(type);
        return w != null ? w.modelId : -1;
    }

    /** The weapon whose model a model id is, null for none */
    static Slot weaponOfModel(int model) {
        if (model < 0)
            return null;
        for (Slot w : slots().weapons)
            if (w.modelId == model)
                return w;
        return null;
    }

    /** The player's words for a match reason */
    static String matchText(String match) {
        switch (match == null ? "none" : match) {
            case "name": return "matched by name";
            case "alias": return "alias";
            case "similar": return "nearest equivalent";
            case "picked": return "picked by you";
            default: return "no match: pick a slot";
        }
    }

    /** Whether the entry goes into a vehicle slot (handling, a car model) */
    static boolean vehicleEntry(Entry e) {
        return e.kind.equals("handling") || e.kind.equals("colours") || (e.kind.equals("model") && e.dialect.equals("car"));
    }

    /** Matches the entry's name to a slot of the game shown (sets target and match) */
    static void autoTarget(Entry e) {
        if (!e.kind.equals("handling") && !e.kind.equals("weapon") && !e.kind.equals("model") && !e.kind.equals("colours"))
            return;
        Object[] m = match(vehicleEntry(e), e.pcName);
        if (m == null) {
            e.target = -1;
            e.match = "none";
            return;
        }
        Slot s = (Slot) m[0];
        e.target = e.kind.equals("model") && !vehicleEntry(e) ? s.modelId : s.id;
        e.match = e.target < 0 ? "none" : (String) m[1];
    }

    /** The slot an entry goes into, null for none */
    static Slot targetSlot(Entry e) {
        if (e.target < 0)
            return null;
        if (vehicleEntry(e))
            return vehicle(e.target);
        if (e.kind.equals("model"))
            return weaponOfModel(e.target);
        return e.kind.equals("weapon") ? weapon(e.target) : null;
    }

    // ---- what a mod brings ------------------------------------------------------------------------------------------

    /** One usable line: a handling.cfg line or a weapon.dat line, its values in the PC file's units */
    static final class Entry {
        final String kind;    // "handling" / "weapon" / "stream"
        final String pcName;  // the line's first word (STINGER, Python...)
        final String dialect; // gta3 / vc / sa / weapon.dat / vcs
        final String line;    // the line as found (for the details)
        final String source;  // the file it came from
        final Map<String, String> fields = new LinkedHashMap<>();
        final List<String> skipped = new ArrayList<>(); // columns VCS has no confirmed place for
        int target = -1; // vehicle model id / weapon type / streaming index, -1: none yet
        int skill = -1;  // an SA weapon.dat gun line's skill level (only while parsing: weaponSkill)
        String match = "none"; // [mods] how the target was found: name / alias / similar / picked / none
        boolean off;           // [mods] set aside: another mod's entry has this slot (the player chose it)

        Entry(String kind, String pcName, String dialect, String line, String source) {
            this.kind = kind;
            this.pcName = pcName;
            this.dialect = dialect;
            this.line = line;
            this.source = source;
        }
    }

    static final class Parsed {
        String title; // from a .vcm's <title>, else the file name
        final List<Entry> entries = new ArrayList<>();
        final List<String> notes = new ArrayList<>(); // what is not used, in plain words
        Files files;                                  // the files read (ModsMenu keeps the model files)
    }

    // ---- handling.cfg ------------------------------------------------------------------------------------------------

    // Column names per dialect, after the identifier (gtamods.com wiki "Handling.cfg"; the same lists as
    // tools/mods.py). "-" = a column the Stories games have no place for (kept in Entry.skipped by name).
    private static final String[] GTA3 = {"mass", "dimx", "dimy",
            "-dimension z (not used: the turn mass comes from x and y)", "comx", "comy", "comz", "submerged",
            "tractionmult", "tractionloss", "tractionbias", "gears", "maxspeed", "accel", "drive", "engine", "brake",
            "brakebias", "abs", "steer", "suspforce", "suspdamp", "seatoffset", "collision", "value",
            "suspupper", "susplower", "suspbias", "flags", "frontlights", "rearlights"};
    private static final String[] VC = {"mass", "dimx", "dimy",
            "-dimension z (not used: the turn mass comes from x and y)", "comx", "comy", "comz", "submerged",
            "tractionmult", "tractionloss", "tractionbias", "gears", "maxspeed", "accel", "drive", "engine", "brake",
            "brakebias", "abs", "steer", "suspforce", "suspdamp", "seatoffset", "collision", "value",
            "suspupper", "susplower", "suspbias", "antidive", "flags", "frontlights", "rearlights"};
    private static final String[] SA = {"mass", "turnmass", "drag", "comx", "comy", "comz", "submerged",
            "tractionmult", "tractionloss", "tractionbias", "gears", "maxspeed", "accel", "inertia", "drive",
            "engine", "brake", "brakebias", "abs", "steer", "suspforce", "suspdamp", "-high speed damping",
            // SA's suspension limits are tuned to SA's own collision (not used: the slot's stays); on LCS's an SA car's
            // 0.1 / -0.05 left its wheels off the ground (tools/mods.py SA, the same rule)
            "-suspension upper limit (SA's, for SA's collision)", "-suspension lower limit (SA's, for SA's collision)",
            "suspbias", "antidive", "seatoffset", "collision", "value",
            "modelflags", "handlingflags", "frontlights", "rearlights", "-anim group"};
    // GTA III / VC flag bits are the Stories games' (read from LCS's 81 records); SA's two words, bit by bit -> the VC
    // bit of the same meaning, -1 = none (tools/mods.py SA_MODEL_FLAGS / SA_HANDLING_FLAGS)
    private static final String[] SA_MODEL_NAMES = {"IS_VAN", "IS_BUS", "IS_LOW", "IS_BIG", "REVERSE_BONNET",
            "HANGING_BOOT", "TAILGATE_BOOT", "NOSWING_BOOT", "NO_DOORS", "TANDEM_SEATS", "SIT_IN_BOAT", "CONVERTIBLE",
            "NO_EXHAUST", "DBL_EXHAUST", "NO1FPS_LOOK_BEHIND", "FORCE_DOOR_CHECK", "AXLE_F_NOTILT", "AXLE_F_SOLID",
            "AXLE_F_MCPHERSON", "AXLE_F_REVERSE", "AXLE_R_NOTILT", "AXLE_R_SOLID", "AXLE_R_MCPHERSON",
            "AXLE_R_REVERSE", "IS_BIKE", "IS_HELI", "IS_PLANE", "IS_BOAT", "BOUNCE_PANELS", "DOUBLE_RWHEELS",
            "FORCE_GROUND_CLEARANCE", "IS_HATCHBACK"};
    private static final int[] SA_MODEL_VC = {5, 6, 7, 14, 2, 3, 9, 10, 4, -1, 23, 13, 20, 8, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, 16, 17, 18, 19, -1, -1, -1, -1};
    private static final String[] SA_HANDLING_NAMES = {"1G_BOOST", "2G_BOOST", "NPC_ANTI_ROLL", "NPC_NEUTRAL_HANDL",
            "NO_HANDBRAKE", "STEER_REARWHEELS", "HB_REARWHEEL_STEER", "ALT_STEER_OPT", "WHEEL_F_NARROW2",
            "WHEEL_F_NARROW", "WHEEL_F_WIDE", "WHEEL_F_WIDE2", "WHEEL_R_NARROW2", "WHEEL_R_NARROW", "WHEEL_R_WIDE",
            "WHEEL_R_WIDE2", "HYDRAULIC_GEOM", "HYDRAULIC_INST", "NOS_INST", "HYDRAULIC_NONE", "OFFROAD_ABILITY",
            "OFFROAD_ABILITY2", "HALOGEN_LIGHTS", "PROC_REARWHEEL_1ST", "USE_MAXSP_LIMIT", "LOW_RIDER", "STREET_RACER",
            "UNDEFINED", "SWINGING_CHASSIS"};
    private static final int[] SA_HANDLING_VC = {0, 1, 11, 12, -1, -1, -1, -1, 25, 25, -1, -1, -1, -1, 24, 24, -1, -1,
            -1, -1, 26, 26, 15, 21, -1, -1, -1, -1, -1};
    private static final long VC_FLAG_MASK = (1L << 27) - 1;

    /** a handling.cfg flag word (hex, with or without 0x), -1 when it isn't one */
    private static long hexWord(String s) {
        s = s.trim().toLowerCase(Locale.ROOT);
        if (s.startsWith("0x"))
            s = s.substring(2);
        return s.matches("[0-9a-f]{1,8}") ? Long.parseLong(s, 16) : -1;
    }

    private static boolean isNumber(String s) {
        try {
            double d = Double.parseDouble(s);
            return !Double.isNaN(d) && !Double.isInfinite(d);
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /** A handling.cfg line (any dialect), null when the line is not one */
    static Entry parseHandling(String raw, String source) {
        String line = raw.trim();
        if (line.isEmpty() || !Character.isLetterOrDigit(line.charAt(0)))
            return null;
        String[] t = line.split("[\\s,]+");
        if (t.length < 2 || !t[0].matches("[A-Za-z0-9_]{1,24}") || isNumber(t[0]))
            return null;
        String[] cols;
        String dialect;
        int n = t.length - 1;
        if (n == GTA3.length) {
            cols = GTA3;
            dialect = "gta3";
        } else if (n == VC.length) {
            cols = VC;
            dialect = "vc";
        } else if (n == SA.length || n == SA.length - 1) {
            cols = SA;
            dialect = "sa";
        } else
            return null;
        // the transmission letters sit in columns 15 / 16 in all three
        if (!t[15].matches("[FfRr4]") || !t[16].matches("[PpDdEe]"))
            return null;
        Entry e = new Entry("handling", t[0], dialect, line, source);
        Map<String, Long> words = new LinkedHashMap<>();
        for (int i = 0; i < n; i++) {
            String name = cols[i], value = t[i + 1];
            if (name.startsWith("-")) {
                e.skipped.add(name.substring(1));
                continue;
            }
            if (name.equals("drive") || name.equals("engine")) {
                e.fields.put(name, value.toUpperCase(Locale.ROOT));
                continue;
            }
            if (name.equals("flags") || name.equals("modelflags") || name.equals("handlingflags")) {
                long w = hexWord(value);
                if (w < 0)
                    e.skipped.add(name + " (" + value + " is not a hex number)");
                else
                    words.put(name, w);
                continue;
            }
            if (!isNumber(value))
                return null; // a word where a number belongs: not a handling line
            if (name.equals("submerged") && Double.parseDouble(value) <= 0) {
                e.skipped.add("percent submerged (" + value + " is not a percentage)");
                continue;
            }
            if (name.equals("frontlights") || name.equals("rearlights") || name.equals("abs"))
                value = Integer.toString((int) Double.parseDouble(value));
            e.fields.put(name, value);
        }
        // the flags as one VC-layout word plus the mask of the bits this dialect decides (the game keeps the others)
        if (words.containsKey("flags")) {
            long mask = dialect.equals("gta3") ? 0xFFFFL : VC_FLAG_MASK, w = words.get("flags");
            e.fields.put("flags", "0x" + Long.toHexString(w & mask));
            e.fields.put("flagmask", "0x" + Long.toHexString(mask));
            if ((w & ~mask) != 0)
                e.skipped.add("flag bits 0x" + Long.toHexString(w & ~mask) + " (no known meaning)");
        } else if (words.containsKey("modelflags") || words.containsKey("handlingflags")) {
            long out = 0, mask = 0;
            long[] ws = {words.containsKey("modelflags") ? words.get("modelflags") : 0,
                    words.containsKey("handlingflags") ? words.get("handlingflags") : 0};
            String[][] names = {SA_MODEL_NAMES, SA_HANDLING_NAMES};
            int[][] vc = {SA_MODEL_VC, SA_HANDLING_VC};
            for (int k = 0; k < 2; k++)
                for (int bit = 0; bit < vc[k].length; bit++) {
                    if (vc[k][bit] >= 0) {
                        mask |= 1L << vc[k][bit];
                        if ((ws[k] >> bit & 1) != 0)
                            out |= 1L << vc[k][bit];
                    } else if ((ws[k] >> bit & 1) != 0)
                        e.skipped.add("flag " + names[k][bit] + " (no equivalent)");
                }
            e.fields.put("flags", "0x" + Long.toHexString(out));
            e.fields.put("flagmask", "0x" + Long.toHexString(mask));
        }
        return e;
    }

    // ---- weapon.dat --------------------------------------------------------------------------------------------------

    private static final String[] FIRE_TYPES = {"MELEE", "INSTANT_HIT", "PROJECTILE", "AREA_EFFECT", "CAMERA", "USE"};
    private static final String[] WEAPON_COLS = {"range", "rate", "reload", "clip", "damage", "speed", "radius",
            "lifespan", "spread"};

    /** SA's gun line (gtamods.com wiki "Weapon.dat"): $ NAME FIRETYPE target-range weapon-range model model2 slot
     *  ANIMGROUP clip damage fire-offset(3) skill ...; one line per skill level (weaponSkill keeps std) */
    private static Entry parseWeaponSa(String[] t, String line, String source) {
        if (t.length < 18 || !t[1].matches("[A-Za-z][A-Za-z0-9_]{0,23}"))
            return null;
        boolean fire = t[2].equalsIgnoreCase("AFFECT_AREA");
        for (String f : FIRE_TYPES)
            fire |= f.equalsIgnoreCase(t[2]);
        if (!fire)
            return null;
        int ag = -1;
        for (int i = 3; i < t.length && ag < 0; i++)
            if (!isNumber(t[i]))
                ag = i;
        if (ag < 6 || ag + 7 >= t.length)
            return null;
        for (int i = 3; i < ag + 8; i++)
            if (i != ag && !isNumber(t[i]))
                return null;
        Entry e = new Entry("weapon", t[1], "sa", line, source);
        e.fields.put("range", t[4]); // D: the damage range (C, the aiming range, has no own field)
        e.fields.put("clip", Integer.toString((int) Double.parseDouble(t[ag + 1])));
        e.fields.put("damage", Integer.toString((int) Double.parseDouble(t[ag + 2])));
        if (Double.parseDouble(t[ag + 3]) != 0 || Double.parseDouble(t[ag + 4]) != 0 || Double.parseDouble(t[ag + 5]) != 0) {
            e.fields.put("firex", t[ag + 3]); // the muzzle flash position (all 0: none given)
            e.fields.put("firey", t[ag + 4]);
            e.fields.put("firez", t[ag + 5]);
        }
        e.skill = (int) Double.parseDouble(t[ag + 6]);
        e.skipped.add("fire type (kept)");
        e.skipped.add("aiming range");
        e.skipped.add("models, slot, animation group");
        e.skipped.add("skill level " + t[ag + 6] + ", accuracy, move speed, animation timings, flags");
        return e;
    }

    /** SA: the std (1) skill line of each gun kept, else the first; the others noted */
    private static void weaponSkill(Parsed p) {
        java.util.List<Entry> sa = new ArrayList<>();
        for (Entry e : p.entries)
            if (e.skill >= 0)
                sa.add(e);
        for (Entry e : sa) {
            Entry keep = null;
            for (Entry x : sa)
                if (x.pcName.equalsIgnoreCase(e.pcName) && (keep == null || (x.skill == 1 && keep.skill != 1)))
                    keep = x;
            if (keep != e && p.entries.remove(e))
                note(p, e.pcName + ": SA skill level " + e.skill + " line not used (the std line is)");
        }
    }

    /** A weapon.dat line: GTA III / VC (name, fire type, then range ... spread) or SA ($ ...), null when not one */
    static Entry parseWeapon(String raw, String source) {
        String line = raw.trim();
        String[] t = line.split("[\\s,]+");
        if (t.length > 0 && t[0].equals("$"))
            return parseWeaponSa(t, line, source);
        if (t.length < 11 || !t[0].matches("[A-Za-z][A-Za-z0-9_]{0,23}"))
            return null;
        boolean fire = false;
        for (String f : FIRE_TYPES)
            fire |= f.equalsIgnoreCase(t[1]);
        if (!fire)
            return null;
        // GTA III: 22 columns, VC: 26 (gtamods.com wiki "Weapon.dat"); the first 11 are the same
        Entry e = new Entry("weapon", t[0], t.length <= 22 ? "gta3" : "vc", line, source);
        for (int i = 0; i < WEAPON_COLS.length; i++) {
            if (!isNumber(t[i + 2]))
                return null;
            e.fields.put(WEAPON_COLS[i], t[i + 2]);
        }
        e.skipped.add("fire type (kept: it decides animations, sounds and the HUD)");
        // L-N: the fire offset (where the shot and the muzzle flash leave the gun; all 0: none given)
        if (t.length > 14 && isNumber(t[11]) && isNumber(t[12]) && isNumber(t[13])
                && (Double.parseDouble(t[11]) != 0 || Double.parseDouble(t[12]) != 0 || Double.parseDouble(t[13]) != 0)) {
            e.fields.put("firex", t[11]);
            e.fields.put("firey", t[12]);
            e.fields.put("firez", t[13]);
        }
        if (t.length > 11)
            e.skipped.add("animations, models, flags, weapon slot");
        return e;
    }

    // ---- VCS-native streaming entries (vcs_stream.txt in a mod) -----------------------------------------------------

    /** "index=<streaming index> file=<chunk in the mod>" or "index=<n> copy=<m>" (a test of the plumbing) */
    static Entry parseStream(String raw, String source) {
        String line = raw.trim();
        if (!line.startsWith("index="))
            return null;
        Entry e = new Entry("stream", "", "vcs", line, source);
        for (String t : line.split("\\s+")) {
            int eq = t.indexOf('=');
            if (eq <= 0)
                return null;
            String k = t.substring(0, eq), v = t.substring(eq + 1);
            if (k.equals("index")) {
                try {
                    e.target = Integer.parseInt(v);
                } catch (NumberFormatException x) {
                    return null;
                }
            } else if (k.equals("copy") && v.matches("\\d{1,5}"))
                e.fields.put("copy", v);
            else if (k.equals("file") && v.matches("[A-Za-z0-9_.\\-]{1,64}") && !v.contains(".."))
                e.fields.put("file", v);
            else
                return null;
        }
        return e.target >= 0 && !e.fields.isEmpty() ? e : null;
    }

    // ---- containers --------------------------------------------------------------------------------------------------

    static final int MAX_FILES = 512;
    static final long MAX_TOTAL = 256L << 20, MAX_TEXT = 1 << 20;

    /** A mod's files by name (paths flattened to the last part), text-ish ones read whole, others as their size only */
    static final class Files {
        final Map<String, byte[]> text = new LinkedHashMap<>();
        final Map<String, Long> other = new LinkedHashMap<>();
        final Map<String, byte[]> models = new LinkedHashMap<>(); // .dff / .txd, read whole (converted on the phone)
        final Map<String, byte[]> gxt = new LinkedHashMap<>();    // [mods] .gxt: a whole text file (replace disc=TEXT/...)
        // [mods] model / .gxt name -> its entry in the picked file ("" = the file itself; a zip's or a .vcm's own entry
        // name): the game reads it from there, no second copy (vcs_modfiles.h). Not set for a .vcm inside a zip.
        final Map<String, String> origin = new LinkedHashMap<>();
        final Map<String, Integer> picked = new LinkedHashMap<>(); // name -> which picked file (readAll)
    }

    private static boolean textLike(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".cfg") || n.endsWith(".dat") || n.endsWith(".txt") || n.endsWith(".ide") || n.endsWith(".xml")
                || n.endsWith(".ini") || !n.contains(".");
    }

    static final int MAX_MODEL = 32 << 20;

    private static boolean contains(byte[] b, String what) {
        return b != null && indexOf(b, what.getBytes(StandardCharsets.ISO_8859_1), 0) >= 0;
    }

    static final int MAX_GXT = 32 << 20;

    private static boolean gxtFile(String name) {
        return name.toLowerCase(Locale.ROOT).endsWith(".gxt");
    }

    private static boolean modelFile(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".dff") || n.endsWith(".txd");
    }

    private static String leaf(String path) {
        String p = path.replace('\\', '/');
        return p.substring(p.lastIndexOf('/') + 1);
    }

    static Files unzip(byte[] zip) throws IOException, FormatException {
        Files f = new Files();
        long total = 0;
        int count = 0;
        try (ZipInputStream in = new ZipInputStream(new ByteArrayInputStream(zip))) {
            for (ZipEntry e; (e = in.getNextEntry()) != null; ) {
                if (e.isDirectory())
                    continue;
                if (++count > MAX_FILES)
                    throw new FormatException("The zip holds more than " + MAX_FILES + " files.");
                String name = leaf(e.getName());
                if (name.isEmpty() || name.startsWith("."))
                    continue;
                if (name.toLowerCase(Locale.ROOT).endsWith(".vcm")) { // a VC Mod Manager file inside the zip
                    byte[] b = readLimited(in, MAX_TOTAL - total);
                    if (b == null)
                        throw new FormatException("The zip unpacks to more than " + (MAX_TOTAL >> 20) + " MB.");
                    total += b.length;
                    Files inner = unvcm(b);
                    f.text.putAll(inner.text);
                    f.models.putAll(inner.models);
                    f.other.putAll(inner.other);
                    for (String n : inner.models.keySet())
                        f.origin.remove(n); // inside a zip: the game can't reach it (files/)
                    continue;
                }
                if (textLike(name)) {
                    byte[] b = readLimited(in, MAX_TEXT);
                    if (b != null)
                        f.text.put(name, b);
                    total += b == null ? MAX_TEXT : b.length;
                } else if (modelFile(name)) {
                    byte[] b = readLimited(in, MAX_MODEL);
                    if (b == null)
                        throw new FormatException(name + " is over " + (MAX_MODEL >> 20) + " MB.");
                    f.models.put(name, b);
                    f.origin.put(name, e.getName());
                    total += b.length;
                } else if (gxtFile(name)) {
                    byte[] b = readLimited(in, MAX_GXT);
                    if (b == null)
                        throw new FormatException(name + " is over " + (MAX_GXT >> 20) + " MB.");
                    f.gxt.put(name, b);
                    f.origin.put(name, e.getName());
                    total += b.length;
                } else {
                    long n = skipAll(in);
                    f.other.put(name, n);
                    total += n;
                }
                if (total > MAX_TOTAL)
                    throw new FormatException("The zip unpacks to more than " + (MAX_TOTAL >> 20) + " MB.");
            }
        }
        return f;
    }

    private static byte[] readLimited(InputStream in, long limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 14];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > limit) {
                skipAll(in);
                return null;
            }
        }
        return out.toByteArray();
    }

    private static long skipAll(InputStream in) throws IOException {
        long total = 0;
        byte[] buf = new byte[1 << 14];
        for (int n; (n = in.read(buf)) > 0; )
            total += n;
        return total;
    }

    /**
     * VC Mod Manager's .vcm: "vcm\0" + 32 hex digits, then entries !NAME!0sf<bytes>!NAME!ef (stored) or
     * !NAME!1sf<zlib>!NAME!ef
     */
    static Files unvcm(byte[] d) throws FormatException {
        if (d.length < 36 || d[0] != 'v' || d[1] != 'c' || d[2] != 'm' || d[3] != 0)
            throw new FormatException("Not a VC Mod Manager file (no \"vcm\" header).");
        Files f = new Files();
        int at = 36, count = 0;
        while (at < d.length) {
            if (d[at] != '!')
                break;
            int nameEnd = indexOf(d, (byte) '!', at + 1, Math.min(d.length, at + 200));
            if (nameEnd < 0 || nameEnd + 3 >= d.length || d[nameEnd + 2] != 's' || d[nameEnd + 3] != 'f')
                throw new FormatException("The .vcm file is damaged near byte " + at + ".");
            String name = new String(d, at + 1, nameEnd - at - 1, StandardCharsets.ISO_8859_1);
            boolean zlib = d[nameEnd + 1] == '1';
            byte[] end = ("!" + name + "!ef").getBytes(StandardCharsets.ISO_8859_1);
            int body = nameEnd + 4, stop = indexOf(d, end, body);
            if (stop < 0)
                throw new FormatException("The .vcm entry " + name + " has no end.");
            if (++count > MAX_FILES)
                throw new FormatException("The .vcm holds too many files.");
            String leafName = leaf(name);
            if (textLike(leafName) && stop - body <= MAX_TEXT) {
                byte[] b = java.util.Arrays.copyOfRange(d, body, stop);
                if (zlib)
                    b = inflate(b, MAX_TEXT);
                if (b != null)
                    f.text.put(leafName, b);
            } else if (modelFile(leafName) && stop - body <= MAX_MODEL) {
                byte[] b = java.util.Arrays.copyOfRange(d, body, stop);
                if (zlib)
                    b = inflate(b, MAX_MODEL);
                if (b != null) {
                    f.models.put(leafName, b);
                    f.origin.put(leafName, name);
                }
            } else
                f.other.put(leafName, (long) (stop - body));
            at = stop + end.length;
        }
        if (count == 0)
            throw new FormatException("The .vcm file holds nothing.");
        return f;
    }

    private static byte[] inflate(byte[] z, long limit) {
        Inflater inf = new Inflater();
        try {
            inf.setInput(z);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 14];
            while (!inf.finished()) {
                int n = inf.inflate(buf);
                if (n == 0 && (inf.needsInput() || inf.needsDictionary()))
                    break;
                out.write(buf, 0, n);
                if (out.size() > limit)
                    return null;
            }
            return out.toByteArray();
        } catch (java.util.zip.DataFormatException e) {
            return null;
        } finally {
            inf.end();
        }
    }

    private static int indexOf(byte[] d, byte b, int from, int to) {
        for (int i = from; i < to; i++)
            if (d[i] == b)
                return i;
        return -1;
    }

    private static int indexOf(byte[] d, byte[] what, int from) {
        outer:
        for (int i = from; i + what.length <= d.length; i++) {
            for (int k = 0; k < what.length; k++)
                if (d[i + k] != what[k])
                    continue outer;
            return i;
        }
        return -1;
    }

    // ---- the whole mod -----------------------------------------------------------------------------------------------

    /** Reads a picked file: .zip, .vcm, or a loose text file (.cfg / .dat / .txt) */
    static Parsed read(String fileName, byte[] bytes) throws IOException, FormatException {
        Files f = files(fileName, bytes);
        for (String n : f.origin.keySet())
            f.picked.put(n, 0);
        return parse(f, fileName);
    }

    /** [mods] Several picked files as one mod (a .dff with its .txd, a handling.cfg with a readme...) */
    static Parsed readAll(String title, List<String> names, List<byte[]> data) throws IOException, FormatException {
        Files all = new Files();
        for (int i = 0; i < names.size(); i++) {
            Files f = files(names.get(i), data.get(i));
            all.text.putAll(f.text);
            all.models.putAll(f.models);
            all.gxt.putAll(f.gxt);
            all.other.putAll(f.other);
            for (String n : f.models.keySet()) {
                all.origin.remove(n);
                all.picked.remove(n);
            }
            for (String n : f.gxt.keySet()) {
                all.origin.remove(n);
                all.picked.remove(n);
            }
            for (Map.Entry<String, String> o : f.origin.entrySet()) {
                all.origin.put(o.getKey(), o.getValue());
                all.picked.put(o.getKey(), i);
            }
        }
        return parse(all, title);
    }

    /** One picked file's contents */
    static Files files(String fileName, byte[] bytes) throws IOException, FormatException {
        String lower = fileName.toLowerCase(Locale.ROOT);
        Files files;
        if (bytes.length >= 4 && bytes[0] == 'P' && bytes[1] == 'K')
            files = unzip(bytes);
        else if (bytes.length >= 4 && bytes[0] == 'v' && bytes[1] == 'c' && bytes[2] == 'm' && bytes[3] == 0)
            files = unvcm(bytes);
        else if (lower.endsWith(".rar") || lower.endsWith(".7z"))
            throw new FormatException("A " + lower.substring(lower.lastIndexOf('.')) + " archive can't be opened on the phone (the app has no "
                    + ".rar / .7z reader). Pick a .zip instead, or unpack it and pick its files (a folder's files).");
        else if (modelFile(fileName) && bytes.length <= MAX_MODEL) {
            files = new Files();
            files.models.put(leaf(fileName), bytes);
            files.origin.put(leaf(fileName), "");
        } else if (gxtFile(fileName) && bytes.length <= MAX_GXT) {
            files = new Files();
            files.gxt.put(leaf(fileName), bytes);
            files.origin.put(leaf(fileName), "");
        } else if (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".bmp")
                || lower.endsWith(".gif") || lower.endsWith(".webp") || lower.endsWith(".col")) {
            files = new Files(); // the screenshots / collision a mod folder carries: left out, as in a .zip (tools/mods.py)
            files.other.put(leaf(fileName), (long) bytes.length);
        } else if (bytes.length <= MAX_TEXT) {
            files = new Files();
            files.text.put(leaf(fileName), bytes);
        } else
            throw new FormatException("Not a .zip, a .vcm, a model or a text file.");
        return files;
    }

    static Parsed parse(Files files, String fileName) {
        Parsed p = new Parsed();
        p.files = files;
        p.title = fileName.replaceFirst("\\.[A-Za-z0-9]{1,4}$", "");
        for (Map.Entry<String, byte[]> f : files.text.entrySet()) {
            String name = f.getKey(), text = new String(f.getValue(), StandardCharsets.ISO_8859_1);
            if (name.equalsIgnoreCase("VCM.XML")) {
                readVcmScript(text, name, p);
                continue;
            }
            boolean stream = name.equalsIgnoreCase("vcs_stream.txt");
            java.util.List<int[]> palette = stream ? new ArrayList<>() : carcolsPalette(files);
            String section = null;
            for (String line : text.split("\r?\n|\r")) {
                String low = line.split("#", -1)[0].trim().toLowerCase(Locale.ROOT);
                if (low.equals("col") || low.equals("car") || low.equals("car4") || low.equals("end"))
                    section = low.equals("end") ? null : low;
                Entry e = stream ? parseStream(line, name) : parseHandling(line, name);
                if (e == null && !stream)
                    e = parseWeapon(line, name);
                if (e == null && !stream && !palette.isEmpty() && ("car".equals(section) || "car4".equals(section)))
                    e = parseCarcols(line, name, palette, "car4".equals(section));
                if (e != null && !duplicate(p, e))
                    p.entries.add(e);
                else if (e == null && !stream && carcols(line) && !"col".equals(section))
                    note(p, "Colours (" + line.trim() + "): not applied (its numbers are the PC game's palette and the "
                            + "mod has no colour list)");
            }
        }
        weaponSkill(p);
        // models: each .dff with the .txd of the same name (or the only .txd); weapons are converted on the phone
        // (src/game/vcs_modconv.cpp), vehicles not yet
        java.util.List<String> txds = new ArrayList<>();
        for (String n : files.models.keySet())
            if (n.toLowerCase(Locale.ROOT).endsWith(".txd"))
                txds.add(n);
        for (String n : files.models.keySet()) {
            String l = n.toLowerCase(Locale.ROOT);
            if (!l.endsWith(".dff"))
                continue;
            String base = n.substring(0, n.length() - 4);
            String txd = null;
            for (String t : txds)
                if (t.substring(0, t.length() - 4).equalsIgnoreCase(base))
                    txd = t;
            if (txd == null && txds.size() == 1)
                txd = txds.get(0);
            byte[] dff = files.models.get(n);
            if (dff.length < 12 || (dff[0] & 0xFF) != 0x10 || dff[1] != 0 || dff[2] != 0 || dff[3] != 0) {
                note(p, n + ": not a RenderWare model (no clump), not used");
                continue;
            }
            // a car (or bike, boat...): its frames carry VC's part names
            boolean car = contains(files.models.get(n), "chassis") || matchVehicle(base) >= 0;
            Entry e = new Entry("model", base, car ? "car" : "weapon", n + (txd != null ? " + " + txd : ""), n);
            e.fields.put("dff", n);
            if (txd != null)
                e.fields.put("txd", txd);
            else
                e.skipped.add("textures (no .txd)");
            if (car)
                e.skipped.add("the mod's wheel model (the slot's is kept, sized to the mod's), extras, collision");
            p.entries.add(e);
        }
        // [mods] a .gxt: the game's text file of that name, whole (replace disc=TEXT/<NAME>.GXT)
        for (String n : files.gxt.keySet()) {
            Entry e = new Entry("replace", leaf(n), "gxt", n, n);
            e.fields.put("disc", "TEXT/" + leaf(n).toUpperCase(Locale.ROOT));
            e.fields.put("file", n);
            e.target = 0;
            e.match = "name";
            p.entries.add(e);
        }
        for (Map.Entry<String, Long> f : files.other.entrySet()) {
            String n = f.getKey(), l = n.toLowerCase(Locale.ROOT);
            if (l.endsWith(".dff") || l.endsWith(".txd"))
                note(p, n + ": a model file too big or unreadable - not used");
            else if (l.endsWith(".col"))
                note(p, n + ": PC collision - not used (the slot's own collision stays)");
            else if (l.endsWith(".rar") || l.endsWith(".7z"))
                note(p, n + ": an archive inside the archive - unpack it and import its files");
        }
        for (Entry e : p.entries)
            autoTarget(e); // [mods] the game shown's slots (mod_slots.txt): name / alias / nearest equivalent
        return p;
    }

    /** The mod's carcols.dat colour list ("col" section, one r,g,b per line), empty if none (tools/mods.py) */
    static java.util.List<int[]> carcolsPalette(Files files) {
        java.util.regex.Pattern rgb = java.util.regex.Pattern.compile("(\\d{1,3})\\s*[,\\s]\\s*(\\d{1,3})\\s*[,\\s]\\s*(\\d{1,3})(\\s.*)?");
        for (byte[] raw : files.text.values()) {
            java.util.List<int[]> cols = new ArrayList<>();
            String section = null;
            for (String line : new String(raw, StandardCharsets.ISO_8859_1).split("\r?\n|\r")) {
                line = line.split("#", -1)[0].trim();
                if (line.isEmpty())
                    continue;
                String low = line.toLowerCase(Locale.ROOT);
                if (low.equals("col") || low.equals("car") || low.equals("car4") || low.equals("end")) {
                    section = low.equals("end") ? null : low;
                    continue;
                }
                java.util.regex.Matcher m = rgb.matcher(line);
                if ("col".equals(section) && m.matches())
                    cols.add(new int[] {Math.min(255, Integer.parseInt(m.group(1))), Math.min(255, Integer.parseInt(m.group(2))),
                            Math.min(255, Integer.parseInt(m.group(3)))});
            }
            if (cols.size() >= 2)
                return cols;
        }
        return new ArrayList<>();
    }

    /** A carcols.dat car line -> a "colours" entry, its pairs as RGB (SA car4: the first two of each four) */
    static Entry parseCarcols(String line, String source, java.util.List<int[]> palette, boolean four) {
        String[] t = line.split("#", -1)[0].trim().split("[\\s,]+");
        if (t.length < 3 || !t[0].matches("[A-Za-z][A-Za-z0-9_]{1,23}"))
            return null;
        int[] nums = new int[t.length - 1];
        for (int i = 1; i < t.length; i++) {
            if (!t[i].matches("\\d{1,3}"))
                return null;
            nums[i - 1] = Integer.parseInt(t[i]);
            if (nums[i - 1] >= palette.size())
                return null;
        }
        Entry e = new Entry("colours", t[0], "carcols", line.trim(), source);
        int step = four ? 4 : 2, n = 0;
        for (int i = 0; i + 1 < nums.length; i += step) {
            if (n == 8) {
                e.skipped.add("colour pairs after the 8th (the game keeps 8)");
                break;
            }
            int[] a = palette.get(nums[i]), b = palette.get(nums[i + 1]);
            e.fields.put("pair" + (++n), a[0] + "," + a[1] + "," + a[2] + "," + b[0] + "," + b[1] + "," + b[2]);
        }
        return n > 0 ? e : null;
    }

    private static boolean carcols(String line) {
        return line.trim().matches("[A-Za-z0-9_%]{2,24}\\s*,\\s*\\d{1,3}\\s*,\\s*\\d{1,3}(\\s*,.*)?");
    }

    private static void note(Parsed p, String n) {
        if (!p.notes.contains(n))
            p.notes.add(n);
    }

    /** The same values found twice (a .vcm's script and its readme): kept once, under the line's own name if one has it */
    private static boolean duplicate(Parsed p, Entry e) {
        for (int i = 0; i < p.entries.size(); i++) {
            Entry x = p.entries.get(i);
            if (x.kind.equals(e.kind) && x.fields.equals(e.fields)) {
                if (x.pcName.equals("VCMCAR") && !e.pcName.equals("VCMCAR"))
                    p.entries.set(i, e);
                return true;
            }
        }
        return false;
    }

    // <title>, and car_updatehandling's values (the handling line without its name: the car is the player's choice)
    private static void readVcmScript(String xml, String source, Parsed p) {
        java.util.regex.Matcher t = java.util.regex.Pattern.compile("<title>([^<]{1,80})</title>").matcher(xml);
        if (t.find())
            p.title = t.group(1).trim();
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("param2=\"([^\"]*)\"\\s*>\\s*car_updatehandling").matcher(xml);
        while (m.find()) {
            Entry e = parseHandling("VCMCAR " + m.group(1), source);
            if (e != null && !duplicate(p, e))
                p.entries.add(e);
        }
        java.util.regex.Matcher c = java.util.regex.Pattern
                .compile("param3=\"([^\"]*)\"\\s*>\\s*config_replaceline").matcher(xml);
        while (c.find())
            note(p, "Colours (" + c.group(1).replace("%%carfilename%%", "car").trim() + "): not applied yet (the PC and VCS colour palettes differ)");
    }

    // ---- what the game reads -------------------------------------------------------------------------------------------

    /** The active.txt line for one entry (vcs_mods.h), null without a target */
    static String directive(Entry e) {
        if (e.target < 0)
            return null;
        StringBuilder b = new StringBuilder();
        if (e.kind.equals("handling"))
            b.append("handling id=").append(e.target);
        else if (e.kind.equals("colours"))
            b.append("colours id=").append(e.target);
        else if (e.kind.equals("weapon"))
            b.append("weapon type=").append(e.target);
        else if (e.kind.equals("stream"))
            b.append("stream index=").append(e.target);
        else if (e.kind.equals("model"))
            b.append("model id=").append(e.target);
        else if (e.kind.equals("replace"))
            return "replace disc=" + e.fields.get("disc") + " file=" + e.fields.get("file");
        else if (e.kind.equals("sound")) // [lcs] an effect sound from an audio file (src/game/lcs_sfxmod.cpp)
            return "sound id=" + e.target + " file=" + e.fields.get("file");
        else if (e.kind.equals("anims")) // [lcs] an animation set made from the other Stories disc (tools/stories_pack.py)
            return "anims set=" + e.fields.get("set") + " file=" + e.fields.get("file");
        else if (e.kind.equals("hero")) // [stories] the other game's player model (tools/stories_hero.py, stories_hero.cpp)
            return "hero name=" + e.fields.get("name") + " mdl=" + e.fields.get("mdl") + " xtx=" + e.fields.get("xtx")
                    + (e.fields.containsKey("outfits") && !e.fields.get("outfits").isEmpty()
                            ? " outfits=" + e.fields.get("outfits") : "");
        else
            return null;
        if (e.kind.equals("stream") || e.kind.equals("model")) {
            for (Map.Entry<String, String> f : e.fields.entrySet())
                b.append(' ').append(f.getKey()).append('=').append(f.getValue());
        } else
            for (Map.Entry<String, String> f : e.fields.entrySet())
                b.append(' ').append(f.getKey()).append(' ').append(f.getValue());
        return b.toString();
    }

    /** The target's name for the menu */
    static String targetName(Entry e) {
        if (e.kind.equals("replace"))
            return e.fields.containsKey("disc") ? e.fields.get("disc") : "a disc file";
        if (e.kind.equals("stream"))
            return "streaming entry " + e.target;
        if (e.kind.equals("sound"))
            return "sound " + e.target;
        if (e.kind.equals("anims")) // [lcs] tools/mods.py target_name
            return "the " + (e.fields.containsKey("set") ? e.fields.get("set") : "?") + " animations";
        if (e.kind.equals("hero"))
            return "the player (" + (e.fields.containsKey("name") ? e.fields.get("name") : "?") + ")";
        if (e.target < 0)
            return "no slot chosen";
        Slot s = targetSlot(e);
        if (s != null)
            return s.label;
        return (vehicleEntry(e) || e.kind.equals("model") ? "model " : "weapon ") + e.target;
    }

    // ---- [mods] where the game reads a mod's file (src/game/vcs_modfiles.h; the same as tools/mods.py file_value) -----

    /** A path as one active.txt token: space, %, ! and non-ASCII bytes as %XX */
    static String enc(String text) {
        StringBuilder b = new StringBuilder();
        for (byte x : text.getBytes(StandardCharsets.UTF_8)) {
            int c = x & 0xFF;
            if (c > 0x20 && c < 0x7F && c != '%' && c != '!')
                b.append((char) c);
            else
                b.append(String.format(Locale.ROOT, "%%%02X", c));
        }
        return b.toString();
    }

    /** "Keep where it is" on Android: the game's activity opens the mod's source <index> as this handle */
    static String place(String modId, int sourceIndex) {
        return "place:" + modId + "/" + sourceIndex;
    }

    /**
     * The value for one model / text file: from its source (sourceName / sourceIndex, entry "" = the whole file) -
     * the app's copy, mods/<id>/source/<name>, or the original where it is (inPlace) - else mods/<id>/files/<name>
     * (taken out of a .vcm inside a zip, or a mod imported before)
     */
    static String fileValue(String modId, boolean inPlace, String sourceName, int sourceIndex, String entry, String fileName) {
        if (sourceName == null)
            return modId + "/files/" + fileName;
        String base = inPlace ? place(modId, sourceIndex) : enc(modId + "/source/" + sourceName);
        return base + (entry == null || entry.isEmpty() ? "" : "!" + enc(entry));
    }
}
