// [android] Keeps an AlertDialog's buttons on screen on short (landscape, folded) screens.
//
// A long message (the phone build's notes, an update's release notes) made the dialog taller than the screen: the
// window ran past the bottom edge or under the navigation bar, and the button row (Build, OK, Update...) with it, so
// the dialog could not be confirmed. fit() keeps the window inside the safe area (system bars and the camera cutout)
// and, if the dialog is still taller than that area, gives the window an exact height: the dialog's own layout then
// shrinks its scrolling message and keeps the title and the buttons. It follows folds and turns.
package io.github.batinium.vcsrecomp;

import android.app.Dialog;
import android.graphics.Insets;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.WindowMetrics;

final class Dialogs {
    private Dialogs() {
    }

    /** Call right after show(); returns the dialog */
    static <T extends Dialog> T fit(T d) {
        Window w = d.getWindow();
        if (w == null)
            return d;
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.setFitInsetsTypes(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        lp.setFitInsetsSides(WindowInsets.Side.all());
        w.setAttributes(lp);
        View decor = w.getDecorView();
        int[] capped = {0}; // the height fit() set, 0 = the dialog's own (wrap content)
        Runnable check = () -> {
            int safe = safeHeight(d);
            if (safe <= 0)
                return;
            int h = decor.getHeight();
            if (capped[0] == 0 && h > safe) {
                capped[0] = safe;
                w.setLayout(w.getAttributes().width, safe);
            } else if (capped[0] != 0 && capped[0] != safe) {
                // the screen changed (fold, turn): measure the dialog afresh, cap it again if it is still too tall
                capped[0] = 0;
                w.setLayout(w.getAttributes().width, ViewGroup.LayoutParams.WRAP_CONTENT);
            }
        };
        decor.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> v.post(check));
        decor.post(check);
        return d;
    }

    /** The screen's height inside the system bars and the cutout, less a margin, in pixels */
    private static int safeHeight(Dialog d) {
        WindowManager wm = d.getContext().getSystemService(WindowManager.class);
        if (wm == null)
            return 0;
        WindowMetrics m = wm.getCurrentWindowMetrics();
        Insets i = m.getWindowInsets().getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        int margin = Math.round(8 * d.getContext().getResources().getDisplayMetrics().density);
        return m.getBounds().height() - i.top - i.bottom - 2 * margin;
    }
}
