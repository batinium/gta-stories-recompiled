// [android] What the launcher keeps between starts: the game chosen in its selector and, for each game, the disc the
// player picked (a document URI with a persisted read permission, never copied) and the imported game library
// (app-private, read-only).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

final class GameFiles {
    private static final String PREFS = "vcs";
    private static final String KEY_DISC = "disc.uri";
    private static final String KEY_DISC_NAME = "disc.name";
    private static final String KEY_DISC_OK = "disc.ok";
    // The game library: the C++ recompiled from the player's own disc, built on their PC (docs/ANDROID.md)
    static final String GAME_LIBRARY = "libvcs_game.so";

    private GameFiles() {}

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    // ---- [lcs] which game: one app, both games side by side (docs/ANDROID.md "Games"). Each game has its own disc
    // (the keys below, ".<id>" after them for a game other than VCS), game library (files/game/<id>/), data folder
    // (files/userdata, files/userdata-<id>) and palette; the launcher's game selector picks the one Play starts. --------

    private static final String KEY_DISC_GAME = "disc.game";
    private static final String KEY_GAME = "game";

    /** The game's own name for a disc key: VCS keeps the keys it always had, another game gets ".<id>" after them */
    private static String key(String base, String game) {
        return DiscCheck.VCS.id.equals(game) ? base : base + "." + game;
    }

    // In the game's process (:game): the game this run is (VCSActivity, from the launcher's intent), so a selector
    // change in the launcher meanwhile changes nothing here
    static String runningGame;

    /** The game Play starts (the selector's choice): a personal build with the game inside the APK has only that one */
    static String game(Context c) {
        if (bundledGame(c))
            return BuildConfig.VCS_GAME;
        if (runningGame != null)
            return runningGame;
        String g = prefs(c).getString(KEY_GAME, null);
        if (g != null && DiscCheck.game(g) != null)
            return g;
        // never chosen: the game whose library is there (LCS first), else LCS (the default). An existing user's choice
        // is in prefs above and is kept.
        for (DiscCheck.Game x : DiscCheck.GAMES)
            if (gameLibrary(c, x.id).isFile())
                return x.id;
        return DiscCheck.LCS.id;
    }

    /** The selector can switch games (not in a personal build with one game inside the APK) */
    static boolean canChooseGame() {
        return !BuildConfig.VCS_GAME_BUNDLED;
    }

    static void selectGame(Context c, String id) {
        if (canChooseGame() && DiscCheck.game(id) != null)
            prefs(c).edit().putString(KEY_GAME, id).commit(); // commit: the game's process reads it at its start
    }

    // The game that runs (the name from before the selector: callers ask "the library's game")
    static String libraryGame(Context c) {
        return game(c);
    }

    static DiscCheck.Game libraryGameInfo(Context c) {
        DiscCheck.Game g = DiscCheck.game(game(c));
        return g != null ? g : DiscCheck.VCS;
    }

    static Uri disc(Context c) {
        String s = prefs(c).getString(key(KEY_DISC, game(c)), null);
        return s == null ? null : Uri.parse(s);
    }

    /** [stories] A given game's picked disc (both cities: both discs at once), null: none picked */
    static Uri disc(Context c, String game) {
        String s = prefs(c).getString(key(KEY_DISC, game), null);
        return s == null ? null : Uri.parse(s);
    }

    /** [stories] A given game's adb test copy (disc.iso / disc-<id>.iso), null: none */
    static File pushedDisc(Context c, String game) {
        File f = new File(c.getExternalFilesDir(null), DiscCheck.VCS.id.equals(game) ? "disc.iso" : "disc-" + game + ".iso");
        return f.isFile() ? f : null;
    }

    static String discName(Context c) {
        return prefs(c).getString(key(KEY_DISC_NAME, game(c)), "");
    }

    static boolean discOk(Context c) {
        return prefs(c).getBoolean(key(KEY_DISC_OK, game(c)), false);
    }

    /** A picked disc: kept as its own game's (game: DiscCheck.Result.game), which the selector then shows; a disc that
     *  is no known game goes to the selected game's slot (as not usable) */
    static void setDisc(Context c, Uri uri, String name, boolean ok, String game) {
        String slot = ok && game != null && DiscCheck.game(game) != null && canChooseGame() ? game : game(c);
        prefs(c).edit().putString(key(KEY_DISC, slot), uri.toString()).putString(key(KEY_DISC_NAME, slot), name)
                .putBoolean(key(KEY_DISC_OK, slot), ok).putString(key(KEY_DISC_GAME, slot), game).commit();
        if (!slot.equals(game(c)))
            selectGame(c, slot);
    }

    // The selected game's disc's game (DiscCheck.Result.game): a disc picked before LCS was known is VCS's
    static String discGame(Context c) {
        String g = prefs(c).getString(key(KEY_DISC_GAME, game(c)), null);
        return g != null ? g : discOk(c) ? DiscCheck.VCS.id : null;
    }

