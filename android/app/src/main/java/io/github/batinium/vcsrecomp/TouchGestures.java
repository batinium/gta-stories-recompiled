package io.github.batinium.vcsrecomp;

/**
 * [android] The touch overlay's extra gestures (docs/ANDROID.md "Controls extras: gestures"), as plain Java with no View
 * or Android classes, so the timing can be checked on a PC's JVM (tools/android_gestures_check.py). TouchControls feeds
 * it the fingers that landed on the free camera area ("camera fingers") with the event time in ms, and tells it about
 * every other finger (on a control) with otherDown / otherUp.
 *
 * - Double-tap = fire: a short tap (up within TAP_MS, moved under SLOP_DP), then a second finger down within
 *   DTAP_GAP_MS of the first's lift and within DTAP_DIST_DP of it. The second finger holds fire for as long as it
 *   stays down (EV_FIRE_ON at its down, EV_FIRE_OFF at its lift). If it moves past SLOP_DP within FIRE_CONFIRM_MS the
 *   "double-tap" was a quick drag after a tap: the fire is cancelled (EV_FIRE_OFF). Later it may drag (aim while firing).
 *   The camera drag itself is untouched: TouchControls turns every camera finger's movement into a look as before.
 * - Two-finger tap = EV_TWO_FINGER_TAP when the second camera finger lifts: both fingers down and up inside
 *   TWO_FINGER_MS of the first's down, neither moved past SLOP_DP, no third camera finger, no finger on a control at
 *   any time in between (so a camera drag + a button press never counts).
 * - swipeDir: the weapon box swipe's classifier.
 */
final class TouchGestures {
    static final long TAP_MS = 200, DTAP_GAP_MS = 250, TWO_FINGER_MS = 300, FIRE_CONFIRM_MS = 150;
    static final float SLOP_DP = 12, DTAP_DIST_DP = 40;

    static final int EV_FIRE_ON = 1, EV_FIRE_OFF = 2, EV_TWO_FINGER_TAP = 4;

    // swipeDir results
    static final int SWIPE_NONE = 0, SWIPE_NEXT = 1, SWIPE_PREV = -1, SWIPE_VERTICAL = 2;
    static final float SWIPE_DP = 36, SWIPE_DECIDE_DP = 14;

    private static final int MAXF = 3;

    private final float slop, dtapDist;
    // the camera fingers now down: id, down position, down time, the furthest it has been from there
    private final int[] id = new int[MAXF];
    private final float[] x0 = new float[MAXF], y0 = new float[MAXF], far = new float[MAXF];
    private final long[] t0 = new long[MAXF];
    private int count;
    // this run of camera fingers (from none down to none down)
    private long firstDown;
    private boolean sawTwo, twoOk, sawMore;
    private int others; // fingers on controls right now
    // the last short tap
    private boolean haveTap;
    private long tapUp;
    private float tapX, tapY;
    // the fire finger
    private int fireId = -1;
    private long fireDown;

    /** @param dp pixels per dp: the slop and the distance are in dp */
    TouchGestures(float dp) {
        slop = SLOP_DP * dp;
        dtapDist = DTAP_DIST_DP * dp;
    }

    boolean firing() {
        return fireId >= 0;
    }

    void reset() {
        count = 0;
        sawTwo = sawMore = false;
        twoOk = false;
        haveTap = false;
        fireId = -1;
        others = 0;
    }

    void otherDown() {
        others++;
        twoOk = false;
    }

    void otherUp() {
        if (others > 0)
            others--;
    }

    private int find(int pid) {
        for (int i = 0; i < count; i++)
            if (id[i] == pid)
                return i;
        return -1;
    }

