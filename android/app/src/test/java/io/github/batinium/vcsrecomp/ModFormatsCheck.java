// [android] A plain-JVM check of the mod importer (ModFormats), no Android or JUnit needed:
//
//   python3 tools/android_mods_check.py [<mod files>...]
//
// Always: the three handling.cfg dialects, weapon.dat lines, a zip and a .vcm built here, slot matching and the
// active.txt lines. The lines below are made up for the test (same columns as the PC files, invented numbers). With
// files: each is imported as the launcher would and what was found is printed. Mods are third-party files: never
// committed.
package io.github.batinium.vcsrecomp;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class ModFormatsCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    // made-up lines in each dialect's column layout
    static final String GTA3 = "STINGER  1111.0 2.0 4.0 1.4 0.0 0.0 -0.2 70 1.1 0.8 0.5 5 222.0 33.0 R P 12.0 0.5 0 31.0 1.5 0.1 0.2 0.5 12345 0.3 -0.1 0.5 2004 1 1";
    static final String VC = "POLICE 1700.0 2.1 4.6 1.6 0.0 0.1 -0.2 75 0.9 0.8 0.5 4 170.0 25.0 4 D 9.5 0.55 1 33.0 1.4 0.12 0.25 0.6 20000 0.25 -0.12 0.45 0.3 4001 1 1";
    static final String SA = "CADRONA 1300.0 2600.0 2.0 0.0 0.1 -0.1 70 0.8 0.85 0.5 5 190.0 12.0 10.0 F P 6.0 0.6 0 33.0 1.2 0.1 0.0 0.3 -0.1 0.5 0.2 0.2 0.5 30000 40000000 1 1 1 0";
    static final String WEAPON = "Python INSTANT_HIT 31.0 260 1001 7 140 -1 -1 -1 -1.0 0.4 0.0 0.1 python 0 7 13 0 7 13 99 99 99 274 -1 1 3";

    public static void main(String[] args) throws Exception {
        // [mods] the slot tables (tools/modslots/mod_slots.txt): args[0] = --slots <path>
        int first = 0;
        if (args.length >= 2 && args[0].equals("--slots")) {
            ModFormats.loadSlots(new String(Files.readAllBytes(new File(args[1]).toPath()), StandardCharsets.UTF_8));
            first = 2;
        }
        ModFormats.useGame("vcs");
        ModFormats.Entry g = ModFormats.parseHandling(GTA3, "t");
        check(g != null && g.dialect.equals("gta3"), "GTA3 line recognised");
        check(g != null && "1111.0".equals(g.fields.get("mass")) && "R".equals(g.fields.get("drive"))
                && "222.0".equals(g.fields.get("maxspeed")) && "12345".equals(g.fields.get("value")), "GTA3 columns");
        check(g != null && !g.fields.containsKey("antidive"), "GTA3 has no anti-dive");
        // [mods3] flags as a VC-layout word with the dialect's mask, lights / ABS / collision passed on
        check(g != null && "0x2004".equals(g.fields.get("flags")) && "0xffff".equals(g.fields.get("flagmask"))
                && "1".equals(g.fields.get("frontlights")) && "0.5".equals(g.fields.get("collision")), "GTA3 flags, lights");
        check(g != null && ModFormats.matchVehicle(g.pcName) == 254, "STINGER -> 254");

        ModFormats.Entry v = ModFormats.parseHandling(VC, "t");
        check(v != null && v.dialect.equals("vc"), "VC line recognised");
        check(v != null && "0.3".equals(v.fields.get("antidive")) && "-0.12".equals(v.fields.get("susplower"))
                && "4".equals(v.fields.get("drive")) && "D".equals(v.fields.get("engine")), "VC columns");
        check(ModFormats.matchVehicle("POLICE") == 207, "POLICE -> 207 (policem)");

        ModFormats.Entry s = ModFormats.parseHandling(SA, "t");
        check(s != null && s.dialect.equals("sa"), "SA line recognised");
        check(s != null && "2600.0".equals(s.fields.get("turnmass")) && "190.0".equals(s.fields.get("maxspeed"))
                && "F".equals(s.fields.get("drive")) && "0.2".equals(s.fields.get("seatoffset")), "SA columns");
        // SA model flags 40000000 = FORCE_GROUND_CLEARANCE (no VC equivalent), handling flags 1 = 1G_BOOST
        check(s != null && "0x1".equals(s.fields.get("flags")) && "2.0".equals(s.fields.get("drag"))
                && "10.0".equals(s.fields.get("inertia")) && s.skipped.toString().contains("FORCE_GROUND_CLEARANCE"),
                "SA flags mapped bit by bit, drag / inertia kept");
        check(ModFormats.matchVehicle("cadrona") == 192 && "similar".equals(ModFormats.match(true, "cadrona")[1]),
                "SA cadrona -> Esperanto, nearest equivalent");
        check(ModFormats.matchVehicle("notacar") == -1, "no match: the player chooses");

        check(ModFormats.parseHandling("; A comment 1 2 3", "t") == null, "comment");
        check(ModFormats.parseHandling("STINGER 1 2 3", "t") == null, "short line");
        check(ModFormats.parseHandling(GTA3.replace(" R P ", " X P "), "t") == null, "bad drive letter");
        check(ModFormats.parseHandling(VC.replace(" 75 ", " -16 "), "t").skipped.toString().contains("submerged"),
                "negative percent submerged skipped");

        ModFormats.Entry w = ModFormats.parseWeapon(WEAPON, "t");
        check(w != null && "140".equals(w.fields.get("damage")) && "7".equals(w.fields.get("clip"))
                && "31.0".equals(w.fields.get("range")), "weapon.dat columns");
        check(w != null && w.target == -1 && ModFormats.matchWeapon(w.pcName) == 20, "Python -> 20");
        check(ModFormats.parseWeapon("Colt45 SOMETHING 1 2 3 4 5 6 7 8 9", "t") == null, "unknown fire type");
        ModFormats.Entry sw = ModFormats.parseWeapon(
                "$ COLT45 INSTANT_HIT 35.0 35.0 346 -1 2 COLT45 17 25 0.10 0.65 0.07 1 2 1.25 1.0 34 39 37 34 39 37 0.0 3000", "t");
        check(sw != null && "sa".equals(sw.dialect) && "17".equals(sw.fields.get("clip")) && "25".equals(sw.fields.get("damage"))
                && "35.0".equals(sw.fields.get("range")) && sw.skill == 1, "SA weapon.dat gun line");
        check(ModFormats.parseWeapon("$ RUSTLER 0.55 0.7 -0.0005 0.0 -0.006 0.003 -0.0005 0.003 -0.001", "t") == null,
                "SA flying handling line is not a weapon");
        // [mods3] carcols.dat with its own colour list -> a colours entry with RGB pairs; without the list, a note only
        ModFormats.Files cf = new ModFormats.Files();
        cf.text.put("carcols.dat", "col\n0,0,0\n255,255,255\n200,20,20\nend\ncar\nkuruma, 2,1, 1,0\nend\n".getBytes(StandardCharsets.ISO_8859_1));
        ModFormats.Parsed cp = ModFormats.parse(cf, "c");
        check(cp.entries.size() == 1 && cp.entries.get(0).kind.equals("colours")
                && "200,20,20,255,255,255".equals(cp.entries.get(0).fields.get("pair1"))
                && "255,255,255,0,0,0".equals(cp.entries.get(0).fields.get("pair2")), "carcols line with a colour list");
        ModFormats.Files cn = new ModFormats.Files();
        cn.text.put("carcols.dat", "kuruma, 2,1, 1,0\n".getBytes(StandardCharsets.ISO_8859_1));
        ModFormats.Parsed cq = ModFormats.parse(cn, "c");
        check(cq.entries.isEmpty() && cq.notes.toString().contains("no colour list"), "carcols line without a colour list");

        // a zip with a readme holding a handling line, and a model the loader can't use
        ByteArrayOutputStream zb = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(zb)) {
            z.putNextEntry(new ZipEntry("My Car/Readme.txt"));
            z.write(("Replace the line:\r\n" + GTA3 + "\r\nstinger, 1,2\r\n").getBytes(StandardCharsets.ISO_8859_1));
            z.putNextEntry(new ZipEntry("My Car/stinger.dff"));
            z.write(new byte[1000]);
            z.putNextEntry(new ZipEntry("../evil/weapon.dat"));
            z.write((WEAPON + "\n").getBytes(StandardCharsets.ISO_8859_1));
        }
        ModFormats.Parsed p = ModFormats.read("my_car.zip", zb.toByteArray());
        check(p.entries.size() == 2, "zip: two entries (" + p.entries.size() + ")");
        check(p.entries.get(0).target == 254 && p.entries.get(1).target == 20, "zip: targets matched");
        check(p.notes.toString().contains("stinger.dff") && p.notes.toString().contains("Colours"), "zip: notes " + p.notes);
        check(ModFormats.directive(p.entries.get(0)).startsWith("handling id=254 mass 1111.0 dimx 2.0 dimy 4.0 comx"),
                "directive " + ModFormats.directive(p.entries.get(0)));
        check(ModFormats.directive(p.entries.get(1)).startsWith("weapon type=20 range 31.0 rate 260"), "weapon directive");

        // a .vcm: header, a stored XML entry and a zlib one
        String xml = "<gtavicemod><info><title>Test Car</title></info><script><command param1=\"%%carhandlingname%%\" "
                + "param2=\" " + VC.substring(7) + "\">car_updatehandling</command></script></gtavicemod>";
        ByteArrayOutputStream vb = new ByteArrayOutputStream();
        vb.write(new byte[] {'v', 'c', 'm', 0});
        vb.write("0123456789ABCDEF0123456789ABCDEF".getBytes(StandardCharsets.ISO_8859_1));
        vb.write(("!VCM.XML!0sf" + xml + "!VCM.XML!ef").getBytes(StandardCharsets.ISO_8859_1));
        Deflater def = new Deflater();
        def.setInput(new byte[5000]);
        def.finish();
        byte[] zz = new byte[6000];
        int zn = def.deflate(zz);
        vb.write("!%%CARFILENAME%%.DFF!1sf".getBytes(StandardCharsets.ISO_8859_1));
        vb.write(zz, 0, zn);
        vb.write("!%%CARFILENAME%%.DFF!ef".getBytes(StandardCharsets.ISO_8859_1));
        ModFormats.Parsed vp = ModFormats.read("test.vcm", vb.toByteArray());
        check("Test Car".equals(vp.title), "vcm title " + vp.title);
        check(vp.entries.size() == 1 && vp.entries.get(0).target == -1 && "4".equals(vp.entries.get(0).fields.get("drive")),
                "vcm handling, no slot");
        check(vp.notes.toString().contains(".DFF"), "vcm model noted");

        ModFormats.Entry st = ModFormats.parseStream("index=347 copy=346", "vcs_stream.txt");
        check(st != null && "stream index=347 copy=346".equals(ModFormats.directive(st)), "stream copy");
        check(ModFormats.parseStream("index=347 file=../x", "t") == null, "stream path refused");

        // [mods] the same names into Liberty City Stories' slots
        ModFormats.useGame("lcs");
        check(ModFormats.matchVehicle("STINGER") == 133, "LCS STINGER -> 133");
        check(ModFormats.matchVehicle("sultan") == 152, "LCS sultan -> Kuruma (similar)");
        check(ModFormats.matchVehicle("LANDSTK") == 131 && "alias".equals(ModFormats.match(true, "LANDSTK")[1]), "LCS key alias");
        check(ModFormats.matchWeapon("Colt45") == 17 && ModFormats.weaponModel(17) == 274, "LCS pistol 17, model 274");
        check(ModFormats.matchWeapon("Desert_Eagle") == 18, "LCS desert eagle -> .357 (similar)");
        ModFormats.Entry lw = ModFormats.parseWeapon(WEAPON, "t");
        ModFormats.autoTarget(lw);
        check(lw.target == 18 && "name".equals(lw.match), "LCS python -> .357 by name " + lw.target + " " + lw.match);
        ModFormats.Parsed gx = ModFormats.read("ENGLISH.GXT", new byte[] {'T', 'A', 'B', 'L', 0, 0, 0, 0});
        check(gx.entries.size() == 1 && "replace disc=TEXT/ENGLISH.GXT file=ENGLISH.GXT".equals(ModFormats.directive(gx.entries.get(0))),
                "gxt replace " + (gx.entries.isEmpty() ? "" : ModFormats.directive(gx.entries.get(0))));
        ModFormats.useGame("vcs");

        // [mods] where the game reads each file: the picked file's own entry (no second copy), kept or in place
        check("My Car/stinger.dff".equals(p.files.origin.get("stinger.dff")) && Integer.valueOf(0).equals(p.files.picked.get("stinger.dff")),
                "zip entry origin " + p.files.origin);
        check("%%CARFILENAME%%.DFF".equals(vp.files.origin.get("%%CARFILENAME%%.DFF")), "vcm entry origin " + vp.files.origin);
        ModFormats.Parsed two = ModFormats.readAll("car", java.util.Arrays.asList("car.dff", "car.txd"),
                java.util.Arrays.asList(new byte[] {0x10, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, new byte[] {0x16, 0, 0, 0}));
        check("".equals(two.files.origin.get("car.txd")) && Integer.valueOf(1).equals(two.files.picked.get("car.txd")),
                "loose files: themselves, by picked index " + two.files.picked);
        ByteArrayOutputStream nb = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(nb)) {
            z.putNextEntry(new ZipEntry("inner.vcm"));
            z.write(vb.toByteArray());
        }
        ModFormats.Parsed nested = ModFormats.read("nested.zip", nb.toByteArray());
        check(nested.files.models.containsKey("%%CARFILENAME%%.DFF") && !nested.files.origin.containsKey("%%CARFILENAME%%.DFF"),
                "a .vcm inside a zip: no origin (taken out into files/)");
        check("a%20b%25c%21d/%C3%A9.dff".equals(ModFormats.enc("a b%c!d/é.dff")), "enc " + ModFormats.enc("a b%c!d/é.dff"));
        check("m/source/pack.zip!Box%20Pack/k.dff".equals(ModFormats.fileValue("m", false, "pack.zip", 0, "Box Pack/k.dff", "k.dff")),
                "copy: the zip in source/");
        check("place:m/2".equals(ModFormats.fileValue("m", true, "k.dff", 2, "", "k.dff")), "in place: the handle");
        check("place:m/0!a%21.txd".equals(ModFormats.fileValue("m", true, "p.zip", 0, "a!.txd", "a_.txd")), "in place: a zip entry");
        check("m/files/k.dff".equals(ModFormats.fileValue("m", false, null, -1, null, "k.dff")), "no source: files/");

        for (int ai = first; ai < args.length; ai++) {
            String a = args[ai];
            File f = new File(a);
            try {
                ModFormats.Parsed m = ModFormats.read(f.getName(), Files.readAllBytes(f.toPath()));
                System.out.println("== " + f.getName() + ": \"" + m.title + "\"");
                for (ModFormats.Entry e : m.entries)
                    System.out.println("   " + e.kind + " " + e.dialect + " " + e.pcName + " -> " + ModFormats.targetName(e)
                            + " | " + ModFormats.directive(e) + " | skipped " + e.skipped);
                for (String n : m.notes)
                    System.out.println("   note: " + n);
            } catch (ModFormats.FormatException e) {
                System.out.println("== " + f.getName() + ": refused: " + e.getMessage());
            }
        }
        System.out.println(checks + " checks, " + failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
