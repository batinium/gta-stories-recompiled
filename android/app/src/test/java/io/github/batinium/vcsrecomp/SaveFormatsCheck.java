// [android] A plain-JVM check of the save import / export code (PS2Save, Lzari, SaveCard), no Android or JUnit needed:
//
//   python3 tools/android_saves_check.py [<saves in> <expected out>]
//
// Always: card-name encoding, region messages, .psu / .zip export -> import round trips, install into a temporary card
// (order file, backup of a replaced save) and export from it. With the two folders: every .max / .cbs / .psu / .psv /
// .zip in <saves in> is decoded and each file compared byte for byte with <expected out>/<id>/<save folder>/<file>,
// where <id> is the second dot-separated part of the input's name ("game.14651.max" -> 14651), as unpacked by a
// reference tool (mymcplus). The sample saves are third-party files: they are never committed.
package io.github.batinium.vcsrecomp;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class SaveFormatsCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    private static void same(PS2Save a, PS2Save b, String what) {
        check(a.dir.equals(b.dir), what + ": folder " + a.dir + " vs " + b.dir);
        check(a.files.size() == b.files.size(), what + ": " + a.files.size() + " vs " + b.files.size() + " files");
        for (PS2Save.File f : a.files) {
            PS2Save.File g = b.file(f.name);
            check(g != null && Arrays.equals(f.data, g.data), what + ": " + f.name);
        }
    }

    private static PS2Save sample() {
        PS2Save s = new PS2Save(SaveCard.GAME_DIR, "test");
        byte[] big = new byte[98304];
        for (int i = 0; i < big.length; i++)
            big[i] = (byte) (i * 31 + 7);
        s.files.add(new PS2Save.File("2 Kill Phil: Part 2", big.clone(), 1_700_000_000_000L));
        big[5] ^= 1;
        s.files.add(new PS2Save.File("1 Soldier", big, 1_700_000_100_000L));
        s.files.add(new PS2Save.File(SaveCard.GAME_DIR, new byte[179350], 1_700_000_000_000L));
        s.files.add(new PS2Save.File("vcsicon.ico", new byte[49990], 1_700_000_000_000L));
        s.files.add(new PS2Save.File("icon.sys", new byte[964], 1_700_000_000_000L));
        s.files.add(new PS2Save.File("10 Last", new byte[1025], 0));
        return s;
    }

    private static void unitChecks() throws Exception {
        check(SaveCard.encodeName("1 Kill Phil: Part 2").equals("1 Kill Phil%3A Part 2"), "encode ':'");
        check(SaveCard.encodeName("a. ").equals("a%2E%20"), "encode trailing dot / space");
        check(SaveCard.encodeName("con.txt").equals("%63on.txt"), "encode device name");
        check(SaveCard.encodeName("50%").equals("50%25"), "encode '%'");
        for (String n : new String[] {"1 Kill Phil: Part 2", "a. ", "con.txt", "50%", "xéy", SaveCard.GAME_DIR})
            check(SaveCard.decodeName(SaveCard.encodeName(n)).equals(n), "decode(encode(" + n + "))");

        check(SaveCard.regionProblem(SaveCard.GAME_DIR) == null, "US save accepted");
        String eu = SaveCard.regionProblem("BESLES-54622GTA40001");
        check(eu != null && eu.contains("Europe"), "EU save refused: " + eu);
        String psp = SaveCard.regionProblem("ULUS10160GTA0001");
        check(psp != null && psp.contains("PSP"), "PSP save refused: " + psp);
        String jp = SaveCard.regionProblem("BISLPM-66817GTA");
        check(jp != null && jp.contains("Japan"), "JP save refused: " + jp);
        System.out.println("region messages:\n  " + eu + "\n  " + psp + "\n  " + jp);

        PS2Save s = sample();
        List<String> names = new ArrayList<>();
        for (PS2Save.File f : s.files)
            names.add(f.name);
        List<String> order = SaveCard.cardOrder(s.dir, names);
        check(order.equals(Arrays.asList("icon.sys", "vcsicon.ico", SaveCard.GAME_DIR, "1 Soldier", "2 Kill Phil: Part 2",
                "10 Last")), "card order " + order);

        same(s, PS2Save.read(s.toPsu(), "t.psu").get(0), "psu round trip");
        same(s, PS2Save.read(s.toZip(), "t.zip").get(0), "zip round trip");
        PS2Save viaPsu = PS2Save.read(s.toPsu(), "t.psu").get(0);
        check(viaPsu.file("1 Soldier").modified == 1_700_000_100_000L, "psu keeps the time");

        // a zip of .psu files, as people share them
        java.io.ByteArrayOutputStream zb = new java.io.ByteArrayOutputStream();
        try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(zb)) {
            z.putNextEntry(new java.util.zip.ZipEntry("readme.txt"));
            z.write("hi".getBytes(StandardCharsets.US_ASCII));
            z.putNextEntry(new java.util.zip.ZipEntry("saves/vcs.psu"));
            z.write(s.toPsu());
        }
        same(s, SaveCard.choose(PS2Save.read(zb.toByteArray(), "t.zip")), "psu in a zip");

        // install into a temporary card, twice (the second replaces the first and backs it up), then export
        File userdata = Files.createTempDirectory("vcs-saves").toFile();
        try {
            SaveCard.Installed r1 = SaveCard.install(userdata, s);
            check(r1.backup == null, "first install: no backup");
            String of = new String(Files.readAllBytes(new File(r1.folder, SaveCard.ORDER_FILE).toPath()), StandardCharsets.ISO_8859_1);
            check(of.equals("icon.sys\nvcsicon.ico\n" + SaveCard.GAME_DIR + "\n1 Soldier\n2 Kill Phil%3A Part 2\n10 Last\n"),
                    ".ps2x-order:\n" + of);
            check(new File(r1.folder, "2 Kill Phil%3A Part 2").isFile(), "host name encoded");
            check(r1.slots.equals(Arrays.asList("1 Soldier", "2 Kill Phil: Part 2", "10 Last")), "slots " + r1.slots);
            PS2Save back = SaveCard.readFolder(SaveCard.gameDir(userdata));
            same(s, back, "card -> export");
            check(back.files.get(0).name.equals("icon.sys") && back.files.get(3).name.equals("1 Soldier"), "export in card order");
            same(s, PS2Save.read(back.toPsu(), "e.psu").get(0), "card -> .psu -> import");
            same(s, PS2Save.read(back.toZip(), "e.zip").get(0), "card -> .zip -> import");

            PS2Save s2 = sample();
            s2.files.remove(s2.file("10 Last"));
            SaveCard.Installed r2 = SaveCard.install(userdata, s2);
            check(r2.backup != null && new File(r2.backup, "10 Last").isFile()
                    && new File(r2.backup, SaveCard.ORDER_FILE).isFile(), "replaced save backed up: " + r2.backup);
            check(!new File(r2.folder, "10 Last").exists(), "replaced, not merged");
            check(!new File(userdata, ".import-tmp").exists(), "temporary folder removed");
            same(s2, SaveCard.readFolder(r2.folder), "second install");
        } finally {
            SaveCard.deleteTree(userdata);
        }
    }

    private static void samples(File in, File expected) throws Exception {
        File[] list = in.listFiles();
        Arrays.sort(list);
        for (File f : list) {
            if (!f.isFile() || f.getName().startsWith("."))
                continue;
            String[] parts = f.getName().split("\\.");
            String id = parts.length >= 3 ? parts[parts.length - 2] : parts[0];
            long t0 = System.nanoTime();
            PS2Save s = SaveCard.choose(PS2Save.read(Files.readAllBytes(f.toPath()), f.getName()));
            long ms = (System.nanoTime() - t0) / 1_000_000;
            File dir = new File(new File(expected, id), s.dir);
            String[] want = dir.list();
            check(want != null && want.length == s.files.size(), f.getName() + ": file count " + s.files.size() + " vs "
                    + (want == null ? "no folder " + dir : want.length));
            int okFiles = 0;
            for (PS2Save.File x : s.files) {
                File e = new File(dir, x.name);
                boolean ok = e.isFile() && Arrays.equals(Files.readAllBytes(e.toPath()), x.data);
                check(ok, f.getName() + ": " + x.name + " differs from " + e);
                okFiles += ok ? 1 : 0;
            }
            same(s, PS2Save.read(s.toPsu(), "x.psu").get(0), f.getName() + " -> .psu -> import");
            same(s, PS2Save.read(s.toZip(), "x.zip").get(0), f.getName() + " -> .zip -> import");
            System.out.println(f.getName() + " [" + s.format + ", " + ms + " ms]: " + s.dir + ", " + okFiles + "/"
                    + s.files.size() + " files identical, slots " + SaveCard.slots(s)
                    + (SaveCard.regionProblem(s.dir) == null ? "" : " REFUSED"));
        }
    }

    public static void main(String[] args) throws Exception {
        unitChecks();
        if (args.length >= 2)
            samples(new File(args[0]), new File(args[1]));
        System.out.println((failures == 0 ? "OK" : failures + " FAILED") + ": " + checks + " checks");
        System.exit(failures == 0 ? 0 : 1);
    }
}