    /** A finger landed on the camera area; returns EV_ flags */
    int camDown(int pid, float x, float y, long t) {
        int ev = 0;
        if (count >= MAXF) {
            sawMore = true;
            twoOk = false;
            return 0;
        }
        int i = count++;
        id[i] = pid; x0[i] = x; y0[i] = y; t0[i] = t; far[i] = 0;
        if (count == 1) {
            firstDown = t;
            sawTwo = sawMore = false;
            twoOk = others == 0;
            if (haveTap && fireId < 0 && t - tapUp <= DTAP_GAP_MS && Math.hypot(x - tapX, y - tapY) <= dtapDist) {
                fireId = pid;
                fireDown = t;
                ev |= EV_FIRE_ON;
            }
            haveTap = false;
        } else {
            haveTap = false;
            sawTwo = true;
            if (count > 2)
                sawMore = true;
            // the second finger must come within the window, with none on a control, none firing
            if (!twoOk || others != 0 || fireId >= 0 || count > 2 || t - firstDown > TWO_FINGER_MS)
                twoOk = false;
        }
        return ev;
    }

    int camMove(int pid, float x, float y, long t) {
        int i = find(pid);
        if (i < 0)
            return 0;
        float d = (float) Math.hypot(x - x0[i], y - y0[i]);
        if (d > far[i])
            far[i] = d;
        int ev = 0;
        if (far[i] > slop) {
            twoOk = false;
            if (pid == fireId && t - fireDown <= FIRE_CONFIRM_MS) { // a drag right after a tap: not a double-tap
                fireId = -1;
                ev |= EV_FIRE_OFF;
            }
        }
        return ev;
    }

    int camUp(int pid, float x, float y, long t) {
        int i = find(pid);
        if (i < 0)
            return 0;
        int ev = 0;
        float d = (float) Math.hypot(x - x0[i], y - y0[i]);
        if (d > far[i])
            far[i] = d;
        if (far[i] > slop)
            twoOk = false;
        if (pid == fireId) {
            fireId = -1;
            ev |= EV_FIRE_OFF;
        } else if (count == 1 && !sawTwo && t - t0[i] <= TAP_MS && far[i] <= slop) {
            haveTap = true; // a short tap by itself: the first half of a double-tap
            tapUp = t;
            tapX = x0[i];
            tapY = y0[i];
        }
        for (int k = i; k < count - 1; k++) { // drop it
            id[k] = id[k + 1]; x0[k] = x0[k + 1]; y0[k] = y0[k + 1]; t0[k] = t0[k + 1]; far[k] = far[k + 1];
        }
        count--;
        if (count == 0) {
            if (sawTwo && twoOk && !sawMore && others == 0 && t - firstDown <= TWO_FINGER_MS) {
                ev |= EV_TWO_FINGER_TAP;
                haveTap = false;
            }
            sawTwo = sawMore = twoOk = false;
        }
        return ev;
    }

    /** The finger stopped being a camera finger without lifting (the platform cancelled it) */
    int camCancel(int pid) {
        int ev = pid == fireId ? EV_FIRE_OFF : 0;
        if (pid == fireId)
            fireId = -1;
        int i = find(pid);
        if (i >= 0) {
            for (int k = i; k < count - 1; k++) {
                id[k] = id[k + 1]; x0[k] = x0[k + 1]; y0[k] = y0[k + 1]; t0[k] = t0[k + 1]; far[k] = far[k + 1];
            }
            count--;
        }
        twoOk = false;
        haveTap = false;
        if (count == 0)
            sawTwo = sawMore = false;
        return ev;
    }

    /** The weapon box swipe: (dx, dy) is the finger's movement from where it landed, dp pixels per dp. SWIPE_NEXT right,
     *  SWIPE_PREV left (once past SWIPE_DP); SWIPE_VERTICAL when it clearly goes up / down first (then it is a camera
     *  drag); SWIPE_NONE while undecided. */
    static int swipeDir(float dx, float dy, float dp) {
        float ax = Math.abs(dx), ay = Math.abs(dy);
        if (Math.hypot(dx, dy) < SWIPE_DECIDE_DP * dp)
            return SWIPE_NONE;
        if (ay > ax)
            return SWIPE_VERTICAL;
        return ax >= SWIPE_DP * dp ? (dx > 0 ? SWIPE_NEXT : SWIPE_PREV) : SWIPE_NONE;
    }
}
