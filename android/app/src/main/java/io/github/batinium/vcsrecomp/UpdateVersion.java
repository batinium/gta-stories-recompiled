// [android] The updater's plain-Java part (docs/ANDROID.md "Releases and updates"), no Android classes, so the plain-JVM
// check (tools/android_update_check.py) runs it: the version a release tag names, their order, the newest release, and
// what its notes say (sha256, game interface, a short summary). UpdateMenu does the network, the files and the screen.
//
// A release is tagged android-vX.Y.Z-N: X.Y.Z the upstream version the build is based on (the repository's VERSION),
// N our build on it (android/BUILD_NUMBER). The fork also carries upstream's plain vX.Y.Z tags: never an update.
package io.github.batinium.vcsrecomp;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class UpdateVersion implements Comparable<UpdateVersion> {
    static final String TAG_PREFIX = "android-v";
    private static final Pattern NAME = Pattern.compile("(\\d{1,3})\\.(\\d{1,2})\\.(\\d{1,2})-(\\d{1,3})");
    // the release notes' lines the updater reads (anywhere in the body, case ignored, optionally in `code` or a list)
    private static final Pattern SHA256 = Pattern.compile("(?im)^[\\s>*`-]*sha-?256\\s*:\\s*`?([0-9a-f]{64})`?\\s*$");
    private static final Pattern GAME_ABI = Pattern.compile("(?im)^[\\s>*`-]*game-abi\\s*:\\s*`?(\\d{1,6})`?\\s*$");

    final int major, minor, patch, build;

    UpdateVersion(int major, int minor, int patch, int build) {
        this.major = major;
        this.minor = minor;
        this.patch = patch;
        this.build = build;
    }

    /** "0.3.0-1" (the app's versionName), else null */
    static UpdateVersion parseName(String s) {
        if (s == null)
            return null;
        Matcher m = NAME.matcher(s.trim());
        if (!m.matches())
            return null;
        return new UpdateVersion(Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)), Integer.parseInt(m.group(3)),
                Integer.parseInt(m.group(4)));
    }

    /** "android-v0.3.0-1" (a release tag), else null: upstream's "v0.3.0" and anything else are not ours */
    static UpdateVersion parseTag(String tag) {
        if (tag == null || !tag.startsWith(TAG_PREFIX))
            return null;
        return parseName(tag.substring(TAG_PREFIX.length()));
    }

    /** The APK's versionCode, as android/app/build.gradle computes it: ordered as the versions are */
    int code() {
        return ((major * 100 + minor) * 100 + patch) * 1000 + build;
    }

    String tag() {
        return TAG_PREFIX + this;
    }

    @Override
    public String toString() {
        return major + "." + minor + "." + patch + "-" + build;
    }

    @Override
    public int compareTo(UpdateVersion o) {
        return Integer.compare(code(), o.code());
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof UpdateVersion && compareTo((UpdateVersion) o) == 0;
    }

    @Override
    public int hashCode() {
        return code();
    }

    // ---- releases ---------------------------------------------------------------------------------------------------

    static final class Asset {
        final String name, url;
        final long size;

        Asset(String name, long size, String url) {
            this.name = name;
            this.size = size;
            this.url = url;
        }
    }

    /** One entry of GET /repos/<owner>/<repo>/releases, as far as the updater reads it */
    static final class Release {
        final String tag, name, body, page;
        final boolean draft, prerelease;
        final List<Asset> assets = new ArrayList<>();
        final UpdateVersion version;

        Release(String tag, String name, String body, String page, boolean draft, boolean prerelease) {
            this.tag = tag;
            this.name = name == null ? "" : name;
            this.body = body == null ? "" : body;
            this.page = page == null ? "" : page;
            this.draft = draft;
            this.prerelease = prerelease;
            this.version = parseTag(tag);
        }

        /** The APK to install: the first .apk asset, else null */
        Asset apk() {
            for (Asset a : assets)
                if (a.name.toLowerCase(Locale.ROOT).endsWith(".apk") && a.url != null && !a.url.isEmpty())
                    return a;
            return null;
        }

        String sha256() {
            return UpdateVersion.sha256(body);
        }

        /** The game interface number the release's APK needs (src/android/game_abi.h), -1: the notes don't say */
        int gameAbi() {
            return UpdateVersion.gameAbi(body);
        }
    }

    /**
     * The newest release an update can come from: an android-v tag, not a draft, not a pre-release (a pre-release is
     * how a test build stays away from players), with an .apk; by version, not by date. null: none.
     */
    static Release newest(List<Release> releases) {
        Release best = null;
        for (Release r : releases) {
            if (r.draft || r.prerelease || r.version == null || r.apk() == null)
                continue;
            if (best == null || r.version.compareTo(best.version) > 0)
                best = r;
        }
        return best;
    }

    /** Releases with our tag that newest() skipped (no .apk, pre-release): for the "nothing found" line */
    static int skipped(List<Release> releases) {
        int n = 0;
        for (Release r : releases)
            if (r.version != null && (r.draft || r.prerelease || r.apk() == null))
                n++;
        return n;
    }

    // ---- the release notes ------------------------------------------------------------------------------------------

    /** The notes' `sha256: <64 hex>` line, lower case, else null */
    static String sha256(String body) {
        Matcher m = SHA256.matcher(body == null ? "" : body);
        return m.find() ? m.group(1).toLowerCase(Locale.ROOT) : null;
    }

    /** The notes' `game-abi: <n>` line, else -1 */
    static int gameAbi(String body) {
        Matcher m = GAME_ABI.matcher(body == null ? "" : body);
        return m.find() ? Integer.parseInt(m.group(1)) : -1;
    }

    /**
     * The notes' first lines for the screen: blank lines, the sha256 / game-abi lines and Markdown's heading / list
     * marks and emphasis dropped; at most maxLines lines and maxChars characters ("…" when cut).
     */
    static String summary(String body, int maxLines, int maxChars) {
        if (body == null)
            return "";
        StringBuilder out = new StringBuilder();
        int lines = 0;
        boolean cut = false;
        for (String raw : body.replace("\r", "").split("\n")) {
            String l = raw.trim();
            if (l.isEmpty() || SHA256.matcher(l).matches() || GAME_ABI.matcher(l).matches() || l.startsWith("```")
                    || l.startsWith("<!--"))
                continue;
            l = l.replaceFirst("^#+\\s*", "").replaceFirst("^[-*+]\\s+", "• ").replace("**", "").replace("__", "")
                    .replace("`", "").trim();
            if (l.isEmpty())
                continue;
            if (lines == maxLines) {
                cut = true;
                break;
            }
            if (out.length() > 0)
                out.append('\n');
            out.append(l);
            lines++;
            if (out.length() >= maxChars)
                break;
        }
        if (out.length() > maxChars) {
            out.setLength(Math.max(0, maxChars - 1));
            cut = true;
        }
        String s = out.toString().trim();
        return cut ? s + "…" : s;
    }
}
