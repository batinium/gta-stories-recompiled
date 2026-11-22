// [android] SaveBackup on the PC's JVM: back up two games' cards, restore into fresh folders, refuse a stranger's zip.
package io.github.batinium.vcsrecomp;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class SaveBackupCheck {
    static int fails;

    static void check(boolean ok, String what) {
        if (!ok) {
            fails++;
            System.out.println("FAIL " + what);
        }
    }

    static void put(File f, String text) throws Exception {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes("UTF-8"));
    }

    public static void main(String[] a) throws Exception {
        File root = Files.createTempDirectory("savebackup").toFile();
        File[] src = {new File(root, "a/userdata"), new File(root, "a/userdata-lcs")};
        put(new File(src[0], "mc0/BASLUS-21590GTA40001/1 Soldier"), "vcs slot");
        put(new File(src[1], "mc0/BASLUS-21423GTA30001/1: Intro"), "lcs slot");
        check(SaveBackup.hasSaves(src[0]) && SaveBackup.hasSaves(src[1]), "hasSaves");
        byte[] zip = SaveBackup.backup(src);
        check(zip != null, "backup made");
        int[] n = SaveBackup.inspect(zip);
        check(n[0] == 1 && n[1] == 1, "inspect counts " + n[0] + "," + n[1]);

        File[] dst = {new File(root, "b/userdata"), new File(root, "b/userdata-lcs")};
        put(new File(dst[0], "mc0/BASLUS-21590GTA40001/1 Soldier"), "old vcs");
        put(new File(dst[0], "mc0/OTHER/keep"), "stay");
        SaveBackup.restore(zip, dst);
        check(new String(Files.readAllBytes(new File(dst[0], "mc0/BASLUS-21590GTA40001/1 Soldier").toPath())).equals("vcs slot"), "vcs restored");
        check(new String(Files.readAllBytes(new File(dst[1], "mc0/BASLUS-21423GTA30001/1: Intro").toPath())).equals("lcs slot"), "lcs restored");
        check(new File(dst[0], "mc0/OTHER/keep").isFile(), "folders not in the backup stay");
        File[] b = new File(dst[0], "backups").listFiles();
        check(b != null && b.length == 1, "old save copied to backups");
        check(!new File(dst[0], ".restore-tmp").exists(), "temp removed");
        check(SaveBackup.backup(new File[] {new File(root, "none1"), new File(root, "none2")}) == null, "nothing to back up");

        ByteArrayOutputStream bad = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(bad)) {
            z.putNextEntry(new ZipEntry("vcs/mc0/../../evil/x"));
            z.write(1);
            z.closeEntry();
            z.putNextEntry(new ZipEntry("something.txt"));
            z.write(1);
            z.closeEntry();
        }
        try {
            SaveBackup.inspect(bad.toByteArray());
            check(false, "stranger zip refused");
        } catch (java.io.IOException e) {
            check(true, "");
        }
        System.out.println(fails == 0 ? "SaveBackup ok" : fails + " failures");
        System.exit(fails == 0 ? 0 : 1);
    }
}