    // The selected game's disc is that game (false: a personal build's one game and a disc of the other)
    static boolean discMatchesLibrary(Context c) {
        return game(c).equals(discGame(c));
    }

    /**
     * Once, from the one-game-at-a-time layout (before 2026-11-18's selector): an LCS disc kept in VCS's keys moves to
     * LCS's, and the library at files/game/libvcs_game.so moves to its stamp's game's folder. Launcher start, UI thread
     * (prefs and a rename only).
     */
    static void migrate(Context c) {
        SharedPreferences p = prefs(c);
        String old = p.getString(KEY_DISC_GAME, null);
        if (canChooseGame() && old != null && !DiscCheck.VCS.id.equals(old) && DiscCheck.game(old) != null
                && p.contains(KEY_DISC)) {
            p.edit().putString(key(KEY_DISC, old), p.getString(KEY_DISC, null))
                    .putString(key(KEY_DISC_NAME, old), p.getString(KEY_DISC_NAME, ""))
                    .putBoolean(key(KEY_DISC_OK, old), p.getBoolean(KEY_DISC_OK, false))
                    .putString(key(KEY_DISC_GAME, old), old)
                    .remove(KEY_DISC).remove(KEY_DISC_NAME).remove(KEY_DISC_OK).remove(KEY_DISC_GAME).commit();
            android.util.Log.i("vcs", "[launcher] the " + old + " disc moved to its own slot");
        }
        File legacy = new File(new File(c.getFilesDir(), "game"), GAME_LIBRARY);
        if (legacy.isFile()) {
            String g = stampGame(readStamp(legacy));
            File to = gameLibrary(c, g);
            to.getParentFile().mkdirs();
            if (!to.exists() && legacy.renameTo(to)) {
                android.util.Log.i("vcs", "[launcher] game library moved to " + to);
                if (!p.contains(KEY_GAME))
                    selectGame(c, g); // the game the app ran before
            }
        }
    }

    /** The game a stamp names (" game=<id>"); a stamp without one is VCS's (made before LCS) */
    static String stampGame(String stamp) {
        if (stamp != null)
            for (String part : stamp.split(" "))
                if (part.startsWith(STAMP_GAME) && part.length() > STAMP_GAME.length())
                    return part.substring(STAMP_GAME.length());
        return DiscCheck.VCS.id;
    }

    /** The game a library file is (its stamp), or null for no stamp */
    static String libraryFileGame(File f) {
        String s = readStamp(f);
        return s == null ? null : stampGame(s);
    }

    // The game's own data folder (memory card, caches: VCS_DATA_DIR): files/userdata for VCS, as always, and
    // files/userdata-<game> for another, so a game in bring-up never writes to VCS's memory card
    static File dataDir(Context c) {
        return dataDir(c, game(c));
    }

    static File dataDir(Context c, String g) {
        return new File(c.getFilesDir(), DiscCheck.VCS.id.equals(g) ? "userdata" : "userdata-" + g);
    }

    // A personal build: the game compiled into the APK itself (tools/android.py build --personal). Never shared. The
    // default APK is the runtime only: the game library is imported (the runtime / game split, docs/ANDROID.md).
    static boolean bundledGame(Context c) {
        return BuildConfig.VCS_GAME_BUNDLED;
    }

    // A disc copied into the app's own folder (Android/data/<app>/files/disc.iso, adb push): used when none was picked.
    // [lcs] Per game: disc.iso is VCS's, disc-lcs.iso LCS's (both may be there; the library's game picks)
    static File pushedDisc(Context c) {
        File f = new File(c.getExternalFilesDir(null), pushedDiscName(c));
        return f.isFile() ? f : null;
    }

    static String pushedDiscName(Context c) {
        String g = game(c);
        return DiscCheck.VCS.id.equals(g) ? "disc.iso" : "disc-" + g + ".iso";
    }

    /** The selected game's library (its code pack): files/game/<id>/libvcs_game.so, imported in the launcher */
    static File gameLibrary(Context c) {
        return gameLibrary(c, game(c));
    }

    static File gameLibrary(Context c, String game) {
        return new File(new File(new File(c.getFilesDir(), "game"), game), GAME_LIBRARY);
    }

    // The game library's interface stamp (src/android/game_abi.h): "vcs-game-library abi=<n> headers=<hash>" in a
    // section of its own, read from the file (as src/android/game_library.cpp does before loading it).
    static final String STAMP_SECTION = ".vcs_game_stamp";
    static final String STAMP_PREFIX = "vcs-game-library abi=";
    static final String STAMP_GAME = "game="; // [lcs] " game=<id>" after the headers hash (game_abi.h)
    // where the game's process leaves the reason it refused the library (src/android/game_main.cpp)
    static final String LOAD_ERROR = "game_library_error.txt";

