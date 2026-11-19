// [android] The launcher's Mods category (docs/ANDROID.md "Mods"): the mod manager. Import a mod (the system's file
// picker: one file, or several picked together - a .dff with its .txd), turn mods on and off, see what each line or
// model changes (which vehicle / weapon slot, how it was matched, the new values and the stock ones the game
// reported), settle conflicts (two mods on one slot), choose the slot, remove a mod. ModFormats reads the files and
// matches names to the shown game's slots (tools/modslots/mod_slots.txt, the APK's assets; the desktop's
// tools/mods.py reads the same table and writes the same storage). This class keeps each mod in the game's data folder,
// mods/<id>/ (manifest.json, source/ = an imported mod's one copy of every picked file, read by the game there - inside
// a zip or .vcm too -, files/ = only what can't be read from source/ (a .vcm inside a zip, older imports), converted/ =
// the game's converted-model cache), and writes mods/active.txt, the list the game's loader (src/game/vcs_mods.cpp)
// applies at its next start. The game writes back mods/applied.txt.
//
// [mods] Two ways to add a mod, like the disc image: "Import" (the copy above: the original may be deleted) or "Keep
// where it is" (manifest "storage": "in_place"): no copy, the picked documents stay where they are and the app keeps a
// persisted read permission for each (its "uri" in the manifest's sources). mods/places.txt lists them; VCSActivity
// opens them at the game's start and hands the fds over (VCS_MODS_PLACES, vcs_modfiles.h). A source that can't be
// reached any more shows "original missing", is skipped by the game ("needs" lines) and offers Find it again /
// Import a copy instead; either kind can be switched to the other later.
//
// Mods only change values of existing vehicle / weapon slots in memory: nothing is written to the disc or to a save,
// so turning a mod off (or removing it) gives the stock game back at the next start.
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ModsMenu {
    // request codes, apart from LauncherActivity's (1-4) and SavesMenu's (20-22): import (a copy), keep where it is,
    // find a mod's original again (kept where it is / as a copy)
    static final int PICK_MOD = 30, PICK_MOD_PLACE = 31, FIND_ORIGINAL = 32, FIND_AS_COPY = 33;
    private Mod finding; // FIND_*: the mod whose original is being picked
    private static final int MAX_PICKED = 128 << 20;
    private static final int ROWS = 32; // mods listed (rows exist up front; the unused ones are hidden)

    private final Activity activity;
    private final Runnable refresh;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final List<Mod> mods = new ArrayList<>();
    private boolean busy;
    private String note;

    /** One imported mod: its manifest, and the entries ModFormats found */
    static final class Mod {
        String id, name, source;
        long imported;
        boolean enabled = true;
        final List<ModFormats.Entry> entries = new ArrayList<>();
        final List<String> notes = new ArrayList<>();
        JSONArray sources = new JSONArray(), files = new JSONArray(); // name, size, sha256 of each kept file
        boolean inPlace;    // [mods] "Keep where it is": sources[i].uri read where it is, no copy in the app
        String original = "ok"; // [mods] in place: ok / missing (no access, gone) / changed (another size)

        int usable() {
            int n = 0;
            for (ModFormats.Entry e : entries)
                if (e.target >= 0)
                    n++;
            return n;
        }
    }

    ModsMenu(Activity activity, Runnable refresh) {
        this.activity = activity;
        this.refresh = refresh;
        // the slot tables (tools/modslots/mod_slots.txt, packed by build.gradle)
        try (InputStream in = activity.getAssets().open("mod_slots.txt")) {
            ModFormats.loadSlots(new String(readAll(in), StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException e) {
            Log.w("vcs", "[mods] mod_slots.txt", e);
        }
        follow();
    }

    // [lcs] the chosen game's own data folder: files/userdata/mods (VCS), files/userdata-lcs/mods (LCS: the same
    // loader with its own tables, vcs_mods.cpp); the list is read again when the launcher's game switch changes it
    private String loadedFor;

    private File dir() {
        return new File(GameFiles.dataDir(activity), "mods");
    }

    private long checkedAt;

    private void follow() {
        String g = GameFiles.game(activity);
        if (!g.equals(loadedFor)) {
            loadedFor = g;
            ModFormats.useGame(g);
            load();
        } else if (System.currentTimeMillis() - checkedAt > 3000 && !busy) {
            // [mods] the originals kept where they are, again now and then (a file moved in the file manager...); the
            // game checks them itself at its start (active.txt "needs" lines)
            checkOriginals();
        }
    }

    private boolean loaderOn() {
        return !new File(dir(), "loader_off").exists();
    }

    private String gameTitle() {
        return DiscCheck.LCS.id.equals(GameFiles.game(activity)) ? "Liberty City Stories" : "Vice City Stories";
    }

    void addTo(MenuView menu) {
        MenuView.Category c = menu.category("Mods", MenuView.G_SETTINGS);
        c.add(new MenuView.Item("Import a mod…")
                .visible(() -> {
                    follow(); // [lcs] the chosen game's mods (rows are read in refresh(), first this one)
                    return true;
                })
                .value(() -> note != null ? note : "A .zip, a .vcm, handling.cfg / weapon.dat, or a .dff with its .txd (pick both)")
                .help("Two ways: Import (the app keeps a copy; you can delete the original), or Keep where it is (the "
                        + "app only reads it there and keeps no copy; don't move or delete it). You choose each time.")
                .enabled(() -> !busy)
                .action(this::pick));
        c.add(new MenuView.Item("Mod loader")
                .choice(new String[] {"Off: the stock game", "On"}, () -> loaderOn() ? 1 : 0, i -> {
                    File off = new File(dir(), "loader_off");
                    try {
                        dir().mkdirs();
                        if (i == 0)
                            new FileOutputStream(off).close();
                        else
                            off.delete();
                    } catch (IOException e) {
                        Log.w("vcs", "[mods] loader switch", e);
                    }
                    writeActive();
                    refresh.run();
                }));
        for (int n = 0; n < ROWS; n++) {
            final int i = n;
            c.add(new MenuView.Item("Mod " + (n + 1))
                    .visible(() -> i < mods.size())
                    .value(() -> i < mods.size() ? rowText(mods.get(i)) : "")
                    .enabled(() -> !busy)
                    .action(() -> {
                        if (i < mods.size())
                            modDialog(mods.get(i));
                    }));
        }
        c.add(new MenuView.Item("What is modified")
                .visible(() -> !mods.isEmpty())
                .value(this::overviewLine)
                .action(this::overview));
        c.add(new MenuView.Item("At the last start")
                .visible(() -> new File(dir(), "applied.txt").isFile() || !mods.isEmpty())
                .value(this::lastStart)
                .action(this::showApplied));
        // [mods3] LCS's sound mods: the music folder next to mods/ (lcs_usertracks.cpp), what converts (docs/LCS.md)
        c.add(new MenuView.Item("Your music")
                .visible(() -> DiscCheck.LCS.id.equals(GameFiles.game(activity)))
                .value(() -> "Copy .mp3 / .ogg / .flac / .wav files into files/userdata-lcs/music")
                .help("They play as a radio station: in the game's side panel, Audio > User tracks chooses the station "
                        + "they replace (LCFR by default); its DJ and adverts go too. Nothing changes while the folder is "
                        + "empty. Replacing a weapon's or a car's sound with a .wav is done on the PC for now "
                        + "(tools/mods.py sound)."));
        c.add(new MenuView.Item("What converts")
                .value(() -> "Handling, weapon lines, car and bike parts, paint, own wheels; not extras")
                .help("Converted: every handling.cfg value with a match here (mass to lights, the flags bit by bit; "
                        + "GTA III, VC and SA lines each in their own columns), weapon.dat range, rate, clip, damage and "
                        + "muzzle position (SA's std skill line), a car's body, doors, bonnet, boot, bumpers, windscreen and "
                        + "damaged parts, its paint, dummies and wheel size, the mod's own wheels (LCS: on that car only), "
                        + "a bike's forks, handlebars and wheels. Not converted: extras, collision, carcols numbers without "
                        + "a colour list, SA's other skill levels and suspension limits, animations, PC plugins. .rar / .7z: "
                        + "unpack on a PC. Each entry lists what it skipped."));
    }

    private String rowText(Mod m) {
        StringBuilder b = new StringBuilder(m.name).append(" · ").append(m.enabled ? "On" : "Off");
        if (m.original.equals("missing"))
            b.append(" · original missing (skipped)");
        else if (m.original.equals("changed"))
            b.append(" · original changed");
        else if (m.inPlace)
            b.append(" · kept where it is");
        List<String> targets = new ArrayList<>();
        for (ModFormats.Entry e : m.entries)
            targets.add(kindWord(e) + " " + ModFormats.targetName(e) + (e.off ? " (set aside)" : ""));
        if (!targets.isEmpty())
            b.append(" · ").append(String.join(", ", targets));
        if (m.usable() < m.entries.size())
            b.append(" · choose a slot");
        for (ModFormats.Entry e : m.entries)
            if (m.enabled && conflict(e)) {
                b.append(" · conflict");
                break;
            }
        return b.toString();
    }

    /** What the line changes, in the player's words: behaviour (handling / weapon.dat) or looks (models) */
    private static String kindWord(ModFormats.Entry e) {
        switch (e.kind) {
            case "handling": return "drives:";
            case "weapon": return "fires:";
            case "model": return "looks:";
            case "colours": return "paint:";
            case "replace": return "file:";
            case "sound": return "sounds:";
            case "anims": return "moves:";
            case "hero": return "player:";
            default: return "data:";
        }
    }

    private static String changes(ModFormats.Entry e) {
        switch (e.kind) {
            case "handling": return "changes how it drives (a handling.cfg line)";
            case "weapon": return "changes how it fires (a weapon.dat line)";
            case "model": return "changes how it looks (a .dff model with its .txd textures)";
            case "colours": return "changes its paint colours (a carcols.dat line)";
            case "replace": return "replaces a whole game file";
            case "sound": return "changes how it sounds (an effect sound from an audio file; LCS)";
            case "anims": return "changes how he moves (an animation set made from your other Stories disc; LCS)";
            case "hero": return "changes who the player is (the other Stories game's hero, made from your disc)";
            default: return "model data";
        }
    }

    private String lastStart() {
        File f = new File(dir(), "applied.txt");
        if (!f.isFile())
            return loaderOn() ? "Mods apply when the game starts" : "The loader is off";
        try {
            List<String> lines = Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
            String last = lines.isEmpty() ? "" : lines.get(lines.size() - 1);
            return last + " · " + android.text.format.DateFormat.format("d MMM, HH:mm", f.lastModified());
        } catch (IOException e) {
            return "";
        }
    }

    private void showApplied() {
        File f = new File(dir(), "applied.txt");
        String text;
        try {
            text = f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8)
                    : "The game has not started with mods yet.";
        } catch (IOException e) {
            text = e.getMessage();
        }
        message("What the game changed at its last start", text);
    }

    // ---- conflicts: two mods' entries on one slot ---------------------------------------------------------------------

    /** The slot an entry occupies: a car's handling and its model are different slots (one drives, one looks) */
    private static String slotKey(ModFormats.Entry e) {
        if (e.target < 0)
            return null;
        if (e.kind.equals("handling") || e.kind.equals("weapon") || e.kind.equals("model"))
            return (ModFormats.vehicleEntry(e) ? "vehicle " : "weapon ") + (e.kind.equals("model") ? "looks " : "behaviour ")
                    + e.target;
        if (e.kind.equals("colours"))
            return "vehicle paint " + e.target;
        return e.kind + " " + (e.fields.containsKey("disc") ? e.fields.get("disc") : String.valueOf(e.target));
    }

    /** The mods (in load order) with an entry on this slot that applies */
    private List<Mod> activeOn(String key) {
        List<Mod> out = new ArrayList<>();
        if (key == null)
            return out;
        for (Mod m : mods)
            if (m.enabled && !m.original.equals("missing")) // (an original missing: the game skips the mod)
                for (ModFormats.Entry x : m.entries)
                    if (!x.off && key.equals(slotKey(x)) && !out.contains(m))
                        out.add(m);
        return out;
    }

    private boolean conflict(ModFormats.Entry e) {
        return !e.off && activeOn(slotKey(e)).size() > 1;
    }

    /** The player chose: this entry has its slot, the other mods' entries on it are set aside */
    private void prefer(Mod winner, ModFormats.Entry e) {
        String key = slotKey(e);
        e.off = false;
        for (Mod m : mods) {
            for (ModFormats.Entry x : m.entries)
                if (x != e && key != null && key.equals(slotKey(x)))
                    x.off = true;
            try {
                save(m);
            } catch (IOException ex) {
                note = "✗ " + ex.getMessage();
            }
        }
        writeActive();
        note = "✓ " + ModFormats.targetName(e) + ": " + winner.name + " has it now";
        refresh.run();
    }

    private String overviewLine() {
        int slots = 0, conflicts = 0;
        List<String> seen = new ArrayList<>();
        for (Mod m : mods)
            if (m.enabled)
                for (ModFormats.Entry e : m.entries) {
                    String k = slotKey(e);
                    if (k == null || e.off || seen.contains(k))
                        continue;
                    seen.add(k);
                    slots++;
                    if (activeOn(k).size() > 1)
                        conflicts++;
                }
        return slots + (slots == 1 ? " slot changed" : " slots changed")
                + (conflicts > 0 ? " · " + conflicts + (conflicts == 1 ? " conflict: choose which mod wins" : " conflicts: choose which mod wins") : "");
    }

    /** Every slot the mods change, overall; a conflict's row opens the choice of the winner */
    private void overview() {
        Map<String, List<Object[]>> bySlot = new LinkedHashMap<>(); // slot -> {Mod, Entry}
        for (Mod m : mods)
            for (ModFormats.Entry e : m.entries) {
                String k = slotKey(e);
                if (k != null && m.enabled)
                    bySlot.computeIfAbsent(k, x -> new ArrayList<>()).add(new Object[] {m, e});
            }
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        for (Map.Entry<String, List<Object[]>> s : bySlot.entrySet()) {
            List<Object[]> by = s.getValue();
            ModFormats.Entry first = (ModFormats.Entry) by.get(0)[1];
            List<String> who = new ArrayList<>();
            Mod winner = null;
            for (Object[] o : by) {
                ModFormats.Entry e = (ModFormats.Entry) o[1];
                who.add(((Mod) o[0]).name + (e.off ? " (set aside)" : ""));
                if (!e.off)
                    winner = (Mod) o[0]; // the last one loaded wins
            }
            boolean conflict = activeOn(s.getKey()).size() > 1;
            labels.add(kindWord(first) + " " + ModFormats.targetName(first) + " ← " + String.join(", ", who)
                    + (conflict ? "  ⚠ " + winner.name + " wins (loaded later): tap to choose"
                    : by.size() > 1 && winner != null ? "  · " + winner.name + " has it" : ""));
            actions.add(by.size() > 1 ? () -> chooseWinner(by) : null);
        }
        if (labels.isEmpty()) {
            message("What is modified", "Nothing yet: no mod that is on has a slot.");
            return;
        }
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("What is modified · " + gameTitle())
                .setItems(labels.toArray(new String[0]), (d, which) -> {
                    if (actions.get(which) != null)
                        actions.get(which).run();
                })
                .setNegativeButton("Close", null)
                .show());
    }

    private void chooseWinner(List<Object[]> by) {
        String[] names = new String[by.size()];
        int current = -1;
        for (int i = 0; i < by.size(); i++) {
            Mod m = (Mod) by.get(i)[0];
            ModFormats.Entry e = (ModFormats.Entry) by.get(i)[1];
            names[i] = m.name + " (" + e.pcName + ")";
            if (!e.off)
                current = i;
        }
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Which mod gets " + ModFormats.targetName((ModFormats.Entry) by.get(0)[1]) + "?")
                .setSingleChoiceItems(names, current, (d, which) -> {
                    d.dismiss();
                    prefer((Mod) by.get(which)[0], (ModFormats.Entry) by.get(which)[1]);
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    // ---- storage -----------------------------------------------------------------------------------------------------

    private void load() {
        mods.clear();
        File[] dirs = dir().listFiles(File::isDirectory);
        if (dirs != null)
            for (File d : dirs) {
                try {
                    mods.add(fromJson(new JSONObject(new String(Files.readAllBytes(new File(d, "manifest.json").toPath()),
                            StandardCharsets.UTF_8))));
                } catch (IOException | JSONException e) {
                    Log.w("vcs", "[mods] skipping " + d.getName() + ": " + e.getMessage());
                }
            }
        Collections.sort(mods, (a, b) -> Long.compare(a.imported, b.imported)); // load order: first imported first
        boolean migrated = false;
        for (Mod m : mods)
            migrated |= migrate(m);
        if (migrated)
            writeActive();
        for (Mod m : mods)
            dropDerived(m); // (after active.txt: it never names a deleted file)
        checkOriginals();
    }

    // ---- [mods] where each file is: the app's copy, or the original kept where it is ----------------------------------

    /** The originals of the mods kept where they are: still reachable (the persisted permission, the document)? */
    private void checkOriginals() {
        checkedAt = System.currentTimeMillis();
        java.util.Set<String> granted = new java.util.HashSet<>();
        for (android.content.UriPermission p : activity.getContentResolver().getPersistedUriPermissions())
            if (p.isReadPermission())
                granted.add(p.getUri().toString());
        for (Mod m : mods) {
            if (!m.inPlace) {
                m.original = "ok";
                continue;
            }
            String state = "ok";
            for (int i = 0; i < m.sources.length() && !state.equals("missing"); i++) {
                JSONObject s = m.sources.optJSONObject(i);
                String uri = s != null ? s.optString("uri", "") : "";
                if (uri.isEmpty() || !granted.contains(uri)) {
                    state = "missing";
                    continue;
                }
                long size = querySize(Uri.parse(uri));
                if (size == -2)
                    state = "missing";
                else if (size >= 0 && size != s.optLong("size", size))
                    state = "changed";
            }
            m.original = state;
        }
    }

    /** The document's size: -1 unknown, -2 not there / no access */
    private long querySize(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c == null || !c.moveToFirst())
                return -2;
            return c.isNull(0) ? -1 : c.getLong(0);
        } catch (Exception e) {
            return -2;
        }
    }

    /** An imported mod from before: source/ and files/ held the same bytes twice; the game reads them from source/ now
     *  (the same bytes: its converted-model cache stays valid) and files/ keeps only what can't be read there */
    private boolean migrate(Mod m) {
        if (m.inPlace)
            return false;
        boolean open = false;
        for (int i = 0; i < m.files.length(); i++) {
            JSONObject f = m.files.optJSONObject(i);
            open |= f != null && !f.has("from");
        }
        if (!open)
            return false;
        boolean changed = false;
        File src = new File(new File(dir(), m.id), "source");
        for (int i = 0; i < m.sources.length(); i++) {
            JSONObject s = m.sources.optJSONObject(i);
            File f = s != null ? new File(src, s.optString("name")) : null;
            if (f == null || !f.isFile() || f.length() > MAX_PICKED)
                continue;
            try {
                changed |= deriveFrom(m, s.optString("name"), Files.readAllBytes(f.toPath()));
            } catch (IOException | JSONException | ModFormats.FormatException | RuntimeException e) {
                Log.w("vcs", "[mods] " + m.id + ": " + e.getMessage());
            }
        }
        if (changed)
            try {
                save(m);
            } catch (IOException e) {
                return false;
            }
        return changed;
    }

    /** files[] entries whose bytes are this source's (the file itself, or its zip / .vcm entry): "from" it */
    private static boolean deriveFrom(Mod m, String sourceName, byte[] data) throws IOException, JSONException, ModFormats.FormatException {
        ModFormats.Files one = ModFormats.files(sourceName, data);
        boolean changed = false;
        for (Map.Entry<String, String> o : one.origin.entrySet()) {
            byte[] blob = one.models.containsKey(o.getKey()) ? one.models.get(o.getKey()) : one.gxt.get(o.getKey());
            if (blob == null)
                continue;
            String hash = sha256(blob);
            for (int i = 0; i < m.files.length(); i++) {
                JSONObject f = m.files.getJSONObject(i);
                if (f.optString("name").equals(safeName(o.getKey())) && !f.has("from") && hash.equals(f.optString("sha256"))) {
                    JSONObject from = new JSONObject();
                    from.put("source", sourceName);
                    from.put("entry", o.getValue());
                    f.put("from", from);
                    changed = true;
                }
            }
        }
        return changed;
    }

    /** files/<name> of every file the game reads from its source now */
    private void dropDerived(Mod m) {
        File d = new File(new File(dir(), m.id), "files");
        for (int i = 0; i < m.files.length(); i++) {
            JSONObject f = m.files.optJSONObject(i);
            if (f != null && f.has("from"))
                new File(d, f.optString("name")).delete();
        }
        String[] left = d.list();
        if (left != null && left.length == 0)
            d.delete();
    }

    /** active.txt's value for one of the mod's model / text files (ModFormats.fileValue) */
    private static String fileValue(Mod m, String name) {
        String safe = safeName(name);
        for (int i = 0; i < m.files.length(); i++) {
            JSONObject f = m.files.optJSONObject(i);
            JSONObject from = f != null && f.optString("name").equals(safe) ? f.optJSONObject("from") : null;
            if (from == null)
                continue;
            for (int k = 0; k < m.sources.length(); k++) {
                JSONObject s = m.sources.optJSONObject(k);
                if (s != null && s.optString("name").equals(from.optString("source")))
                    return ModFormats.fileValue(m.id, m.inPlace, s.optString("name"), k, from.optString("entry"), safe);
            }
        }
        return ModFormats.fileValue(m.id, false, null, -1, null, safe);
    }

    private static Mod fromJson(JSONObject o) throws JSONException {
        Mod m = new Mod();
        m.id = o.getString("id");
        m.name = o.getString("name");
        m.source = o.optString("source");
        m.imported = o.optLong("imported");
        m.enabled = o.optBoolean("enabled", true);
        m.inPlace = "in_place".equals(o.optString("storage")); // none: an import (a copy), as before
        JSONArray es = o.getJSONArray("entries");
        for (int i = 0; i < es.length(); i++) {
            JSONObject e = es.getJSONObject(i);
            ModFormats.Entry x = new ModFormats.Entry(e.getString("kind"), e.optString("pcName"), e.optString("dialect"),
                    e.optString("line"), e.optString("source"));
            x.target = e.optInt("target", -1);
            x.match = e.optString("match", x.target >= 0 ? "picked" : "none");
            x.off = e.optBoolean("off", false);
            JSONObject f = e.getJSONObject("fields");
            for (java.util.Iterator<String> k = f.keys(); k.hasNext(); ) {
                String key = k.next();
                x.fields.put(key, f.getString(key));
            }
            JSONArray s = e.optJSONArray("skipped");
            for (int k = 0; s != null && k < s.length(); k++)
                x.skipped.add(s.getString(k));
            m.entries.add(x);
        }
        JSONArray ns = o.optJSONArray("notes");
        for (int i = 0; ns != null && i < ns.length(); i++)
            m.notes.add(ns.getString(i));
        if (o.optJSONArray("sources") != null)
            m.sources = o.getJSONArray("sources");
        if (o.optJSONArray("files") != null)
            m.files = o.getJSONArray("files");
        return m;
    }

    // the same manifest as tools/mods.py writes (loader 2: match, off, sources, files)
    private JSONObject toJson(Mod m) throws JSONException {
        JSONObject o = new JSONObject();
        o.put("loader", 2);
        o.put("game", loadedFor);
        o.put("id", m.id);
        o.put("name", m.name);
        o.put("source", m.source);
        o.put("imported", m.imported);
        o.put("enabled", m.enabled);
        o.put("storage", m.inPlace ? "in_place" : "copy"); // [mods] sources[].uri: where an in-place mod is read
        JSONArray es = new JSONArray();
        for (ModFormats.Entry e : m.entries) {
            JSONObject x = new JSONObject();
            x.put("kind", e.kind);
            x.put("pcName", e.pcName);
            x.put("dialect", e.dialect);
            x.put("line", e.line);
            x.put("source", e.source);
            x.put("target", e.target);
            x.put("targetName", ModFormats.targetName(e));
            x.put("match", e.match);
            x.put("off", e.off);
            JSONObject f = new JSONObject();
            for (Map.Entry<String, String> kv : e.fields.entrySet())
                f.put(kv.getKey(), kv.getValue());
            x.put("fields", f);
            x.put("skipped", new JSONArray(e.skipped));
            es.put(x);
        }
        o.put("entries", es);
        o.put("notes", new JSONArray(m.notes));
        o.put("sources", m.sources);
        o.put("files", m.files);
        return o;
    }

    private void save(Mod m) throws IOException {
        File d = new File(dir(), m.id);
        d.mkdirs();
        File tmp = new File(d, "manifest.json.part");
        try {
            Files.write(tmp.toPath(), toJson(m).toString(2).getBytes(StandardCharsets.UTF_8));
        } catch (JSONException e) {
            throw new IOException(e.getMessage());
        }
        if (!tmp.renameTo(new File(d, "manifest.json")))
            throw new IOException("can't write the manifest");
    }

    /** mods/active.txt: the enabled mods' lines in load order (entries set aside left out); no file while off */
    private void writeActive() {
        File active = new File(dir(), "active.txt");
        if (!loaderOn()) {
            active.delete();
            new File(dir(), "places.txt").delete();
            return;
        }
        StringBuilder b = new StringBuilder("# written by the mod manager (the launcher's Mods); the game reads it at start (src/game/vcs_mods.h)\n");
        // [mods] mods/places.txt: "<handle> <uri>" of every original kept where it is, for VCSActivity to open at the
        // game's start (the handle is the place:<handle> of active.txt)
        StringBuilder places = new StringBuilder();
        int lines = 0;
        for (Mod m : mods) {
            if (!m.enabled)
                continue;
            StringBuilder part = new StringBuilder();
            for (ModFormats.Entry e : m.entries) {
                String d = e.off ? null : ModFormats.directive(e);
                if (d == null)
                    continue;
                if (e.kind.equals("stream") && e.fields.containsKey("file"))
                    d = d.replace("file=" + e.fields.get("file"), "file=" + m.id + "/files/" + e.fields.get("file"));
                if (e.kind.equals("replace") && e.fields.containsKey("file"))
                    d = d.replace("file=" + e.fields.get("file"), "file=" + fileValue(m, e.fields.get("file")));
                if (e.kind.equals("sound") || e.kind.equals("anims")) // [lcs] files/ of the mod
                    d = d.replace("file=" + e.fields.get("file"), "file=" + fileValue(m, e.fields.get("file")));
                if (e.kind.equals("hero")) // [stories]
                    for (String k : new String[] {"mdl", "xtx", "outfits"})
                        if (e.fields.containsKey(k) && !e.fields.get(k).isEmpty())
                            d = d.replace(k + "=" + e.fields.get(k), k + "=" + fileValue(m, e.fields.get(k)));
                if (e.kind.equals("model"))
                    for (String k : new String[] {"dff", "txd"})
                        if (e.fields.containsKey(k))
                            d = d.replace(k + "=" + e.fields.get(k), k + "=" + fileValue(m, e.fields.get(k)));
                part.append(d).append('\n');
                lines++;
            }
            if (part.length() > 0 && m.inPlace)
                for (int i = 0; i < m.sources.length(); i++) {
                    // the game skips the whole mod when one of its originals can't be opened
                    String handle = ModFormats.place(m.id, i);
                    part.insert(0, "needs file=" + handle + "\n");
                    JSONObject s = m.sources.optJSONObject(i);
                    places.append(handle.substring("place:".length())).append(' ')
                            .append(s != null ? s.optString("uri") : "").append('\n');
                }
            if (part.length() > 0)
                b.append("mod ").append(m.id).append(' ').append(m.name.replaceAll("[\\r\\n]", " ")).append('\n').append(part);
        }
        try {
            dir().mkdirs();
            File placesFile = new File(dir(), "places.txt");
            if (places.length() == 0 || lines == 0)
                placesFile.delete();
            else {
                File tmp = new File(dir(), "places.txt.part");
                Files.write(tmp.toPath(), places.toString().getBytes(StandardCharsets.UTF_8));
                if (!tmp.renameTo(placesFile))
                    throw new IOException("rename");
            }
            if (lines == 0) {
                active.delete();
                return;
            }
            File tmp = new File(dir(), "active.txt.part");
            Files.write(tmp.toPath(), b.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(active))
                throw new IOException("rename");
        } catch (IOException e) {
            Log.w("vcs", "[mods] writing active.txt", e);
            note = "✗ Can't write the mod list: " + e.getMessage();
        }
    }

    private void changed(Mod m) {
        try {
            save(m);
        } catch (IOException e) {
            note = "✗ " + e.getMessage();
        }
        writeActive();
        refresh.run();
    }

    // ---- import ------------------------------------------------------------------------------------------------------

    // [mods] the two ways, said plainly each time (like the disc: read where it is; unlike the code pack, which must be
    // a copy: Android runs code only from the app's own storage)
    static final String IMPORT_CHOICE = "Import (the app keeps a copy; you can delete the original)";
    static final String PLACE_CHOICE = "Keep where it is (the app only reads it; don't move or delete it)";

    private void pick() {
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Add a mod")
                .setItems(new String[] {IMPORT_CHOICE, PLACE_CHOICE}, (d, which) -> openPicker(which == 1 ? PICK_MOD_PLACE : PICK_MOD))
                .setNegativeButton("Cancel", null)
                .show());
    }

    /** The system's file picker: several files at once (a .dff and its .txd: one mod); kept where it is: a permission
     *  that lasts (takePersistableUriPermission in onResult) */
    private void openPicker(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*"); // .vcm / .cfg / .dat have no MIME types
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        if (request == PICK_MOD_PLACE || request == FIND_ORIGINAL)
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        activity.startActivityForResult(i, request);
    }

    /** LauncherActivity.onActivityResult: true when the request was ours */
    boolean onResult(int request, int result, Intent data) {
        if (request != PICK_MOD && request != PICK_MOD_PLACE && request != FIND_ORIGINAL && request != FIND_AS_COPY)
            return false;
        Mod target = finding;
        finding = null;
        if (result == Activity.RESULT_OK && data != null) {
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null)
                for (int i = 0; i < clip.getItemCount(); i++)
                    if (clip.getItemAt(i).getUri() != null)
                        uris.add(clip.getItemAt(i).getUri());
            if (uris.isEmpty() && data.getData() != null)
                uris.add(data.getData());
            boolean keep = request == PICK_MOD_PLACE || request == FIND_ORIGINAL;
            if (keep)
                for (Uri u : uris)
                    try {
                        activity.getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (SecurityException e) {
                        Log.w("vcs", "[mods] no lasting permission for " + u + ": " + e.getMessage());
                    }
            if (!uris.isEmpty()) {
                if (request == FIND_ORIGINAL || request == FIND_AS_COPY) {
                    if (target != null)
                        relocate(target, uris, request == FIND_AS_COPY);
                } else
                    importMod(uris, keep);
            }
        }
        return true;
    }

    private void release(Uri u) {
        // a permission still used by another mod (the same file twice) or by the disc is kept
        for (Mod o : mods)
            for (int i = 0; i < o.sources.length(); i++) {
                JSONObject s = o.sources.optJSONObject(i);
                if (o.inPlace && s != null && u.toString().equals(s.optString("uri")))
                    return;
            }
        if (u.equals(GameFiles.disc(activity)))
            return;
        try {
            activity.getContentResolver().releasePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null)
                return c.getString(0);
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s : "mod";
    }

    private static String sha256(byte[] b) {
        try {
            StringBuilder s = new StringBuilder();
            for (byte x : MessageDigest.getInstance("SHA-256").digest(b))
                s.append(String.format(Locale.ROOT, "%02x", x));
            return s.toString();
        } catch (NoSuchAlgorithmException e) {
            return "";
        }
    }

    private static JSONObject fileInfo(String name, byte[] b) throws JSONException {
        JSONObject o = new JSONObject();
        o.put("name", name);
        o.put("size", b.length);
        o.put("sha256", sha256(b));
        return o;
    }

    private List<byte[]> readPicked(List<Uri> uris, List<String> names) throws IOException, ModFormats.FormatException {
        List<byte[]> data = new ArrayList<>();
        long total = 0;
        for (int i = 0; i < uris.size(); i++)
            try (InputStream in = activity.getContentResolver().openInputStream(uris.get(i))) {
                if (in == null)
                    throw new IOException("no data");
                byte[] bytes = readAll(in);
                if (bytes == null || (total += bytes.length) > MAX_PICKED)
                    throw new ModFormats.FormatException(names.get(i) + ": over " + (MAX_PICKED >> 20) + " MB.");
                data.add(bytes);
            }
        return data;
    }

    private void importMod(List<Uri> uris, boolean keepInPlace) {
        List<String> names = new ArrayList<>();
        for (Uri u : uris)
            names.add(displayName(u));
        String fileName = String.join(", ", names);
        busy = true;
        note = "Reading " + fileName + "…";
        refresh.run();
        new Thread(() -> {
            Mod mod = null;
            String error = null;
            try {
                List<byte[]> data = readPicked(uris, names);
                ModFormats.Parsed p = uris.size() == 1 ? ModFormats.read(names.get(0), data.get(0))
                        : ModFormats.readAll(names.get(0).replaceFirst("\\.[A-Za-z0-9]{1,4}$", ""), names, data);
                if (p.entries.isEmpty())
                    error = "Nothing in " + fileName + " that the game can use: no handling.cfg or weapon.dat line, no .dff "
                            + "model with its .txd, no .gxt text." + (p.notes.isEmpty() ? "" : "\n\n• " + String.join("\n• ", p.notes));
                else {
                    mod = new Mod();
                    mod.name = p.title;
                    mod.source = fileName;
                    mod.imported = System.currentTimeMillis();
                    mod.id = idFor(p.title);
                    mod.inPlace = keepInPlace;
                    mod.entries.addAll(p.entries);
                    mod.notes.addAll(p.notes);
                    File folder = new File(dir(), mod.id);
                    folder.mkdirs();
                    File src = new File(folder, "source");
                    List<String> keys = new ArrayList<>();
                    for (int i = 0; i < names.size(); i++) {
                        String n = safeName(names.get(i));
                        while (keys.contains(n))
                            n = "_" + n;
                        keys.add(n);
                        JSONObject info = fileInfo(n, data.get(i));
                        if (keepInPlace)
                            info.put("uri", uris.get(i).toString()); // read where it is (a persisted permission)
                        else {
                            // "Import": the app's own copy of every picked file, once: the original may be deleted; it
                            // never leaves the phone
                            src.mkdirs();
                            Files.write(new File(src, n).toPath(), data.get(i));
                        }
                        mod.sources.put(info);
                    }
                    // what the game reads: model files for the converter (vcs_modconv), whole text files (.gxt) - from
                    // the picked file itself (the file, its zip's or .vcm's entry: no second copy); only what the game
                    // can't reach there (a .vcm inside a zip) is taken out into files/
                    File files = new File(folder, "files");
                    Map<String, byte[]> keep = new LinkedHashMap<>(p.files.models);
                    keep.putAll(p.files.gxt);
                    for (Map.Entry<String, byte[]> f : keep.entrySet()) {
                        JSONObject info = fileInfo(safeName(f.getKey()), f.getValue());
                        Integer from = p.files.picked.get(f.getKey());
                        if (from != null && p.files.origin.containsKey(f.getKey())) {
                            JSONObject o = new JSONObject();
                            o.put("source", keys.get(from));
                            o.put("entry", p.files.origin.get(f.getKey()));
                            info.put("from", o);
                        } else {
                            files.mkdirs();
                            Files.write(new File(files, safeName(f.getKey())).toPath(), f.getValue());
                        }
                        mod.files.put(info);
                    }
                    save(mod);
                    Log.i("vcs", "[mods] " + (keepInPlace ? "added (kept where it is) " : "imported ") + fileName + " as "
                            + mod.id + ": " + mod.entries.size() + " entries");
                }
            } catch (ModFormats.FormatException e) {
                error = e.getMessage();
            } catch (IOException | JSONException | RuntimeException e) {
                error = "Can't read " + fileName + " (" + e.getMessage() + ").";
                Log.w("vcs", "[mods] import of " + fileName, e);
            }
            final Mod m = mod;
            final String failure = error;
            ui.post(() -> {
                busy = false;
                if (failure != null) {
                    note = "✗ Not imported: " + fileName;
                    if (keepInPlace)
                        for (Uri u : uris)
                            release(u);
                    message("Not imported", failure);
                } else {
                    mods.add(m);
                    note = m.inPlace ? "✓ Added " + m.name + " · applies at the next start · kept where it is: don't move or delete it"
                            : "✓ Imported " + m.name + " · applies at the next start · the original file can be deleted";
                    writeActive();
                    ModFormats.Entry open = null;
                    for (ModFormats.Entry e : m.entries)
                        if (e.target < 0 && open == null && !e.kind.equals("stream"))
                            open = e;
                    if (open != null)
                        chooseTarget(m, open);
                    else
                        details(m);
                }
                refresh.run();
            });
        }).start();
    }

    // ---- [mods] switching: find the original again, import a copy, use the original instead of the copy --------------

    private void findOriginal(Mod m, boolean asCopy) {
        finding = m;
        openPicker(asCopy ? FIND_AS_COPY : FIND_ORIGINAL);
    }

    /** The bytes of a source kept where it is (null: can't be read) */
    private byte[] readSource(JSONObject s) {
        try (InputStream in = activity.getContentResolver().openInputStream(Uri.parse(s.optString("uri")))) {
            return in == null ? null : readAll(in);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /**
     * The picked files matched to the mod's sources by their contents (sha256): kept where they are now (asCopy false:
     * the app's copy, if any, is deleted) or copied into the app (asCopy). Every source must be reachable afterwards.
     */
    private void relocate(Mod m, List<Uri> uris, boolean asCopy) {
        List<String> names = new ArrayList<>();
        for (Uri u : uris)
            names.add(displayName(u));
        busy = true;
        note = "Reading " + String.join(", ", names) + "…";
        refresh.run();
        new Thread(() -> {
            String error = null;
            List<Uri> oldUris = new ArrayList<>();
            List<Uri> unused = new ArrayList<>(uris);
            try {
                List<byte[]> data = readPicked(uris, names);
                Map<Integer, Integer> found = new LinkedHashMap<>(); // source index -> picked index
                for (int k = 0; k < data.size(); k++) {
                    String hash = sha256(data.get(k));
                    for (int i = 0; i < m.sources.length(); i++)
                        if (hash.equals(m.sources.getJSONObject(i).optString("sha256")) && !found.containsKey(i))
                            found.put(i, k);
                }
                if (found.isEmpty())
                    throw new ModFormats.FormatException("None of the picked files is this mod's original: their contents differ. "
                            + "To use a new version, add it as a new mod.");
                File src = new File(new File(dir(), m.id), "source");
                for (int i = 0; i < m.sources.length(); i++) {
                    JSONObject s = m.sources.getJSONObject(i);
                    boolean copied = new File(src, s.optString("name")).isFile();
                    if (!found.containsKey(i) && !copied && !(m.inPlace && m.original.equals("ok")))
                        throw new ModFormats.FormatException("Also pick " + s.optString("name") + " (this mod has "
                                + m.sources.length() + " files: pick them together).");
                }
                for (Map.Entry<Integer, Integer> f : found.entrySet()) {
                    JSONObject s = m.sources.getJSONObject(f.getKey());
                    deriveFrom(m, s.optString("name"), data.get(f.getValue())); // an older import's files/ copies
                    if (asCopy)
                        continue;
                    if (!s.optString("uri").isEmpty())
                        oldUris.add(Uri.parse(s.optString("uri")));
                    s.put("uri", uris.get(f.getValue()).toString());
                    unused.remove(uris.get(f.getValue()));
                }
                if (asCopy) {
                    src.mkdirs();
                    for (int i = 0; i < m.sources.length(); i++) {
                        JSONObject s = m.sources.getJSONObject(i);
                        File target = new File(src, s.optString("name"));
                        if (target.isFile())
                            continue;
                        byte[] b = found.containsKey(i) ? data.get(found.get(i)) : readSource(s);
                        if (b == null || !sha256(b).equals(s.optString("sha256")))
                            throw new IOException(s.optString("name") + " can't be read, or isn't the same file any more");
                        Files.write(target.toPath(), b);
                    }
                    for (int i = 0; i < m.sources.length(); i++)
                        if (!m.sources.getJSONObject(i).optString("uri").isEmpty())
                            oldUris.add(Uri.parse(m.sources.getJSONObject(i).optString("uri")));
                }
                m.inPlace = !asCopy;
                save(m);
            } catch (ModFormats.FormatException e) {
                error = e.getMessage();
            } catch (IOException | JSONException | RuntimeException e) {
                error = "Can't read " + String.join(", ", names) + " (" + e.getMessage() + ").";
                Log.w("vcs", "[mods] relocate " + m.id, e);
            }
            final String failure = error;
            ui.post(() -> {
                busy = false;
                if (failure != null) {
                    note = "✗ " + m.name + ": not changed";
                    for (Uri u : uris)
                        release(u);
                    message(m.name, failure);
                } else {
                    switched(m, oldUris);
                    for (Uri u : unused)
                        release(u);
                    note = "✓ " + m.name + (m.inPlace ? ": read where it is (the app's copy is deleted) · don't move or delete it"
                            : ": the app keeps its own copy now · you may delete the original");
                }
                refresh.run();
            });
        }).start();
    }

    /** After a switch (saved): active.txt, then the copy deleted (kept where it is) and the permissions no longer used */
    private void switched(Mod m, List<Uri> oldUris) {
        writeActive(); // (first: active.txt never names a deleted file)
        if (m.inPlace) {
            deleteTree(new File(new File(dir(), m.id), "source"));
            dropDerived(m);
        }
        for (Uri u : oldUris)
            release(u);
        checkOriginals();
    }

    /** Kept where it is -> imported: the originals (still where they were) copied into the app */
    private void importCopy(Mod m) {
        busy = true;
        note = "Copying " + m.name + "…";
        refresh.run();
        new Thread(() -> {
            String error = null;
            List<Uri> oldUris = new ArrayList<>();
            try {
                File src = new File(new File(dir(), m.id), "source");
                src.mkdirs();
                for (int i = 0; i < m.sources.length(); i++) {
                    JSONObject s = m.sources.getJSONObject(i);
                    byte[] b = readSource(s);
                    if (b == null || !sha256(b).equals(s.optString("sha256")))
                        throw new IOException(s.optString("name") + " can't be read, or isn't the same file any more");
                    Files.write(new File(src, s.optString("name")).toPath(), b);
                    oldUris.add(Uri.parse(s.optString("uri")));
                }
                m.inPlace = false;
                save(m);
            } catch (IOException | JSONException | RuntimeException e) {
                error = e.getMessage();
            }
            final String failure = error;
            ui.post(() -> {
                busy = false;
                if (failure != null) {
                    note = "✗ " + m.name + ": not copied";
                    message(m.name, "Not copied: " + failure);
                } else {
                    switched(m, oldUris);
                    note = "✓ " + m.name + ": the app keeps its own copy now · you may delete the original";
                }
                refresh.run();
            });
        }).start();
    }

    private String idFor(String title) {
        String base = title.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "_").replaceAll("^_+|_+$", "");
        if (base.isEmpty())
            base = "mod";
        if (base.length() > 32)
            base = base.substring(0, 32);
        String id = base;
        for (int i = 2; new File(dir(), id).exists(); i++)
            id = base + "_" + i;
        return id;
    }

    private static String safeName(String n) {
        String s = n.replaceAll("[^A-Za-z0-9_.\\-]+", "_");
        return s.isEmpty() || s.startsWith(".") ? "mod" + s : s;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > MAX_PICKED)
                return null;
        }
        return out.toByteArray();
    }

    // ---- a mod's dialog ----------------------------------------------------------------------------------------------

    private void modDialog(Mod m) {
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        labels.add(m.enabled ? "Turn off" : "Turn on");
        actions.add(() -> {
            m.enabled = !m.enabled;
            changed(m);
        });
        for (ModFormats.Entry e : m.entries) {
            if (e.kind.equals("stream") || e.kind.equals("replace"))
                continue;
            String who = e.pcName.equals("VCMCAR") ? "this car" : e.pcName;
            labels.add(kindWord(e) + " " + who + " → " + ModFormats.targetName(e) + " (" + ModFormats.matchText(e.match) + ")"
                    + (e.off ? " · set aside" : conflict(e) ? " · conflict" : "") + "…");
            actions.add(() -> entryDialog(m, e));
        }
        // [mods] where it is read from, and the switch to the other way
        if (m.inPlace && !m.original.equals("ok")) {
            labels.add("Find it again… (pick the same file where it is now)");
            actions.add(() -> findOriginal(m, false));
            labels.add("Import a copy instead… (pick it; the app keeps a copy)");
            actions.add(() -> findOriginal(m, true));
        } else if (m.inPlace) {
            labels.add("Import a copy (then you may move or delete the original)");
            actions.add(() -> importCopy(m));
        } else {
            labels.add("Use the original instead… (pick it; the app's copy is deleted)");
            actions.add(() -> findOriginal(m, false));
        }
        labels.add("Details");
        actions.add(() -> details(m));
        labels.add("Remove");
        actions.add(() -> confirmRemove(m));
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(m.name + (m.original.equals("missing") ? " · original missing"
                        : m.original.equals("changed") ? " · original changed" : m.inPlace ? " · kept where it is" : " · imported"))
                .setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run())
                .setNegativeButton("Close", null)
                .show());
    }

    /** One line / model: change its slot, or (a conflict) make it the one that applies */
    private void entryDialog(Mod m, ModFormats.Entry e) {
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        labels.add((ModFormats.vehicleEntry(e) ? "Choose the vehicle…" : "Choose the weapon…") + " (now " + ModFormats.targetName(e) + ")");
        actions.add(() -> chooseTarget(m, e));
        if (e.match.equals("picked")) {
            labels.add("Match by name again");
            actions.add(() -> {
                ModFormats.autoTarget(e);
                changed(m);
            });
        }
        List<Mod> others = activeOn(slotKey(e));
        others.remove(m);
        boolean shared = false;
        for (Mod o : mods)
            if (o != m && o.enabled)
                for (ModFormats.Entry x : o.entries)
                    shared |= slotKey(e) != null && slotKey(e).equals(slotKey(x));
        if (shared && (e.off || !others.isEmpty())) {
            labels.add("Use this mod's " + kindWord(e).replace(":", "") + " line for " + ModFormats.targetName(e));
            actions.add(() -> prefer(m, e));
        }
        String who = e.pcName.equals("VCMCAR") ? "this car" : e.pcName;
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(who + " → " + ModFormats.targetName(e))
                .setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run())
                .setNegativeButton("Close", null)
                .show());
    }

    private void chooseTarget(Mod m, ModFormats.Entry e) {
        boolean vehicle = ModFormats.vehicleEntry(e);
        boolean model = e.kind.equals("model");
        List<ModFormats.Slot> list = new ArrayList<>(vehicle ? ModFormats.slots().vehicles : ModFormats.slots().weapons);
        if (model && !vehicle)
            list.removeIf(s -> s.modelId < 0); // a weapon model needs the weapon's own model
        Collections.sort(list, (a, b) -> a.label.compareToIgnoreCase(b.label));
        String[] names = new String[list.size()];
        int current = -1;
        for (int i = 0; i < list.size(); i++) {
            ModFormats.Slot s = list.get(i);
            int id = model && !vehicle ? s.modelId : s.id;
            names[i] = s.label + "  (" + s.cls + ")";
            if (id == e.target)
                current = i;
        }
        String what = e.pcName.equals("VCMCAR") ? "this car" : e.pcName;
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle((vehicle ? "Which " + gameTitle() + " vehicle gets " : "Which " + gameTitle() + " weapon gets ") + what
                        + (model ? "'s model?" : "'s values?"))
                .setSingleChoiceItems(names, current, (d, which) -> {
                    ModFormats.Slot s = list.get(which);
                    e.target = model && !vehicle ? s.modelId : s.id;
                    e.match = "picked";
                    d.dismiss();
                    changed(m);
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    private static long treeSize(File f) {
        if (f.isFile())
            return f.length();
        long n = 0;
        File[] c = f.listFiles();
        if (c != null)
            for (File x : c)
                n += treeSize(x);
        return n;
    }

    private static String size(long b) {
        return b >= 1 << 20 ? String.format(Locale.ROOT, "%.1f MB", b / 1048576.0) : Math.max(1, b / 1024) + " KB";
    }

    /** The stock values the game reported at its last start, "weapon <type>" -> field -> value ("(was X)") */
    private Map<String, Map<String, String>> stock() {
        Map<String, Map<String, String>> out = new LinkedHashMap<>();
        Pattern p = Pattern.compile("^\\S+ -> weapon (\\d+): (\\w+) (\\S+) \\(was (\\S+)\\)");
        try {
            File f = new File(dir(), "applied.txt");
            if (f.isFile())
                for (String l : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
                    Matcher x = p.matcher(l);
                    if (x.find())
                        out.computeIfAbsent("weapon " + x.group(1), k -> new LinkedHashMap<>()).putIfAbsent(x.group(2), x.group(4));
                }
        } catch (IOException ignored) {
        }
        return out;
    }

    private void details(Mod m) {
        StringBuilder b = new StringBuilder();
        File folder = new File(dir(), m.id);
        long converted = treeSize(new File(folder, "converted"));
        b.append("From ").append(m.source).append(". ").append(m.enabled ? "On" : "Off")
                .append(": applies when the game starts; nothing is written to your saves.\n");
        if (!m.inPlace)
            b.append("Imported: the app keeps its own copy (").append(size(treeSize(folder)))
                    .append(converted > 0 ? ", converted models " + size(converted) : "")
                    .append("), so you can delete the file you imported from.\n");
        else
            b.append("Kept where it is: the app only reads the original there and keeps no copy (").append(size(treeSize(folder)))
                    .append(converted > 0 ? ", converted models " + size(converted) : "")
                    .append("). Don't move, rename or delete it.\n")
                    .append(m.original.equals("missing") ? "The original is missing (moved, renamed, deleted, or the app's "
                            + "permission is gone): the game skips this mod until you find it again.\n"
                            : m.original.equals("changed") ? "The original changed (another size): find the one you added "
                            + "again, or add the new one as a new mod.\n" : "");
        Map<String, Map<String, String>> stock = stock();
        for (ModFormats.Entry e : m.entries) {
            ModFormats.Slot s = ModFormats.targetSlot(e);
            b.append("\n").append(e.pcName.equals("VCMCAR") ? "this car" : e.pcName).append(" → ").append(ModFormats.targetName(e));
            if (!e.kind.equals("stream") && !e.kind.equals("replace"))
                b.append(" (").append(ModFormats.matchText(e.match))
                        .append(e.match.equals("similar") && s != null ? ", " + s.cls : "").append(")");
            b.append("\n").append(changes(e)).append(", from ").append(e.source)
                    .append(e.kind.equals("handling") ? " (" + e.dialect.toUpperCase(Locale.ROOT) + " handling.cfg)" : "").append("\n");
            if (e.off)
                b.append("Set aside: another mod has this slot.\n");
            else if (conflict(e))
                b.append("Conflict: another mod changes this slot too; the one loaded later wins (What is modified).\n");
            Map<String, String> before = e.kind.equals("weapon") ? stock.get("weapon " + e.target) : null;
            List<String> f = new ArrayList<>();
            for (Map.Entry<String, String> kv : e.fields.entrySet()) {
                String was = before != null ? before.get(kv.getKey()) : null;
                f.add(kv.getKey() + " " + kv.getValue() + (was != null && !was.equals(kv.getValue()) ? " (stock " + was + ")" : ""));
            }
            b.append("Values: ").append(String.join(", ", f)).append("\n");
            if (e.kind.equals("handling"))
                b.append("Max speed is approximate (scaled from the slot's own); the game clamps values outside "
                        + "safe ranges.\n");
            if (!e.skipped.isEmpty())
                b.append("Not used: ").append(String.join(", ", e.skipped)).append("\n");
        }
        if (!m.notes.isEmpty())
            b.append("\n• ").append(String.join("\n• ", m.notes)).append("\n");
        List<String> applied = appliedLines(m.id);
        if (!applied.isEmpty())
            b.append("\nAt the last start (\"was\" = the value before the mod):\n").append(String.join("\n", applied));
        message(m.name, b.toString());
    }

    private List<String> appliedLines(String id) {
        List<String> out = new ArrayList<>();
        File f = new File(dir(), "applied.txt");
        try {
            if (f.isFile())
                for (String l : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8))
                    if (l.startsWith(id + " ") || l.startsWith(id + ":"))
                        out.add(l.substring(id.length()).replaceFirst("^:?\\s*", ""));
        } catch (IOException ignored) {
        }
        return out.size() > 40 ? out.subList(0, 40) : out;
    }

    private void confirmRemove(Mod m) {
        AlertDialog d = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Remove " + m.name + "?")
                .setMessage("What the app keeps of it (files/" + GameFiles.dataDir(activity).getName() + "/mods/" + m.id
                        + (m.inPlace ? ": its list and converted models" : ": its copy and converted models") + ") is deleted; "
                        + "your original file is not touched. The slot is stock again at the next start. Saves are not affected.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Remove", (x, w) -> {
                    mods.remove(m);
                    writeActive();
                    deleteTree(new File(dir(), m.id));
                    for (int i = 0; i < m.sources.length(); i++) {
                        JSONObject s = m.sources.optJSONObject(i);
                        if (m.inPlace && s != null && !s.optString("uri").isEmpty())
                            release(Uri.parse(s.optString("uri"))); // the read permission it no longer needs
                    }
                    note = "Removed " + m.name;
                    refresh.run();
                })
                .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus());
        d.show();
        Dialogs.fit(d);
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null)
            for (File c : children)
                deleteTree(c);
        f.delete();
    }

    private void message(String title, String text) {
        Dialogs.fit(new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton("OK", null)
                .show());
    }
}
