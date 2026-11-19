// [android] A plain-JVM check of the updater's version and release-notes code (UpdateVersion), no Android or JUnit:
//
//   python3 tools/android_update_check.py
package io.github.batinium.vcsrecomp;

import java.util.ArrayList;
import java.util.List;

public final class UpdateVersionCheck {
    private static int failures, checks;

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    private static UpdateVersion.Release release(String tag, boolean draft, boolean pre, String body, String... apks) {
        UpdateVersion.Release r = new UpdateVersion.Release(tag, tag, body, "https://example.invalid/" + tag, draft, pre);
        for (String a : apks)
            r.assets.add(new UpdateVersion.Asset(a, 1234, "https://example.invalid/" + a));
        return r;
    }

    public static void main(String[] args) {
        // names and tags
        UpdateVersion v = UpdateVersion.parseName("0.3.0-1");
        check(v != null && v.major == 0 && v.minor == 3 && v.patch == 0 && v.build == 1, "parse 0.3.0-1");
        check(v != null && v.toString().equals("0.3.0-1") && v.tag().equals("android-v0.3.0-1"), "round trip");
        check(v != null && v.code() == 300001, "versionCode of 0.3.0-1: " + (v == null ? "-" : v.code()));
        check(UpdateVersion.parseName("0.3.0-android") == null, "the old versionName is no version");
        check(UpdateVersion.parseName("0.3.0") == null, "no build number");
        check(UpdateVersion.parseName("1.2.3-4-5") == null, "two build numbers");
        check(UpdateVersion.parseName(null) == null, "null");
        check(UpdateVersion.parseTag("v0.3.0") == null, "upstream's tag is not ours");
        check(UpdateVersion.parseTag("v0.3.0-1") == null, "no android- prefix");
        check(UpdateVersion.parseTag("android-v0.3.0") == null, "android tag without build");
        check(UpdateVersion.parseTag("android-v0.3.0-1x") == null, "trailing junk");
        check(UpdateVersion.parseTag("android-v0.3.0-1").equals(v), "tag parses to the same version");
        check(UpdateVersion.parseTag("android-v12.34.56-789").code() == 123456789, "large code");

        // order: numeric, not text; the upstream version first, then our build
        String[] ordered = {"0.2.9-9", "0.3.0-1", "0.3.0-2", "0.3.0-10", "0.3.1-1", "0.10.0-1", "1.0.0-1"};
        for (int i = 0; i + 1 < ordered.length; i++) {
            UpdateVersion a = UpdateVersion.parseName(ordered[i]), b = UpdateVersion.parseName(ordered[i + 1]);
            check(a.compareTo(b) < 0 && b.compareTo(a) > 0 && a.code() < b.code(), ordered[i] + " < " + ordered[i + 1]);
        }
        check(UpdateVersion.parseName("0.3.0-1").compareTo(UpdateVersion.parseName("0.3.0-1")) == 0, "equal");
        // versionCode stays above the first APKs' 1 and below int's limit at the format's largest version
        check(UpdateVersion.parseName("0.0.0-2").code() > 1, "above the old versionCode 1");
        check(UpdateVersion.parseName("999.99.99-999").code() > 0, "no overflow");

        // the newest release: ours only, no drafts / pre-releases / releases without an .apk; by version, not order
        List<UpdateVersion.Release> rs = new ArrayList<>();
        check(UpdateVersion.newest(rs) == null, "no releases");
        rs.add(release("v0.4.0", false, false, "", "app.apk"));
        check(UpdateVersion.newest(rs) == null, "upstream tag only");
        rs.add(release("android-v0.3.0-2", false, false, "", "vcs-recomp-0.3.0-2.apk", "notes.txt"));
        rs.add(release("android-v0.3.0-10", false, false, "", "vcs-recomp-0.3.0-10.APK"));
        rs.add(release("android-v0.3.1-1", true, false, "", "a.apk"));
        rs.add(release("android-v0.3.1-2", false, true, "", "a.apk"));
        rs.add(release("android-v0.3.1-3", false, false, "", "source.zip"));
        UpdateVersion.Release n = UpdateVersion.newest(rs);
        check(n != null && n.tag.equals("android-v0.3.0-10"), "newest: " + (n == null ? "-" : n.tag));
        check(n != null && n.apk().name.equals("vcs-recomp-0.3.0-10.APK"), "its .apk");
        check(UpdateVersion.skipped(rs) == 3, "skipped " + UpdateVersion.skipped(rs));
        UpdateVersion.Release two = rs.get(1);
        check(two.apk().name.endsWith(".apk"), "the .apk among other assets");

        // the notes
        String hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        String body = "## Faster loading\r\n\r\n- **Saves** import from PS3\n- rain fixed\n* third\nfourth line\n\n"
                + "sha256: " + hex.toUpperCase() + "\ngame-abi: 1\n";
        check(hex.equals(UpdateVersion.sha256(body)), "sha256, lower-cased");
        check(UpdateVersion.gameAbi(body) == 1, "game-abi");
        check(hex.equals(UpdateVersion.sha256("- `sha256: " + hex + "`")), "sha256 in a list / code");
        check(hex.equals(UpdateVersion.sha256("SHA-256: " + hex)), "SHA-256 spelling");
        check(UpdateVersion.sha256("sha256: " + hex.substring(1)) == null, "63 digits");
        check(UpdateVersion.sha256("sha256: " + hex + "0") == null, "65 digits");
        check(UpdateVersion.sha256("the sha256: " + hex) == null, "not at the start of a line");
        check(UpdateVersion.gameAbi("Game-ABI: `12`") == 12, "game-abi in code");
        check(UpdateVersion.gameAbi("no line") == -1 && UpdateVersion.gameAbi(null) == -1, "game-abi absent");
        check(UpdateVersion.sha256(null) == null, "sha256 of null");
        String s = UpdateVersion.summary(body, 3, 400);
        check(s.equals("Faster loading\n• Saves import from PS3\n• rain fixed…"), "summary 3 lines: [" + s + "]");
        s = UpdateVersion.summary(body, 10, 400);
        check(s.equals("Faster loading\n• Saves import from PS3\n• rain fixed\n• third\nfourth line"), "summary all: [" + s + "]");
        s = UpdateVersion.summary(body, 10, 20);
        check(s.length() == 20 && s.endsWith("…"), "summary cut at 20: [" + s + "]");
        check(UpdateVersion.summary("", 3, 100).isEmpty() && UpdateVersion.summary(null, 3, 100).isEmpty(), "empty");
        check(UpdateVersion.summary("sha256: " + hex + "\ngame-abi: 2", 3, 100).isEmpty(), "only machine lines");
        UpdateVersion.Release withNotes = release("android-v0.3.0-1", false, false, body, "a.apk");
        check(hex.equals(withNotes.sha256()) && withNotes.gameAbi() == 1, "a release's own lines");

        System.out.println(checks + " checks, " + failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
