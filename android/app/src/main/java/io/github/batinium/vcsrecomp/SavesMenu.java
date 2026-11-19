// [android] The launcher's Saves category (docs/ANDROID.md "Saves: import and export"): a save from a PC, a cheat
// device or a PS3 onto the app's memory card, and the game's save out as .psu (mymc+ puts it on a PCSX2 card,
// uLaunchELF on a PS2) or as a .zip of the folder. The system's file picker does the choosing (ACTION_OPEN_DOCUMENT /
// ACTION_CREATE_DOCUMENT):
// no storage permission. The formats are PS2Save's, the card is SaveCard's; this class is the menu rows, the dialogs and
// the threads. Nothing happens while the game's :game process runs: it holds the card's state in memory and would
// write over an import (VcsIni.gameRunning, as the vcs.ini rows).
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class SavesMenu {
    // request codes, apart from LauncherActivity's (1-4)
    static final int PICK_IMPORT = 20, CREATE_PSU = 21, CREATE_ZIP = 22;
    // the picked file is read whole: a zip of a few saves at most (a full memory card is 8 MB)
    private static final int MAX_PICKED = 64 << 20;

    private final Activity activity;
    private final Runnable refresh;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean busy;
    private String note; // a running task's line or the last result, shown in place of the card's state
    private boolean running;
    private long runningAt;

    SavesMenu(Activity activity, Runnable refresh) {
        this.activity = activity;
        this.refresh = refresh;
    }

    // [lcs] the library's game's own data folder and save folder (GameFiles.dataDir: files/userdata for VCS,
    // files/userdata-lcs for LCS). LCS's slots are listed and can start the game; import is VCS's only (its checks
    // know VCS's saves), export takes either game's folder.
    private File userdata() {
        return GameFiles.dataDir(activity);
    }

    private boolean lcs() {
        return DiscCheck.LCS.id.equals(GameFiles.libraryGame(activity));
    }

    private String saveDir() {
        return lcs() ? SaveCard.LCS_GAME_DIR : SaveCard.GAME_DIR;
    }

    private File gameDir() {
        return SaveCard.gameDir(userdata(), saveDir());
    }

    /** VcsIni.gameRunning, asked at most every 300 ms (the rows' suppliers all want it in one refresh) */
    private boolean gameRunning() {
        long now = SystemClock.uptimeMillis();
        if (runningAt == 0 || now - runningAt > 300) {
            running = VcsIni.gameRunning(activity);
            runningAt = now;
        }
        return running;
    }

    private static final String QUIT_FIRST = "The game is running: it keeps the memory card's state and would write over "
            + "the card. Quit it first (side panel, or swipe it away in Recents).";

    void addTo(MenuView menu) {
        MenuView.Category c = menu.category("Saves", MenuView.G_SAVES);
        // the slots on the card, each with the picture the game took as it was saved (save_shots.cpp
        // "Save pictures": <data folder>/save_shots/slot<N>.png, both games: src/game/save_shots.cpp)
        for (int n = 1; n <= 8; n++) {
            final int slot = n;
            c.add(new MenuView.Item("Slot " + n)
                    .visible(() -> slotName(slot) != null)
                    .value(() -> {
                        String name = slotName(slot);
                        File pic = shot(slot);
                        String when = pic.isFile() ? " · " + android.text.format.DateFormat.format("d MMM, HH:mm", pic.lastModified()) : "";
                        return (name == null || name.isEmpty() ? "Saved game" : name) + when;
                    })
                    .picture(() -> shot(slot).isFile() ? shot(slot) : null)
                    .help(() -> (shot(slot).isFile() ? "" : "No picture: saved before pictures were kept, or elsewhere. ")
                            + "Start the game from it, or load it in the game: pause > Game > Load Game.")
                    .action(() -> askStart(slot)));
        }
        c.add(new MenuView.Item("Import a save…")
                .visible(() -> !lcs()) // [lcs] VCS's saves only (SaveCard.regionProblem)
                .value(this::cardState)
                .help(() -> gameRunning() ? QUIT_FIRST : "A save of the US PS2 disc (" + SaveCard.GAME_PREFIX + ") from "
                        + "a PC or another console: Max Drive .max, CodeBreaker .cbs, .psu (PCSX2, mymc, uLaunchELF), PS3 "
                        + ".psv, or a .zip of the save folder. Your current save is backed up first.")
                .enabled(() -> !busy && !gameRunning())
                .action(() -> open(PICK_IMPORT)));
        c.add(new MenuView.Item("Export as .psu…")
                .value(() -> haveSave() ? "For mymc+ (PCSX2 cards), uLaunchELF, PS2 save tools" : "No save on the card yet")
                .help(() -> gameRunning() ? QUIT_FIRST : "Your save folder (" + saveDir() + ", every slot) as one "
                        + ".psu file. Choose where it goes.")
                .enabled(() -> !busy && !gameRunning() && haveSave())
                .action(() -> create(CREATE_PSU)));
        c.add(new MenuView.Item("Export as .zip…")
                .value(() -> haveSave() ? "The save folder as it is on the card" : "No save on the card yet")
                .help(() -> gameRunning() ? QUIT_FIRST : "The save folder zipped, for a PC build of the game (unzip it "
                        + "into its mc0 folder) or to keep. This app imports it back.")
                .enabled(() -> !busy && !gameRunning() && haveSave())
                .action(() -> create(CREATE_ZIP)));
    }

    // ---- state (read in refresh: names only, no file contents) ------------------------------------------------------

    private File shot(int slot) {
        return new File(new File(userdata(), "save_shots"), "slot" + slot + ".png");
    }

    private List<String> slotsCache;
    private long slotsAt;

    /** Slot `n`'s name on the card (the last mission passed, "" when none), null when the slot is empty */
    private String slotName(int n) {
        long now = SystemClock.uptimeMillis();
        if (slotsAt == 0 || now - slotsAt > 300) {
            slotsCache = slotsOnCard();
            slotsAt = now;
        }
        if (slotsCache != null)
            for (String e : slotsCache)
                if (SaveCard.slotNumber(e) == n)
                    return e.replaceFirst("^\\d+:?\\s*", ""); // VCS "1 Soldier", [lcs] LCS "1: In the beginning..."
        return null;
    }

    private boolean haveSave() {
        return gameDir().isDirectory();
    }

    private String cardState() {
        if (note != null)
            return note;
        List<String> slots = slotsOnCard();
        if (slots == null)
            return "No save on the card yet";
        return slots.isEmpty() ? "On the card: the game's folder, no saved game in it"
                : "On the card: " + String.join(", ", slots);
    }

    /** The card's numbered slots in slot order, null without a save folder */
    private List<String> slotsOnCard() {
        File dir = gameDir();
        String[] names = dir.list();
        if (names == null)
            return null;
        List<String> card = new ArrayList<>();
        for (String n : names)
            if (!n.equals(SaveCard.ORDER_FILE))
                card.add(SaveCard.decodeName(n));
        List<String> slots = new ArrayList<>();
        for (String n : SaveCard.cardOrder(saveDir(), card))
            if (SaveCard.slotNumber(n) >= 0)
                slots.add(n);
        return slots;
    }

    private void setNote(String n) {
        note = n;
        refresh.run();
    }

    // ---- pickers -----------------------------------------------------------------------------------------------------

    private void open(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*"); // .max / .cbs / .psu / .psv have no MIME types
        activity.startActivityForResult(i, request);
    }

    private void create(int request) {
        if (gameRunning()) {
            setNote("✗ " + QUIT_FIRST);
            return;
        }
        File dir = gameDir();
        long newest = 0;
        File[] files = dir.listFiles();
        if (files != null)
            for (File f : files)
                newest = Math.max(newest, f.lastModified());
        String date = new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.ROOT).format(new Date(newest > 0 ? newest : System.currentTimeMillis()));
        boolean zip = request == CREATE_ZIP;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(zip ? "application/zip" : "application/octet-stream");
        i.putExtra(Intent.EXTRA_TITLE, (lcs() ? "GTA LCS save " : "GTA VCS save ") + date + (zip ? ".zip" : ".psu"));
        activity.startActivityForResult(i, request);
    }

    /** LauncherActivity.onActivityResult: true when the request was ours */
    boolean onResult(int request, int result, Intent data) {
        if (request != PICK_IMPORT && request != CREATE_PSU && request != CREATE_ZIP)
            return false;
        if (result != Activity.RESULT_OK || data == null || data.getData() == null)
            return true;
        if (request == PICK_IMPORT)
            readImport(data.getData());
        else
            export(data.getData(), request == CREATE_ZIP);
        return true;
    }

    private String displayName(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null)
                return c.getString(0);
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s : "the file";
    }

    // ---- import ------------------------------------------------------------------------------------------------------

    private void readImport(Uri uri) {
        String name = displayName(uri);
        busy = true;
        setNote("Reading " + name + "…");
        new Thread(() -> {
            PS2Save save = null;
            String error = null;
            try (InputStream in = activity.getContentResolver().openInputStream(uri)) {
                if (in == null)
                    throw new IOException("no data");
                byte[] bytes = readAll(in);
                if (bytes == null)
                    error = name + " is far larger than a memory card: not a PS2 save.";
                else {
                    List<PS2Save> saves = PS2Save.read(bytes, name);
                    save = SaveCard.choose(saves);
                    error = SaveCard.regionProblem(save.dir);
                    if (error == null && save.files.isEmpty())
                        error = "The save in " + name + " is empty.";
                    if (error == null && saves.size() > 1)
                        Log.i("vcs", "[saves] " + name + " holds " + saves.size() + " saves; importing " + save.dir);
                }
            } catch (PS2Save.FormatException e) {
                error = e.getMessage();
            } catch (IOException | RuntimeException e) {
                error = "Can't read " + name + " (" + e.getMessage() + ").";
                Log.w("vcs", "[saves] reading " + name, e);
            }
            final PS2Save s = save;
            final String failure = error;
            ui.post(() -> {
                busy = false;
                if (failure != null) {
                    Log.i("vcs", "[saves] import of " + name + " refused: " + failure);
                    setNote("✗ " + failure);
                    message("Not imported", failure);
                } else {
                    setNote(null);
                    confirmImport(s, name);
                }
            });
        }).start();
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > MAX_PICKED)
                return null;
        }
        return out.toByteArray();
    }

    private void confirmImport(PS2Save save, String name) {
        if (gameRunning()) {
            setNote("✗ " + QUIT_FIRST);
            message("Quit the game first", QUIT_FIRST);
            return;
        }
        List<String> incoming = SaveCard.slots(save);
        String in = incoming.isEmpty() ? "no saved game (only the game's settings)" : String.join(", ", incoming);
        List<String> current = slotsOnCard();
        if (current == null) {
            install(save, name); // nothing to replace
            return;
        }
        String now = current.isEmpty() ? "no saved game" : String.join(", ", current);
        AlertDialog d = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Replace your save?")
                .setMessage("The card already has this game's save folder. A PS2 game has one folder, so the import "
                        + "replaces it whole.\n\nOn the card now: " + now + "\nFrom " + name + " (" + save.format + "): "
                        + in + "\n\nYour current save is copied to files/userdata/backups first.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Replace", (dlg, w) -> install(save, name))
                .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus()); // a stray A press keeps it
        d.show();
    }

    private void install(PS2Save save, String name) {
        if (gameRunning()) { // started from the side while the dialog was open
            setNote("✗ " + QUIT_FIRST);
            return;
        }
        busy = true;
        setNote("Importing " + name + "…");
        new Thread(() -> {
            String result;
            boolean ok = false;
            try {
                SaveCard.Installed r = SaveCard.install(userdata(), save);
                ok = true;
                result = "✓ Imported " + (r.slots.isEmpty() ? "the save" : String.join(", ", r.slots))
                        + (r.backup != null ? " · the old save is in backups/" + r.backup.getParentFile().getName() : "");
                Log.i("vcs", "[saves] imported " + name + " (" + save.format + ") into " + r.folder + ", slots " + r.slots
                        + (r.backup != null ? ", old save backed up to " + r.backup : ""));
            } catch (IOException | RuntimeException e) {
                result = "✗ Import failed: " + e.getMessage();
                Log.w("vcs", "[saves] import of " + name, e);
            }
            final String text = result;
            final boolean done = ok;
            ui.post(() -> {
                busy = false;
                setNote(text);
                if (!done)
                    message("Import failed", text.substring(2));
            });
        }).start();
    }

    // ---- export ------------------------------------------------------------------------------------------------------

    private void export(Uri uri, boolean zip) {
        if (gameRunning()) {
            setNote("✗ " + QUIT_FIRST);
            return;
        }
        String name = displayName(uri);
        busy = true;
        setNote("Writing " + name + "…");
        new Thread(() -> {
            String result;
            try {
                PS2Save save = SaveCard.readFolder(gameDir());
                if (save == null)
                    throw new IOException("no save on the card");
                byte[] bytes = zip ? save.toZip() : save.toPsu();
                // what we write must come back: read it again before handing it over
                PS2Save back = PS2Save.read(bytes, name).get(0);
                if (!back.dir.equals(save.dir) || back.files.size() != save.files.size() || back.totalSize() != save.totalSize())
                    throw new IOException("the written file did not read back the same");
                OutputStream out;
                try {
                    out = activity.getContentResolver().openOutputStream(uri, "wt");
                } catch (IllegalArgumentException | UnsupportedOperationException e) {
                    out = activity.getContentResolver().openOutputStream(uri, "w"); // providers without truncate
                }
                if (out == null)
                    throw new IOException("can't write there");
                try (OutputStream o = out) {
                    o.write(bytes);
                }
                List<String> slots = SaveCard.slots(save);
                result = "✓ Exported " + (slots.isEmpty() ? "the save" : String.join(", ", slots)) + " to " + name;
                Log.i("vcs", "[saves] exported " + save.dir + " (" + save.files.size() + " files, " + bytes.length
                        + " bytes) as " + name);
            } catch (IOException | PS2Save.FormatException | RuntimeException e) {
                result = "✗ Export failed: " + e.getMessage();
                Log.w("vcs", "[saves] export to " + name, e);
            }
            final String text = result;
            ui.post(() -> {
                busy = false;
                setNote(text);
            });
        }).start();
    }

    // ---- start from a slot (VCSActivity.EXTRA_LOAD_SLOT: the boot's load takes it instead of the newest) -------------

    private void askStart(int slot) {
        if (gameRunning()) {
            message("The game is running", "Slot " + slot + " can only be chosen as the game starts. Quit the game "
                    + "first (side panel, or swipe it away in Recents), or load the slot in the game: pause > Game > "
                    + "Load Game.");
            return;
        }
        LauncherActivity launcher = (LauncherActivity) activity;
        if (!launcher.canPlay()) {
            message("Not ready to play", "Choose your disc and import the game library under Disc first.");
            return;
        }
        String name = slotName(slot);
        if (name == null) { // gone since the list was drawn
            refresh.run();
            return;
        }
        File pic = shot(slot);
        String when = pic.isFile() ? " (" + android.text.format.DateFormat.format("d MMM, HH:mm", pic.lastModified()) + ")" : "";
        new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Start from slot " + slot + "?")
                .setMessage((name.isEmpty() ? "Saved game" : name) + when + "\n\nThe game starts and loads this save "
                        + "instead of the newest one. Nothing on the memory card changes.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Start", (dlg, w) -> {
                    if (gameRunning()) // started from the side while the dialog was open
                        message("The game is running", "Quit it first to start from slot " + slot + ".");
                    else
                        launcher.playSlot(slot);
                })
                .show();
    }

    private void message(String title, String text) {
        new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton("OK", null)
                .show();
    }
}
