// [android] The controller the game reads (docs/ANDROID.md "Controllers"): Auto (the one last pressed), a chosen pad,
// or touch only. The choice lives in SharedPreferences "touch_controls" (pad_choice, pad_vid, pad_pid, pad_name), set
// in the launcher's Controls category or the side panel; the game's side is src/android/gamepads.cpp, which looks at
// every pad SDL reports, ignores the ones that are no controller and groups one pad's devices.
package io.github.batinium.vcsrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import android.view.InputDevice;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class Gamepads {
    static final int AUTO = 0, CHOSEN = 1, TOUCH_ONLY = 2;

    // native side (gamepads.cpp, in libmain.so and libsmoke.so)
    private static native void nativeSetChoice(int choice, int vendor, int product, String name);
    private static native String nativeList();

    static final class Pad {
        String name = "", ignored = "";
        int vendor, product, state, devices = 1; // state: 0 connected, 1 in use, 2 ignored
        long secondsAgo = -1;                     // since its last input; -1 none yet

        boolean same(int v, int p, String n) {
            return v != 0 || p != 0 ? vendor == v && product == p : name.equals(n);
        }

        String status() {
            if (state == 2)
                return "ignored: " + ignored;
            String s = state == 1 ? "in use" : "connected";
            if (devices > 1)
                s += " · " + devices + " devices, read as one";
            return s;
        }
    }

    private Gamepads() {
    }

    private static SharedPreferences prefs(Context c) {
        return TouchControls.prefs(c);
    }

    static int choice(Context c) {
        int v = prefs(c).getInt("pad_choice", AUTO);
        return v >= AUTO && v <= TOUCH_ONLY ? v : AUTO;
    }

    static String chosenName(Context c) {
        return prefs(c).getString("pad_name", "");
    }

    static int chosenVendor(Context c) {
        return prefs(c).getInt("pad_vid", 0);
    }

    static int chosenProduct(Context c) {
        return prefs(c).getInt("pad_pid", 0);
    }

    static void choose(Context c, int choice, Pad pad) {
        SharedPreferences.Editor e = prefs(c).edit().putInt("pad_choice", choice);
        if (choice == CHOSEN && pad != null)
            e.putString("pad_name", pad.name).putInt("pad_vid", pad.vendor).putInt("pad_pid", pad.product);
        e.commit();
        apply(c);
    }

    /** The choice to the game's native side (the game's process, once its library is loaded) */
    static void apply(Context c) {
        try {
            nativeSetChoice(choice(c), chosenVendor(c), chosenProduct(c), chosenName(c));
        } catch (UnsatisfiedLinkError e) {
            // the launcher's process: nothing to tell
        }
    }

    static String summary(Context c) {
        switch (choice(c)) {
            case TOUCH_ONLY: return "Touch only";
            case CHOSEN: return chosenName(c).isEmpty() ? "Auto" : chosenName(c);
            default: return "Auto";
        }
    }

    /** The pads the game's SDL sees (in the game), the same pad's devices as one; ignored ones last */
    static List<Pad> connected() {
        List<Pad> out = new ArrayList<>();
        String list;
        try {
            list = nativeList();
        } catch (UnsatisfiedLinkError e) {
            return android();
        }
        if (list == null)
            return out;
        for (String line : list.split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length < 7)
                continue;
            try {
                Pad p = new Pad();
                p.name = f[0];
                p.vendor = Integer.parseInt(f[1]);
                p.product = Integer.parseInt(f[2]);
                p.state = Integer.parseInt(f[3]);
                p.devices = Integer.parseInt(f[4]);
                p.secondsAgo = Long.parseLong(f[5]);
                p.ignored = f[6];
                out.add(p);
            } catch (NumberFormatException ignored) {
            }
        }
        out.sort((a, b) -> Boolean.compare(a.state == 2, b.state == 2));
        return out;
    }

    /** The pads Android lists (the launcher has no SDL), by the same rules as gamepads.cpp */
    static List<Pad> android() {
        List<Pad> out = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual() || !isPadSource(d.getSources()))
                continue;
            String why = ignoreReason(d.getName());
            if (!why.isEmpty() || !hasSticks(d))
                continue;
            Pad p = new Pad();
            p.name = d.getName();
            p.vendor = d.getVendorId();
            p.product = d.getProductId();
            boolean twin = false;
            for (Pad o : out)
                if (o.vendor == p.vendor && o.product == p.product && o.name.equals(p.name)) {
                    o.devices++;
                    twin = true;
                }
            if (!twin)
                out.add(p);
        }
        return out;
    }

    private static boolean isPadSource(int s) {
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static boolean hasSticks(InputDevice d) {
        return d.getMotionRange(android.view.MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null
                || d.getMotionRange(android.view.MotionEvent.AXIS_HAT_X, InputDevice.SOURCE_JOYSTICK) != null;
    }

    /** gamepads.cpp's ignoreReason in Java */
    static String ignoreReason(String name) {
        String n = name == null ? "" : name.toLowerCase(Locale.ROOT);
        for (String t : new String[] {" motion sensors", " touchpad", " imu", " consumer control", " system control",
                " keyboard", " mouse"})
            if (n.endsWith(t))
                return "not a gamepad (" + t.substring(1) + ")";
        for (String p : new String[] {"uinput-", "gpio", "fingerprint", "-keys", "_keys", "avrcp", "headset"})
            if (n.contains(p))
                return "the phone's or an accessory's keys";
        return "";
    }

    /** Every input device in the log, once at the game's start: what to look at when a pad shows twice */
    static void logDevices() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null)
                continue;
            int s = d.getSources();
            if ((s & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_DPAD)) == 0)
                continue;
            String desc = d.getDescriptor();
            Log.i("vcs", String.format(Locale.ROOT, "[gamepad] android device %d \"%s\" %04x:%04x sources 0x%08x%s%s "
                            + "sticks %b descriptor %s", id, d.getName(), d.getVendorId(), d.getProductId(), s,
                    d.isExternal() ? " external" : "", d.isVirtual() ? " virtual" : "", hasSticks(d),
                    desc == null ? "-" : desc.substring(0, Math.min(12, desc.length()))));
        }
    }
}
