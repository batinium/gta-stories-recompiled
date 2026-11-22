// [android] One-file backup of every save, both games: a .zip with vcs/mc0/... and lcs/mc0/... (the memory card folders as
// they are on disk), and its restore. SavesMenu has the rows. Pure file work (no UI), so the PC JVM can test it
// (tools/android_gpu_check.py).
package io.github.batinium.vcsrecomp;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

final class SaveBackup {
    private SaveBackup() {
    }

    static final String[] GAMES = {"vcs", "lcs"};
    static final int MAX_BACKUP = 64 << 20;

    /** True when the game's card folder holds at least one file */
    static boolean hasSaves(File dataDir) {
        return count(new File(dataDir, "mc0")) > 0;
    }

    private static int count(File f) {
        if (f.isFile())
            return 1;
        int n = 0;
        File[] list = f.listFiles();
        if (list != null)
            for (File c : list)
                n += count(c);
        return n;
    }

    /** Every save of both games (dataDirs[i] belongs to GAMES[i]) as a zip; null when there is nothing to back up */
    static byte[] backup(File[] dataDirs) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        int files = 0;
        try (ZipOutputStream z = new ZipOutputStream(bytes, StandardCharsets.UTF_8)) {
            for (int i = 0; i < GAMES.length; i++)
                files += add(z, new File(dataDirs[i], "mc0"), GAMES[i] + "/mc0");
            z.putNextEntry(new ZipEntry("README.txt"));
            z.write(("Backup of the memory cards of GTA Stories Recompiled (Android): vcs/mc0 and lcs/mc0.\n"
                    + "Restore it in the launcher: Saves > Restore a backup.\n").getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
        }
        return files == 0 ? null : bytes.toByteArray();
    }

    private static int add(ZipOutputStream z, File f, String name) throws IOException {
        if (f.isDirectory()) {
            int n = 0;
            File[] list = f.listFiles();
            if (list != null) {
                Arrays.sort(list);
                for (File c : list)
                    n += add(z, c, name + "/" + c.getName());
            }
            return n;
        }
        if (!f.isFile())
            return 0;
        ZipEntry e = new ZipEntry(name);
        e.setTime(f.lastModified());
        z.putNextEntry(e);
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; )
                z.write(buf, 0, n);
        }
        z.closeEntry();
        return 1;
    }

    /** What a backup zip holds, for the confirmation: how many files per game (null/IOException when it is no backup) */
    static int[] inspect(byte[] zip) throws IOException {
        int[] n = new int[GAMES.length];
        try (ZipInputStream z = new ZipInputStream(new ByteArrayInputStream(zip), StandardCharsets.UTF_8)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                int g = game(e.getName());
                if (g >= 0 && !e.isDirectory())
                    n[g]++;
            }
        }
        if (Arrays.stream(n).sum() == 0)
            throw new IOException("This is not a backup made by this app (no vcs/mc0 or lcs/mc0 folder inside).");
        return n;
    }

    /** Index into GAMES of an entry under "<game>/mc0/<folder>/<file>", else -1; names with .. or absolute are never ok */
    private static int game(String name) {
        if (name.startsWith("/") || name.contains("..") || name.contains("\\"))
            return -1;
        String[] p = name.split("/");
        if (p.length < 4 || !p[1].equals("mc0"))
            return -1;
        for (int i = 0; i < GAMES.length; i++)
            if (GAMES[i].equals(p[0]))
                return i;
        return -1;
    }

    /**
     * Puts the backup's save folders onto the cards. Each folder in the backup replaces the same one on the card; the
     * replaced folders are copied to <data dir>/backups/<date-time>/ first. Folders not in the backup stay.
     * Written beside the card first, then moved in, so a bad zip leaves the cards as they were.
     * @return the files restored per game
     */
    static int[] restore(byte[] zip, File[] dataDirs) throws IOException {
        int[] n = inspect(zip);
        File[] tmp = new File[GAMES.length];
        try {
            for (int i = 0; i < GAMES.length; i++)
                if (n[i] > 0) {
                    tmp[i] = new File(dataDirs[i], ".restore-tmp");
                    SaveCard.deleteTree(tmp[i]);
                }
            try (ZipInputStream z = new ZipInputStream(new ByteArrayInputStream(zip), StandardCharsets.UTF_8)) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    int g = game(e.getName());
                    if (g < 0 || e.isDirectory())
                        continue;
                    String[] p = e.getName().split("/", 3); // game, mc0, folder/file
                    File out = new File(tmp[g], p[2]);
                    File parent = out.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs())
                        throw new IOException("can't make " + parent);
                    try (OutputStream o = new FileOutputStream(out)) {
                        byte[] buf = new byte[1 << 16];
                        for (int r; (r = z.read(buf)) > 0; )
                            o.write(buf, 0, r);
                    }
                    if (e.getTime() > 0)
                        out.setLastModified(e.getTime());
                }
            }
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
            for (int i = 0; i < GAMES.length; i++) {
                if (tmp[i] == null)
                    continue;
                File card = new File(dataDirs[i], "mc0");
                if (!card.isDirectory() && !card.mkdirs())
                    throw new IOException("can't make " + card);
                File[] folders = tmp[i].listFiles();
                if (folders == null)
                    continue;
                for (File f : folders) {
                    File dest = new File(card, f.getName());
                    if (dest.exists()) {
                        File b = new File(new File(new File(dataDirs[i], "backups"), "restore-" + stamp), f.getName());
                        SaveCard.copyTree(dest, b);
                        SaveCard.deleteTree(dest);
                        if (dest.exists())
                            throw new IOException("can't replace " + dest.getName() + " (its copy is in " + b + ")");
                    }
                    if (!f.renameTo(dest))
                        SaveCard.copyTree(f, dest);
                }
            }
        } finally {
            for (File t : tmp)
                if (t != null)
                    SaveCard.deleteTree(t);
        }
        return n;
    }

    /** A name for the backup file */
    static String fileName() {
        return "GTA Stories backup " + new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.ROOT).format(new Date()) + ".zip";
    }

    static List<String> describe(int[] n) {
        List<String> out = new ArrayList<>();
        if (n[0] > 0)
            out.add("Vice City Stories (" + n[0] + " files)");
        if (n[1] > 0)
            out.add("Liberty City Stories (" + n[1] + " files)");
        return out;
    }
}
