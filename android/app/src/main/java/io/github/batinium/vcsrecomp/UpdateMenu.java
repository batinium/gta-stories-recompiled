// [android] The in-app updater, rows in the launcher's About category (docs/ANDROID.md "Releases and updates"):
// check GitHub's release list of the fork (BuildConfig.VCS_UPDATE_REPO; a plain unauthenticated GET, nothing about the
// player is sent), show the newest android-v release against this app's version, download its APK into the app's
// cache, check the size, the notes' sha256 and the signing key, then hand it to Android's installer (a PackageInstaller
// session: Android asks the player to confirm). Only when the player asks, or once a day at the launcher's start if
// they turned that on (off by default).
//
// The APK holds no game code: an update keeps the player's game library unless the release's `game-abi:` line names
// another interface number than the library's stamp (GameFiles.readStamp); the confirmation says so before anything is
// downloaded. Version parsing and the notes' lines are UpdateVersion's (plain Java, checked on the PC).
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

final class UpdateMenu {
    private static final String TAG = "vcs";
    // a plain client name, nothing personal (GitHub's API wants a User-Agent)
    private static final String USER_AGENT = "vcs-recomp-android-updater";
    private static final String PREFS = "updates", KEY_AUTO = "auto_check", KEY_LAST = "last_check";
    private static final long DAY_MS = 24L * 60 * 60 * 1000;
    private static final String ACTION_STATUS = BuildConfig.APPLICATION_ID + ".UPDATE_STATUS";
    private static final String[] OFF_ON = {"Off", "On: once a day, at the launcher's start"};

    private final Activity activity;
    private final Runnable refresh;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final UpdateVersion current = UpdateVersion.parseName(BuildConfig.VERSION_NAME);

    private boolean busy, checked, autoTried;
    private String note;                 // a running task's line or the last failure, in place of the check row's state
    private UpdateVersion.Release newest; // the newest release found (null: none, or not checked)
    private int skipped;                 // our tags without an .apk or marked pre-release, at the last check

    // the installer's answer comes to a receiver on the application's context; it finds the menu here
    private static UpdateMenu live;
    private static BroadcastReceiver receiver;

    UpdateMenu(Activity activity, Runnable refresh) {
        this.activity = activity;
        this.refresh = refresh;
        live = this;
        cleanCache();
    }

