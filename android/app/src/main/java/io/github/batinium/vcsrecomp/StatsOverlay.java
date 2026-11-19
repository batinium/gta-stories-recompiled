// [android] The stats overlay (docs/ANDROID.md "Stats overlay"): a row of small "dusk" chips at the top centre, under the
// on-screen controls' top buttons, with what a player tuning the game on a phone wants to see: the frame rate presented
// and the game's own, the frame time (average and longest), the GPU's time a frame, the CPU clock caps, the thermal
// status and headroom, the battery, the memory (this process and the phone) and the build tag. Off by default; a
// master switch and one per item (SharedPreferences "stats_overlay", shared with the launcher's process), set on the
// launcher and in the on-screen controls' settings (hold the show / hide button).
//
// The numbers are read on a thread of its own twice a second (the frame numbers from native code, src/android/stats.cpp;
// the rest from sysfs, /proc and the system services), turned into text there, and handed to the UI thread, which only
// measures and draws: nothing is allocated in onDraw. A plain View over SDL's surface, which takes no touches.
//
// Not the game's own counter (Display > Show frame rate, drawn by the game in a corner): that is one number, the game's
// frames a second; this one is the phone's view of the whole app.
//
// Compact (the default): one line of short values, small; Full: the chips with every detail, in rows. Hidden while the
// trainer or the settings menu is open (they draw where the chips are: setSuppressed, from TouchControls).
package io.github.batinium.vcsrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class StatsOverlay extends View {
    // native side (stats.cpp): presented fps, the game's fps (-1: none), frame ms, longest frame ms, GPU ms (-1: none),
    // cooling (1: held at 30 fps while hot, cooling.cpp); false when nothing was presented lately
    private static native boolean nativeFrameStats(float[] out);
    private static native void nativeSetWanted(boolean overlay, boolean gpu);

    // ---- settings ----
    /** The items: the preference key and the switch's label */
    static final String[][] ITEMS = {
            {"fps", "Frame rate (shown / the game's) and frame time"},
            {"gpu", "GPU time a frame"},
            {"cpu", "CPU clock caps (now / top, GHz)"},
            {"thermal", "Thermal status and headroom"},
            {"battery", "Battery temperature, level, charging, power draw"},
            {"memory", "Memory: the game's and the phone's"},
            {"tag", "Build tag"},
    };
    static final String HELP = "Over the game, twice a second, at the top centre. The game's own Display > Show frame rate "
            + "is a separate single number (the game's frames a second) and can stay off.";

    @SuppressWarnings("deprecation") // MODE_MULTI_PROCESS: the launcher and the game (:game) are two processes
    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("stats_overlay", Context.MODE_PRIVATE | Context.MODE_MULTI_PROCESS);
    }

    static boolean enabled(Context c) {
        return prefs(c).getBoolean("on", false);
    }

    static void setEnabled(Context c, boolean on) {
        prefs(c).edit().putBoolean("on", on).commit();
    }

    static boolean item(Context c, String key) {
        return prefs(c).getBoolean("item_" + key, true);
    }

    /** One short line (the default) or the full chips in rows */
    static boolean compact(Context c) {
        return prefs(c).getBoolean("compact", true);
    }

    /** The master switch, the help, one indented switch per item; `changed` runs after each change (may be null) */
    static void addSettings(Context c, LinearLayout box, Runnable changed) {
        CheckBox master = new CheckBox(c);
        master.setText("Stats overlay");
        master.setChecked(enabled(c));
        box.addView(master);
        TextView help = new TextView(c);
        help.setText(HELP);
        box.addView(help);
        int indent = Math.round(24 * c.getResources().getDisplayMetrics().density);
        List<CheckBox> items = new ArrayList<>();
        CheckBox full = new CheckBox(c);
        full.setText("Full details (several rows) instead of one compact line");
        full.setChecked(!compact(c));
        full.setEnabled(master.isChecked());
        full.setOnCheckedChangeListener((v, on) -> {
            prefs(c).edit().putBoolean("compact", !on).commit();
            if (changed != null)
                changed.run();
        });
        LinearLayout.LayoutParams fl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        fl.leftMargin = indent;
        box.addView(full, fl);
        items.add(full);
        for (String[] it : ITEMS) {
            CheckBox b = new CheckBox(c);
            b.setText(it[1]);
            b.setChecked(item(c, it[0]));
            b.setEnabled(master.isChecked());
            b.setOnCheckedChangeListener((v, on) -> {
                prefs(c).edit().putBoolean("item_" + it[0], on).commit();
                if (changed != null)
                    changed.run();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = indent;
            box.addView(b, lp);
            items.add(b);
        }
        master.setOnCheckedChangeListener((v, on) -> {
            setEnabled(c, on);
            for (CheckBox b : items)
                b.setEnabled(on);
            if (changed != null)
                changed.run();
        });
    }

    /** "fps, GPU, ..." for the launcher's card: what is on */
    static String summary(Context c) {
        if (!enabled(c))
            return "Off";
        StringBuilder s = new StringBuilder();
        final String[] shortName = {"frame rate", "GPU", "CPU", "thermals", "battery", "memory", "build tag"};
        for (int i = 0; i < ITEMS.length; i++)
            if (item(c, ITEMS[i][0]))
                s.append(s.length() > 0 ? ", " : "").append(shortName[i]);
        return s.length() > 0 ? "On: " + s : "On (every item off)";
    }

    // ---- the readings, on the stats thread ----
    private static final class Cluster {
        File cap;
        int cores;
        long maxKHz;
    }

    /** One tick's text: chips of a label and a value, some marked (a cap, heat, low memory) */
    private static final class Chips {
        final String[] label = new String[MAX_CHIPS], value = new String[MAX_CHIPS];
        final boolean[] warn = new boolean[MAX_CHIPS];
        int n;

        void add(String l, String v, boolean w) {
            if (n >= MAX_CHIPS)
                return;
            label[n] = l;
            value[n] = v;
            warn[n] = w;
            n++;
        }
    }

    private static final int MAX_CHIPS = 8;
    private static final String[] THERMAL = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};

    private final Context app;
    private final PowerManager power;
    private final ActivityManager am;
    private final ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
    private final float[] frame = new float[6];
    private List<Cluster> clusters;
    private int ticks, thermal = -1, batLevel = -1;
    private float headroom = Float.NaN, batTemp = Float.NaN, batWatts = Float.NaN;
    private BatteryManager battery;
    private boolean charging, memLow;
    private long rss = -1, memTotal, memAvail;

    private HandlerThread thread;
    private volatile Handler worker; // the running thread's; a tick of an older one stops
    private boolean on, running, suppressed;
    private volatile boolean compactMode;
    private volatile boolean wantFps, wantGpu, wantCpu, wantThermal, wantBattery, wantMemory, wantTag;

    /** Every half second on `h`'s thread while it is the running one */
    private Runnable ticker(Handler h) {
        return new Runnable() {
            @Override
            public void run() {
                if (worker != h)
                    return;
                Chips c = read();
                post(() -> show(c));
                h.postDelayed(this, 500);
            }
        };
    }

    // ---- drawing (UI thread) ----
    private Chips shown;
    private final RectF[] chip = new RectF[MAX_CHIPS];
    private final float[] labelW = new float[MAX_CHIPS];
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG), valuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float dp;

    StatsOverlay(Context c) {
        super(c);
        app = c.getApplicationContext();
        power = c.getSystemService(PowerManager.class);
        am = c.getSystemService(ActivityManager.class);
        dp = getResources().getDisplayMetrics().density;
        for (int i = 0; i < MAX_CHIPS; i++)
            chip[i] = new RectF();
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(Math.max(1f, dp));
        labelPaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 9, getResources().getDisplayMetrics()));
        labelPaint.setFakeBoldText(true);
        labelPaint.setLetterSpacing(0.12f);
        valuePaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 11, getResources().getDisplayMetrics()));
        setClickable(false);
        setFocusable(false);
        refresh();
    }

    /** Rereads the settings (after a change in the dialog) and starts or stops the readings */
    void refresh() {
        Context c = getContext();
        on = enabled(c);
        compactMode = compact(c);
        float sp = compactMode ? 0.85f : 1f;
        labelPaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 9 * sp, getResources().getDisplayMetrics()));
        valuePaint.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 11 * sp, getResources().getDisplayMetrics()));
        wantFps = item(c, "fps");
        wantGpu = item(c, "gpu");
        wantCpu = item(c, "cpu");
        wantThermal = item(c, "thermal");
        wantBattery = item(c, "battery");
        wantMemory = item(c, "memory");
        wantTag = item(c, "tag");
        nativeSetWanted(on, on && wantGpu);
        Log.i("vcs", "[stats] overlay " + (on ? "on" : "off"));
        update();
    }

    /** While a game menu is open (the trainer at the left, the settings menu in the middle) nothing is drawn */
    void setSuppressed(boolean s) {
        if (s == suppressed)
            return;
        suppressed = s;
        invalidate();
    }

    private void update() {
        boolean run = on && getWindowVisibility() == VISIBLE && isAttachedToWindow();
        if (run == running)
            return;
        running = run;
        if (run) {
            thread = new HandlerThread("vcs-stats");
            thread.start();
            Handler h = new Handler(thread.getLooper());
            worker = h;
            h.post(ticker(h));
        } else {
            worker = null;
            if (thread != null)
                thread.quitSafely();
            thread = null;
            shown = null;
            invalidate();
        }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        update();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        update();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        if (shown != null)
            show(shown); // the rows again for the new width (a fold / unfold)
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        update();
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent e) {
        return false; // every touch goes on (the controls, SDL)
    }

    // ---- readings ----

    private static long readLong(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String s = r.readLine();
            return s != null ? Long.parseLong(s.trim()) : -1;
        } catch (IOException | RuntimeException e) {
            return -1;
        }
    }

    /** The cpufreq policies, the one with the most cores (where the game's threads mostly run) first */
    private void findClusters() {
        clusters = new ArrayList<>();
        File[] ps = new File("/sys/devices/system/cpu/cpufreq").listFiles((d, name) -> name.startsWith("policy"));
        if (ps == null)
            return;
        for (File p : ps) {
            Cluster k = new Cluster();
            k.cap = new File(p, "scaling_max_freq");
            k.maxKHz = readLong(new File(p, "cpuinfo_max_freq"));
            try (BufferedReader r = new BufferedReader(new FileReader(new File(p, "related_cpus")))) {
                String s = r.readLine();
                k.cores = s != null ? s.trim().split("\\s+").length : 1;
            } catch (IOException | RuntimeException e) {
                k.cores = 1;
            }
            if (k.maxKHz > 0)
                clusters.add(k);
        }
        clusters.sort((a, b) -> a.cores != b.cores ? b.cores - a.cores : Long.compare(a.maxKHz, b.maxKHz));
    }

    /** VmRSS of this process (the game's: it runs in :game) in bytes, -1 when unreadable */
    private static long vmRss() {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/self/status"))) {
            for (String s; (s = r.readLine()) != null; )
                if (s.startsWith("VmRSS:"))
                    return Long.parseLong(s.substring(6).replace("kB", "").trim()) * 1024;
        } catch (IOException | RuntimeException ignored) {
        }
        return -1;
    }

    private void readBattery() {
        Intent b = app.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED)); // the sticky broadcast
        if (b == null)
            return;
        int l = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), s = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
        int t = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE), st = b.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
        batLevel = l >= 0 && s > 0 ? l * 100 / s : -1;
        batTemp = t != Integer.MIN_VALUE ? t / 10f : Float.NaN;
        // [android] power: the battery's current x voltage, what the phone draws (the sign differs between phones, and
        // on a charger it is the charge current: compare unplugged). CURRENT_NOW is in microamperes on most phones.
        int mv = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1);
        if (battery == null)
            battery = app.getSystemService(BatteryManager.class);
        long ua = battery != null ? battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) : Long.MIN_VALUE;
        batWatts = mv > 0 && ua != Long.MIN_VALUE && ua != 0 ? Math.abs(ua) / 1e6f * (mv / 1000f) : Float.NaN;
        // plugged in counts too: some phones report "discharging" on a charger that only holds the level
        charging = st == BatteryManager.BATTERY_STATUS_CHARGING || st == BatteryManager.BATTERY_STATUS_FULL
                || b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
    }

    private static String gib(long bytes) {
        return String.format(Locale.ROOT, "%.1f", bytes / (double) (1L << 30));
    }

    /** One tick on the stats thread: the readings the items want (the slow ones every 2 s), as chips */
    private Chips read() {
        boolean slow = ticks++ % 4 == 0;
        Chips c = new Chips();
        boolean small = compactMode;
        if (wantFps || wantGpu) {
            boolean fresh = nativeFrameStats(frame);
            if (wantFps) {
                if (!fresh)
                    c.add("FPS", "--", false);
                else {
                    c.add("FPS", frame[1] >= 0 ? String.format(Locale.ROOT, small ? "%.0f/%.0f" : "%.0f · game %.0f", frame[0], frame[1])
                            : String.format(Locale.ROOT, "%.0f", frame[0]), false);
                    // the longest frame in the half second: a hitch shows even when the average is fine
                    boolean hitch = frame[3] > frame[2] * 2 && frame[3] > 40;
                    c.add(small ? "MS" : "FRAME", small ? (hitch ? String.format(Locale.ROOT, "%.1f ↑%.0f", frame[2], frame[3])
                            : String.format(Locale.ROOT, "%.1f", frame[2]))
                            : String.format(Locale.ROOT, "%.1f ms · max %.0f", frame[2], frame[3]), hitch);
                }
            }
            if (wantGpu)
                c.add("GPU", fresh && frame[4] >= 0 ? String.format(Locale.ROOT, small ? "%.1f" : "%.1f ms", frame[4]) : "--", false);
        }
        if (wantCpu) {
            if (clusters == null)
                findClusters();
            StringBuilder s = new StringBuilder();
            boolean capped = false;
            for (int i = 0; i < clusters.size(); i++) {
                Cluster k = clusters.get(i);
                long cap = readLong(k.cap);
                if (cap <= 0)
                    continue;
                if (small && i > 0)
                    break; // compact: the main cluster alone
                if (s.length() > 0)
                    s.append(" · ");
                s.append(String.format(Locale.ROOT, "%.1f/%.1f", cap / 1e6, k.maxKHz / 1e6));
                if (i == 0 && cap < k.maxKHz * 0.85)
                    capped = true; // the main cluster held below 85 % of its top clock: the phone is throttling
            }
            c.add("CPU", s.length() > 0 ? (small ? s.toString() : s.append(" GHz").toString()) : "?", capped);
        }
        if (wantThermal && power != null) {
            thermal = power.getCurrentThermalStatus();
            if (slow) // NaN when asked more often than about once a second
                headroom = power.getThermalHeadroom(10);
            String name = thermal >= 0 && thermal < THERMAL.length ? THERMAL[thermal] : "?";
            if (!(wantFps || wantGpu))
                nativeFrameStats(frame); // the cooling flag below
            boolean cooling = frame[5] > 0; // [android] power: the game holds 30 fps until the phone cools (cooling.cpp)
            c.add("HEAT", (Float.isNaN(headroom) || small ? name : String.format(Locale.ROOT, "%s · %.2f", name, headroom))
                            + (cooling ? (small ? " 30" : " · 30 fps") : ""),
                    cooling || thermal >= PowerManager.THERMAL_STATUS_MODERATE || headroom >= 0.85f);
        }
        if (wantBattery) {
            if (slow || batLevel < 0)
                readBattery();
            if (batLevel >= 0)
                c.add(small ? "BAT" : "BATTERY", (Float.isNaN(batTemp) ? "" : String.format(Locale.ROOT, small ? "%.0f° " : "%.0f°C · ", batTemp))
                        + batLevel + "%" + (charging ? (small ? "+" : " · charging") : "")
                        + (Float.isNaN(batWatts) ? "" : String.format(Locale.ROOT, small ? " %.1fW" : " · %.1f W", batWatts)),
                        !Float.isNaN(batTemp) && batTemp >= 42f);
        }
        if (wantMemory) {
            if (slow || memTotal == 0) {
                rss = vmRss();
                if (am != null) {
                    am.getMemoryInfo(memInfo);
                    memTotal = memInfo.totalMem;
                    memAvail = memInfo.availMem;
                    memLow = memInfo.lowMemory;
                }
            }
            String app = rss >= 0 ? "app " + gib(rss) : "app ?";
            String sys = memTotal > 0 ? " · RAM " + gib(Math.max(0, memTotal - memAvail)) + "/" + gib(memTotal) + " GiB" : "";
            if (small)
                c.add("MEM", (rss >= 0 ? gib(rss) + "G" : "?") + (memLow ? " low" : ""), memLow);
            else
                c.add("MEMORY", app + sys + (memLow ? " · low" : ""), memLow);
        }
        if (wantTag)
            c.add("BUILD", BuildConfig.VCS_BUILD_TAG, false);
        return c;
    }

    // ---- layout and drawing (UI thread) ----

    /** New text: the chips measured and placed (flowing rows, centred, under the controls' top buttons) */
    private void show(Chips c) {
        if (!running)
            return;
        shown = c;
        boolean small = compactMode;
        float padX = (small ? 5 : 7) * dp, gap = (small ? 3 : 5) * dp, h = (small ? 15 : 20) * dp, inner = (small ? 3 : 5) * dp;
        float maxRow = getWidth() * (small ? 0.9f : 0.7f);
        float y = 44 * dp; // below the top buttons (TouchControls: 14 + 2 x 10 units, the touch area)
        int i = 0;
        while (i < c.n) {
            if (small && i > 0) { // compact: one line, what doesn't fit is left out
                c.n = i;
                break;
            }
            // as many chips as fit in this row
            int j = i;
            float rowW = 0;
            while (j < c.n) {
                labelW[j] = labelPaint.measureText(c.label[j]);
                float w = padX * 2 + labelW[j] + inner + valuePaint.measureText(c.value[j]);
                if (j > i && rowW + gap + w > maxRow)
                    break;
                rowW += (j > i ? gap : 0) + w;
                chip[j].set(0, 0, w, h);
                j++;
            }
            float x = (getWidth() - rowW) / 2;
            for (int k = i; k < j; k++) {
                float w = chip[k].width();
                chip[k].set(x, y, x + w, y + h);
                x += w + gap;
            }
            y += h + gap;
            i = j;
        }
        invalidate();
    }

    private static int col(float a, int rgb) {
        return (Math.max(0, Math.min(255, Math.round(255 * a))) << 24) | (rgb & 0xFFFFFF);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        Chips c = shown;
        if (c == null || !running || suppressed)
            return;
        boolean small = compactMode;
        float padX = (small ? 5 : 7) * dp, inner = (small ? 3 : 5) * dp, r = (small ? 4 : 6) * dp;
        for (int i = 0; i < c.n; i++) {
            RectF b = chip[i];
            fill.setColor(col(0.72f, Palette.GLASS));
            canvas.drawRoundRect(b, r, r, fill);
            line.setColor(col(c.warn[i] ? 0.8f : 0.28f, c.warn[i] ? Palette.PINK : Palette.INK));
            canvas.drawRoundRect(b, r, r, line);
            // a short accent tick on the left edge: teal, pink when the item is in a bad state
            fill.setColor(col(0.9f, c.warn[i] ? Palette.PINK : Palette.TEAL));
            canvas.drawRect(b.left + 2 * dp, b.top + (small ? 4 : 5) * dp, b.left + 3.5f * dp, b.bottom - (small ? 4 : 5) * dp, fill);
            float base = b.centerY() - (valuePaint.descent() + valuePaint.ascent()) / 2;
            labelPaint.setColor(col(0.95f, Palette.TEAL));
            canvas.drawText(c.label[i], b.left + padX, base, labelPaint);
            valuePaint.setColor(col(1f, c.warn[i] ? Palette.PINK : Palette.INK));
            canvas.drawText(c.value[i], b.left + padX + labelW[i] + inner, base, valuePaint);
        }
    }
}
