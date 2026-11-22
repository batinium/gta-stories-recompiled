// [android] A plain-JVM check of the touch gestures' timing (TouchGestures), no Android or JUnit:
//
//   python3 tools/android_gestures_check.py
package io.github.batinium.vcsrecomp;

public final class TouchGesturesCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    private static final int ON = TouchGestures.EV_FIRE_ON, OFF = TouchGestures.EV_FIRE_OFF, TWO = TouchGestures.EV_TWO_FINGER_TAP;

    public static void main(String[] args) {
        // double-tap: tap, second finger within 250 ms and 40 dp holds fire until it lifts
        TouchGestures g = new TouchGestures(1f);
        check(g.camDown(1, 500, 200, 0) == 0, "first down: nothing");
        check(g.camUp(1, 501, 200, 90) == 0 && !g.firing(), "first tap: nothing yet");
        check(g.camDown(2, 520, 210, 250) == ON && g.firing(), "second down within 250 ms / 40 px: fire on");
        check(g.camMove(2, 600, 210, 1000) == 0 && g.firing(), "a late drag keeps firing (aim while firing)");
        check(g.camUp(2, 600, 210, 1500) == OFF && !g.firing(), "lift: fire off");

        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camUp(1, 500, 200, 90);
        check(g.camDown(2, 500, 200, 400) == 0, "second tap too late (310 ms after the lift): no fire");
        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camUp(1, 500, 200, 90);
        check(g.camDown(2, 600, 200, 200) == 0, "second tap too far (100 px): no fire");
        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camUp(1, 500, 200, 300);
        check(g.camDown(2, 500, 200, 350) == 0, "first touch too long (300 ms): not a tap");
        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camMove(1, 560, 200, 50);
        g.camUp(1, 560, 200, 100);
        check(g.camDown(2, 560, 200, 150) == 0, "first touch was a drag: not a tap");

        g = new TouchGestures(1f); // a tap and at once a drag: the fire is cancelled
        g.camDown(1, 500, 200, 0);
        g.camUp(1, 500, 200, 80);
        check(g.camDown(2, 500, 200, 200) == ON, "tap then drag: starts");
        check(g.camMove(2, 540, 200, 260) == OFF && !g.firing(), "moved past the slop within 150 ms: cancelled");
        check(g.camUp(2, 540, 200, 400) == 0, "and its lift does nothing");

        g = new TouchGestures(2f); // dp scale 2: 12 dp = 24 px
        g.camDown(1, 500, 200, 0);
        check(g.camUp(1, 520, 200, 80) == 0, "20 px at 2 px/dp is under the slop");
        check(g.camDown(2, 560, 200, 200) == ON, "40 px away at 2 px/dp: within 40 dp");

        // two-finger tap
        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camDown(2, 560, 220, 60);
        check(g.camUp(1, 500, 200, 120) == 0, "one finger up: not yet");
        check(g.camUp(2, 561, 220, 150) == TWO, "both up within 300 ms: the tap");

        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camDown(2, 560, 220, 60);
        g.camUp(1, 500, 200, 200);
        check(g.camUp(2, 560, 220, 400) == 0, "too slow (400 ms): no");

        g = new TouchGestures(1f);
        g.camDown(1, 500, 200, 0);
        g.camMove(1, 560, 200, 40);
        g.camDown(2, 560, 220, 60);
        g.camUp(1, 560, 200, 100);
        check(g.camUp(2, 560, 220, 120) == 0, "a drag with a second finger: no");

        g = new TouchGestures(1f); // a button held all along
        g.otherDown();
        g.camDown(1, 500, 200, 0);
        g.camDown(2, 560, 220, 60);
        g.camUp(1, 500, 200, 100);
        check(g.camUp(2, 560, 220, 120) == 0, "a finger on a control all along: no");
        g = new TouchGestures(1f); // a button pressed in between
        g.camDown(1, 500, 200, 0);
        g.otherDown();
        g.camDown(2, 560, 220, 60);
        g.camUp(1, 500, 200, 100);
        check(g.camUp(2, 560, 220, 120) == 0, "a button pressed in between: no");

        g = new TouchGestures(1f); // one camera finger + one button finger is not two camera fingers
        g.camDown(1, 500, 200, 0);
        g.otherDown();
        check(g.camUp(1, 500, 200, 80) == 0, "camera finger + button: no two-finger tap");

        g = new TouchGestures(1f); // three fingers
        g.camDown(1, 500, 200, 0);
        g.camDown(2, 560, 220, 20);
        g.camDown(3, 600, 220, 40);
        g.camUp(1, 500, 200, 80);
        g.camUp(2, 560, 220, 90);
        check(g.camUp(3, 600, 220, 100) == 0, "three fingers: no");

        g = new TouchGestures(1f); // a two-finger tap is not also half a double-tap
        g.camDown(1, 500, 200, 0);
        g.camDown(2, 560, 220, 30);
        g.camUp(1, 500, 200, 80);
        g.camUp(2, 560, 220, 90);
        check(g.camDown(3, 500, 200, 150) == 0, "after a two-finger tap: no fire");

        g = new TouchGestures(1f); // fire finger, then a second camera finger
        g.camDown(1, 500, 200, 0);
        g.camUp(1, 500, 200, 60);
        g.camDown(2, 500, 200, 150);
        g.camDown(3, 560, 220, 180);
        check(g.camUp(3, 560, 220, 200) == 0, "second camera finger while firing: no two-finger tap");
        check(g.camUp(2, 500, 200, 500) == OFF, "fire ends at its own lift");

        // weapon swipe
        check(TouchGestures.swipeDir(5, 3, 1) == TouchGestures.SWIPE_NONE, "swipe: small movement undecided");
        check(TouchGestures.swipeDir(30, 2, 1) == TouchGestures.SWIPE_NONE, "swipe: 30 px not far enough");
        check(TouchGestures.swipeDir(40, 5, 1) == TouchGestures.SWIPE_NEXT, "swipe right: next");
        check(TouchGestures.swipeDir(-40, 5, 1) == TouchGestures.SWIPE_PREV, "swipe left: previous");
        check(TouchGestures.swipeDir(3, 30, 1) == TouchGestures.SWIPE_VERTICAL, "swipe down: a camera drag");
        check(TouchGestures.swipeDir(60, 0, 2) == TouchGestures.SWIPE_NONE, "swipe: dp scaled (60 px = 30 dp)");

        System.out.println(failures == 0 ? "OK: " + checks + " checks" : failures + " of " + checks + " checks FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
