// Behind everything: the sky and its glows, and on Home Vice City at dusk - a striped sun going down behind the towers
// of the beach, over a sea that moves (the app icon's motif, launcher/make_icon.py), all drawn with the renderer's own
// primitives. Alive while Home shows and the game isn't running: the sun's stripes slide toward the horizon, clouds
// come, drift and go, far-off jets draw trails that spread and fade, windows go on and off, the sea's lines roll in
// and the sun's reflection shimmers - at 30 frames a second (15 when the window isn't focused), slow enough to cost
// almost nothing; standing still with reduce motion on. Every other screen draws once and sleeps. [lcs] With Liberty
// City Stories shown, Home has Liberty City at night instead (liberty(): the app icon's bridge, moon and skyline); [stories] on the
// GTA Stories tab both, joined (stories(): Liberty City's night on the left fading into Vice City's dusk). (A later option: art
// from the player's own disc in this slot - docs/LAUNCHER.md.)
#include "shell.h"

#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;

    namespace
    {
        // the same "random" numbers every frame: a hash of an index into [0, 1)
        float hash(uint32_t n)
        {
            n ^= n >> 16;
            n *= 0x7FEB352Du;
            n ^= n >> 15;
            n *= 0x846CA68Bu;
            n ^= n >> 16;
            return float(n & 0xFFFFFF) / 16777216.0f;
        }

        float smooth(float a, float b, float x)
        {
            const float t = core::clamp01((x - a) / (b - a));
            return t * t * (3 - 2 * t);
        }

        struct Scene
        {
            float w = 0, h = 0, s = 0; // size, and the smaller side (what the scene scales with)
            float horizon = 0;
            float t = 0;               // the ambient clock, seconds
            bool mirror = false;       // drawing the reflection: y flips about the horizon
            float y(float v) const { return mirror ? 2 * horizon - v : v; }
        };

        // a rectangle standing on the scene (mirrored in the sea when reflecting)
        void block(const Scene &S, Rect r, Color c, Corners radius = {})
        {
            if (S.mirror)
            {
                r.y = 2 * S.horizon - r.y - r.h;
                radius = Corners(radius.bl, radius.br, radius.tr, radius.tl);
            }
            rect(r, c, radius);
        }

        // the towers of the beach: art deco - setbacks, parapets, a spire
        struct Tower
        {
            float x;      // left edge, a fraction of the width
            float width;  // fractions of the scene's size
            float height;
            int style;    // 0 flat, 1 stepped, 2 a parapet, 3 spire
        };
        constexpr Tower kTowers[] = {
            {0.425f, 0.050f, 0.095f, 1}, {0.468f, 0.036f, 0.158f, 3}, {0.500f, 0.058f, 0.118f, 2},
            {0.551f, 0.040f, 0.072f, 0}, {0.583f, 0.048f, 0.092f, 1}, {0.900f, 0.046f, 0.084f, 2},
            {0.938f, 0.052f, 0.128f, 1}, {0.982f, 0.044f, 0.070f, 0},
        };

        void tower(const Scene &S, const Tower &T, Color c, Rect &body)
        {
            const float bw = T.width * S.s, bh = T.height * S.s;
            body = {T.x * S.w, S.horizon - bh, bw, bh + px(1)};
            switch (T.style)
            {
            case 1: // two setbacks
                block(S, body, c);
                block(S, {body.x + bw * 0.14f, body.y - bh * 0.08f, bw * 0.72f, bh * 0.08f + px(1)}, c);
                block(S, {body.x + bw * 0.30f, body.y - bh * 0.15f, bw * 0.40f, bh * 0.07f + px(1)}, c);
                break;
            case 2: // a parapet, narrower, and a slab on it
                block(S, body, c);
                block(S, {body.x + bw * 0.08f, body.y - bh * 0.035f, bw * 0.84f, bh * 0.035f + px(1)}, c);
                block(S, {body.x + bw * 0.38f, body.y - bh * 0.09f, bw * 0.24f, bh * 0.055f + px(1)}, c);
                break;
            case 3: // a crown and a spire
                block(S, body, c);
                block(S, {body.x + bw * 0.2f, body.y - bh * 0.06f, bw * 0.6f, bh * 0.06f + px(1)}, c);
                block(S, {body.x + bw * 0.5f - px(1), body.y - bh * 0.30f, px(2), bh * 0.25f}, c);
                break;
            default:
                block(S, body, c);
            }
        }

        void sky(const Scene &S, float hero)
        {
            gradient({0, 0, S.w, S.h}, theme::kSky, theme::kNight);
            // the glow along the horizon, Home only
            if (hero > 0.01f)
                gradient({0, S.horizon - S.h * 0.42f, S.w, S.h * 0.42f + px(1)}, withAlpha(theme::kHorizon, 0),
                         withAlpha(theme::kHorizon, hero));
            // glows: a pink one up and to the right, a softer one low on the left
            shadow({S.w * 0.62f, -S.h * 0.25f, S.w * 0.5f, S.h * 0.55f}, withAlpha(theme::kPink, 0.09f + 0.05f * hero), S.s * 0.22f, S.s * 0.2f);
            shadow({-S.w * 0.2f, S.h * 0.65f, S.w * 0.55f, S.h * 0.5f}, withAlpha(theme::kPinkHi, 0.06f), S.s * 0.25f, S.s * 0.2f);
        }

        // a few stars in the dark, each twinkling at its own pace
        void stars(const Scene &S)
        {
            const float dark = theme::darkness;
            if (dark < 0.01f)
                return;
            for (uint32_t i = 0; i < 60; ++i)
            {
                const float x = hash(i * 3) * S.w, y = std::pow(hash(i * 3 + 1), 1.6f) * S.horizon * 0.62f;
                const float twinkle = 0.55f + 0.45f * std::sin(S.t * (0.6f + hash(i * 5) * 1.4f) + hash(i * 7) * 6.28f);
                const float a = (0.25f + 0.55f * hash(i * 3 + 2)) * twinkle * dark;
                circle(x, y, px(0.55f + 0.7f * hash(i * 11)), withAlpha(theme::kText, a));
            }
        }

        // a flight's own random number for one of its traits: the flight and the trait mixed apart first, so neighbouring
        // flights (and a flight's traits) don't come out alike
        float trait(int flight, uint32_t which)
        {
            return hash(uint32_t(flight) * 0x9E3779B9u ^ (which * 0x85EBCA6Bu + 0x27D4EB2Fu));
        }

        // how much of a trail to keep where it crosses behind the words on the left or the title bar (they stay easy to
        // read; a jet coming down from the top seems to come out from under the bar)
        float behindWords(const Scene &S, float x, float y)
        {
            const float inside = (1.0f - smooth(S.w * 0.50f, S.w * 0.58f, x)) * smooth(S.h * 0.17f, S.h * 0.23f, y) *
                                 (1.0f - smooth(S.h * 0.56f, S.h * 0.63f, y));
            const float bar = px(theme::kTitleBar);
            return (1.0f - 0.7f * inside) * (0.15f + 0.85f * smooth(bar * 0.8f, bar * 1.5f, y));
        }

        // jets crossing high up, far away: a speck drawing a trail that starts thin and bright behind it, then spreads,
        // drifts and fades. A flight may set off in each 24 s slot, at a random moment in it (some slots stay empty).
        // Each flies a line of its own: through a random point of the upper sky at up to 24 degrees either way, so it
        // comes in from the left or right edge or down from the top and leaves by another edge or behind the skyline.
        // Slots pair up flying opposite ways (in random order); each flight also has its own pace, distance (a far one
        // is smaller, fainter, slower), how long its trail lasts and how patchy it is - all from its number, so nothing
        // is stored. Usually one to three trails are in the sky
        void jets(const Scene &S)
        {
            constexpr float kSlot = 24;
            const float dark = theme::darkness;
            const int last = int(std::floor(S.t / kSlot));
            for (int k = last - 6; k <= last; ++k)
            {
                if (trait(k, 0) < 0.15f)
                    continue; // a quiet spell
                const float start = float(k) * kSlot + trait(k, 1) * 20.0f;
                const float far = trait(k, 2); // 0 near .. 1 far
                // its line: a point it passes and an angle; it enters and leaves where that line meets the edges of the
                // window (a little outside them) or goes down behind the skyline
                const int pair = k >= 0 ? k / 2 : (k - 1) / 2;
                const bool rightward = (trait(pair, 5) < 0.5f) != (k - pair * 2 == 1);
                const float angle = (trait(k, 6) * 2.0f - 1.0f) * 24.0f * 3.14159265f / 180.0f;
                const float dx = std::cos(angle) * (rightward ? 1.0f : -1.0f), dy = std::sin(angle);
                const float px0 = S.w * (0.10f + 0.80f * trait(k, 7)), py0 = S.h * (0.08f + 0.24f * trait(k, 12));
                float lo = -1e9f, hi = 1e9f;
                auto slab = [&](float from, float d, float a, float b) {
                    if (std::fabs(d) < 1e-6f)
                        return;
                    const float t0 = (a - from) / d, t1 = (b - from) / d;
                    lo = std::max(lo, std::min(t0, t1));
                    hi = std::min(hi, std::max(t0, t1));
                };
                slab(px0, dx, -0.05f * S.w, 1.05f * S.w);
                slab(py0, dy, -0.04f * S.h, S.horizon - S.s * 0.05f);
                const float ax = px0 + dx * lo, ay = py0 + dy * lo, pathLen = hi - lo;
                const float speed = S.w * 1.1f / ((30.0f + 22.0f * trait(k, 3)) * (0.9f + 0.3f * far)); // pixels a second
                const float cross = pathLen / speed;                                                     // seconds
                const float fade = 22.0f + 18.0f * trait(k, 4); // how long its trail lasts
                const float since = S.t - start;
                if (since < 0 || since > cross + fade || pathLen <= 0)
                    continue;
                const float size = 1.25f - 0.5f * far, strength = (0.85f - 0.35f * dark) * (1.0f - 0.35f * far);
                const float patchy = 0.6f * trait(k, 8), patchFreq = 8.0f + 14.0f * trait(k, 9), patchPhase = 6.28f * trait(k, 10);
                const float jet = std::min(since / cross, 1.0f);

                // the trail: soft dots along the line, wider, fainter and lower the longer ago the jet passed
                float u = 0;
                while (u < jet)
                {
                    const float age = since - u * cross; // seconds since the jet was here
                    const float life = 1.0f - smooth(fade * 0.3f, fade, age);
                    const float width = (px(1.4f) + age * px(0.45f)) * size;
                    const float step = std::max(px(2.5f), width * 0.55f);
                    if (age > 0.35f && life > 0.01f) // it forms a little behind the jet
                    {
                        const float x = ax + dx * pathLen * u, y = ay + dy * pathLen * u + age * px(0.12f);
                        const float patch = 1.0f - patchy * (0.5f + 0.5f * std::sin(u * patchFreq + patchPhase));
                        const float a = strength * life * patch * behindWords(S, x, y) * smooth(0.35f, 1.2f, age) *
                                        std::min(1.0f, step / width * 1.6f);
                        shadow({x - width * 0.5f, y - width * 0.5f, width, width}, withAlpha(theme::kContrail, a), width * 0.35f, width * 0.5f);
                    }
                    u += step / pathLen;
                }
                // the jet itself, a speck catching the sun, its strobe blinking at night
                if (since < cross)
                {
                    const float x = ax + dx * pathLen * jet, y = ay + dy * pathLen * jet;
                    circle(x, y, px(1.1f) * size, withAlpha(theme::kContrail, 0.9f));
                    if (dark > 0.01f && std::fmod(S.t + trait(k, 11), 1.3f) < 0.08f)
                    {
                        shadow({x - px(3), y - px(3), px(6), px(6)}, withAlpha(theme::kText, 0.5f * dark), px(1.5f), px(3));
                        circle(x, y, px(1.0f) * size, withAlpha(theme::kText, dark));
                    }
                }
            }
        }

        void sun(const Scene &S, float cx, float cy, float radius)
        {
            // its glow over the horizon, two layers
            shadow({cx - radius * 1.3f, cy - radius * 1.2f, radius * 2.6f, radius * 2.4f}, withAlpha(theme::kPinkHi, 0.16f), radius * 0.6f, radius);
            shadow({cx - radius * 1.05f, cy - radius * 1.05f, radius * 2.1f, radius * 2.1f}, withAlpha(theme::kPink, 0.22f), radius * 0.18f, radius);

            // the disc, solid above its middle, then in slices sliding down into the horizon; the slices thin and the
            // gaps between them widen toward it
            const Rect disc{cx - radius, cy - radius, radius * 2, radius * 2};
            // [lcs] LCS's text on its amber is dark: the sun's top still lightens toward white
            const Color top = mix(theme::kPinkHi, mix(theme::kOnPink, core::rgb(0xFFFFFF), theme::lcsness), 0.22f), bottom = theme::kPink;
            const float from = std::round(cy - radius * 0.05f), span = S.horizon - from;
            pushClip({0, 0, S.w, S.horizon});
            pushClip({0, disc.y, S.w, from - disc.y});
            gradient(disc, top, bottom, radius);
            popClip();
            constexpr int kBands = 6;
            const float slide = S.t * 0.10f - std::floor(S.t * 0.10f);
            auto yAt = [&](float u) { return from + span * u * (1.35f - 0.35f * u); };
            for (int k = -1; k < kBands; ++k)
            {
                const float u0 = (float(k) + slide) / kBands, u1 = (float(k) + 1 + slide) / kBands;
                const float gap = 0.10f + 0.55f * core::clamp01((u0 + u1) * 0.5f);
                const float y0 = yAt(std::max(u0, 0.0f)), y1 = yAt(std::min(u0 + (u1 - u0) * (1 - gap), 1.0f));
                if (y1 <= y0)
                    continue;
                pushClip({0, y0, S.w, y1 - y0});
                gradient(disc, top, bottom, radius);
                popClip();
            }
            popClip();
        }

        // streaks across the sun, coming and going: one appears every 10 s or so at a random place around the sun,
        // fades in, drifts right for 35-55 s and fades out, so three to five are in the sky at any time and the sky never
        // runs out of them. Each has its own height, length, thickness, pace and maybe a thinner one riding above it
        // (traits from its number, as the jets'). Dark with a lit edge at night, white by day; faded out over the words
        void clouds(const Scene &S, float cx, float radius)
        {
            constexpr float kSlot = 10, kLongest = 55;
            const float dark = theme::darkness;
            const int last = int(std::floor(S.t / kSlot));
            for (int k = last - int(kLongest / kSlot) - 1; k <= last; ++k)
            {
                const float start = float(k) * kSlot + trait(k, 20) * kSlot;
                const float life = 35.0f + 20.0f * trait(k, 21);
                const float since = S.t - start;
                if (since < 0 || since > life)
                    continue;
                const float shown = smooth(0.0f, 6.0f, since) * (1.0f - smooth(life - 9.0f, life, since));
                const float len = S.s * (0.16f + 0.24f * trait(k, 22));
                const float thick = std::max(S.s * (0.0045f + 0.0035f * trait(k, 23)), px(2.5f));
                const float y = S.h * (0.36f + 0.25f * trait(k, 24));
                const float x = cx - radius * 1.5f + radius * 2.3f * trait(k, 25) - len * 0.5f + since * px(3.0f + 5.0f * trait(k, 26));
                const float a = shown * smooth(S.w * 0.50f, S.w * 0.64f, x + len * 0.5f);
                if (a <= 0.01f)
                    continue;
                // a crisp streak, maybe a thinner one riding above it, in a faint haze
                shadow({x - len * 0.1f, y - thick, len * 1.2f, thick * 3}, withAlpha(theme::kCloud, 0.25f * a), thick * 1.2f, thick);
                rect({x, y, len, thick}, withAlpha(theme::kCloud, 0.92f * a), thick * 0.5f);
                if (trait(k, 27) < 0.6f)
                {
                    const float rx = x + len * (0.1f + 0.3f * trait(k, 28)), rw = len * (0.3f + 0.25f * trait(k, 29));
                    rect({rx, y - thick * 1.7f, rw, thick * 0.7f}, withAlpha(theme::kCloud, 0.85f * a), thick * 0.35f);
                }
                if (dark > 0.01f)
                    rect({x + len * 0.08f, y + thick - px(1), len * 0.84f, px(1)}, withAlpha(theme::kPinkHi, 0.30f * dark * a), px(0.5f));
            }
        }

        // a building's windows: each goes on or off now and then, on its own schedule. `scale` shrinks the grid (the far
        // row), `strength` dims it
        void windows(const Scene &S, Rect body, uint32_t building, float scale, float strength)
        {
            const float ww = px(2) * scale, wh = px(2.6f) * scale, gx = px(5) * scale, gy = px(6.5f) * scale;
            const float top = px(8) * scale;
            const int cols = int((body.w - px(6) * scale) / gx), rows = int((body.h - top - px(4) * scale) / gy);
            if (cols <= 0 || rows <= 0)
                return;
            const float x0 = body.x + (body.w - float(cols - 1) * gx - ww) * 0.5f;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const uint32_t id = building * 977 + uint32_t(r) * 31 + uint32_t(c);
                    const float epoch = std::floor(S.t / 50.0f + hash(id * 3) * 50.0f);
                    if (hash(id * 5 + uint32_t(epoch) * 7919) > 0.3f)
                        continue;
                    rect({x0 + float(c) * gx, body.y + top + float(r) * gy, ww, wh},
                         withAlpha(theme::kLights, strength * (0.45f + 0.4f * theme::darkness) * (0.6f + 0.4f * hash(id * 9))));
                }
        }

        // the towers; once on the sky, once mirrored in the sea (lights off, sunk toward the sea's colour by `faded`:
        // opaque, so where pieces overlap nothing shows darker)
        void skyline(const Scene &S, bool lights, float faded = 0)
        {
            const Color near = mix(theme::kSkyline, theme::kSea, faded);
            // far towers, lower and paler in the haze, their windows smaller and dimmer
            const Color far = mix(mix(theme::kSkyline, theme::kHorizon, 0.55f), theme::kSea, faded);
            float x = S.w * 0.30f;
            for (uint32_t i = 0; x < S.w; ++i)
            {
                const float bw = S.s * (0.022f + 0.03f * hash(i * 13 + 1));
                const float bh = S.s * (0.016f + 0.045f * hash(i * 13 + 2));
                const Rect body{x, S.horizon - bh, bw + px(1), bh + px(1)};
                block(S, body, far);
                if (lights)
                    windows(S, body, 5000 + i, 0.7f, 0.55f);
                x += bw;
            }

            float tallest = 0;
            Rect beacon{};
            for (uint32_t b = 0; b < std::size(kTowers); ++b)
            {
                Rect body;
                tower(S, kTowers[b], near, body);
                if (body.h > tallest)
                {
                    tallest = body.h;
                    beacon = body;
                }
                if (lights)
                    windows(S, body, b, 1.0f, 1.0f);
            }
            // the tallest tower's light, blinking
            if (lights && std::fmod(S.t, 1.6f) < 0.18f)
            {
                const float bx = beacon.cx(), by = beacon.y - beacon.h * 0.30f - px(1);
                shadow({bx - px(4), by - px(4), px(8), px(8)}, withAlpha(theme::kPinkHi, 0.6f), px(2.5f), px(4));
                circle(bx, by, px(1.5f), theme::kPinkHi);
            }
        }

        void sea(Scene &S, float cx, float radius)
        {
            const float top = S.horizon, depth = S.h - top;
            gradient({0, top, S.w, depth}, theme::kSea, theme::kNight);
            pushClip({0, top, S.w, depth});

            // the towers upside down, faint, fading out with depth
            const float reach = depth * 0.45f;
            S.mirror = true;
            pushClip({0, top, S.w, reach});
            skyline(S, false, 0.8f);
            popClip();
            S.mirror = false;
            gradient({0, top, S.w, reach + px(1)}, withAlpha(theme::kSea, 0), mix(theme::kSea, theme::kNight, 0.45f));

            // the sun's light on the water: a soft column and short lines that waver
            shadow({cx - radius * 0.6f, top, radius * 1.2f, depth * 0.75f}, withAlpha(theme::kPink, 0.10f), radius * 0.3f, radius * 0.5f);
            for (int i = 0; i < 9; ++i)
            {
                const float fi = float(i);
                const float ly = top + px(7) + fi * px(9) * (1 + fi * 0.35f);
                if (ly > S.h)
                    break;
                const float rw = radius * (1.45f - 0.14f * fi) * (0.82f + 0.18f * std::sin(S.t * 1.1f + fi * 1.7f));
                const float dx = std::sin(S.t * 0.7f + fi * 2.3f) * px(5);
                rect({cx - rw * 0.5f + dx, std::round(ly), rw, px(2.5f)},
                     withAlpha(mix(theme::kPinkHi, theme::kPink, fi / 9.0f), 0.34f - 0.032f * fi), px(1.25f));
            }

            // lines on the water rolling in: close together at the horizon, spreading as they come
            constexpr int kLines = 11;
            const float roll = S.t * 0.05f;
            for (int i = 0; i < kLines; ++i)
            {
                float u = (float(i) + roll * kLines) / kLines;
                u -= std::floor(u);
                const float ly = top + depth * u * u;
                const float a = (0.13f * (1 - u) + 0.03f) * smooth(0.0f, 0.06f, u);
                rect({0, ly, S.w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, a));
            }
            popClip();

            // the horizon, a line of light
            shadow({0, top - px(2), S.w, px(4)}, withAlpha(theme::kPink, 0.35f), px(3), 0);
            rect({0, top, S.w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, 0.6f));
        }

        // ---- [lcs] Liberty City at night, from the water: the app icon's scene (launcher/make_icon.py) as a panorama.
        // A suspension bridge spans the window - a near tower on the right with its legs in the water, a smaller far
        // one in the middle, the cable sagging between them past a big amber moon - in front of a lit skyline in three
        // layers (a pale far shore, the plum city with setbacks, a spire and pyramid roofs, darker buildings near the
        // edges), under a sky going from navy to crimson, over water holding the city's and the moon's broken
        // reflections. Alive like the sunset: windows go on and off, the moon's reflection shimmers, traffic crosses
        // the deck, the towers' lights blink. The left side, under the words, stays low and dark; in the light palette
        // the same city by day, pale. Its own colours (the icon's, a little calmer), not the theme's: the scene is
        // the same picture in either palette and only goes pale by day.
        struct LibertyColors
        {
            Color sky0, sky1, sky2, sky3;  // the sky from the top down to the horizon
            Color sea0, sea1, sea2;        // the water from the horizon down
            Color far;                     // the far shore
            Color cityTop, cityBottom;     // the city's gradient
            Color nearTop, nearBottom;     // the near buildings'
            Color rim, window;             // the sky's light on roofs, lit windows
            Color bridge;                  // steel
            Color moonTop, moonBottom, glow, shimmer;
            Color star, beacon, head, tail; // stars, the towers' lights, traffic going each way
        };
        constexpr LibertyColors kLibertyNight{
            .sky0 = core::rgb(0x0B0A24), .sky1 = core::rgb(0x2A1034), .sky2 = core::rgb(0x861C24), .sky3 = core::rgb(0xE0582F),
            .sea0 = core::rgb(0x5E1A24), .sea1 = core::rgb(0x2A1028), .sea2 = core::rgb(0x0A0920),
            .far = core::rgb(0x5C1C2E),
            .cityTop = core::rgb(0x55244A), .cityBottom = core::rgb(0x261030),
            .nearTop = core::rgb(0x241030), .nearBottom = core::rgb(0x12081C),
            .rim = core::rgb(0xF07A4A), .window = core::rgb(0xFFC560),
            .bridge = core::rgb(0x07061A),
            .moonTop = core::rgb(0xFFDC90), .moonBottom = core::rgb(0xF0861A), .glow = core::rgb(0xF9B035), .shimmer = core::rgb(0xF9B035),
            .star = core::rgb(0xF4E8E0), .beacon = core::rgb(0xFF3B2B), .head = core::rgb(0xFFF1D6), .tail = core::rgb(0xFF4636),
        };
        constexpr LibertyColors kLibertyDay{
            .sky0 = core::rgb(0xD5DFF0), .sky1 = core::rgb(0xE4DDE8), .sky2 = core::rgb(0xF4CFB8), .sky3 = core::rgb(0xF7B98C),
            .sea0 = core::rgb(0xEDC9B8), .sea1 = core::rgb(0xE2E0EC), .sea2 = core::rgb(0xEAEEF7),
            .far = core::rgb(0xC6B3C6),
            .cityTop = core::rgb(0x8E7CA2), .cityBottom = core::rgb(0x5E5680),
            .nearTop = core::rgb(0x4C4670), .nearBottom = core::rgb(0x383458),
            .rim = core::rgb(0xFFF0D2), .window = core::rgb(0xFFE7B0),
            .bridge = core::rgb(0x2A2E52),
            .moonTop = core::rgb(0xFFFCF2), .moonBottom = core::rgb(0xFFE3A6), .glow = core::rgb(0xFFCC6A), .shimmer = core::rgb(0xF2A20C),
            .star = core::rgb(0xFFFFFF), .beacon = core::rgb(0xE5392A), .head = core::rgb(0xFFFFFF), .tail = core::rgb(0xE5392A),
        };

        LibertyColors libertyColors()
        {
            LibertyColors out;
            const Color *a = reinterpret_cast<const Color *>(&kLibertyDay);
            const Color *b = reinterpret_cast<const Color *>(&kLibertyNight);
            Color *o = reinterpret_cast<Color *>(&out);
            for (size_t i = 0; i < sizeof(LibertyColors) / sizeof(Color); ++i)
                o[i] = mix(a[i], b[i], theme::darkness);
            return out;
        }

        // the bridge and the moon, laid out for the window
        struct Liberty
        {
            LibertyColors c;
            float deckTop = 0, deckH = 0;
            float nearX = 0, nearTop = 0, farX = 0, farTop = 0; // the towers' centres and tops
            float moonX = 0, moonY = 0, moonR = 0;
        };

        // a cable hanging between (xa, ya) and (xb, yb), lowest at ym (y down): y = a (x - xm)^2 + ym
        struct Cable
        {
            float xm = 0, a = 0, ym = 0;
            float at(float x) const { return a * (x - xm) * (x - xm) + ym; }
        };
        Cable hang(float xa, float ya, float xb, float yb, float ym)
        {
            const float ra = std::sqrt(std::max(ym - ya, 0.0f)), rb = std::sqrt(std::max(ym - yb, 0.0f));
            Cable k;
            k.xm = (xa * rb + xb * ra) / std::max(ra + rb, 1e-3f);
            k.ym = ym;
            const float d = xa - k.xm;
            k.a = std::fabs(d) > 1e-3f ? (ya - ym) / (d * d) : 0.0f;
            return k;
        }

        // a rectangle with a vertical gradient taken from the band [y0, y1] (so neighbours match), mirrored in the
        // water when reflecting (the gradient flips with it)
        void banded(const Scene &S, Rect r, Color top, Color bottom, float y0, float y1)
        {
            const Color a = mix(top, bottom, (r.y - y0) / (y1 - y0)), b = mix(top, bottom, (r.bottom() - y0) / (y1 - y0));
            if (S.mirror)
            {
                r.y = 2 * S.horizon - r.y - r.h;
                gradient(r, b, a);
            }
            else
                gradient(r, a, b);
        }

        // the sky in four stops, top to horizon
        void libertySky(const Scene &S, const LibertyColors &c)
        {
            const float h = S.horizon;
            gradient({0, 0, S.w, h * 0.50f + 1}, c.sky0, c.sky1);
            gradient({0, h * 0.50f, S.w, h * 0.32f + 1}, c.sky1, c.sky2);
            gradient({0, h * 0.82f, S.w, h * 0.18f + 1}, c.sky2, c.sky3);
        }

        // a building's lit windows, each on or off on its own schedule (as the towers' on Vice Beach). In the water:
        // longer, fainter, wavering streaks
        void libertyWindows(const Scene &S, Rect body, uint32_t building, float scale, float strength, Color color)
        {
            const float ww = std::max(1.0f, std::round(S.s * 0.0044f * scale)), wh = std::max(1.0f, std::round(ww * 1.35f));
            const float gx = std::round(ww * 2.2f), gy = std::round(wh * 2.15f);
            const float top = gy * 0.9f;
            const int cols = int((body.w - ww * 1.6f) / gx) + 1, rows = int((body.h - top - gy) / gy);
            if (cols <= 0 || rows <= 0)
                return;
            const float x0 = std::round(body.x + (body.w - float(cols - 1) * gx - ww) * 0.5f);
            const float lit = 0.35f + 0.65f * theme::darkness;
            for (int r = 0; r < rows; ++r)
                for (int col = 0; col < cols; ++col)
                {
                    const uint32_t id = building * 977 + uint32_t(r) * 31 + uint32_t(col);
                    const float epoch = std::floor(S.t / 45.0f + hash(id * 3) * 45.0f);
                    if (hash(id * 5 + uint32_t(epoch) * 7919) > 0.40f)
                        continue;
                    const float a = strength * lit * (0.55f + 0.45f * hash(id * 9));
                    const float y = std::round(body.y + top + float(r) * gy);
                    if (!S.mirror) // dimmer under the words on the left
                        rect({x0 + float(col) * gx, y, ww, wh},
                             withAlpha(color, a * (0.55f + 0.45f * smooth(S.w * 0.42f, S.w * 0.58f, x0))));
                    else
                    {
                        const float my = 2 * S.horizon - y - wh * 2.2f;
                        const float wob = std::sin(S.t * 1.3f + my * 0.21f + float(col)) * ww * 0.6f;
                        const float near = 1.0f - core::clamp01((my - S.horizon) / (S.s * 0.3f));
                        if (hash(id * 13 + 1) < 0.45f)
                            rect({x0 + float(col) * gx + wob, my, ww, wh * 2.2f}, withAlpha(color, a * 0.24f * near));
                    }
                }
        }

        struct Building
        {
            Rect body;
            int crown; // 0 flat, 1 a setback, 2 two setbacks and a spire, 3 a pyramid roof
            uint32_t id;
        };

        // the city's middle row: low on the left (under the words), highest behind the moon, lower again past the
        // near tower. Widths and heights in the scene's size, so a wide window gets more city, not wider buildings
        void cityRow(const Scene &S, const Liberty &L, std::vector<Building> &out)
        {
            out.clear();
            float x = -S.s * 0.02f;
            size_t tallest = size_t(-1);
            float tallestH = 0;
            for (uint32_t i = 0; x < S.w; ++i)
            {
                const float u = x / S.w;
                const float bw = S.s * (0.036f + 0.028f * hash(i * 13 + 101));
                const float peak = smooth(0.40f, 0.60f, u) * (1.0f - 0.5f * smooth(0.80f, 0.93f, u));
                const float p = 0.22f + 0.78f * peak;
                const float bh = S.s * (0.045f + 0.21f * p * (0.45f + 0.55f * hash(i * 13 + 102)));
                const float k = hash(i * 13 + 103);
                Building b{{std::round(x), S.horizon - bh, std::round(bw) + 1, bh + 1}, k < 0.42f ? 0 : k < 0.78f ? 1 : 3, 7000 + i};
                // the spire goes on the tallest between the far tower and the near one, away from the moon's middle
                if (b.body.h > tallestH && x > L.farX + S.s * 0.05f && x < L.nearX - S.s * 0.08f &&
                    std::fabs(b.body.cx() - L.moonX) > L.moonR * 0.6f)
                {
                    tallest = out.size();
                    tallestH = b.body.h;
                }
                out.push_back(b);
                x += bw * (0.98f + 0.1f * hash(i * 13 + 104));
            }
            if (tallest < out.size())
                out[tallest].crown = 2; // the one with the spire
        }

        // one building and its crown; returns the roofs' rims for the sky's light
        void building(const Scene &S, const Building &b, Color top, Color bottom, float y0, float y1, Color rim, float rimA)
        {
            const Rect &r = b.body;
            const float w = r.w, rimH = std::max(1.0f, std::round(S.s * 0.0035f));
            banded(S, r, top, bottom, y0, y1);
            auto roof = [&](float x, float y, float rw) {
                if (!S.mirror && rimA > 0.01f)
                    rect({x, y, rw, rimH}, withAlpha(rim, rimA));
            };
            switch (b.crown)
            {
            case 1:
            {
                const Rect s{std::round(r.x + w * 0.2f), r.y - S.s * 0.016f, std::round(w * 0.6f), S.s * 0.016f + 1};
                banded(S, s, top, bottom, y0, y1);
                roof(r.x, r.y, w);
                roof(s.x, s.y, s.w);
                break;
            }
            case 2:
            {
                const Rect s1{std::round(r.x + w * 0.15f), r.y - S.s * 0.018f, std::round(w * 0.7f), S.s * 0.018f + 1};
                const Rect s2{std::round(r.x + w * 0.32f), s1.y - S.s * 0.018f, std::round(w * 0.36f), S.s * 0.018f + 1};
                const float sw = std::max(1.5f, S.s * 0.004f);
                const Rect spire{r.cx() - sw * 0.5f, s2.y - S.s * 0.05f, sw, S.s * 0.05f + 1};
                banded(S, s1, top, bottom, y0, y1);
                banded(S, s2, top, bottom, y0, y1);
                banded(S, spire, top, bottom, y0, y1);
                roof(r.x, r.y, w);
                roof(s1.x, s1.y, s1.w);
                roof(s2.x, s2.y, s2.w);
                break;
            }
            case 3:
            {
                // a pyramid roof (a quad with a tiny flat top)
                const float ph = S.s * 0.022f, yb = S.mirror ? 2 * S.horizon - r.y : r.y + 0.5f;
                const float yt = S.mirror ? yb + ph : yb - ph;
                const Color c = S.mirror ? mix(top, bottom, (r.y - y0) / (y1 - y0)) : mix(top, bottom, (r.y - ph - y0) / (y1 - y0));
                const float q[8] = {r.x, yb, r.cx() - 0.5f, yt, r.cx() + 0.5f, yt, r.right(), yb};
                quad(q, c);
                if (!S.mirror && rimA > 0.01f)
                {
                    // the lit edge on the moon's side (the right)
                    line(r.cx(), yt, r.right() - 0.5f, yb, rimH, withAlpha(rim, rimA * 0.8f));
                }
                break;
            }
            default:
                roof(r.x, r.y, w);
            }
        }

        // the city: far shore, middle row, near buildings (`faded` toward the water for the reflection)
        void libertyCity(const Scene &S, const Liberty &L, bool lights, float faded)
        {
            const LibertyColors &c = L.c;
            const Color water = c.sea1;
            const float y0 = S.horizon - S.s * 0.32f, y1 = S.horizon;

            // the far shore: a low pale row in the haze, a few dim lights
            {
                const Color far = mix(c.far, water, faded);
                float x = 0;
                for (uint32_t i = 0; x < S.w; ++i)
                {
                    const float bw = S.s * (0.022f + 0.03f * hash(i * 7 + 5));
                    const float bh = S.s * (0.018f + 0.05f * hash(i * 7 + 6)) * (0.7f + 0.5f * smooth(0.3f, 0.7f, x / S.w));
                    const Rect r{std::round(x), S.horizon - bh, std::round(bw) + 1, bh + 1};
                    block(S, r, far);
                    if (lights)
                        libertyWindows(S, r, 3000 + i, 0.7f, 0.35f, c.window);
                    x += bw;
                }
            }

            // the middle row
            static thread_local std::vector<Building> row;
            cityRow(S, L, row);
            for (const Building &b : row)
            {
                building(S, b, mix(c.cityTop, water, faded), mix(c.cityBottom, water, faded), y0, y1, c.rim, faded > 0 ? 0 : 0.8f);
                if (lights)
                    libertyWindows(S, b.body, b.id, 1.0f, 0.9f, c.window);
            }

            // near buildings, darker, at the two edges: lower on the left so the words stay clear
            for (int side = 0; side < 2; ++side)
            {
                float x = side == 0 ? -S.s * 0.03f : L.nearX + S.s * 0.06f;
                const float end = side == 0 ? S.w * 0.30f : S.w + S.s * 0.03f;
                for (uint32_t i = 0; x < end; ++i)
                {
                    const uint32_t id = 9000 + uint32_t(side) * 100 + i;
                    const float bw = S.s * (0.05f + 0.035f * hash(id * 3 + 1));
                    const float fall = side == 0 ? 1.0f - 0.65f * smooth(S.w * 0.05f, S.w * 0.30f, x) : 1.0f;
                    const float bh = S.s * (0.04f + 0.08f * hash(id * 3 + 2)) * fall;
                    const float k = hash(id * 3 + 3);
                    const Building b{{std::round(x), S.horizon - bh, std::round(bw) + 1, bh + 1}, k < 0.5f ? 0 : k < 0.8f ? 1 : 3, id};
                    building(S, b, mix(c.nearTop, water, faded), mix(c.nearBottom, water, faded), S.horizon - S.s * 0.14f, y1, c.rim,
                             faded > 0 ? 0 : 0.45f);
                    if (lights)
                        libertyWindows(S, b.body, id, 1.15f, 1.0f, c.window);
                    x += bw * (1.0f + 0.25f * hash(id * 3 + 4));
                }
            }
        }

        void moon(const Scene &S, const Liberty &L)
        {
            const LibertyColors &c = L.c;
            const float x = L.moonX, y = L.moonY, r = L.moonR;
            // its glow: wide and faint, then a brighter ring close to it
            shadow({x - r * 2.4f, y - r * 2.4f, r * 4.8f, r * 4.8f}, withAlpha(c.glow, 0.10f), r * 0.9f, r * 2.4f);
            shadow({x - r * 1.6f, y - r * 1.6f, r * 3.2f, r * 3.2f}, withAlpha(c.glow, 0.16f), r * 0.35f, r * 1.6f);
            shadow({x - r * 1.18f, y - r * 1.18f, r * 2.36f, r * 2.36f}, withAlpha(c.glow, 0.30f), r * 0.12f, r * 1.18f);
            gradient({x - r, y - r, 2 * r, 2 * r}, c.moonTop, c.moonBottom, r);
        }

        // the moon on the water: broken dashes down a column, narrower and fainter going out, each wavering
        void moonOnWater(const Scene &S, const Liberty &L)
        {
            const LibertyColors &c = L.c;
            const float top = S.horizon, r = L.moonR;
            shadow({L.moonX - r * 0.7f, top, r * 1.4f, (S.h - top) * 0.8f}, withAlpha(c.glow, 0.12f), r * 0.35f, r * 0.5f);
            const float th0 = std::max(2.0f, S.s * 0.0085f);
            float y = top + th0 * 1.2f;
            for (int i = 0; i < 9 && y < S.h; ++i)
            {
                const float fi = float(i);
                const float th = std::max(1.0f, std::round(th0 * (1.0f - 0.07f * fi)));
                const float w = r * (1.75f - 0.17f * fi) * (0.85f + 0.15f * std::sin(S.t * 1.05f + fi * 1.9f));
                const float dx = std::sin(S.t * 0.65f + fi * 2.4f) * r * 0.06f;
                const float split = 0.3f + 0.4f * (0.5f + 0.5f * std::sin(S.t * 0.4f + fi * 1.3f)); // where it breaks
                const float gap = r * 0.09f * (1.0f + 0.4f * fi * 0.2f);
                const float a = (0.75f - 0.07f * fi) * (0.55f + 0.45f * theme::darkness);
                const float x0 = L.moonX - w * 0.5f + dx;
                const Color col = withAlpha(mix(c.moonTop, c.shimmer, 0.4f + fi * 0.06f), a);
                rect({std::round(x0), std::round(y), std::round(w * split - gap * 0.5f), th}, col, th * 0.5f);
                rect({std::round(x0 + w * split + gap * 0.5f), std::round(y), std::round(w * (1 - split) - gap * 0.5f), th}, col, th * 0.5f);
                y += th * 1.9f + fi * th * 0.55f;
            }
        }

        void libertyWater(Scene &S, const Liberty &L)
        {
            const LibertyColors &c = L.c;
            const float top = S.horizon, depth = S.h - top;
            gradient({0, top, S.w, depth * 0.28f + 1}, c.sea0, c.sea1);
            gradient({0, top + depth * 0.28f, S.w, depth * 0.72f + 1}, c.sea1, c.sea2);
            pushClip({0, top, S.w, depth});

            // the city upside down, sunk toward the water, fading out with depth; its lights as wavering streaks
            const float reach = depth * 0.5f;
            S.mirror = true;
            pushClip({0, top, S.w, reach});
            libertyCity(S, L, false, 0.7f);
            // the far tower's legs
            const float lw = S.s * 0.012f, gap = S.s * 0.026f, fh = S.horizon - L.farTop;
            block(S, {L.farX - gap * 0.5f - lw, L.farTop, lw, fh}, mix(c.bridge, c.sea1, 0.45f));
            block(S, {L.farX + gap * 0.5f, L.farTop, lw, fh}, mix(c.bridge, c.sea1, 0.45f));
            popClip();
            gradient({0, top, S.w, reach + 1}, withAlpha(c.sea1, 0), c.sea1);
            static thread_local std::vector<Building> row;
            cityRow(S, L, row);
            for (const Building &b : row)
                libertyWindows(S, b.body, b.id, 1.0f, 0.9f, c.window);
            S.mirror = false;

            moonOnWater(S, L);

            // faint lines on the water rolling in, close together near the horizon
            constexpr int kLines = 10;
            const float roll = S.t * 0.04f;
            for (int i = 0; i < kLines; ++i)
            {
                float u = (float(i) + roll * kLines) / kLines;
                u -= std::floor(u);
                const float a = (0.07f * (1 - u) + 0.015f) * smooth(0.0f, 0.08f, u);
                rect({0, top + depth * u * u, S.w, 1}, withAlpha(c.sky3, a));
            }
            popClip();
        }

        // the bridge: the far tower, the deck, the cables and hangers, the near tower; the towers' lights; traffic
        void bridge(const Scene &S, const Liberty &L)
        {
            const LibertyColors &c = L.c;
            const Color steel = c.bridge;
            const float cableW = std::max(1.5f, S.s * 0.0055f), hangerW = std::max(1.0f, S.s * 0.0018f);
            const float step = S.s * 0.034f;

            // the far tower: two legs to the deck, a cap, a beam
            const float flw = S.s * 0.012f, fgap = S.s * 0.026f;
            rect({L.farX - fgap * 0.5f - flw, L.farTop, flw, S.horizon - L.farTop}, steel);
            rect({L.farX + fgap * 0.5f, L.farTop, flw, S.horizon - L.farTop}, steel);
            rect({L.farX - fgap * 0.5f - flw * 1.4f, L.farTop - S.s * 0.008f, fgap + flw * 2.8f, S.s * 0.011f}, steel);
            rect({L.farX - fgap * 0.5f, L.farTop + (S.horizon - L.farTop) * 0.36f, fgap, S.s * 0.008f}, steel);

            // the deck, over the horizon from edge to edge, and its rail
            rect({0, L.deckTop, S.w, S.horizon - L.deckTop + 1}, steel);

            // the cables: the main span between the towers, the side span from the far tower down to the left, the
            // tail from the near tower off the right edge; hangers down to the deck
            const float anchorY = L.deckTop - S.s * 0.002f;
            const Cable main = hang(L.farX, L.farTop, L.nearX, L.nearTop, L.deckTop - S.s * 0.03f);
            const float leftEnd = L.farX - (L.nearX - L.farX) * 0.62f;
            const Cable side = hang(L.farX, L.farTop, leftEnd, anchorY, anchorY + S.s * 0.003f);
            const float rightEnd = L.nearX + (L.nearX - L.farX) * 0.62f;
            const Cable tail = hang(L.nearX, L.nearTop, rightEnd, anchorY, anchorY + S.s * 0.003f);
            auto cable = [&](const Cable &k, float xa, float xb, float width) {
                constexpr int kSegments = 48;
                float px0 = xa, py0 = k.at(xa);
                for (int i = 1; i <= kSegments; ++i)
                {
                    const float x = xa + (xb - xa) * float(i) / kSegments, y = std::min(k.at(x), anchorY);
                    line(px0, py0, x, y, width, steel);
                    px0 = x;
                    py0 = y;
                }
            };
            auto hangers = [&](const Cable &k, float xa, float xb) {
                const float lo = std::min(xa, xb), hi = std::max(xa, xb);
                for (float x = lo + step * 0.7f; x < hi - step * 0.4f; x += step)
                {
                    const float y = k.at(x);
                    if (y < L.deckTop - 2)
                        rect({std::round(x - hangerW * 0.5f), y, hangerW, L.deckTop - y + 1}, steel);
                }
            };
            hangers(main, L.farX, L.nearX);
            hangers(side, leftEnd, L.farX);
            hangers(tail, L.nearX, std::min(rightEnd, S.w + step));
            cable(main, L.farX, L.nearX, cableW);
            cable(side, L.farX, leftEnd, cableW * 0.85f);
            cable(tail, L.nearX, rightEnd, cableW);

            // the near tower: two legs down into the water, a cap, two beams above the deck and one under it
            const float lw = S.s * 0.026f, gap = S.s * 0.052f;
            const float lx = L.nearX - gap * 0.5f - lw, rx = L.nearX + gap * 0.5f;
            rect({lx, L.nearTop, lw, S.h - L.nearTop + 1}, steel);
            rect({rx, L.nearTop, lw, S.h - L.nearTop + 1}, steel);
            const float span = L.deckTop - L.nearTop;
            rect({lx - S.s * 0.008f, L.nearTop - S.s * 0.016f, gap + 2 * lw + S.s * 0.016f, S.s * 0.026f}, steel);
            rect({lx + lw - 1, L.nearTop + span * 0.30f, gap + 2, S.s * 0.02f}, steel);
            rect({lx + lw - 1, L.nearTop + span * 0.66f, gap + 2, S.s * 0.018f}, steel);
            rect({lx + lw - 1, S.horizon + (S.h - S.horizon) * 0.35f, gap + 2, S.s * 0.016f}, steel);
            // the moon's light on the tower's left edges
            const float edge = std::max(1.0f, S.s * 0.0025f);
            rect({lx, L.nearTop + S.s * 0.01f, edge, L.deckTop - L.nearTop - S.s * 0.01f}, withAlpha(c.rim, 0.22f));
            rect({rx, L.nearTop + S.s * 0.01f, edge, L.deckTop - L.nearTop - S.s * 0.01f}, withAlpha(c.rim, 0.14f));

            // the towers' red lights, blinking out of step
            auto beacon = [&](float x, float y, float phase, float size) {
                const float on = std::fmod(S.t + phase, 2.2f) < 1.1f ? 1.0f : 0.25f;
                const float a = on * (0.45f + 0.55f * theme::darkness);
                shadow({x - size * 3, y - size * 3, size * 6, size * 6}, withAlpha(c.beacon, 0.5f * a), size * 1.4f, size * 3);
                circle(x, y, size, withAlpha(c.beacon, a));
            };
            const float bs = std::max(1.2f, S.s * 0.0032f);
            beacon(lx + lw * 0.5f, L.nearTop - S.s * 0.02f, 0.0f, bs);
            beacon(rx + lw * 0.5f, L.nearTop - S.s * 0.02f, 0.0f, bs);
            beacon(L.farX, L.farTop - S.s * 0.012f, 1.1f, bs * 0.8f);

            // traffic on the deck: headlights one way, tail lights the other
            const float lane = L.deckTop + L.deckH * 0.5f, dot = std::max(0.8f, S.s * 0.0015f);
            const float lit = 0.35f + 0.65f * theme::darkness;
            for (int way = 0; way < 2; ++way)
            {
                const float speed = S.s * (way ? 0.030f : 0.024f);
                const int cars = int(S.w / (S.s * 0.13f)) + 2;
                for (int i = 0; i < cars; ++i)
                {
                    const uint32_t id = uint32_t(way * 1000 + i);
                    const float loop = S.w + S.s * 0.2f;
                    float x = std::fmod(hash(id * 7) * loop + S.t * speed, loop);
                    if (way)
                        x = loop - x;
                    x -= S.s * 0.1f;
                    const Color col = way ? c.tail : c.head;
                    const float y = lane + (way ? dot * 0.8f : -dot * 0.8f);
                    circle(x, y, dot, withAlpha(col, 0.7f * lit));
                    circle(x + dot * 3.0f, y, dot, withAlpha(col, 0.7f * lit));
                }
            }
        }

        void stars(const Scene &S, const LibertyColors &c)
        {
            const float dark = theme::darkness;
            if (dark < 0.01f)
                return;
            for (uint32_t i = 0; i < 45; ++i)
            {
                const float x = hash(i * 3 + 400) * S.w, y = std::pow(hash(i * 3 + 401), 1.4f) * S.horizon * 0.5f;
                const float twinkle = 0.6f + 0.4f * std::sin(S.t * (0.5f + hash(i * 5 + 400) * 1.3f) + hash(i * 7 + 400) * 6.28f);
                const float a = (0.25f + 0.5f * hash(i * 3 + 402)) * twinkle * dark * behindWords(S, x, y);
                circle(x, y, std::max(0.6f, S.s * (0.0008f + 0.0012f * hash(i * 11 + 400))), withAlpha(c.star, a));
            }
        }

        Liberty libertyLayout(const Scene &S, float e)
        {
            Liberty L;
            L.c = libertyColors();
            L.deckH = std::max(4.0f, std::round(S.s * 0.016f));
            L.deckTop = S.horizon - L.deckH;
            // the near tower toward the right, the far one past the words, the moon between them
            L.nearX = S.w - std::max(S.w * 0.13f, S.s * 0.17f);
            L.nearTop = S.horizon - S.s * 0.50f;
            L.farX = std::max(S.w * 0.53f, L.nearX - S.s * 1.05f);
            L.farTop = S.horizon - S.s * 0.215f;
            // the moon between them, clear of the name on the left (home.cpp: its column starts at 7% of the width,
            // the name is about 520 units wide); in a narrow window it shrinks and may hide behind the near tower
            const float words = std::max(px(56), S.w * 0.07f) + px(530);
            L.moonR = std::clamp((L.nearX - words) * 0.5f, S.s * 0.075f, S.s * 0.125f);
            L.moonX = std::min(std::max(L.farX + (L.nearX - L.farX) * 0.62f, words + L.moonR), L.nearX);
            L.moonY = S.horizon - S.s * (0.25f + 0.05f * e); // it rises a little as Home appears
            return L;
        }

        void liberty(Scene &S, float e)
        {
            const Liberty L = libertyLayout(S, e);
            libertySky(S, L.c);
            stars(S, L.c);
            moon(S, L);
            libertyCity(S, L, true, 0);
            libertyWater(S, L);
            bridge(S, L);

            // the words' side kept calm: a little darker toward the left edge (light: paler)
            const Color calm = mix(L.c.sky1, L.c.sky0, 0.5f);
            gradient({0, 0, S.w * 0.62f, S.horizon}, withAlpha(calm, 0.35f * theme::darkness), withAlpha(calm, 0), {},
                     Gradient::Horizontal);
        }

        // ---- [stories] both cities, joined: the GTA Stories tab's Home. Vice City at dusk on the right - its striped
        // sun moved out toward the edge and smaller, its towers, jets, clouds and sea as on Vice City's Home - and
        // Liberty City at night on the left, laid out as if the window ended at 84% of its width (so its bridge's near
        // tower stands at about three quarters across, the moon between the towers, clear of the words). Liberty City
        // ends at the near tower's right leg (the tower hides the cut); from there its sky and water fade into Vice
        // City's, row by row, each row a horizontal gradient from Liberty's colour to nothing (smooth, and cheap: the
        // scene itself is drawn once). The deck runs on out of the tower into the fade, toward Vice City's sun: the
        // bridge crossing over. Both alive on the same clock; by day both pale.
        void stories(Scene &S, float e)
        {
            const float radius = S.s * 0.17f;
            const float cx = S.w * 0.88f;
            const float cy = S.horizon - radius * (0.15f + 0.45f * e);
            stars(S);
            sun(S, cx, cy, radius);
            jets(S);
            clouds(S, cx, radius);
            skyline(S, true);
            sea(S, cx, radius);

            Scene LS = S;
            LS.w = S.w * 0.84f;
            const Liberty L = libertyLayout(LS, e);
            const LibertyColors &c = L.c;
            // the cut: the near tower's right leg's outer edge (bridge(): legs lw wide, gap apart)
            const float lw = S.s * 0.026f, gap = S.s * 0.052f;
            const float cut = std::round(L.nearX + gap * 0.5f + lw);
            const float fade = std::max(S.w * 0.14f, S.s * 0.16f);
            pushClip({0, 0, cut, S.h});
            liberty(LS, e);
            popClip();
            // the fade: Liberty City's sky (its four stops) and water (three), a thin row at a time
            auto skyAt = [&](float y) {
                const float t = y / S.horizon;
                return t < 0.5f ? mix(c.sky0, c.sky1, t / 0.5f) : t < 0.82f ? mix(c.sky1, c.sky2, (t - 0.5f) / 0.32f)
                                                                            : mix(c.sky2, c.sky3, (t - 0.82f) / 0.18f);
            };
            auto seaAt = [&](float y) {
                const float t = (y - S.horizon) / std::max(1.0f, S.h - S.horizon);
                return t < 0.28f ? mix(c.sea0, c.sea1, t / 0.28f) : mix(c.sea1, c.sea2, (t - 0.28f) / 0.72f);
            };
            const float row = std::max(2.0f, std::round(px(3)));
            for (int part = 0; part < 2; ++part) // the sky's rows, then the water's (none straddles the horizon)
            {
                const float from = part ? S.horizon : 0, to = part ? S.h : S.horizon;
                for (float y = from; y < to; y += row)
                {
                    const float h = std::min(row, to - y), mid = y + h * 0.5f;
                    const Color col = part ? seaAt(mid) : skyAt(mid);
                    gradient({cut, y, fade, h}, col, withAlpha(col, 0), {}, Gradient::Horizontal);
                }
            }
            // the deck goes on out of the tower, toward Vice City's sun, fading as the city does
            gradient({cut - 1, L.deckTop, fade * 1.3f, S.horizon - L.deckTop + 1}, c.bridge, withAlpha(c.bridge, 0), {},
                     Gradient::Horizontal);
        }
    }

    void drawBackdrop(Rect r, float hero, bool alive)
    {
        Scene S;
        S.w = r.w;
        S.h = r.h;
        S.s = std::min(r.w, r.h);
        S.horizon = std::round(r.h * 0.70f);
        // the ambient clock: real time (a still frame with reduce motion on)
        S.t = ui::state.motion > 0 ? float(std::fmod(ui::state.wall + shell.options.clock, 3600.0)) : 20.0f;

        sky(S, hero);
        if (hero <= 0.01f)
            return;
        const float e = core::easeOutCubic(hero);
        const float radius = S.s * 0.24f;
        const float cx = S.w * 0.73f;
        const float cy = S.horizon - radius * (0.15f + 0.45f * e); // it rises a little as Home appears
        // [lcs] Vice City's sunset for VCS, Liberty City's night for LCS; a game switch crossfades the two
        const float lcs = theme::lcsness;
        if (lcs < 0.999f)
        {
            pushOpacity(hero);
            stars(S);
            sun(S, cx, cy, radius);
            jets(S); // in front of the sun; behind the cloud streaks and the towers
            clouds(S, cx, radius);
            skyline(S, true);
            sea(S, cx, radius);
            popOpacity();
        }
        if (lcs > 0.001f)
        {
            pushOpacity(hero * lcs);
            liberty(S, e);
            popOpacity();
        }
        // [stories] the GTA Stories tab: both cities, joined; a switch crossfades it with a game's own scene
        if (const float st = theme::storiesness; st > 0.001f)
        {
            pushOpacity(hero * st);
            stories(S, e);
            popOpacity();
        }

        // the next frame of the scene; never while the game runs (it wants the GPU) or with reduce motion on
        if (alive && ui::state.motion > 0)
            ui::wakeAt(ui::state.wall + (platform::window.focused ? 1.0 / 30.0 : 1.0 / 15.0));
    }
}
