// [android] The app's colours, one place for every view (launcher MenuView / WaveView, in-game SidePanel, StatsOverlay,
// TouchControls), chosen by the game: Vice City Stories' "dusk" (pink and teal on violet glass, the overlay's look since
// the start) or Liberty City Stories' (amber and steel blue on navy, sampled from LCS's own pause menu and HUD:
// the desktop launcher's kLcsDarkPalette). Roles, not hues: PINK is the primary accent
// (selection marks, the primary row, warnings), TEAL the secondary (frames, values, "done"), GLASS the panels' fill,
// INK the text, BAD an error value, EDIT the layout editor's unselected outline.
//
// Text is INK (near white) on GLASS, or an accent on GLASS; an accent fill carries GLASS-coloured text. Contrast at full
// opacity (WCAG 2): LCS INK on GLASS 17.2:1, amber on GLASS 10.2:1, steel blue on GLASS 7.9:1, GLASS on amber 10.2:1,
// BAD on GLASS 7.0:1; VCS's 15.8 / 6.2 / 10.8 / 6.2 / 7.1 (docs/ANDROID.md "Colours"). Crimson is an accent only (the launcher's horizon glow, the editor's
// outline), never text: the desktop LCS menu's dark red text was hard to read.
//
// The fields change only on the UI thread (select / fadeTo); views read them while drawing, so a switch (the launcher's
// game selector) crossfades by invalidating them for ~0.35 s.
package io.github.batinium.vcsrecomp;

import android.animation.ValueAnimator;
import android.view.animation.DecelerateInterpolator;

final class Palette {
    private Palette() {}

    /** One game's colours */
    static final class Set {
        final int glass, teal, pink, ink, bad, edit;
        // the launcher's sky at six hours (WaveView.HOURS): {top, bottom, glow / ribbon A, ribbon B}
        final int[][] sky;

        Set(int glass, int teal, int pink, int ink, int bad, int edit, int[][] sky) {
            this.glass = glass;
            this.teal = teal;
            this.pink = pink;
            this.ink = ink;
            this.bad = bad;
            this.edit = edit;
            this.sky = sky;
        }
    }

    /** Vice City Stories: the overlay's dusk (unchanged from before the palette class) */
    static final Set VCS = new Set(0x1A1430, 0x3FE0CF, 0xFF5AAE, 0xF4F0FF, 0xFF7A86, 0xFF9A3C, new int[][] {
            {0x07051A, 0x150C33, 0x3FE0CF, 0x6A5CFF}, // night: deep blue-purple, teal ribbons
            {0x1C0F38, 0x5A2350, 0xFF8A9A, 0xFF5AAE}, // dawn: warm pink
            {0x16123E, 0x2E2A6E, 0x3FE0CF, 0x8FA8FF}, // day: brighter blue-purple
            {0x1A1430, 0x4A1A4E, 0xFF5AAE, 0x3FE0CF}, // dusk: the overlay's pink and teal on purple
            {0x0E0A24, 0x2A1240, 0xFF5AAE, 0x3FE0CF}, // evening
            {0x07051A, 0x150C33, 0x3FE0CF, 0x6A5CFF}, // night again
    });

    /** Liberty City Stories: amber (#F9B035, the pause menu's tabs) for PINK, steel blue (#74AEDA, its entries) for
     *  TEAL, navy (#0F0F22, the launcher's surface; the game's #01011F lifted a little) for GLASS, crimson (#AC0000, its
     *  page titles) only in the sky's glow and lifted (#E5484D) as the editor's outline */
    static final Set LCS = new Set(0x0F0F22, 0x74AEDA, 0xF9B035, 0xF4F4F8, 0xFF6F6F, 0xE5484D, new int[][] {
            {0x04040E, 0x0F0F22, 0x74AEDA, 0x446680}, // night: navy, steel-blue ribbons
            {0x0F0F22, 0x3A1218, 0xF9B035, 0xAC0000}, // dawn: a crimson horizon, amber
            {0x0B1430, 0x1E2A4A, 0x74AEDA, 0xF9B035}, // day: a brighter navy
            {0x0F0F22, 0x3A0E14, 0xF9B035, 0x74AEDA}, // dusk: amber and steel blue over a crimson glow
            {0x06061E, 0x1A0A16, 0xF9B035, 0xAC0000}, // evening
            {0x04040E, 0x0F0F22, 0x74AEDA, 0x446680}, // night again
    });

    static Set of(String game) {
        return DiscCheck.LCS.id.equals(game) ? LCS : VCS;
    }

    // the colours in use (a mix of two sets during a crossfade)
    static int GLASS = VCS.glass, TEAL = VCS.teal, PINK = VCS.pink, INK = VCS.ink, BAD = VCS.bad, EDIT = VCS.edit;
    private static int[][] sky = VCS.sky;
    private static Set current = VCS;
    private static ValueAnimator fade;
    private static int generation; // bumped on every change: WaveView rebuilds its shaders

    static int[][] sky() { return sky; }

    static int generation() { return generation; }

    /** At once (the game's process, the launcher's first draw) */
    static void select(String game) {
        if (fade != null)
            fade.cancel();
        fade = null;
        apply(of(game), of(game), 1f);
    }

    /** A crossfade to the game's colours; onFrame runs at each step (invalidate the views) */
    static void fadeTo(String game, Runnable onFrame) {
        Set to = of(game);
        if (to == current && fade == null) {
            onFrame.run();
            return;
        }
        if (fade != null)
            fade.cancel();
        final Set from = snapshot();
        fade = ValueAnimator.ofFloat(0f, 1f);
        fade.setDuration(350);
        fade.setInterpolator(new DecelerateInterpolator());
        fade.addUpdateListener(a -> {
            apply(from, to, (float) a.getAnimatedValue());
            onFrame.run();
        });
        fade.start();
    }

    private static Set snapshot() {
        return new Set(GLASS, TEAL, PINK, INK, BAD, EDIT, sky);
    }

    private static void apply(Set a, Set b, float t) {
        GLASS = mix(a.glass, b.glass, t);
        TEAL = mix(a.teal, b.teal, t);
        PINK = mix(a.pink, b.pink, t);
        INK = mix(a.ink, b.ink, t);
        BAD = mix(a.bad, b.bad, t);
        EDIT = mix(a.edit, b.edit, t);
        int[][] s = new int[a.sky.length][];
        for (int i = 0; i < s.length; i++) {
            s[i] = new int[a.sky[i].length];
            for (int k = 0; k < s[i].length; k++)
                s[i][k] = mix(a.sky[i][k], b.sky[i][k], t);
        }
        sky = s;
        if (t >= 1f)
            current = b;
        generation++;
    }

    /** rgb a..b at t (0..1), alpha dropped */
    static int mix(int a, int b, float t) {
        int r = Math.round(((a >> 16) & 0xff) + (((b >> 16) & 0xff) - ((a >> 16) & 0xff)) * t);
        int g = Math.round(((a >> 8) & 0xff) + (((b >> 8) & 0xff) - ((a >> 8) & 0xff)) * t);
        int bl = Math.round((a & 0xff) + ((b & 0xff) - (a & 0xff)) * t);
        return (r << 16) | (g << 8) | bl;
    }
}
