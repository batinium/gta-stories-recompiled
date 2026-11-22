// [storage] About > Where your data is: what the app keeps on the phone, how big each part is, and how much is free.
// Read only (docs/ANDROID.md "Storage location (issue #3)": why there is no "move to the SD card" switch).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.os.StatFs;

import java.io.File;
import java.util.Locale;

final class StorageInfo {
    private StorageInfo() {
    }

    static long free(File where) {
        try {
            return new StatFs(where.getPath()).getAvailableBytes();
        } catch (RuntimeException e) {
            return -1;
        }
    }

    /** Bytes under f (a file's own length; symlinks are not followed), 0 for none */
    static long size(File f) {
        if (f == null || !f.exists())
            return 0;
        if (!f.isDirectory())
            return f.length();
        File[] kids = f.listFiles();
        long n = 0;
        if (kids != null)
            for (File k : kids)
                if (!java.nio.file.Files.isSymbolicLink(k.toPath()))
                    n += size(k);
        return n;
    }

    static String text(long bytes) {
        if (bytes < 0)
            return "?";
        if (bytes >= (1L << 30))
            return String.format(Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30));
        if (bytes >= (1L << 20))
            return (bytes >> 20) + " MB";
        return Math.max(0, bytes >> 10) + " KB";
    }

    private static void line(StringBuilder s, String name, File f, String what) {
        s.append(String.format(Locale.ROOT, "%-22s %9s\n", name, text(size(f))));
        s.append("   ").append(what).append("\n");
    }

    /** The dialog's text. Walks the folders: off the UI thread. */
    static String report(Context c) {
        File files = c.getFilesDir();
        File data = GameFiles.dataDir(c);
        String game = GameFiles.game(c);
        StringBuilder s = new StringBuilder();
        s.append("Inside the app's private folder (on the phone's own storage), for ")
                .append(DiscCheck.game(game).name).append(":\n\n");
        line(s, "Code pack", GameFiles.gameLibrary(c).getParentFile(),
                "files/game/" + game + "/ (the game, built from your disc; Android only runs code from here)");
        line(s, "Saves", new File(data, "mc0"), "files/" + data.getName() + "/mc0 (the memory card)");
        line(s, "Caches", new File(data, "cache"), "files/" + data.getName() + "/cache (made again when missing)");
        line(s, "Mods", new File(data, "mods"), "files/" + data.getName() + "/mods (imported mods, converted models)");
        line(s, "All of this game's data", data, "files/" + data.getName() + "/ (with save backups and shots)");
        line(s, "Logs", CrashReport.dir(c), "files/logs");
        line(s, "Phone build", new File(files, "build"), "files/build (the code-pack build's working files)");
        line(s, "Build tools", new File(files, "builder"), "files/builder (the compiler it downloaded)");
        line(s, "Temporary", c.getCacheDir(), "cache/ (updates, screenshots on their way out)");
        s.append("\nFree on the phone: ").append(text(free(files))).append("\n");
        File[] ext = c.getExternalFilesDirs(null);
        File sd = ext != null && ext.length > 1 ? ext[1] : null;
        if (sd != null)
            s.append("Free on the SD card: ").append(text(free(sd))).append(" (nothing of the app lives there)\n");
        File disc = GameFiles.pushedDisc(c);
        s.append("\nThe disc image is never copied: it stays where you chose it, on the phone or the SD card, and the "
                + "game reads it from there. To move it, copy the .iso to the new place with a file manager, then "
                + "choose it again under Disc.");
        if (disc != null)
            s.append("\nA test copy sits in the app's own folder: ").append(disc.getName()).append(" (")
                    .append(text(disc.length())).append(").");
        s.append("\nUninstalling the app deletes everything above.");
        return s.toString();
    }
}
