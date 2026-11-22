// [android] Edge-to-edge, system bars hidden, for the game's window (issue #1: a permanent black bar at the bottom
// with 3-button navigation on Android 13 after the bar hid). docs/ANDROID.md "The window's surface".
package io.github.batinium.vcsrecomp;

import android.app.Activity;
import android.graphics.Insets;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

final class Immersive {
    private Immersive() {}

    /** Once, after SDL made its views: the content is laid out under the bars and the cutout (never inset by them),
     *  and every change of the window's insets / size is logged (tag vcs) so a report can be checked. */
    static void install(Activity a) {
        Window w = a.getWindow();
        w.setDecorFitsSystemWindows(false);
        w.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        w.setAttributes(w.getAttributes());
        View decor = w.getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> {
            log(a, "insets", insets);
            return v.onApplyWindowInsets(insets); // the default dispatch: SDL's surface still gets them
        });
        decor.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol || b - t != ob - ot)
                Log.i("vcs", "[window] decor " + (r - l) + "x" + (b - t) + ", display "
                        + a.getWindowManager().getCurrentWindowMetrics().getBounds().width() + "x"
                        + a.getWindowManager().getCurrentWindowMetrics().getBounds().height());
        });
        hideBars(a);
        log(a, "start", decor.getRootWindowInsets());
    }

    /** Hides the status and navigation bars; a swipe from the edge shows them for a moment. Called at start, on resume
     *  and on every window focus change (the system shows the bars again after a dialog, the shade, a task switch). */
    static void hideBars(Activity a) {
        WindowInsetsController c = a.getWindow().getInsetsController();
        if (c == null)
            return;
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        c.hide(WindowInsets.Type.systemBars());
    }

    static void log(Activity a, String what, WindowInsets insets) {
        if (insets == null) {
            Log.i("vcs", "[window] " + what + ": no insets yet");
            return;
        }
        Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
        Insets cut = insets.getInsets(WindowInsets.Type.displayCutout());
        Insets tap = insets.getInsets(WindowInsets.Type.tappableElement());
        Log.i("vcs", "[window] " + what + ": bars " + bars + " (nav " + insets.isVisible(WindowInsets.Type.navigationBars())
                + ", status " + insets.isVisible(WindowInsets.Type.statusBars()) + "), cutout " + cut + ", tappable " + tap
                + ", window " + a.getWindowManager().getCurrentWindowMetrics().getBounds().flattenToString());
    }
}