    private SharedPreferences prefs() {
        return activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static boolean enabled() {
        return !BuildConfig.VCS_UPDATE_REPO.isEmpty();
    }

    private boolean available() {
        return newest != null && (current == null || newest.version.compareTo(current) > 0);
    }

    void addTo(MenuView.Category about) {
        about.add(new MenuView.Item("Check for updates")
                .value(this::checkState)
                .help(() -> enabled() ? "Looks at the project's releases on GitHub (" + BuildConfig.VCS_UPDATE_REPO + ")."
                        : "This build has no update source (built with -PvcsUpdateRepo=).")
                .enabled(() -> enabled() && !busy)
                .action(() -> check(true)));
        about.add(new MenuView.Item("What's new")
                .visible(this::available)
                .value(() -> newest == null ? "" : summaryLine(newest))
                .action(this::showNotes));
        about.add(new MenuView.Item("Update")
                .visible(this::available)
                .value(this::updateState)
                .enabled(() -> !busy)
                .action(this::startUpdate));
        about.add(new MenuView.Item("Check at start")
                .help("Once a day at most, when the launcher opens. Nothing is downloaded without asking.")
                .enabled(UpdateMenu::enabled)
                .choice(OFF_ON, () -> prefs().getBoolean(KEY_AUTO, false) ? 1 : 0, i -> {
                    prefs().edit().putBoolean(KEY_AUTO, i == 1).apply();
                    refresh.run();
                }));
    }

    /** The launcher's onResume: the daily check, if the player turned it on */
    void onResume() {
        live = this;
        if (!enabled() || busy || autoTried || !prefs().getBoolean(KEY_AUTO, false))
            return;
        long last = prefs().getLong(KEY_LAST, 0), now = System.currentTimeMillis();
        if (now - last < DAY_MS && now >= last)
            return;
        autoTried = true;
        check(false);
    }

    /** A short note for the launcher's corner when an update is waiting, else "" */
    String cornerNote() {
        return available() ? " · update " + newest.version + " available (About)" : "";
    }

    // ---- the rows' text ---------------------------------------------------------------------------------------------

    private String checkState() {
        String here = "This app " + (current != null ? current : BuildConfig.VERSION_NAME);
        if (note != null)
            return note;
        if (!enabled())
            return here + " · updates are off in this build";
        if (!checked)
            return here + " · not checked yet";
        if (newest == null)
            return here + " · no releases yet" + (skipped > 0 ? " (" + skipped + " without an APK or marked pre-release)" : "");
        if (available())
            return here + " · newest " + newest.version;
        return "✓ " + here + " · up to date (newest " + newest.version + ")";
    }

    private static String summaryLine(UpdateVersion.Release r) {
        String s = UpdateVersion.summary(r.body, 3, 240).replace("\n", " · ");
        return s.isEmpty() ? (r.name.isEmpty() ? r.tag : r.name) : s;
    }

    private String updateState() {
        if (newest == null)
            return "";
        UpdateVersion.Asset apk = newest.apk();
        return newest.version + " · " + size(apk.size) + " · " + (needsNewLibrary(newest) ? "needs a new game library"
                : GameFiles.bundledGame(activity) ? "the app alone: a personal build loses its game"
                : "keeps your game library");
    }

    private static String size(long bytes) {
        return bytes <= 0 ? "size unknown" : bytes >= (1 << 20) ? (bytes >> 20) + " MB" : (bytes >> 10) + " KB";
    }

    /** The imported library's interface number (its stamp), -1: none imported, -2: no readable stamp */
    private int libraryAbi() {
        File lib = GameFiles.gameLibrary(activity);
        if (!lib.exists())
            return -1;
        String stamp = GameFiles.readStamp(lib);
        if (stamp == null || !stamp.startsWith(GameFiles.STAMP_PREFIX))
            return -2;
        try {
            return Integer.parseInt(stamp.substring(GameFiles.STAMP_PREFIX.length()).split(" ", 2)[0].trim());
        } catch (NumberFormatException e) {
            return -2;
        }
    }

    private boolean needsNewLibrary(UpdateVersion.Release r) {
        int want = r.gameAbi(), have = libraryAbi();
        return !GameFiles.bundledGame(activity) && want >= 0 && have != -1 && have != want;
    }

    /** What the update means for the player's game library, for the confirmation */
    private String libraryLine(UpdateVersion.Release r) {
        int want = r.gameAbi(), have = libraryAbi();
        String rebuild = "build it with tools/android.py build-game from this release's source (git checkout " + r.tag
                + ") and import it under Disc";
        if (GameFiles.bundledGame(activity))
            return "This app is a personal build with the game inside. The release is the app alone: afterwards, "
                    + rebuild + ".";
        if (want < 0)
            return "The release doesn't say which game library it needs. If Play then says your library is for "
                    + "another version, " + rebuild + ".";
        if (have == -1)
            return "No game library imported yet: " + rebuild + " (game interface " + want + ").";
        if (have == want)
            return "Your game library stays: it fits this release (game interface " + want + ").";
        return "This release needs a NEW game library: it uses game interface " + want + ", yours "
                + (have < 0 ? "has no readable stamp" : "is " + have) + ". Play refuses the old one until you "
                + rebuild + ".";
    }

    private void setNote(String n) {
        note = n;
        refresh.run();
    }

    // ---- check ------------------------------------------------------------------------------------------------------

    private void check(boolean loud) {
        if (busy || !enabled())
            return;
        busy = true;
        setNote("Checking GitHub for releases…");
        new Thread(() -> {
            String error = null;
            List<UpdateVersion.Release> list = null;
            try {
                list = fetchReleases();
            } catch (IOException e) {
                error = e.getMessage();
            } catch (JSONException e) {
                error = "GitHub's answer could not be read (" + e.getMessage() + ")";
            }
            final String failure = error;
            final List<UpdateVersion.Release> releases = list;
            Log.i(TAG, "[update] check (" + (loud ? "asked" : "daily") + "): " + (failure != null ? failure
                    : releases.size() + " releases"));
            ui.post(() -> {
                busy = false;
                if (failure != null) {
                    note = "✗ " + failure;
                } else {
                    note = null;
                    checked = true;
                    newest = UpdateVersion.newest(releases);
                    skipped = UpdateVersion.skipped(releases);
                    prefs().edit().putLong(KEY_LAST, System.currentTimeMillis()).apply();
                    Log.i(TAG, "[update] this app " + current + ", newest release "
                            + (newest == null ? "none" : newest.tag) + (available() ? ": an update" : ""));
                }
                refresh.run();
            });
        }, "vcs-update-check").start();
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", USER_AGENT);
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    private static List<UpdateVersion.Release> fetchReleases() throws IOException, JSONException {
        String url = "https://api.github.com/repos/" + BuildConfig.VCS_UPDATE_REPO + "/releases?per_page=50";
        HttpURLConnection c;
        int code;
        try {
            c = open(url);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
            code = c.getResponseCode();
        } catch (IOException e) {
            throw new IOException("No connection to GitHub (" + e.getMessage() + ")");
        }
        try {
            if (code == 404)
                throw new IOException("No release page at github.com/" + BuildConfig.VCS_UPDATE_REPO);
            if (code == 403 || code == 429)
                throw new IOException("GitHub's limit for checks without an account is reached: try again in an hour");
            if (code != 200)
                throw new IOException("GitHub answered " + code);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    if (out.size() > (8 << 20))
                        throw new IOException("GitHub's answer is too long");
                }
            }
            JSONArray a = new JSONArray(new String(out.toByteArray(), StandardCharsets.UTF_8));
            List<UpdateVersion.Release> list = new ArrayList<>();
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.getJSONObject(i);
                UpdateVersion.Release r = new UpdateVersion.Release(o.optString("tag_name", ""), o.optString("name", ""),
                        o.optString("body", ""), o.optString("html_url", ""), o.optBoolean("draft", false),
                        o.optBoolean("prerelease", false));
                JSONArray assets = o.optJSONArray("assets");
                for (int k = 0; assets != null && k < assets.length(); k++) {
                    JSONObject x = assets.getJSONObject(k);
                    r.assets.add(new UpdateVersion.Asset(x.optString("name", ""), x.optLong("size", -1),
                            x.optString("browser_download_url", "")));
                }
                list.add(r);
            }
            return list;
        } finally {
            c.disconnect();
        }
    }

    // ---- notes, confirmation ----------------------------------------------------------------------------------------

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    private void showNotes() {
        UpdateVersion.Release r = newest;
        if (r == null)
            return;
        String notes = UpdateVersion.summary(r.body, 14, 1500);
        AlertDialog.Builder b = dialog()
                .setTitle((r.name.isEmpty() ? r.tag : r.name))
                .setMessage((notes.isEmpty() ? "No notes." : notes) + "\n\n" + r.version + " · this app " + current)
                .setNegativeButton("Close", null)
                .setPositiveButton("Update…", (d, w) -> startUpdate());
        if (r.page.startsWith("https://github.com/"))
            b.setNeutralButton("Release page", (d, w) -> {
                try {
                    activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r.page)));
                } catch (RuntimeException e) {
                    setNote("✗ No browser to open " + r.page);
                }
            });
        b.show();
    }

    private void startUpdate() {
        UpdateVersion.Release r = newest;
        if (busy || r == null || !available())
            return;
        // Android lets an app install only once the player allowed it ("Install unknown apps"): they do that themselves
        if (!activity.getPackageManager().canRequestPackageInstalls()) {
            dialog().setTitle("Allow this app to install its update")
                    .setMessage("Android asks once: in the next screen turn on \"Allow from this source\" for this app, "
                            + "then come back and choose Update again. Android still asks before installing anything.")
                    .setNegativeButton("Not now", null)
                    .setPositiveButton("Open the setting", (d, w) -> {
                        try {
                            activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                    Uri.parse("package:" + activity.getPackageName())));
                        } catch (RuntimeException e) {
                            setNote("✗ The setting could not be opened: Settings > Apps > Special access > Install unknown apps");
                        }
                    })
                    .show();
            return;
        }
        UpdateVersion.Asset apk = r.apk();
        String sha = r.sha256();
        StringBuilder m = new StringBuilder();
        m.append("This app ").append(current).append(" → ").append(r.version).append("\n");
        m.append(apk.name).append(", ").append(size(apk.size)).append(sha != null ? ", checked against the notes' sha256"
                : ", the notes carry no sha256: only its size is checked").append(".\n\n");
        m.append(libraryLine(r)).append("\n\n");
        m.append("Settings and saves stay. Android asks you to confirm the install.");
        if (BuildConfig.DEBUG)
            m.append("\n\nThis app is a build of your own (debug key): Android refuses a release signed with another key. "
                    + "The download is checked before installing.");
        AlertDialog d = dialog()
                .setTitle(needsNewLibrary(r) ? "Update? It needs a new game library" : "Update to " + r.version + "?")
                .setMessage(m.toString())
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Download and install", (x, w) -> download(r))
                .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus()); // a stray A press cancels
        d.show();
    }

    // ---- download, verify, install ----------------------------------------------------------------------------------

    private File cacheDir() {
        return new File(activity.getCacheDir(), "update");
    }

    private void cleanCache() {
        File[] old = cacheDir().listFiles();
        if (old != null)
            for (File f : old)
                f.delete();
    }

    private void download(UpdateVersion.Release r) {
        if (busy)
            return;
        busy = true;
        UpdateVersion.Asset apk = r.apk();
        setNote("Downloading " + r.version + "…");
        new Thread(() -> {
            File dir = cacheDir();
            cleanCache();
            dir.mkdirs();
            File out = new File(dir, "vcs-recomp-" + r.version + ".apk");
            String error = null;
            try {
                fetch(apk, out);
                String want = r.sha256();
                if (want != null) {
                    String got = sha256(out);
                    if (!want.equals(got))
                        error = "The download doesn't match the release's sha256 (" + got.substring(0, 12) + "… instead of "
                                + want.substring(0, 12) + "…): not installed. Try again.";
                }
                if (error == null)
                    error = checkArchive(out, r);
            } catch (IOException e) {
                error = "Download failed: " + e.getMessage();
            }
            final String failure = error;
            Log.i(TAG, "[update] download " + apk.url + ": " + (failure == null ? out.length() + " bytes ok" : failure));
            if (failure != null) {
                out.delete();
                ui.post(() -> {
                    busy = false;
                    setNote("✗ " + failure);
                    if (failure.startsWith(SIGNED_ELSEWHERE))
                        explainSigning(failure);
                });
                return;
            }
            ui.post(() -> setNote("Handing " + r.version + " to Android's installer…"));
            String installError = install(out);
            ui.post(() -> {
                if (installError != null) {
                    busy = false;
                    out.delete();
                    setNote("✗ " + installError);
                } else {
                    setNote("Waiting for Android's installer…");
                }
            });
        }, "vcs-update-download").start();
    }

    private void fetch(UpdateVersion.Asset apk, File out) throws IOException {
        HttpURLConnection c = open(apk.url);
        try {
            int code = c.getResponseCode();
            if (code != 200)
                throw new IOException("GitHub answered " + code);
            long total = apk.size > 0 ? apk.size : c.getContentLengthLong();
            if (total > (1L << 30))
                throw new IOException("the APK is over 1 GB: not an update of this app");
            long got = 0, shown = 0;
            try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) {
                    o.write(buf, 0, n);
                    got += n;
                    if (apk.size > 0 && got > apk.size)
                        throw new IOException("more data than the release lists (" + size(apk.size) + ")");
                    long now = SystemClock.uptimeMillis();
                    if (now - shown > 250) {
                        shown = now;
                        final long g = got;
                        ui.post(() -> setNote("Downloading… " + (g >> 20) + " / " + (total > 0 ? (total >> 20) + " MB" : "? MB")));
                    }
                }
            }
            if (apk.size > 0 && got != apk.size)
                throw new IOException("got " + got + " bytes, the release lists " + apk.size);
        } finally {
            c.disconnect();
        }
    }

    private static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; )
                md.update(buf, 0, n);
            StringBuilder s = new StringBuilder();
            for (byte b : md.digest())
                s.append(String.format(Locale.ROOT, "%02x", b));
            return s.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    private static final String SIGNED_ELSEWHERE = "Signed with another key";

    /** The downloaded APK is this app, newer, and signed with this app's key (else Android would refuse it), or why not */
    private String checkArchive(File apk, UpdateVersion.Release r) {
        PackageManager pm = activity.getPackageManager();
        int flags = PackageManager.GET_SIGNING_CERTIFICATES;
        PackageInfo got = pm.getPackageArchiveInfo(apk.getPath(), PackageManager.PackageInfoFlags.of(flags));
        if (got == null)
            return "The download is not an Android app (damaged?): not installed.";
        if (!activity.getPackageName().equals(got.packageName))
            return "The release's APK is another app (" + got.packageName + "): not installed.";
        PackageInfo mine;
        try {
            mine = pm.getPackageInfo(activity.getPackageName(), PackageManager.PackageInfoFlags.of(flags));
        } catch (PackageManager.NameNotFoundException e) {
            return null; // can't compare: Android decides
        }
        if (got.getLongVersionCode() <= mine.getLongVersionCode())
            return "The APK is version " + got.versionName + ", not newer than this app (" + mine.versionName
                    + "): the release's tag and its APK disagree. Not installed.";
        if (got.signingInfo == null || mine.signingInfo == null)
            return null;
        List<String> have = digests(mine.signingInfo.getApkContentsSigners());
        Signature[] history = got.signingInfo.hasPastSigningCertificates() ? got.signingInfo.getSigningCertificateHistory()
                : got.signingInfo.getApkContentsSigners();
        for (String d : digests(history))
            if (have.contains(d))
                return null;
        return SIGNED_ELSEWHERE + ": Android refuses to install the release over this app.";
    }

    private static List<String> digests(Signature[] sigs) {
        List<String> out = new ArrayList<>();
        if (sigs == null)
            return out;
        for (Signature s : sigs) {
            try {
                out.add(Arrays.toString(MessageDigest.getInstance("SHA-256").digest(s.toByteArray())));
            } catch (NoSuchAlgorithmException ignored) {
            }
        }
        return out;
    }

    private void explainSigning(String why) {
        if (activity.isFinishing())
            return;
        dialog().setTitle("This app can't be updated in place")
                .setMessage(why + "\n\nAndroid installs an update only over an app signed with the same key. Releases "
                        + "are signed with the project's release key; this app " + (BuildConfig.DEBUG
                        ? "is a build of your own, signed with your PC's debug key" : "was signed with another key")
                        + ".\n\nTo switch to the releases: export your saves (Saves > Export), uninstall this app (that "
                        + "deletes its data), install the release's APK from its page, then import your saves and the "
                        + "game library again.")
                .setPositiveButton("OK", null)
                .show();
    }

    /** A PackageInstaller session with the APK; the answer comes to the receiver. Returns why it couldn't start, or null */
    private String install(File apk) {
        Context app = activity.getApplicationContext();
        registerReceiver(app);
        PackageInstaller pi = app.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        p.setAppPackageName(app.getPackageName());
        p.setSize(apk.length());
        int id = -1;
        try {
            id = pi.createSession(p);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("base.apk", 0, apk.length())) {
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; )
                        out.write(buf, 0, n);
                    s.fsync(out);
                }
                // explicit (our package) and mutable: the installer adds its status to it
                Intent i = new Intent(ACTION_STATUS).setPackage(app.getPackageName());
                PendingIntent pend = PendingIntent.getBroadcast(app, id, i,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                s.commit(pend.getIntentSender());
            }
            Log.i(TAG, "[update] install session " + id + " committed (" + apk.length() + " bytes)");
            return null;
        } catch (IOException | RuntimeException e) {
            if (id >= 0)
                try {
                    pi.abandonSession(id);
                } catch (RuntimeException ignored) {
                }
            return "Android's installer couldn't take the APK (" + e.getMessage() + ")";
        }
    }

    private static synchronized void registerReceiver(Context app) {
        if (receiver != null)
            return;
        receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                int status = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Log.i(TAG, "[update] installer status " + status + (msg != null ? ": " + msg : ""));
                UpdateMenu m = live;
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                    if (confirm != null) {
                        try {
                            if (m != null && !m.activity.isFinishing())
                                m.activity.startActivity(confirm);
                            else
                                c.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                        } catch (RuntimeException e) {
                            Log.w(TAG, "[update] the installer's confirmation could not be shown: " + e);
                        }
                    }
                    return;
                }
                if (m != null)
                    m.ui.post(() -> m.installed(status, msg));
            }
        };
        app.registerReceiver(receiver, new IntentFilter(ACTION_STATUS), Context.RECEIVER_NOT_EXPORTED);
    }

    /** The installer's final answer (success usually ends this process before it arrives) */
    private void installed(int status, String msg) {
        busy = false;
        cleanCache();
        String m = msg == null ? "" : msg;
        String up = m.toUpperCase(Locale.ROOT);
        switch (status) {
            case PackageInstaller.STATUS_SUCCESS:
                setNote("✓ Updated: start the app again");
                return;
            case PackageInstaller.STATUS_FAILURE_ABORTED:
                setNote("Update cancelled");
                return;
            case PackageInstaller.STATUS_FAILURE_STORAGE:
                setNote("✗ Not enough space on the phone for the update");
                return;
            case PackageInstaller.STATUS_FAILURE_BLOCKED:
                setNote("✗ The phone blocked the install (a policy or Play Protect)" + (m.isEmpty() ? "" : ": " + m));
                return;
            default:
                break;
        }
        if (up.contains("SIGNATURE") || up.contains("UPDATE_INCOMPATIBLE") || up.contains("INCONSISTENT_CERTIFICATES")) {
            setNote("✗ " + SIGNED_ELSEWHERE + ": Android refused the update");
            explainSigning(SIGNED_ELSEWHERE + " (Android: " + m + ").");
        } else if (up.contains("VERSION_DOWNGRADE")) {
            setNote("✗ Android refused: the release is older than this app");
        } else {
            setNote("✗ Android refused the update" + (m.isEmpty() ? " (status " + status + ")" : ": " + m));
        }
    }
}
