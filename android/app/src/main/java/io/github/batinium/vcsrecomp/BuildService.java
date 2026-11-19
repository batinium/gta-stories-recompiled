// [phonebuild] The foreground service that runs PhoneBuild (docs/PHONE_BUILD.md): a progress notification with Cancel,
// a partial wake lock (the screen may turn off), one build at a time. The game being built is remembered in
// files/build/want.txt: a build the system killed goes on when the service restarts (START_STICKY) or when the player
// taps the launcher's row again. The launcher reads the state through status() / listener.
package io.github.batinium.vcsrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.IOException;

public class BuildService extends Service {
    static final String EXTRA_GAME = "game";
    static final String ACTION_CANCEL = "io.github.batinium.vcsrecomp.BUILD_CANCEL";
    private static final String CHANNEL = "phonebuild";
    private static final int NOTE = 7301;

    // the launcher's view of it (same process)
    static volatile String building; // the game id while a build runs
    static volatile String statusText, statusGame; // the last build's line, and its game
    static volatile int done, total;
    static volatile Runnable listener;

    private static File wantFile(Context c) {
        return new File(new File(c.getFilesDir(), "build"), "want.txt");
    }

    /** A build started and not finished (killed, cancelled by the system): its game, else null */
    static String pending(Context c) {
        try {
            File f = wantFile(c);
            return f.exists() ? PhoneBuild.read(f).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** The last failure for a game (files/build/<game>/error.txt), else null */
    static String lastError(Context c, String game) {
        File f = new File(PhoneBuild.buildDir(c, game), "error.txt");
        try {
            return f.exists() ? PhoneBuild.read(f).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    static void start(Context c, String game) {
        Intent i = new Intent(c, BuildService.class).putExtra(EXTRA_GAME, game);
        c.startForegroundService(i);
    }

    private PhoneBuild current;
    private Thread thread;
    private PowerManager.WakeLock wake;
    private NotificationManager nm;

    @Override
    public void onCreate() {
        super.onCreate();
        nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Code pack build", NotificationManager.IMPORTANCE_LOW));
    }

    private Notification note(String text, int d, int t, boolean ongoing) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(building != null ? "Building the " + gameName(building) + " code pack" : "Code pack")
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setOngoing(ongoing)
                .setOnlyAlertOnce(true)
                .setContentIntent(PendingIntent.getActivity(this, 0, new Intent(this, LauncherActivity.class),
                        PendingIntent.FLAG_IMMUTABLE));
        if (ongoing) {
            b.setProgress(Math.max(t, 0), Math.max(d, 0), t <= 0);
            b.addAction(new Notification.Action.Builder(null, "Cancel", PendingIntent.getService(this, 1,
                    new Intent(this, BuildService.class).setAction(ACTION_CANCEL), PendingIntent.FLAG_IMMUTABLE)).build());
        }
        return b.build();
    }

    private static String gameName(String id) {
        DiscCheck.Game g = DiscCheck.game(id);
        return g != null ? g.name : id;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_CANCEL.equals(intent.getAction())) {
            if (current != null)
                current.cancel();
            wantFile(this).delete();
            return START_NOT_STICKY;
        }
        String game = intent != null ? intent.getStringExtra(EXTRA_GAME) : pending(this);
        try {
            startForeground(NOTE, note("Starting…", 0, 0, true), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } catch (RuntimeException e) { // a restart from the background may be refused (Android 12+): the row resumes it
            Log.w(PhoneBuild.TAG, "[phonebuild] can't run in the foreground now: " + e);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (game == null || DiscCheck.game(game) == null || thread != null) {
            if (thread == null)
                stopSelf();
            return START_STICKY;
        }
        begin(game);
        return START_STICKY;
    }

    private void begin(String game) {
        building = game;
        statusGame = game;
        statusText = "Starting…";
        done = total = 0;
        try {
            new File(getFilesDir(), "build").mkdirs();
            PhoneBuild.write(wantFile(this), game + "\n");
        } catch (IOException ignored) {
        }
        new File(PhoneBuild.buildDir(this, game), "error.txt").delete();
        wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "vcsrecomp:phonebuild");
        wake.acquire(4 * 3600 * 1000L);
        current = new PhoneBuild(this, game, (text, d, t) -> {
            statusText = text;
            done = d;
            total = t;
            nm.notify(NOTE, note(text, d, t, true));
            Runnable l = listener;
            if (l != null)
                l.run();
        });
        thread = new Thread(() -> {
            String result;
            boolean ok = false;
            long t0 = System.currentTimeMillis();
            try {
                current.run();
                ok = true;
                result = gameName(game) + ": code pack built in "
                        + Math.max(1, (System.currentTimeMillis() - t0) / 60000) + " min and installed. Ready to play.";
            } catch (PhoneBuild.Cancelled e) {
                result = "Cancelled. Start it again to go on where it stopped.";
                new File(PhoneBuild.buildDir(this, game), "compiling.txt").delete(); // not a low-memory stop
            } catch (Throwable e) {
                result = "✗ " + (e.getMessage() != null ? e.getMessage() : e.toString());
                Log.e(PhoneBuild.TAG, "[phonebuild] failed", e);
                try {
                    File dir = PhoneBuild.buildDir(this, game);
                    dir.mkdirs();
                    PhoneBuild.write(new File(dir, "error.txt"), result + "\n");
                } catch (IOException ignored) {
                }
            }
            if (ok || current != null)
                wantFile(this).delete();
            current.logLine("[phonebuild] " + result);
            statusText = result;
            building = null;
            if (wake.isHeld())
                wake.release();
            stopForeground(STOP_FOREGROUND_DETACH);
            nm.notify(NOTE, note(result, 0, 0, false));
            Runnable l = listener;
            if (l != null)
                l.run();
            thread = null;
            stopSelf();
        }, "phonebuild");
        thread.start();
    }

    @Override
    public void onDestroy() {
        if (wake != null && wake.isHeld())
            wake.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
