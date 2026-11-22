// [android] Custom GPU drivers (docs/ANDROID.md "Custom GPU drivers", docs/ANDROID_LAUNCHER.md): a Vulkan driver package
// the player downloaded themselves (e.g. Mesa Turnip for Adreno, as packaged for emulators: a .zip with meta.json and
// libvulkan_freedreno.so) imported here, then loaded by the game through libadrenotools (src/android/gpu_driver.cpp).
// Qualcomm Adreno only: adrenotools hooks Qualcomm's driver loading; Mali / PowerVR / Xclipse phones cannot use it.
//
// The app ships no driver. Import validates the package (meta.json's fields, every .so an ARM64 ELF, plain file names)
// and extracts meta.json and the .so files only to files/gpu_drivers/<name>/; nothing in it is run by the launcher.
//
// Safety: VCSActivity (env) writes files/gpu_drivers/attempt.txt when it starts the game with a custom driver. The
// launcher's next resume (checkLastRun) reads the game's log: no "[gpu-driver] confirmed" line (the driver did not load
// or the device could not be made), a device lost (files/gpu_lost.marker) or a crash -> the system driver is selected
// again and the player is told. One try per selection: it never loops.
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class GpuDriver {
    private GpuDriver() {
    }

    static final String SYSTEM = "System driver (default)";
    static final String ATTEMPT = "attempt.txt";
    private static final long MAX_TOTAL = 256L << 20; // a Turnip package is ~10-30 MB unpacked
    private static final int MAX_ENTRIES = 64;

    /** meta.json of an installed or imported package */
    static final class Meta {
        String name, description, author, vendor, driverVersion, libraryName;
        int minApi;
        boolean hardwareBufferFeature; // optional in the format; read for the record, not used
        String dir; // the folder under files/gpu_drivers

        String label() {
            return name + (driverVersion != null && !driverVersion.isEmpty() ? " · " + driverVersion : "");
        }
    }

    static File root(Context c) {
        return new File(c.getFilesDir(), "gpu_drivers");
    }

    private static android.content.SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("gpu_driver", Context.MODE_PRIVATE);
    }

    // ---- which phones ------------------------------------------------------------------------------------------------

    /** True on a Qualcomm chip (Build.SOC_MANUFACTURER "QTI" / "Qualcomm") or an Adreno GPU in the last run's log */
    static boolean supported(Context c) {
        String soc = Build.SOC_MANUFACTURER == null ? "" : Build.SOC_MANUFACTURER.toLowerCase(Locale.ROOT);
        if (soc.contains("qti") || soc.contains("qualcomm"))
            return true;
        String gpu = DeviceNotice.gpuName(c);
        return gpu != null && gpu.toLowerCase(Locale.ROOT).contains("adreno");
    }

    // ---- the installed packages and the choice -----------------------------------------------------------------------

    static List<Meta> installed(Context c) {
        List<Meta> out = new ArrayList<>();
        File[] dirs = root(c).listFiles();
        if (dirs == null)
            return out;
        java.util.Arrays.sort(dirs);
        for (File d : dirs) {
            if (!d.isDirectory() || d.getName().startsWith(".") || d.getName().equals("tmp") || d.getName().equals("redirect"))
                continue;
            try {
                Meta m = parseMeta(new String(Files.readAllBytes(new File(d, "meta.json").toPath()), StandardCharsets.UTF_8));
                m.dir = d.getName();
                if (new File(d, m.libraryName).isFile())
                    out.add(m);
            } catch (IOException | JSONException | IllegalArgumentException e) {
                Log.w("vcs", "[gpu-driver] " + d.getName() + ": " + e.getMessage());
            }
        }
        return out;
    }

    /** The selected package, or null for the system driver (also when the selected one is gone) */
    static Meta active(Context c) {
        String dir = prefs(c).getString("active", "");
        if (dir.isEmpty())
            return null;
        for (Meta m : installed(c))
            if (m.dir.equals(dir))
                return m;
        return null;
    }

    static void setActive(Context c, String dir) {
        prefs(c).edit().putString("active", dir == null ? "" : dir).commit();
        Log.i("vcs", "[gpu-driver] selected: " + (dir == null || dir.isEmpty() ? "system driver" : dir));
    }

    static boolean warned(Context c) {
        return prefs(c).getBoolean("warned", false);
    }

    static void setWarned(Context c) {
        prefs(c).edit().putBoolean("warned", true).commit();
    }

    static String summary(Context c) {
        if (!supported(c))
            return "Adreno (Qualcomm) GPUs only";
        Meta m = active(c);
        int n = installed(c).size();
        return (m == null ? SYSTEM : "✓ " + m.label()) + (n > 0 ? " · " + n + " imported" : "");
    }

    // ---- meta.json ---------------------------------------------------------------------------------------------------

    static Meta parseMeta(String json) throws JSONException {
        JSONObject o = new JSONObject(json);
        Meta m = new Meta();
        m.name = o.getString("name").trim();
        m.libraryName = o.getString("libraryName").trim();
        m.minApi = o.optInt("minApi", 0);
        m.driverVersion = o.optString("driverVersion", "").trim();
        m.description = o.optString("description", "").trim();
        m.author = o.optString("author", "").trim();
        m.vendor = o.optString("vendor", "").trim();
        m.hardwareBufferFeature = o.has("hardwareBufferFeature") && !o.isNull("hardwareBufferFeature")
                && !"false".equalsIgnoreCase(o.optString("hardwareBufferFeature"));
        if (m.name.isEmpty())
            throw new IllegalArgumentException("meta.json: empty name");
        if (!plainFile(m.libraryName) || !m.libraryName.endsWith(".so"))
            throw new IllegalArgumentException("meta.json: libraryName is not a plain .so name: " + m.libraryName);
        return m;
    }

    /** A file name with no folder part (the zip's entries are extracted flat, never outside the folder) */
    static boolean plainFile(String s) {
        return s != null && !s.isEmpty() && s.length() <= 128 && !s.startsWith(".") && s.matches("[A-Za-z0-9._+-]+");
    }

    /** The folder's name: the package's name, letters / digits / . _ - only (the native side checks the same) */
    static String folderName(String name) {
        String f = name.replaceAll("[^A-Za-z0-9._+-]+", "_").replaceAll("^[._]+", "");
        if (f.length() > 64)
            f = f.substring(0, 64);
        return f.isEmpty() ? "driver" : f;
    }

    /** True for a 64-bit little-endian ARM64 (EM_AARCH64 = 183) shared object */
    static boolean arm64Elf(byte[] h) {
        return h.length >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 /* 64-bit */
                && h[5] == 1 /* little endian */ && (h[16] & 0xff) == 3 /* ET_DYN */ && (h[18] & 0xff) == 183 && h[19] == 0;
    }

    // ---- import / remove ---------------------------------------------------------------------------------------------

    static final class Imported {
        final Meta meta;
        final String error;

        Imported(Meta meta, String error) {
            this.meta = meta;
            this.error = error;
        }
    }

    /** Validates and extracts a driver package (a .zip); off the UI thread. Replaces a package of the same name. */
    static Imported install(Context c, Uri uri) {
        File root = root(c);
        root.mkdirs();
        File stage = new File(root, ".incoming-" + System.currentTimeMillis());
        try {
            if (!stage.mkdirs())
                return new Imported(null, "Could not make a folder in the app's storage");
            String metaText = null;
            List<String> libs = new ArrayList<>();
            long total = 0;
            int entries = 0;
            try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
                if (raw == null)
                    return new Imported(null, "The file could not be opened");
                ZipInputStream zip = new ZipInputStream(raw);
                ZipEntry e;
                byte[] buf = new byte[64 * 1024];
                while ((e = zip.getNextEntry()) != null) {
                    if (++entries > MAX_ENTRIES)
                        return new Imported(null, "Not a driver package: too many files in the .zip");
                    if (e.isDirectory())
                        continue;
                    String path = e.getName();
                    String base = path.substring(path.lastIndexOf('/') + 1);
                    boolean meta = base.equals("meta.json");
                    boolean so = base.endsWith(".so");
                    if (!meta && !so)
                        continue; // anything else (readme, licences) is left out
                    if (path.contains("..") || !plainFile(base))
                        return new Imported(null, "Not a driver package: a file has an unsafe name (" + path + ")");
                    File out = new File(stage, base);
                    if (out.exists())
                        return new Imported(null, "Not a driver package: " + base + " is in it twice");
                    ByteArrayOutputStream head = new ByteArrayOutputStream();
                    try (OutputStream o = new FileOutputStream(out)) {
                        for (int n; (n = zip.read(buf)) > 0; ) {
                            total += n;
                            if (total > MAX_TOTAL)
                                return new Imported(null, "Not a driver package: more than " + (MAX_TOTAL >> 20) + " MB unpacked");
                            o.write(buf, 0, n);
                            if (head.size() < 64 * 1024 && (meta || head.size() < 64))
                                head.write(buf, 0, Math.min(n, meta ? n : 64));
                        }
                    }
                    if (meta) {
                        if (out.length() > 64 * 1024)
                            return new Imported(null, "meta.json is too large");
                        metaText = new String(Files.readAllBytes(out.toPath()), StandardCharsets.UTF_8);
                    } else {
                        if (!arm64Elf(head.toByteArray()))
                            return new Imported(null, base + " is not an ARM64 (arm64-v8a) library: wrong package for this phone");
                        libs.add(base);
                    }
                }
            }
            if (metaText == null)
                return new Imported(null, "Not a driver package: no meta.json in the .zip (it must be an adrenotools / "
                        + "emulator driver package, e.g. Turnip's)");
            Meta m;
            try {
                m = parseMeta(metaText);
            } catch (JSONException | IllegalArgumentException e) {
                return new Imported(null, "meta.json is not valid: " + e.getMessage());
            }
            if (!libs.contains(m.libraryName))
                return new Imported(null, "The package has no " + m.libraryName + " (meta.json's libraryName)");
            if (m.minApi > Build.VERSION.SDK_INT)
                return new Imported(null, "This driver needs Android API " + m.minApi + "; this phone has " + Build.VERSION.SDK_INT);
            m.dir = folderName(m.name);
            File dest = new File(root, m.dir);
            if (dest.exists())
                deleteTree(dest);
            if (!stage.renameTo(dest))
                return new Imported(null, "Could not move the driver into place");
            for (File f : dest.listFiles())
                f.setReadOnly();
            Log.i("vcs", "[gpu-driver] imported '" + m.name + "' (" + m.driverVersion + ", " + m.libraryName + ", "
                    + (total >> 10) + " KB) into gpu_drivers/" + m.dir);
            return new Imported(m, null);
        } catch (IOException | RuntimeException e) {
            Log.w("vcs", "[gpu-driver] import: " + e);
            return new Imported(null, "Could not read the package: " + e.getMessage());
        } finally {
            if (stage.exists())
                deleteTree(stage);
        }
    }

    static boolean remove(Context c, String dir) {
        if (!plainFile(dir))
            return false;
        File d = new File(root(c), dir);
        if (!d.isDirectory())
            return false;
        Meta a = active(c);
        if (a != null && a.dir.equals(dir))
            setActive(c, "");
        deleteTree(d);
        Log.i("vcs", "[gpu-driver] removed gpu_drivers/" + dir);
        return !d.exists();
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids)
                deleteTree(k);
        f.setWritable(true);
        if (!f.delete())
            Log.w("vcs", "[gpu-driver] could not delete " + f);
    }

    // ---- the game's start and the next launcher start ----------------------------------------------------------------

    /** VCSActivity.onCreate (the game's process): the native side's environment (src/android/gpu_driver.h) */
    static void env(Context c, boolean stories) {
        try {
            Meta m = supported(c) && !stories ? active(c) : null;
            File attempt = new File(root(c), ATTEMPT);
            if (m == null) {
                Os.unsetenv("VCS_GPU_DRIVER");
                Os.unsetenv("VCS_GPU_DRIVER_LIB");
                if (attempt.isFile())
                    attempt.delete();
                return;
            }
            Os.setenv("VCS_GPU_DRIVER", m.dir, true);
            Os.setenv("VCS_GPU_DRIVER_LIB", m.libraryName, true);
            Os.setenv("VCS_NATIVE_LIB_DIR", c.getApplicationInfo().nativeLibraryDir, true);
            try (FileOutputStream o = new FileOutputStream(attempt)) {
                o.write((m.dir + "\n").getBytes(StandardCharsets.UTF_8));
            }
            Log.i("vcs", "[gpu-driver] starting with the custom driver '" + m.name + "' (" + m.libraryName + ")");
        } catch (ErrnoException | IOException e) {
            Log.e("vcs", "[gpu-driver] env: " + e);
        }
    }

    /** The launcher's resume, the game not running: the notice to show if the last run's custom driver failed (the
     *  system driver is then selected again), else null. Takes the device-lost marker when it was the driver's run. */
    static String checkLastRun(Context c) {
        File attempt = new File(root(c), ATTEMPT);
        if (!attempt.isFile())
            return null;
        String dir;
        try {
            dir = new String(Files.readAllBytes(attempt.toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            dir = "?";
        }
        attempt.delete();
        String log = DeviceNotice.head(CrashReport.gameLog(c), 1024 * 1024);
        File lost = CompatMode.marker(c);
        boolean wasLost = lost.isFile();
        boolean confirmed = log.contains("[gpu-driver] confirmed");
        boolean crashed = tailHas(CrashReport.gameLog(c), "[crash]") || tailHas(CrashReport.gameLog(c), "[java crash]");
        if (confirmed && !wasLost && !crashed)
            return null;
        if (wasLost)
            lost.delete(); // the driver's fault first: compatibility mode is the next step only if the system driver fails too
        Meta a = active(c);
        if (a != null && a.dir.equals(dir))
            setActive(c, "");
        String why = wasLost ? "the graphics device was lost"
                : crashed ? "the game crashed"
                : log.contains("[gpu-driver] failed") ? "the driver could not be loaded"
                : "the game could not start its graphics with it";
        Log.w("vcs", "[gpu-driver] the custom driver '" + dir + "' failed (" + why + "): system driver selected again");
        return "The custom driver didn't work (" + why + "), so the app went back to the system driver.\n\nDriver: " + dir
                + "\n\nYou can pick it again in Settings > Custom GPU driver, or try another version.";
    }

    private static boolean tailHas(File f, String what) {
        if (!f.isFile())
            return false;
        try (RandomAccessFile in = new RandomAccessFile(f, "r")) {
            long len = in.length(), start = Math.max(0, len - 256 * 1024);
            byte[] b = new byte[(int) (len - start)];
            in.seek(start);
            in.readFully(b);
            return new String(b, StandardCharsets.UTF_8).contains(what);
        } catch (IOException e) {
            return false;
        }
    }

    /** Lines for the crash report */
    static String reportLines(Context c) {
        StringBuilder r = new StringBuilder("GPU driver: ");
        Meta a = active(c);
        if (a == null)
            r.append("system");
        else
            r.append("custom '").append(a.name).append("' (").append(a.driverVersion).append(", ").append(a.libraryName)
                    .append(", folder ").append(a.dir).append(')');
        r.append(supported(c) ? "" : " [custom drivers not available: not an Adreno phone]").append('\n');
        List<Meta> all = installed(c);
        if (!all.isEmpty()) {
            r.append("GPU drivers imported:");
            for (Meta m : all)
                r.append(" ").append(m.dir);
            r.append('\n');
        }
        return r.toString();
    }
}
