// [android] Lossless Scaling frame generation (LSFG, docs/ANDROID.md "Frame generation (LSFG)"): the player's own
// Lossless.dll and the settings, after the earlier port's Lsfg.java. The DLL is never part of the APK or the repository: the
// player picks it (the system file picker) and the launcher keeps a copy in the app's private files
// (files/lsfg/Lossless.dll). VCSActivity hands the settings to the native side as VCS_LSFG* (read once at start by the
// Vulkan backend, src/rhi/vulkan/framegen.h).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;

final class Lsfg {
    private Lsfg() {}

    static final int[] MULTS = {2, 3, 4};
    static final int[] FLOWS = {25, 50, 75, 100};

    // commit(), never apply(): the game's own process (:game) reads the file when it starts
    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("lsfg", Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS); // the panel (:game) writes too
    }

    static boolean on(Context c) { return prefs(c).getBoolean("on", false); }
    static int multiplier(Context c) { return prefs(c).getInt("mult", 2); }
    /** The optical flow's resolution, % of the frame: 50 by default (the earlier port's: at 1280x720 on an Adreno 740, 100 % was not worth it) */
    static int flow(Context c) { return prefs(c).getInt("flow", 50); }

    static void setOn(Context c, boolean v) { prefs(c).edit().putBoolean("on", v).commit(); }
    static void setMultiplier(Context c, int v) { prefs(c).edit().putInt("mult", v).commit(); }
    static void setFlow(Context c, int v) { prefs(c).edit().putInt("flow", v).commit(); }

    static File privateCopy(Context c) { return new File(new File(c.getFilesDir(), "lsfg"), "Lossless.dll"); }

    /** A Windows DLL at least (MZ, not tiny); the native side checks it holds the shaders */
    static boolean looksLikeDll(File f) {
        if (!f.isFile() || f.length() < 100_000)
            return false;
        try (InputStream in = new FileInputStream(f)) {
            return in.read() == 'M' && in.read() == 'Z';
        } catch (IOException | SecurityException e) {
            return false;
        }
    }

    static boolean installed(Context c) { return looksLikeDll(privateCopy(c)); }

    /** The picked document copied into the private files. Null = done, else the error */
    static String install(Context c, Uri uri) {
        File to = privateCopy(c);
        File tmp = new File(to.getPath() + ".part");
        try {
            File dir = to.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs())
                return "can't create " + dir;
            try (InputStream in = c.getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
                if (in == null)
                    return "can't open it";
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0; )
                    out.write(b, 0, n);
            }
            if (!looksLikeDll(tmp))
                return "that file is not a Windows DLL (Lossless.dll from Lossless Scaling's folder)";
            if (to.exists() && !to.delete())
                return "can't replace the copy";
            if (!tmp.renameTo(to))
                return "can't store it in the app's folder";
        } catch (IOException | SecurityException e) {
            return e.getMessage();
        } finally {
            tmp.delete();
        }
        return null;
    }

    // ---- the auto-find: a folder the player granted once (ACTION_OPEN_DOCUMENT_TREE, a persisted read permission),
    // searched for Lossless.dll (emulator users often keep it with their BIOS files). No broad storage permission.

    static final String DLL_NAME = "Lossless.dll";

    /** The granted folder (a tree URI), or null; null too when the system no longer holds its permission */
    static Uri tree(Context c) {
        String s = prefs(c).getString("tree", null);
        if (s == null)
            return null;
        Uri u = Uri.parse(s);
        for (android.content.UriPermission p : c.getContentResolver().getPersistedUriPermissions())
            if (p.getUri().equals(u) && p.isReadPermission())
                return u;
        return null;
    }

    static void setTree(Context c, Uri tree) {
        prefs(c).edit().putString("tree", tree == null ? null : tree.toString()).commit();
    }

    /** Where the folder picker opens: the shared Documents folder */
    static Uri documentsUri() {
        return DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Documents");
    }

    /** The folder's path as the picker shows it, e.g. "Documents/Emulators" */
    static String treeName(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.lastIndexOf(':');
        String path = colon >= 0 ? id.substring(colon + 1) : id;
        return path.isEmpty() ? "the whole storage" : path;
    }

    /** Lossless.dll in the folder or below (breadth first: the nearest one; bounded), or null. Off the UI thread. */
    static Uri findInTree(Context c, Uri tree) {
        ArrayDeque<String> dirs = new ArrayDeque<>();
        ArrayDeque<Integer> depths = new ArrayDeque<>();
        dirs.add(DocumentsContract.getTreeDocumentId(tree));
        depths.add(0);
        int seen = 0;
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        while (!dirs.isEmpty() && seen < 20000) {
            String dir = dirs.poll();
            int depth = depths.poll();
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dir);
            try (Cursor cur = c.getContentResolver().query(children, cols, null, null, null)) {
                if (cur == null)
                    continue;
                while (cur.moveToNext()) {
                    seen++;
                    String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                    boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                    if (!isDir && DLL_NAME.equalsIgnoreCase(name))
                        return DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    if (isDir && depth < 8) {
                        dirs.add(id);
                        depths.add(depth + 1);
                    }
                }
            } catch (RuntimeException e) {
                Log.w("vcs", "[launcher] can't list " + dir + ": " + e);
            }
        }
        return null;
    }

    /** The native side's switches, set before SDL starts the native main (VCSActivity.onCreate) */
    static void env(Context c) {
        File dll = privateCopy(c);
        try {
            Os.setenv("VCS_LSFG", on(c) ? "1" : "0", true);
            Os.setenv("VCS_LSFG_MULT", Integer.toString(multiplier(c)), true);
            Os.setenv("VCS_LSFG_FLOW", Integer.toString(flow(c)), true);
            if (looksLikeDll(dll))
                Os.setenv("VCS_LSFG_DLL", dll.getAbsolutePath(), true);
            else
                Os.unsetenv("VCS_LSFG_DLL");
        } catch (ErrnoException e) {
            Log.e("vcs", "setenv: " + e);
        }
        if (on(c))
            Log.i("vcs", "[android] frame generation asked for: x" + multiplier(c) + ", flow " + flow(c) + "%, "
                    + (looksLikeDll(dll) ? "Lossless.dll " + dll.length() + " bytes" : "no Lossless.dll")
                    + (BuildConfig.VCS_LSFG ? "" : " (not in this build)"));
    }
}