    static String readStamp(File f) {
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            byte[] eh = new byte[64];
            in.readFully(eh);
            ByteBuffer h = ByteBuffer.wrap(eh).order(ByteOrder.LITTLE_ENDIAN);
            if (h.getInt(0) != 0x464c457f || eh[4] != 2)
                return null;
            long shoff = h.getLong(0x28);
            int shentsize = h.getShort(0x3a) & 0xffff, shnum = h.getShort(0x3c) & 0xffff, shstrndx = h.getShort(0x3e) & 0xffff;
            if (shentsize != 64 || shnum == 0 || shstrndx >= shnum || shoff <= 0 || shoff + (long) shnum * 64 > in.length())
                return null;
            byte[] sh = new byte[shnum * 64];
            in.seek(shoff);
            in.readFully(sh);
            ByteBuffer s = ByteBuffer.wrap(sh).order(ByteOrder.LITTLE_ENDIAN);
            long namesOff = s.getLong(shstrndx * 64 + 0x18), namesSize = s.getLong(shstrndx * 64 + 0x20);
            if (namesSize <= 0 || namesSize > (1 << 20))
                return null;
            byte[] names = new byte[(int) namesSize];
            in.seek(namesOff);
            in.readFully(names);
            byte[] want = STAMP_SECTION.getBytes(StandardCharsets.US_ASCII);
            for (int i = 0; i < shnum; i++) {
                int name = s.getInt(i * 64);
                if (name < 0 || name + want.length >= names.length || names[name + want.length] != 0)
                    continue;
                boolean match = true;
                for (int k = 0; k < want.length && match; k++)
                    match = names[name + k] == want[k];
                if (!match)
                    continue;
                long off = s.getLong(i * 64 + 0x18), size = s.getLong(i * 64 + 0x20);
                if (size <= 0 || size > 4096)
                    return null;
                byte[] text = new byte[(int) size];
                in.seek(off);
                in.readFully(text);
                int end = 0;
                while (end < text.length && text[end] != 0)
                    end++;
                return new String(text, 0, end, StandardCharsets.US_ASCII);
            }
        } catch (IOException | RuntimeException e) {
            return null;
        }
        return null;
    }

    private static String cachedKey, cachedProblem;

    // Why this game library can't be used by this APK (its interface number differs, or it has none), else null
    static synchronized String stampProblem(File f) {
        String key = f.getAbsolutePath() + ":" + f.length() + ":" + f.lastModified();
        if (key.equals(cachedKey))
            return cachedProblem;
        String stamp = readStamp(f), problem = null;
        if (stamp == null || !stamp.startsWith(STAMP_PREFIX)) {
            problem = "Not a game library for this app (no interface stamp): build it with tools/android.py build-game.";
        } else {
            String n = stamp.substring(STAMP_PREFIX.length()).split(" ", 2)[0];
            if (!n.equals(String.valueOf(BuildConfig.VCS_GAME_ABI)))
                problem = "Game library built for another version (interface " + n + ", this app " + BuildConfig.VCS_GAME_ABI
                        + "): rebuild it with tools/android.py build-game and import it again.";
        }
        cachedKey = key;
        cachedProblem = problem;
        return problem;
    }

    // The imported library's problem, if any (null: none, or a personal build, or none imported yet)
    static String libraryProblem(Context c) {
        if (bundledGame(c))
            return null;
        File lib = gameLibrary(c);
        return lib.exists() ? stampProblem(lib) : null;
    }

    static File loadError(Context c) {
        return new File(c.getFilesDir(), LOAD_ERROR);
    }

    // The reason the game refused the library at its last start (game_main.cpp), or null
    static String lastLoadError(Context c) {
        File f = loadError(c);
        if (!f.isFile())
            return null;
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    // An ELF shared object for arm64: the header's class (64-bit), type (ET_DYN) and machine (EM_AARCH64 = 183)
    static String checkLibrary(File f) {
        byte[] h = new byte[20];
        try (InputStream in = new FileInputStream(f)) {
            if (in.read(h) != h.length)
                return "Too short to be a library.";
        } catch (IOException e) {
            return "Can't read it (" + e.getMessage() + ").";
        }
        if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
            return "Not a library (no ELF header): pick the " + GAME_LIBRARY + " your PC build made.";
        if (h[4] != 2)
            return "A 32-bit library: the game is built for arm64 only.";
        int type = (h[16] & 0xff) | ((h[17] & 0xff) << 8);
        int machine = (h[18] & 0xff) | ((h[19] & 0xff) << 8);
        if (machine != 183)
            return "Built for another processor (ELF machine " + machine + "): build it for Android arm64.";
        if (type != 3)
            return "Not a shared library (ELF type " + type + ").";
        return null;
    }
}
