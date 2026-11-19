// [android] Is a disc image the supported one? The launcher's copy of tools/vcs_disc.py's check(): the same codes and
// messages, read straight from the document the player picked (no copy of a multi-GB image). Reads the ISO 9660
// volume descriptor, the root directory, SYSTEM.CNF and the executable only (about a second).
package io.github.batinium.vcsrecomp;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class DiscCheck {
    // [lcs] The games the app can run (src/disc/known_games.h, tools/vcs_disc.py KNOWN_GAMES: change all three): the
    // boot executable SYSTEM.CNF names, the one build of it the recompiled code matches, and the id the game library's
    // stamp carries (" game=<id>", GameFiles.libraryGame)
    static final class Game {
        final String id, elf, sha1, title, name;

        Game(String id, String elf, String sha1, String title, String name) {
            this.id = id;
            this.elf = elf;
            this.sha1 = sha1;
            this.title = title;
            this.name = name;
        }
    }

    static final Game VCS = new Game("vcs", "SLUS_215.90", "d1832508caf0990c2291c0f255c96a889cce9f88",
            "GTA: Vice City Stories - SLUS-21590 (USA) v1.00", "Vice City Stories");
    static final Game LCS = new Game("lcs", "SLUS_214.23", "9b021b4c4e42087bb9b374709f26cf67744b75ab",
            "GTA: Liberty City Stories - SLUS-21423 (USA) v1.01", "Liberty City Stories");
    static final Game[] GAMES = {LCS, VCS}; // [lcs] Liberty City Stories first: the selector's order and the default
    private static final int SECTOR = 2048;

    /** The game with this id ("vcs", "lcs"), or null */
    static Game game(String id) {
        for (Game g : GAMES)
            if (g.id.equals(id))
                return g;
        return null;
    }

    static final class Result {
        final boolean ok;
        final String code; // ok, not_iso, not_ps2, other_release, unsupported_build
        final String message;
        final String game; // [lcs] the known game the disc is ("vcs", "lcs"), else null

        Result(boolean ok, String code, String message) {
            this(ok, code, message, null);
        }

        Result(boolean ok, String code, String message, String game) {
            this.ok = ok;
            this.code = code;
            this.message = message;
            this.game = game;
        }
    }

    private static final class Entry {
        final long lba;
        final int size;
        final boolean dir;

        Entry(long lba, int size, boolean dir) {
            this.lba = lba;
            this.size = size;
            this.dir = dir;
        }
    }

    private final FileChannel channel;

    private DiscCheck(FileChannel channel) {
        this.channel = channel;
    }

    static Result check(FileChannel channel, String displayName) {
        String lower = displayName == null ? "" : displayName.toLowerCase(Locale.ROOT);
        for (String ext : new String[] {".bin", ".cue", ".chd", ".cso", ".zso", ".zip", ".7z", ".rar", ".gz", ".mdf", ".nrg", ".img"}) {
            if (lower.endsWith(ext)) {
                return new Result(false, "not_iso", "A " + ext + " file: the game reads .iso images only. "
                        + "Extract or convert it to .iso first (on a PC: 7-Zip for archives, chdman extractdvd for .chd, maxcso for .cso).");
            }
        }
        DiscCheck d = new DiscCheck(channel);
        try {
            Entry root = d.root();
            Entry cnf = d.find(root, "SYSTEM.CNF");
            String boot = "";
            if (cnf != null && !cnf.dir) {
                String text = new String(d.read(cnf), StandardCharsets.US_ASCII);
                Matcher m = Pattern.compile("BOOT2\\s*=\\s*cdrom0:\\\\?([^;\\s]+)").matcher(text);
                if (m.find())
                    boot = m.group(1);
            }
            if (boot.isEmpty())
                return new Result(false, "not_ps2", "Not a PS2 game disc (no SYSTEM.CNF).");
            Game game = null;
            for (Game g : GAMES)
                if (boot.equalsIgnoreCase(g.elf))
                    game = g;
            if (game == null) {
                String other = boot.equalsIgnoreCase("SLES_546.22") ? "the European release of Vice City Stories" : "another game";
                return new Result(false, "other_release", "This disc is " + other + " (" + boot
                        + "). Supported: GTA: Vice City Stories (SLUS-21590) and Liberty City Stories (SLUS-21423), USA.");
            }
            Entry elf = d.find(root, game.elf);
            if (elf == null)
                return new Result(false, "not_ps2", game.elf + " missing from the image (a damaged copy?)", game.id);
            String sha1 = sha1(d.read(elf));
            if (!sha1.equals(game.sha1))
                return new Result(false, "unsupported_build", "Unsupported build of " + game.name + "'s executable (sha1 "
                        + sha1 + "): a modified or damaged copy?", game.id);
            return new Result(true, "ok", game.title, game.id);
        } catch (IOException | RuntimeException e) {
            return new Result(false, "not_iso", "Not a readable ISO image (" + e.getMessage() + ").");
        }
    }

    /** [phonebuild] The game's executable read out of the image, checked against its sha1 (the phone build's input) */
    static byte[] readExecutable(FileChannel channel, Game game) throws IOException {
        DiscCheck d = new DiscCheck(channel);
        Entry elf = d.find(d.root(), game.elf);
        if (elf == null)
            throw new IOException(game.elf + " missing from the image");
        byte[] data = d.read(elf);
        if (!sha1(data).equals(game.sha1))
            throw new IOException("unsupported build of " + game.elf);
        return data;
    }

    private void readFully(ByteBuffer buffer, long position) throws IOException {
        while (buffer.hasRemaining()) {
            int got = channel.read(buffer, position + buffer.position());
            if (got < 0)
                throw new IOException("the image ends early");
        }
        buffer.flip();
    }

    private byte[] read(Entry e) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(e.size);
        readFully(b, e.lba * SECTOR);
        return b.array();
    }

    // The primary volume descriptor at sector 16: "CD001", the root directory record at byte 156
    private Entry root() throws IOException {
        ByteBuffer pvd = ByteBuffer.allocate(SECTOR);
        readFully(pvd, 16L * SECTOR);
        byte[] p = pvd.array();
        if (p[0] != 1 || p[1] != 'C' || p[2] != 'D' || p[3] != '0' || p[4] != '0' || p[5] != '1')
            throw new IOException("no ISO 9660 volume descriptor");
        Entry r = record(p, 156);
        if (r == null)
            throw new IOException("no root directory");
        return r;
    }

    private static Entry record(byte[] b, int at) {
        int len = b[at] & 0xff;
        if (len == 0)
            return null;
        ByteBuffer le = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
        long lba = le.getInt(at + 2) & 0xffffffffL;
        int size = le.getInt(at + 10);
        boolean dir = (b[at + 25] & 2) != 0;
        return new Entry(lba, size, dir);
    }

    // A name in one directory; ";1" versions and case ignored, as the PS2 does
    private Entry find(Entry dir, String name) throws IOException {
        byte[] data = read(dir);
        int at = 0;
        while (at < data.length) {
            int len = data[at] & 0xff;
            if (len == 0) { // records don't cross sectors: the rest of this one is padding
                at = (at / SECTOR + 1) * SECTOR;
                continue;
            }
            int nameLen = data[at + 32] & 0xff;
            String n = new String(data, at + 33, nameLen, StandardCharsets.US_ASCII);
            int semi = n.indexOf(';');
            if (semi >= 0)
                n = n.substring(0, semi);
            if (n.equalsIgnoreCase(name))
                return record(data, at);
            at += len;
        }
        return null;
    }

    private static String sha1(byte[] data) {
        try {
            byte[] h = MessageDigest.getInstance("SHA-1").digest(data);
            StringBuilder s = new StringBuilder();
            for (byte x : h)
                s.append(String.format(Locale.ROOT, "%02x", x));
            return s.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        }
    }
}
