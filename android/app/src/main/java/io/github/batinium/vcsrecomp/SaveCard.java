// [android] The memory card on the phone's side (docs/ANDROID.md "Saves: import and export"): the runtime keeps it as a
// folder tree, files/userdata/mc0/<save folder>/<files> (VCSActivity sets VCS_DATA_DIR = files/userdata; src/main.cpp
// mounts <data>/mc0). This class puts an imported save there and reads the game's save back for an export.
//
// Two runtime conventions matter (third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp):
//  - Card names are stored on the host encoded (encodeMcName): <>:"/\|?*, control bytes, bytes >= 0x7F, '%', trailing
//    dots / spaces and device names as %XX. "1 Kill Phil: Part 2" lives in "1 Kill Phil%3A Part 2".
//  - sceMcGetDir lists a folder in card order, kept in a hidden ".ps2x-order" file (one host name per line). VCS looks
//    for its save slots from the sixth entry on (after ".", "..", and the three files the folder was made with;
//    docs/game-internals.md 10c), so an imported folder gets an order file: icon.sys, the icons, the data file named
//    like the folder, then the slot files by number ("1 Soldier", "2 ..."). Without it a slot listed early is never found.
//
// Plain Java (java.io only), so the JVM check runs it on a PC.
package io.github.batinium.vcsrecomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class SaveCard {
    static final String ORDER_FILE = ".ps2x-order";
    /** GTA: Vice City Stories, USA (SLUS-21590): the only save folder it makes */
    static final String GAME_DIR = "BASLUS-21590GTA40001";
    static final String GAME_PREFIX = "BASLUS-21590";
    /** [lcs] GTA: Liberty City Stories, USA (SLUS-21423): its save folder (src/game/lcs_save.cpp slots()); slot files
     *  "<slot>: <last mission passed>" (the card stores ':' and a trailing '.' as %XX) */
    static final String LCS_GAME_DIR = "BASLUS-21423GTA30001";

    // ---- names (MemoryCard.cpp encodeMcName / decodeMcName) ----------------------------------------------------------

    private static final char[] HEX = "0123456789ABCDEF".toCharArray();

    /** A card name (bytes as ISO-8859-1) as the runtime stores it on the host */
    static String encodeName(String name) {
        byte[] b = name.getBytes(StandardCharsets.ISO_8859_1);
        int keep = b.length;
        while (keep > 0 && (b[keep - 1] == '.' || b[keep - 1] == ' '))
            keep--;
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < b.length; i++) {
            int c = b[i] & 0xFF;
            if (c < 0x20 || c >= 0x7F || c == '%' || "<>:\"/\\|?*".indexOf(c) >= 0 || i >= keep)
                out.append('%').append(HEX[c >> 4]).append(HEX[c & 0xF]);
            else
                out.append((char) c);
        }
        String s = out.toString();
        int dot = s.indexOf('.');
        String base = (dot < 0 ? s : s.substring(0, dot)).toLowerCase(Locale.ROOT);
        if (base.equals("con") || base.equals("prn") || base.equals("aux") || base.equals("nul")
                || (base.length() == 4 && (base.startsWith("com") || base.startsWith("lpt")) && base.charAt(3) >= '1'
                && base.charAt(3) <= '9')) {
            int c = s.charAt(0);
            s = "%" + HEX[c >> 4] + HEX[c & 0xF] + s.substring(1);
        }
        return s;
    }

    static String decodeName(String name) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '%' && i + 2 < name.length() && hex(name.charAt(i + 1)) >= 0 && hex(name.charAt(i + 2)) >= 0) {
                out.append((char) (hex(name.charAt(i + 1)) * 16 + hex(name.charAt(i + 2))));
                i += 2;
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    private static int hex(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    }

    // ---- which save is it --------------------------------------------------------------------------------------------

    /** null when the folder is this port's (VCS, US PS2 disc); else what it is, for the player */
    static String regionProblem(String dir) {
        if (dir.startsWith(GAME_PREFIX))
            return null;
        String d = dir.toUpperCase(Locale.ROOT);
        String what;
        if (d.matches("^(UL|UC|NP)[A-Z]{2}\\d{5}.*")) {
            char r = d.charAt(2);
            String region = r == 'U' ? "USA" : r == 'E' ? "Europe" : r == 'J' ? "Japan" : r == 'K' || r == 'A' ? "Asia" : "PSP";
            what = "the PSP (" + region + ", " + dir.substring(0, 9) + "): PSP saves don't work on the PS2 version";
        } else if (d.length() >= 12 && d.charAt(0) == 'B' && d.charAt(6) == '-') {
            char r = d.charAt(1);
            String region = r == 'A' ? "USA" : r == 'E' ? "Europe" : r == 'I' ? "Japan" : r == 'K' ? "Korea"
                    : r == 'C' ? "China" : "another region";
            String id = dir.substring(2, 12);
            what = r == 'A' ? "another US PS2 disc (" + id + ")" : "a PS2 disc from " + region + " (" + id + ")";
        } else {
            what = "something other than this game (" + dir + ")";
        }
        return "This is a save for " + what + "; this port runs the US PS2 disc (SLUS-21590) and only reads its saves.";
    }

    /** The save to import from a file's saves: the first one for this game, else the first (to name it) */
    static PS2Save choose(List<PS2Save> saves) {
        for (PS2Save s : saves)
            if (regionProblem(s.dir) == null)
                return s;
        return saves.get(0);
    }

    /** "1 Soldier" -> 1; -1 when the name is not a numbered slot */
    static int slotNumber(String name) {
        int i = 0;
        while (i < name.length() && Character.isDigit(name.charAt(i)))
            i++;
        if (i == 0 || i > 6)
            return -1;
        return Integer.parseInt(name.substring(0, i));
    }

    /** The card order for a folder made from scratch: icon.sys, *.ico / *.icn, the data file, the slots, the rest */
    static List<String> cardOrder(String dir, List<String> names) {
        List<String> out = new ArrayList<>(names);
        out.sort(Comparator.<String>comparingInt(n -> rank(dir, n))
                .thenComparingInt(n -> rank(dir, n) == 3 ? slotNumber(n) : 0)
                .thenComparing(Comparator.naturalOrder()));
        return out;
    }

    private static int rank(String dir, String n) {
        String l = n.toLowerCase(Locale.ROOT);
        if (l.equals("icon.sys"))
            return 0;
        if (l.endsWith(".ico") || l.endsWith(".icn"))
            return 1;
        if (n.equals(dir))
            return 2;
        return slotNumber(n) >= 0 ? 3 : 4;
    }

    /** The numbered slot files of a save, in slot order ("1 Soldier", "2 Last Stand") */
    static List<String> slots(PS2Save s) {
        List<String> out = new ArrayList<>();
        for (String n : cardOrder(s.dir, names(s)))
            if (rank(s.dir, n) == 3)
                out.add(n);
        return out;
    }

    private static List<String> names(PS2Save s) {
        List<String> n = new ArrayList<>();
        for (PS2Save.File f : s.files)
            n.add(f.name);
        return n;
    }

    // ---- the card on disk --------------------------------------------------------------------------------------------

    static File card(File userdata) {
        return new File(userdata, "mc0");
    }

    static File gameDir(File userdata) {
        return gameDir(userdata, GAME_DIR);
    }

    /** [lcs] another game's save folder on the card */
    static File gameDir(File userdata, String dir) {
        return new File(card(userdata), encodeName(dir));
    }

    /** A save folder on the card, its files in card order (.ps2x-order, then any others by name); null if none */
    static PS2Save readFolder(File folder) throws IOException {
        if (!folder.isDirectory())
            return null;
        File[] list = folder.listFiles();
        if (list == null)
            return null;
        List<File> files = new ArrayList<>();
        for (File f : list)
            if (f.isFile() && !f.getName().equals(ORDER_FILE))
                files.add(f);
        List<String> order = new ArrayList<>();
        File of = new File(folder, ORDER_FILE);
        if (of.isFile())
            for (String line : new String(Files.readAllBytes(of.toPath()), StandardCharsets.ISO_8859_1).split("\n"))
                order.add(line.endsWith("\r") ? line.substring(0, line.length() - 1) : line);
        files.sort(Comparator.<File>comparingInt(f -> {
            int i = order.indexOf(f.getName());
            return i < 0 ? Integer.MAX_VALUE : i;
        }).thenComparing(File::getName));
        PS2Save s = new PS2Save(decodeName(folder.getName()), "the memory card");
        for (File f : files) {
            if (f.length() > PS2Save.MAX_SAVE)
                throw new IOException(f.getName() + " is larger than a memory card");
            s.files.add(new PS2Save.File(decodeName(f.getName()), Files.readAllBytes(f.toPath()), f.lastModified()));
            s.modified = Math.max(s.modified, f.lastModified());
        }
        return s;
    }

    /** What install() did */
    static final class Installed {
        File folder;
        File backup; // the old save's copy, or null when there was none
        List<String> slots;
    }

    /**
     * Writes the save into userdata/mc0/<its folder>. An existing folder of that name is first copied to
     * userdata/backups/<date-time>/<folder> and then replaced. The new folder is written beside the card first and moved
     * in whole, so a failure leaves the old save in place.
     */
    static Installed install(File userdata, PS2Save save) throws IOException {
        String host = encodeName(save.dir);
        File card = card(userdata);
        File dest = new File(card, host);
        File tmpRoot = new File(userdata, ".import-tmp");
        deleteTree(tmpRoot);
        File tmp = new File(tmpRoot, host);
        if (!tmp.mkdirs())
            throw new IOException("can't make " + tmp);
        List<String> order = new ArrayList<>();
        for (String n : cardOrder(save.dir, names(save)))
            order.add(encodeName(n));
        for (PS2Save.File f : save.files) {
            File out = new File(tmp, encodeName(f.name));
            try (OutputStream o = new FileOutputStream(out)) {
                o.write(f.data);
            }
            long t = f.modified > 0 ? f.modified : save.modified;
            if (t > 0)
                out.setLastModified(t);
        }
        StringBuilder sb = new StringBuilder();
        for (String n : order)
            sb.append(n).append('\n');
        try (OutputStream o = new FileOutputStream(new File(tmp, ORDER_FILE))) {
            o.write(sb.toString().getBytes(StandardCharsets.ISO_8859_1));
        }

        Installed r = new Installed();
        r.folder = dest;
        r.slots = slots(save);
        if (dest.exists()) {
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
            File b = new File(new File(new File(userdata, "backups"), stamp), host);
            for (int i = 2; b.exists(); i++)
                b = new File(new File(new File(userdata, "backups"), stamp + "-" + i), host);
            copyTree(dest, b);
            r.backup = b;
            deleteTree(dest);
            if (dest.exists())
                throw new IOException("can't remove the old " + dest.getName() + " (its copy is in " + b + ")");
        }
        if (!card.isDirectory() && !card.mkdirs())
            throw new IOException("can't make " + card);
        if (!tmp.renameTo(dest)) {
            copyTree(tmp, dest);
        }
        deleteTree(tmpRoot);
        return r;
    }

    static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs())
                throw new IOException("can't make " + to);
            File[] list = from.listFiles();
            if (list != null) {
                Arrays.sort(list);
                for (File f : list)
                    copyTree(f, new File(to, f.getName()));
            }
        } else {
            try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; )
                    out.write(buf, 0, n);
            }
            to.setLastModified(from.lastModified());
        }
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null)
            for (File c : list)
                deleteTree(c);
        f.delete();
    }
}
