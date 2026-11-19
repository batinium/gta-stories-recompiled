// [android] The launcher's background: soft ribbons of light drifting over a dusk sky, our own drawing in the in-game
// overlay's palette (TouchControls / SidePanel: teal and pink on dark purple). A plain Canvas view: a gradient and a few
// filled paths, redrawn at ~30 fps (15 in battery saver) and not at all while the window is hidden or the activity is
// paused. The sky's tint follows the time of day (night, dawn, day, dusk).
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.os.PowerManager;
import android.os.SystemClock;
import android.view.View;

import java.util.Calendar;

final class WaveView extends View {
    // Sky (top, bottom), glow and the two ribbon colours at six hours, mixed by the hour: the game's palette's
    // (Palette.sky(): VCS's dusk, or LCS's navy, amber and steel blue), crossfaded when the game selector changes
    private static final float[] HOURS = {0, 6, 9, 17, 20, 24};
    private int paletteGen = -1;

    private final Paint sky = new Paint(), glow = new Paint(Paint.ANTI_ALIAS_FLAG), band = new Paint(Paint.ANTI_ALIAS_FLAG),
            line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final long start = SystemClock.uptimeMillis();
    private final PowerManager power;
    private boolean running, visible;
    private int[] tint = Palette.sky()[3];
    private long tintAt;
    private final Runnable tick = this::invalidate;

    WaveView(Context c) {
        super(c);
        power = c.getSystemService(PowerManager.class);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
    }

    /** The activity's resume / pause */
    void setRunning(boolean r) {
        running = r;
        schedule();
    }

    @Override
    protected void onWindowVisibilityChanged(int v) {
        super.onWindowVisibilityChanged(v);
        visible = v == VISIBLE;
        schedule();
    }

    private void schedule() {
        removeCallbacks(tick);
        if (running && visible)
            postOnAnimation(tick);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        tintAt = 0; // new shaders for the new size
    }

    private static int mix(int a, int b, float t) {
        int r = Math.round(((a >> 16) & 0xff) + (((b >> 16) & 0xff) - ((a >> 16) & 0xff)) * t);
        int g = Math.round(((a >> 8) & 0xff) + (((b >> 8) & 0xff) - ((a >> 8) & 0xff)) * t);
        int bl = Math.round((a & 0xff) + ((b & 0xff) - (a & 0xff)) * t);
        return (r << 16) | (g << 8) | bl;
    }

    private static int alpha(float a, int rgb) {
        return (Math.max(0, Math.min(255, Math.round(a * 255))) << 24) | (rgb & 0xffffff);
    }

    private void updateTint(int w, int h) {
        long now = System.currentTimeMillis();
        if (tintAt != 0 && now - tintAt < 60_000 && paletteGen == Palette.generation())
            return;
        tintAt = now;
        paletteGen = Palette.generation();
        final int[][] SKY = Palette.sky();
        final int GLASS = Palette.GLASS;
        Calendar cal = Calendar.getInstance();
        float hour = cal.get(Calendar.HOUR_OF_DAY) + cal.get(Calendar.MINUTE) / 60f;
        int i = 0;
        while (i < HOURS.length - 2 && hour >= HOURS[i + 1])
            i++;
        float t = (hour - HOURS[i]) / (HOURS[i + 1] - HOURS[i]);
        t = t * t * (3 - 2 * t); // ease
        tint = new int[4];
        for (int k = 0; k < 4; k++)
            tint[k] = mix(SKY[i][k], SKY[i + 1][k], t);
        sky.setShader(new LinearGradient(0, 0, 0, h, new int[] {alpha(1, tint[0]), alpha(1, tint[1]), alpha(1, GLASS)},
                new float[] {0f, 0.62f, 1f}, Shader.TileMode.CLAMP));
        float r = Math.max(w, h) * 0.7f;
        glow.setShader(new RadialGradient(w * 0.72f, h * 0.62f, r, alpha(0.22f, tint[2]), 0, Shader.TileMode.CLAMP));
    }

    // One ribbon's centre line: a few slow sines, so it never repeats visibly
    private static float wave(float x, float t, float phase, float amp, float h) {
        return h * amp * (0.55f * (float) Math.sin(x * 2.1f + t * 0.35f + phase)
                + 0.30f * (float) Math.sin(x * 4.7f - t * 0.52f + phase * 1.7f)
                + 0.15f * (float) Math.sin(x * 9.3f + t * 0.81f + phase * 0.6f));
    }

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0)
            return;
        updateTint(w, h);
        c.drawRect(0, 0, w, h, sky);
        c.drawRect(0, 0, w, h, glow);

        float t = (SystemClock.uptimeMillis() - start) / 1000f;
        int steps = 48;
        // ribbons: {base height (share of h), amplitude, thickness, phase, colour index, alpha}
        float[][] ribbons = {
                {0.58f, 0.060f, 0.070f, 0.0f, 2, 0.16f},
                {0.63f, 0.075f, 0.045f, 1.9f, 3, 0.18f},
                {0.67f, 0.050f, 0.090f, 3.7f, 2, 0.10f},
                {0.72f, 0.065f, 0.030f, 5.1f, 3, 0.20f},
        };
        float stroke = Math.max(1.5f, Math.min(w, h) / 600f);
        for (float[] rb : ribbons) {
            int colour = tint[(int) rb[4]];
            float base = rb[0] * h, thick = rb[2] * h;
            path.rewind();
            for (int i = 0; i <= steps; i++) {
                float x = i / (float) steps;
                float y = base + wave(x, t, rb[3], rb[1], h) - thick * (0.6f + 0.4f * (float) Math.sin(x * 3 + t * 0.3f + rb[3]));
                if (i == 0)
                    path.moveTo(0, y);
                else
                    path.lineTo(x * w, y);
            }
            for (int i = steps; i >= 0; i--) {
                float x = i / (float) steps;
                float y = base + wave(x, t * 1.07f, rb[3] + 0.4f, rb[1], h) + thick * 0.5f;
                path.lineTo(x * w, y);
            }
            path.close();
            band.setColor(alpha(rb[5], colour));
            c.drawPath(path, band);
            // the ribbon's bright edge
            path.rewind();
            for (int i = 0; i <= steps; i++) {
                float x = i / (float) steps;
                float y = base + wave(x, t * 1.07f, rb[3] + 0.4f, rb[1], h) + thick * 0.5f;
                if (i == 0)
                    path.moveTo(0, y);
                else
                    path.lineTo(x * w, y);
            }
            line.setStrokeWidth(stroke);
            line.setColor(alpha(rb[5] * 2.2f, mix(colour, Palette.INK, 0.35f)));
            c.drawPath(path, line);
        }

        if (running && visible) {
            boolean saver = power != null && power.isPowerSaveMode();
            removeCallbacks(tick); // one pending frame at most, whatever else invalidated
            postOnAnimationDelayed(tick, saver ? 66 : 33);
        }
    }
}
