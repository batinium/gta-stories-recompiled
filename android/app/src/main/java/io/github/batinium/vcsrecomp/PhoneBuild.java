// [phonebuild] "Build code pack on this phone" (experimental; docs/PHONE_BUILD.md): the player's disc -> the code pack
// (libvcs_game.so), on the phone, with tools only: the recompiler and the VU1 translator from the APK
// (libps2recomp.so, libvu1recomp.so in nativeLibraryDir), the runtime headers / NDK sysroot / game configs from its
// assets, and Termux's clang + lld downloaded on first use (official packages, SHA-256 pinned in assets/phonebuild/
// termux.txt), unpacked into files/builder/<arch>/ and run through /system/bin/linker64 (an app may not exec() its own
// files; the linker maps them as dlopen does). Every step leaves a marker in files/build/<game>/: a build killed at any
// point goes on where it stopped. Runs on BuildService's thread.
package io.github.batinium.vcsrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.os.StatFs;
import android.system.Os;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class PhoneBuild {
    static final String TAG = "vcs";
    static final String TERMUX = "https://packages-cf.termux.dev/apt/termux-main/";
    static final long NEED_FREE = 1L << 30; // about 1 GB while building (docs/PHONE_BUILD.md, "Disk")
    // the VU1 unit (vu1_compiled.cpp: huge straight-line functions): without the pre-RA scheduler (76% of its time) and
    // without the vectorizers (with them Termux's clang 21 needs ~6 GB for it; without: ~620 MB, 68 s on the emulator)
    static final String[] VU1_FLAGS = {"-mllvm", "-enable-misched=false", "-fno-slp-vectorize", "-fno-vectorize"};

    interface Progress {
        void update(String text, int done, int total);
    }

    static final class Cancelled extends IOException {
        Cancelled() {
            super("cancelled");
        }
    }

    private final Context c;
    private final String game;
    private final DiscCheck.Game info;
    private final Progress progress;
    private final File build, tools, kit, log;
    private volatile boolean cancelled;
    private final List<Process> running = new ArrayList<>();

    PhoneBuild(Context c, String game, Progress progress) {
        this.c = c.getApplicationContext();
        this.game = game;
        this.info = DiscCheck.game(game);
        this.progress = progress;
        this.build = buildDir(c, game);
        this.tools = new File(new File(c.getFilesDir(), "builder"), hostArch());
        this.kit = new File(new File(c.getFilesDir(), "builder"), "kit");
        this.log = new File(build, "build.log");
    }

    static File buildDir(Context c, String game) {
        return new File(new File(c.getFilesDir(), "build"), game);
    }

    static File toolsDir(Context c) {
        return new File(c.getFilesDir(), "builder");
    }

    /** The CPU the compiler runs on: the phone's (aarch64), or x86_64 (an emulator); it always targets aarch64 */
    static String hostArch() {
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "arm64-v8a";
        return abi.startsWith("x86_64") ? "x86_64" : "aarch64";
    }

    void cancel() {
        cancelled = true;
        synchronized (running) {
            for (Process p : running)
                p.destroyForcibly();
        }
    }

    private void check() throws Cancelled {
        if (cancelled)
            throw new Cancelled();
    }

    /** The whole build; returns normally once the pack is installed (files/game/<id>/libvcs_game.so) */
    void run() throws IOException, InterruptedException {
        build.mkdirs();
        long installed = 0; // another install of the APK (even the same version) starts over: its tools may differ
        try {
            installed = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).lastUpdateTime;
        } catch (Exception ignored) {
        }
        String key = BuildConfig.VERSION_CODE + " " + installed + " " + asset("phonebuild/headers_hash.txt").trim() + " " + game;
        File keyFile = new File(build, "key.txt");
        if (keyFile.exists() && !read(keyFile).trim().equals(key)) { // another APK version: start over
            deleteTree(build);
            build.mkdirs();
        }
        write(keyFile, key + "\n");
        if (new StatFs(c.getFilesDir().getPath()).getAvailableBytes() < NEED_FREE / 2 && !new File(build, "units.ok").exists())
            throw new IOException("Needs about 1 GB free in the phone's storage ("
                    + (new StatFs(c.getFilesDir().getPath()).getAvailableBytes() >> 20) + " MB free).");
        logLine("[phonebuild] " + game + " on " + Build.MODEL + ", " + Runtime.getRuntime().availableProcessors()
                + " cores, tools " + hostArch());
        fetchTools();
        unpackKit();
        extractElf();
        recompile();
        writeUnits();
        compileAll();
        link();
        install();
        deleteTree(build);
    }

    // ---------------------------------------------------------------------------------------------- the compiler

    /** termux.txt's packages for this CPU: {name, version, size, sha256, path, licence} */
    static List<String[]> packages(Context c) throws IOException {
        List<String[]> out = new ArrayList<>();
        String arch = hostArch();
        for (String line : asset(c, "phonebuild/termux.txt").split("\n")) {
            String[] f = line.trim().split(" ");
            if (f.length == 7 && !line.startsWith("#") && f[0].equals(arch))
                out.add(Arrays.copyOfRange(f, 1, 7));
        }
        return out;
    }

    static long downloadSize(Context c) {
        long n = 0;
        try {
            for (String[] p : packages(c))
                n += Long.parseLong(p[2]);
        } catch (IOException | RuntimeException ignored) {
        }
        return n;
    }

    static boolean toolsReady(Context c) {
        return new File(new File(toolsDir(c), hostArch()), "ok").exists();
    }

    private void fetchTools() throws IOException {
        File ok = new File(tools, "ok");
        String want = asset("phonebuild/termux.txt");
        if (ok.exists() && read(ok).equals(want))
            return;
        deleteTree(tools);
        tools.mkdirs();
        List<String[]> pkgs = packages(c);
        long total = 0, done = 0;
        for (String[] p : pkgs)
            total += Long.parseLong(p[2]);
        for (String[] p : pkgs) {
            check();
            progress.update("Downloading the compiler: " + p[0] + " " + p[1], (int) (done >> 20), (int) (total >> 20));
            byte[] deb = download(TERMUX + p[4], Long.parseLong(p[2]));
            String sha = hex(MessageDigest256(deb));
            if (!sha.equals(p[3]))
                throw new IOException("The download of " + p[0] + " doesn't match its pinned SHA-256 (got " + sha
                        + "): try again, or update the app.");
            unpackDeb(deb);
            done += deb.length;
            logLine("[tools] " + p[0] + " " + p[1] + " " + (deb.length >> 10) + " KB ok");
        }
        if (!new File(tools, "bin/clang-21").isFile() && clangBinary() == null)
            throw new IOException("The compiler package has no clang.");
        write(ok, want);
    }

    private byte[] download(String url, long size) throws IOException {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setRequestProperty("User-Agent", "GTAStoriesRecomp-phonebuild"); // the CDN refuses some default agents
        h.setConnectTimeout(20000);
        h.setReadTimeout(60000);
        if (h.getResponseCode() != 200)
            throw new IOException("Couldn't download " + url + " (HTTP " + h.getResponseCode()
                    + "): Termux may have updated it; update the app.");
        try (InputStream in = new BufferedInputStream(h.getInputStream())) {
            ByteArrayOutputStream out = new ByteArrayOutputStream((int) Math.max(size, 1 << 16));
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) {
                check();
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        } finally {
            h.disconnect();
        }
    }

    // A .deb: an ar archive; its data.tar.xz holds ./data/data/com.termux/files/usr/... Only the compiler's parts come
    // out (bin/clang-21, bin/lld, lib/*.so*, clang's own headers), under tools/ without the prefix.
    private void unpackDeb(byte[] deb) throws IOException {
        if (deb.length < 8 || !new String(deb, 0, 8, StandardCharsets.US_ASCII).equals("!<arch>\n"))
            throw new IOException("not a .deb");
        int off = 8;
        while (off + 60 <= deb.length) {
            String name = new String(deb, off, 16, StandardCharsets.US_ASCII).trim();
            int size = Integer.parseInt(new String(deb, off + 48, 10, StandardCharsets.US_ASCII).trim());
            int body = off + 60;
            off = body + size + (size & 1);
            if (!name.startsWith("data.tar"))
                continue;
            InputStream raw = new java.io.ByteArrayInputStream(deb, body, size);
            InputStream in = name.contains(".xz") ? new org.tukaani.xz.XZInputStream(raw, -1)
                    : name.contains(".gz") ? new java.util.zip.GZIPInputStream(raw) : raw;
            untar(new BufferedInputStream(in, 1 << 16));
        }
    }

    private static final String PREFIX = "data/data/com.termux/files/usr/";

    private static boolean wanted(String rel) {
        if (rel.equals("bin/clang-21") || rel.equals("bin/lld"))
            return true;
        if (rel.startsWith("lib/") && rel.indexOf('/', 4) < 0 && rel.contains(".so"))
            return true;
        return rel.startsWith("lib/clang/") && rel.contains("/include/");
    }

    private void untar(InputStream in) throws IOException {
        byte[] h = new byte[512];
        String longName = null, longLink = null;
        while (true) {
            if (readFully(in, h) < 512)
                return;
            boolean zero = true;
            for (byte b : h)
                if (b != 0) {
                    zero = false;
                    break;
                }
            if (zero)
                return;
            String name = cstr(h, 0, 100);
            String prefix = cstr(h, 345, 155);
            if (!prefix.isEmpty() && new String(h, 257, 5, StandardCharsets.US_ASCII).equals("ustar"))
                name = prefix + "/" + name;
            long size = Long.parseLong(cstr(h, 124, 12).trim().isEmpty() ? "0" : cstr(h, 124, 12).trim(), 8);
            char type = (char) h[156];
            String link = cstr(h, 157, 100);
            if (type == 'L' || type == 'K' || type == 'x') {
                byte[] data = new byte[(int) size];
                readFully(in, data);
                skip(in, pad(size));
                String text = cstr(data, 0, data.length);
                if (type == 'L')
                    longName = text;
                else if (type == 'K')
                    longLink = text;
                else
                    for (String rec : new String(data, StandardCharsets.UTF_8).split("\n")) {
                        int sp = rec.indexOf(' '), eq = rec.indexOf('=');
                        if (sp > 0 && eq > sp) {
                            String k = rec.substring(sp + 1, eq), v = rec.substring(eq + 1);
                            if (k.equals("path"))
                                longName = v;
                            else if (k.equals("linkpath"))
                                longLink = v;
                        }
                    }
                continue;
            }
            if (longName != null)
                name = longName;
            if (longLink != null)
                link = longLink;
            longName = longLink = null;
            if (name.startsWith("./"))
                name = name.substring(2);
            String rel = name.startsWith(PREFIX) ? name.substring(PREFIX.length()) : null;
            boolean take = rel != null && wanted(rel) && !rel.contains("..");
            File dest = take ? new File(tools, rel) : null;
            if (take && (type == '0' || type == '\0' || type == '7')) {
                dest.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(dest)) {
                    copy(in, out, size);
                }
                dest.setReadable(true, true);
                dest.setExecutable(true, true);
            } else {
                skip(in, size);
            }
            skip(in, pad(size));
            if (take && type == '2') { // a symbolic link: libz.so.1 -> libz.so.1.4 (what the loader asks for)
                dest.getParentFile().mkdirs();
                dest.delete();
                try {
                    Os.symlink(link, dest.getPath());
                } catch (Exception e) {
                    throw new IOException("symlink " + rel + ": " + e.getMessage());
                }
            }
        }
    }

    private File clangBinary() {
        File f = new File(tools, "bin/clang-21");
        return f.isFile() ? f : null;
    }

    private File resourceDir() throws IOException {
        File[] v = new File(tools, "lib/clang").listFiles();
        if (v == null || v.length == 0)
            throw new IOException("no clang resource directory");
        return v[0];
    }

    // ------------------------------------------------------------------------------- headers, sysroot, configs

    private void unpackKit() throws IOException {
        File ok = new File(kit, "ok");
        String want = BuildConfig.VERSION_CODE + " " + asset("phonebuild/headers_hash.txt").trim();
        if (ok.exists() && read(ok).equals(want))
            return;
        progress.update("Unpacking the headers", 0, 0);
        deleteTree(kit);
        kit.mkdirs();
        unzipAsset("phonebuild/runtime.zip", new File(kit, "inc"));
        unzipAsset("phonebuild/sysroot.zip", new File(kit, "sysroot"));
        write(ok, want);
    }

    private void unzipAsset(String name, File dest) throws IOException {
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(c.getAssets().open(name), 1 << 16))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory() || e.getName().contains(".."))
                    continue;
                File f = new File(dest, e.getName());
                f.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(f)) {
                    copy(z, out, Long.MAX_VALUE);
                }
            }
        }
    }

    // ----------------------------------------------------------------------------- disc -> generated code

    private File elf() {
        return new File(build, info.elf);
    }

    private void extractElf() throws IOException {
        if (elf().exists())
            return;
        progress.update("Reading " + info.elf + " from the disc", 0, 0);
        Uri uri = GameFiles.disc(c);
        if (uri == null || !game.equals(GameFiles.discGame(c)))
            throw new IOException("Choose " + info.name + "'s disc first (Disc > Choose disc image).");
        byte[] data;
        try (ParcelFileDescriptor pfd = c.getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream in = new FileInputStream(pfd.getFileDescriptor())) {
            data = DiscCheck.readExecutable(in.getChannel(), info);
        }
        File tmp = new File(build, info.elf + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
        }
        if (!tmp.renameTo(elf()))
            throw new IOException("can't write " + elf());
    }

    private void recompile() throws IOException, InterruptedException {
        File gen = new File(build, "gen");
        File ee = new File(gen, "ee"), vu1 = new File(gen, "vu1/vu1_compiled.cpp");
        if (!new File(build, "recomp.ok").exists()) {
            progress.update("Recompiling " + info.elf + " (MIPS -> C++)", 0, 0);
            deleteTree(ee);
            ee.mkdirs();
            File cfg = new File(build, "game/" + game);
            cfg.mkdirs();
            write(new File(cfg, "functions.csv"), asset("phonebuild/game/" + game + "/functions.csv"));
            String toml = asset("phonebuild/game/" + game + "/" + game + ".toml")
                    .replaceAll("(?m)^input = .*$", "input = \"" + elf().getPath() + "\"")
                    .replaceAll("(?m)^output = .*$", "output = \"" + ee.getPath() + "\"");
            File tomlFile = new File(build, game + ".toml");
            write(tomlFile, toml);
            exec(Arrays.asList(nativeTool("libps2recomp.so"), tomlFile.getPath()), build, "recompiler", false);
            write(new File(build, "recomp.ok"), "");
        }
        if (!new File(build, "vu1.ok").exists()) {
            String uploads = assetOrNull("phonebuild/game/" + game + "/vu1_uploads.txt");
            if (uploads != null) {
                progress.update("Translating the VU1 microcode", 0, 0);
                File up = new File(build, "game/" + game + "/vu1_uploads.txt");
                write(up, uploads);
                vu1.getParentFile().mkdirs();
                exec(Arrays.asList(nativeTool("libvu1recomp.so"), elf().getPath(), vu1.getPath(), up.getPath()), build,
                        "vu1_recomp", false);
            }
            write(new File(build, "vu1.ok"), "");
        }
    }

    private String nativeTool(String name) throws IOException {
        File f = new File(c.getApplicationInfo().nativeLibraryDir, name);
        if (!f.isFile())
            throw new IOException("This APK has no " + name + " (a build without the phone build's tools).");
        return f.getPath();
    }

    // ------------------------------------------------------------------------------------------- compiling

    private File[] units() {
        File[] u = new File(build, "unity").listFiles((d, n) -> n.endsWith(".cpp"));
        if (u == null)
            return new File[0];
        Arrays.sort(u);
        return u;
    }

    // CMake's unity batches: 32 generated files a unit, the VU1 code last
    private void writeUnits() throws IOException {
        if (new File(build, "unity.ok").exists())
            return;
        File unity = new File(build, "unity");
        deleteTree(unity);
        unity.mkdirs();
        String[] names = new File(build, "gen/ee").list((d, n) -> n.endsWith(".cpp"));
        if (names == null || names.length == 0)
            throw new IOException("the recompiler wrote no code");
        Arrays.sort(names);
        List<String> files = new ArrayList<>();
        for (String n : names)
            files.add("gen/ee/" + n);
        int n = 0;
        for (int b = 0; b < files.size(); b += 32, n++) {
            StringBuilder s = new StringBuilder();
            for (int i = b; i < Math.min(files.size(), b + 32); i++)
                s.append("#include \"").append(files.get(i)).append("\"\n");
            write(new File(unity, String.format(Locale.ROOT, "u%03d.cpp", n)), s.toString());
        }
        // the VU1 code: its parts each a unit of its own (they sort after the uNNN units: compiled first)
        File vu1 = new File(build, "gen/vu1/vu1_compiled.cpp");
        if (vu1.exists()) {
            int k = 0;
            for (String f : splitVu1(vu1, VU1_PARTS))
                write(new File(unity, String.format(Locale.ROOT, "v%02d.cpp", k++)), "#include \"" + f + "\"\n");
        }
        write(new File(build, "pch.hxx"), "#pragma clang system_header\n#include \"ps2_runtime_macros.h\"\n"
                + "#include \"ps2_runtime.h\"\n#include \"ps2_recompiled_functions.h\"\n#include \"ps2_recompiled_stubs.h\"\n"
                + "#include \"ps2_syscalls.h\"\n#include \"ps2_stubs.h\"\n");
        write(new File(build, "unity.ok"), "");
    }

    static final int VU1_PARTS = 24;

    /** vu1_compiled.cpp (one translation unit of ~100 huge functions) as several: part files with a share of the
     *  functions each (in namespace vu1c_split, not static) and a main file with their declarations, the tables and
     *  the registrar. One file needs up to ~7 GB with Termux's clang 21 (VCS); the parts compile in parallel. The text
     *  is vu1_recomp's: functions start with "static Vu1CompiledResult " at the line start and run to the next such
     *  line or the next top-level "const " table. Returns the files for the unity batches (relative to build/). */
    private List<String> splitVu1(File vu1, int nparts) throws IOException {
        String[] lines = read(vu1).split("\n", -1);
        int ns = -1;
        for (int i = 0; i + 1 < lines.length; i++)
            if (lines[i].equals("namespace") && lines[i + 1].equals("{")) {
                ns = i;
                break;
            }
        int undef = -1;
        for (int i = lines.length - 1; i >= 0; i--)
            if (lines[i].startsWith("#undef VU1C_EXIT")) {
                undef = i;
                while (undef > 0 && lines[undef - 1].startsWith("#undef"))
                    undef--;
                break;
            }
        if (ns < 0 || undef < 0)
            return java.util.Collections.singletonList("gen/vu1/vu1_compiled.cpp"); // not the expected shape: as is
        String head = String.join("\n", Arrays.copyOfRange(lines, 0, ns)) + "\n";
        String tail = String.join("\n", Arrays.copyOfRange(lines, undef, lines.length));
        List<List<String>> funcs = new ArrayList<>();
        StringBuilder rest = new StringBuilder();
        long totalSize = 0;
        for (int i = ns + 2; i < lines.length; ) {
            if (lines[i].startsWith("static Vu1CompiledResult ")) {
                int j = i + 1;
                while (j < lines.length && !lines[j].startsWith("static Vu1CompiledResult ") && !lines[j].startsWith("const "))
                    j++;
                List<String> f = new ArrayList<>(Arrays.asList(lines).subList(i, j));
                f.set(0, f.get(0).substring("static ".length()));
                for (String l : f)
                    totalSize += l.length() + 1;
                funcs.add(f);
                i = j;
            } else {
                rest.append(lines[i]).append('\n');
                i++;
            }
        }
        StringBuilder decls = new StringBuilder();
        List<StringBuilder> parts = new ArrayList<>();
        for (int k = 0; k < nparts; k++)
            parts.add(new StringBuilder(head).append("namespace vu1c_split\n{\n"));
        long acc = 0;
        for (List<String> f : funcs) {
            int k = (int) Math.min(nparts - 1, acc * nparts / Math.max(1, totalSize));
            for (String l : f) {
                parts.get(k).append(l).append('\n');
                acc += l.length() + 1;
            }
            decls.append(f.get(0)).append(";\n");
        }
        List<String> out = new ArrayList<>();
        File dir = vu1.getParentFile();
        for (int k = 0; k < nparts; k++) {
            write(new File(dir, String.format(Locale.ROOT, "vu1_part%02d.cpp", k)), parts.get(k).append("}\n").append(tail)
                    .append('\n').toString());
            out.add(String.format(Locale.ROOT, "gen/vu1/vu1_part%02d.cpp", k));
        }
        write(new File(dir, "vu1_main.cpp"), head + "namespace vu1c_split\n{\n" + decls + "}\nusing namespace vu1c_split;\n\n"
                + "namespace\n{\n" + rest);
        out.add("gen/vu1/vu1_main.cpp");
        return out;
    }

    // the game library's flags (tools/android.py build-game's CMake line) without -g
    private List<String> flags() throws IOException {
        File inc = new File(kit, "inc");
        return new ArrayList<>(Arrays.asList(
                "--target=aarch64-none-linux-android33", "--sysroot=" + new File(kit, "sysroot"),
                "-resource-dir", resourceDir().getPath(),
                "-DPS2X_ENABLE_IOP_RPC_TRACE=1", "-DPS2_RUNTIME_LOGS=1", "-DSSE2NEON_PRECISE_DIV=1",
                "-DSSE2NEON_PRECISE_DP=1", "-DSSE2NEON_PRECISE_MINMAX=1", "-DSSE2NEON_PRECISE_SQRT=1", "-DUSE_SSE2NEON",
                "-DVCS_HAVE_VULKAN=1", "-Dps2GameLibrary_EXPORTS",
                "-I" + build, "-I" + inc, "-I" + new File(build, "gen/ee"), "-I" + new File(inc, "src"),
                "-I" + new File(inc, "ps2xRuntime/include"), "-I" + new File(inc, "kernel"),
                "-I" + new File(inc, "ps2xIOP/include"),
                "-DANDROID", "-fdata-sections", "-ffunction-sections", "-funwind-tables", "-no-canonical-prefixes",
                "-D_FORTIFY_SOURCE=2", "-std=gnu++20", "-fPIC", "-ffp-contract=off", "-fno-strict-aliasing", "-O2",
                "-DNDEBUG", "-march=armv8.2-a", "-fno-math-errno", "-fno-stack-protector", "-Wno-everything"));
    }

    private List<String> clang(List<String> args) throws IOException {
        File cl = clangBinary();
        if (cl == null)
            throw new IOException("the compiler isn't installed");
        List<String> cmd = new ArrayList<>();
        cmd.add("/system/bin/linker64");
        cmd.add(cl.getPath());
        cmd.addAll(args);
        return cmd;
    }

    private long availMem() {
        ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
        ((ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(m);
        return m.availMem;
    }

    /** Parallel compiles that fit in memory: ~900 MB a clang process (Termux's clang 21), 1.5 GB kept for the system;
     *  halved for every time the system killed this build before (low-memory kills seen on an 8 GB emulator) */
    private int jobs(int kills) {
        long spare = availMem() - (1536L << 20);
        int byMem = (int) Math.max(1, spare / (900L << 20));
        int n = Math.max(1, Math.min(Math.min(8, Runtime.getRuntime().availableProcessors()), byMem));
        return Math.max(1, n >> Math.min(kills, 3));
    }

    private void compileAll() throws IOException, InterruptedException {
        if (new File(build, "units.ok").exists())
            return;
        File obj = new File(build, "obj");
        obj.mkdirs();
        File[] tmp = obj.listFiles((d, n) -> n.endsWith(".tmp"));
        if (tmp != null)
            for (File f : tmp)
                f.delete();
        File pch = new File(build, "pch.pch");
        if (!new File(build, "pch.ok").exists()) {
            progress.update("Compiling the headers", 0, 0);
            List<String> a = flags();
            a.addAll(Arrays.asList("-x", "c++-header", "-Xclang", "-emit-pch", "-o", pch.getPath(),
                    new File(build, "pch.hxx").getPath()));
            exec(clang(a), build, "pch", false);
            write(new File(build, "pch.ok"), "");
        }
        File[] all = units();
        List<File> todo = new ArrayList<>();
        for (int i = all.length - 1; i >= 0; i--) // the last (the VU1 code, the longest) first
            if (!new File(obj, all[i].getName().replace(".cpp", ".o.ok")).exists())
                todo.add(all[i]);
        final int total = all.length;
        final AtomicInteger done = new AtomicInteger(total - todo.size());
        final long t0 = System.currentTimeMillis();
        final int startDone = done.get();
        // a build that was running when the process died (the low-memory killer, mostly): fewer jobs this time
        File running = new File(build, "compiling.txt");
        int kills = 0;
        try {
            kills = running.exists() ? Integer.parseInt(read(running).trim()) + 1 : 0;
        } catch (NumberFormatException ignored) {
        }
        write(running, kills + "\n");
        if (kills >= 4) {
            running.delete();
            throw new IOException("The system stopped the build " + kills + " times (not enough free memory). Close other "
                    + "apps and try again, or build the code pack on a PC (Player's guide).");
        }
        int jobs = jobs(kills);
        // a unit starts only while 700 MB are free
        final java.util.concurrent.Semaphore room = new java.util.concurrent.Semaphore(jobs);
        logLine("[build] " + todo.size() + " of " + total + " units to compile, " + jobs + " jobs"
                + (kills > 0 ? " (after " + kills + " stop" + (kills > 1 ? "s" : "") + ")" : ""));
        ExecutorService pool = Executors.newFixedThreadPool(jobs);
        PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
        List<Future<?>> futures = new ArrayList<>();
        for (File u : todo) {
            futures.add(pool.submit(() -> {
                while (pm.getCurrentThermalStatus() >= PowerManager.THERMAL_STATUS_SEVERE && !cancelled) {
                    progress.update("Paused: the phone is too hot", done.get(), total);
                    Thread.sleep(10000);
                }
                check();
                boolean vu1 = read(u).contains("gen/vu1/");
                int weight = 1; // the VU1 parts are small since the split
                room.acquire(weight);
                try {
                    while (availMem() < (700L << 20) && !cancelled)
                        Thread.sleep(2000);
                    compileUnit(u, obj, pch, vu1);
                } finally {
                    room.release(weight);
                }
                int n = done.incrementAndGet();
                long el = System.currentTimeMillis() - t0;
                int fresh = n - startDone;
                String eta = fresh >= 4 ? String.format(Locale.ROOT, ", about %d min left",
                        Math.max(1, el * (total - n) / fresh / 60000)) : "";
                progress.update("Compiling: " + n + " of " + total + " parts" + eta, n, total);
                return null;
            }));
        }
        pool.shutdown();
        try {
            for (Future<?> f : futures)
                f.get();
        } catch (java.util.concurrent.ExecutionException e) {
            cancel();
            Throwable t = e.getCause();
            if (t instanceof IOException)
                throw (IOException) t;
            throw new IOException(t.toString());
        } finally {
            pool.shutdownNow();
        }
        write(new File(build, "units.ok"), "");
        running.delete();
        deleteTree(new File(build, "gen/ee")); // the generated code is in the objects now (keep the headers? no need)
        deleteTree(new File(build, "gen/vu1"));
        new File(build, "pch.pch").delete();
    }

    private void compileUnit(File u, File obj, File pch, boolean vu1) throws IOException, InterruptedException {
        String base = u.getName().replace(".cpp", "");
        File o = new File(obj, base + ".o"), part = new File(obj, base + ".o.tmp");
        List<String> a = flags();
        if (vu1)
            a.addAll(Arrays.asList(VU1_FLAGS));
        a.addAll(Arrays.asList("-Xclang", "-include-pch", "-Xclang", pch.getPath(), "-c", u.getPath(), "-o", part.getPath()));
        long s = System.currentTimeMillis();
        exec(clang(a), build, base, true);
        if (!part.renameTo(o))
            throw new IOException("can't write " + o);
        write(new File(obj, base + ".o.ok"), "");
        logLine("[unit] " + base + " " + (System.currentTimeMillis() - s) / 1000 + "s");
    }

    private File pack() {
        return new File(build, GameFiles.GAME_LIBRARY);
    }

    // the stamp (game_library_stamp.cpp: this APK's headers hash and the game), then lld as clang's driver runs it
    private void link() throws IOException, InterruptedException {
        if (new File(build, "link.ok").exists())
            return;
        progress.update("Linking the code pack", 0, 0);
        File obj = new File(build, "obj");
        File inc = new File(kit, "inc");
        List<String> a = flags();
        a.addAll(Arrays.asList("-DVCS_GAME_HEADERS_HASH=\"" + asset("phonebuild/headers_hash.txt").trim() + "\"",
                "-DVCS_GAME_ID=\"" + game + "\"", "-c", new File(inc, "src/android/game_library_stamp.cpp").getPath(),
                "-o", new File(obj, "stamp.o").getPath()));
        exec(clang(a), build, "stamp", false);
        File sys = new File(kit, "sysroot");
        String lib = new File(sys, "usr/lib/aarch64-linux-android").getPath(), lib33 = lib + "/33";
        String builtins = new File(sys, "usr/lib/aarch64-linux-android/libclang_rt.builtins-aarch64-android.a").getPath();
        String nat = c.getApplicationInfo().nativeLibraryDir;
        List<String> l = new ArrayList<>(Arrays.asList("/system/bin/linker64", new File(tools, "bin/lld").getPath(),
                "-flavor", "gnu", "--sysroot=" + sys, "-EL", "--fix-cortex-a53-843419", "-z", "now", "-z", "relro",
                "-z", "max-page-size=16384", "--use-android-relr-tags", "--pack-dyn-relocs=relr", "--hash-style=gnu",
                "--eh-frame-hdr", "-m", "aarch64linux", "-shared", "-o", pack().getPath(), lib33 + "/crtbegin_so.o",
                "-L" + lib33, "-L" + lib, "--build-id=sha1", "--no-undefined", "--gc-sections", "-soname",
                GameFiles.GAME_LIBRARY, "--strip-debug"));
        String[] objs = obj.list((d, n) -> n.endsWith(".o"));
        Arrays.sort(objs);
        for (String o : objs)
            l.add(new File(obj, o).getPath());
        l.addAll(Arrays.asList(nat + "/libmain.so", nat + "/libc++_shared.so", "-lm", builtins, "-l:libunwind.a",
                "-ldl", "-lc", builtins, "-l:libunwind.a", "-ldl", lib33 + "/crtend_so.o"));
        exec(l, build, "link", false);
        write(new File(build, "link.ok"), "");
    }

    // the same checks and place as the launcher's import (LauncherActivity.importLibrary)
    private void install() throws IOException {
        File f = pack();
        String error = GameFiles.checkLibrary(f);
        if (error == null)
            error = GameFiles.stampProblem(f);
        if (error == null && !game.equals(GameFiles.libraryFileGame(f)))
            error = "The built pack names another game (" + GameFiles.libraryFileGame(f) + ").";
        if (error != null)
            throw new IOException("The built pack was refused: " + error);
        File dest = GameFiles.gameLibrary(c, game);
        dest.getParentFile().mkdirs();
        GameFiles.loadError(c).delete();
        if (dest.exists()) {
            dest.setWritable(true);
            dest.delete();
        }
        if (!f.renameTo(dest) || !dest.setReadOnly())
            throw new IOException("Couldn't store the pack in the app's folder.");
        logLine("[phonebuild] installed " + dest + " (" + (dest.length() >> 20) + " MB)");
    }

    // ----------------------------------------------------------------------------------------------- helpers

    private void exec(List<String> cmd, File dir, String what, boolean tracked) throws IOException, InterruptedException {
        check();
        ProcessBuilder pb = new ProcessBuilder(cmd).directory(dir).redirectErrorStream(true);
        if (cmd.get(0).equals("/system/bin/linker64")) // the compiler's libraries; not for the APK's own tools (an
            pb.environment().put("LD_LIBRARY_PATH", new File(tools, "lib").getPath()); // emulator's ARM translator breaks)
        File out = new File(build, "log-" + what + ".txt");
        pb.redirectOutput(out);
        Process p = pb.start();
        synchronized (running) {
            running.add(p);
        }
        int rc;
        try {
            rc = p.waitFor();
        } finally {
            synchronized (running) {
                running.remove(p);
            }
        }
        check();
        if (rc != 0) {
            String tail = tail(out, 1500);
            logLine("[" + what + "] failed (" + rc + "): " + tail);
            throw new IOException(what + " failed (exit " + rc + "): " + tail);
        }
        out.delete();
    }

    synchronized void logLine(String s) {
        Log.i(TAG, s);
        try (OutputStream o = new FileOutputStream(log, true)) {
            o.write((s + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    private String asset(String name) throws IOException {
        return asset(c, name);
    }

    private String assetOrNull(String name) {
        try {
            return asset(c, name);
        } catch (IOException e) {
            return null;
        }
    }

    static String asset(Context c, String name) throws IOException {
        try (InputStream in = c.getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            copy(in, out, Long.MAX_VALUE);
            return out.toString("UTF-8");
        }
    }

    static String read(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            copy(in, out, Long.MAX_VALUE);
            return out.toString("UTF-8");
        }
    }

    static void write(File f, String s) throws IOException {
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String tail(File f, int n) {
        try {
            String s = read(f).trim();
            return s.length() > n ? "…" + s.substring(s.length() - n) : s;
        } catch (IOException e) {
            return "";
        }
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists() && !isLink(f))
            return;
        if (f.isDirectory() && !isLink(f)) {
            File[] k = f.listFiles();
            if (k != null)
                for (File x : k)
                    deleteTree(x);
        }
        f.setWritable(true);
        f.delete();
    }

    private static boolean isLink(File f) {
        try {
            return android.system.Os.lstat(f.getPath()).st_mode != 0
                    && android.system.OsConstants.S_ISLNK(android.system.Os.lstat(f.getPath()).st_mode);
        } catch (Exception e) {
            return false;
        }
    }

    static long treeSize(File f) {
        if (isLink(f))
            return 0;
        if (f.isFile())
            return f.length();
        long n = 0;
        File[] k = f.listFiles();
        if (k != null)
            for (File x : k)
                n += treeSize(x);
        return n;
    }

    private static void copy(InputStream in, OutputStream out, long max) throws IOException {
        byte[] buf = new byte[1 << 16];
        while (max > 0) {
            int n = in.read(buf, 0, (int) Math.min(buf.length, max));
            if (n < 0) {
                if (max != Long.MAX_VALUE)
                    throw new IOException("truncated archive");
                return;
            }
            out.write(buf, 0, n);
            if (max != Long.MAX_VALUE)
                max -= n;
        }
    }

    private static int readFully(InputStream in, byte[] b) throws IOException {
        int got = 0;
        while (got < b.length) {
            int n = in.read(b, got, b.length - got);
            if (n < 0)
                break;
            got += n;
        }
        return got;
    }

    private static void skip(InputStream in, long n) throws IOException {
        byte[] buf = new byte[8192];
        while (n > 0) {
            int r = in.read(buf, 0, (int) Math.min(buf.length, n));
            if (r < 0)
                throw new IOException("truncated archive");
            n -= r;
        }
    }

    private static long pad(long size) {
        return (512 - size % 512) % 512;
    }

    private static String cstr(byte[] b, int at, int len) {
        int end = at;
        while (end < at + len && end < b.length && b[end] != 0)
            end++;
        return new String(b, at, end - at, StandardCharsets.UTF_8);
    }

    private static byte[] MessageDigest256(byte[] data) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(data);
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        }
    }

    private static String hex(byte[] h) {
        StringBuilder s = new StringBuilder();
        for (byte x : h)
            s.append(String.format(Locale.ROOT, "%02x", x));
        return s.toString();
    }
}
