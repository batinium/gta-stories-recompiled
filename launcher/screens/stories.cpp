// [stories] The GTA Stories tab: both cities at once (docs/STORIES.md) - Liberty City Stories in front, Vice City Stories
// running behind it as the city across the sea (tools/stories.py). Its Home says where each city stands (its disc, its
// build), what is still needed, and does it right here without leaving the tab: the missing disc is chosen (the usual picker
// and check), "Build and play" builds each city that is not built or out of date (the same build tasks as the two game
// tabs, one after the other: App::startStoriesBuild), then Play starts both. Before Play the Stories packs are made or refreshed from the player's own discs
// (tools/stories_auto.py; screens.cpp play()). Liberty City is the shown game underneath (App::stories, gameId "lcs").
//
// storiesLogo(): the tab's logo - the two icons this project has had, joined: Liberty City's suspension bridge and amber
// moon at night on the left, its deck crossing the water to Vice City's striped neon sun and a palm at dusk on the right.
// The shapes and colours are launcher/make_icon.py's "stories" scene (its 108-unit grid, the 72-unit view from 18),
// drawn here with the renderer's primitives (it has no textures). Our own plain shapes: no game or Rockstar art.
#include "shell.h"

#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>

namespace screens
{
    using namespace render;
    using core::mix;
    using core::rgb;
    using core::withAlpha;
    using ui::ButtonStyle;

    namespace
    {
        // ---- the logo ----------------------------------------------------------------------------------------------
        constexpr float kHorizon = 68.0f, kDeck = 65.4f, kTowerX = 27.0f, kTowerTop = 30.0f;

        struct Stop
        {
            float at;
            uint32_t color;
        };
        // make_icon.py STORIES_SKY_L / _R, STORIES_SEA_L / _R
        constexpr Stop kSkyL[] = {{0, 0x1C1838}, {0.42f, 0x4A1630}, {0.78f, 0xB02620}, {1, 0xF2683A}};
        constexpr Stop kSkyR[] = {{0, 0x120C2A}, {0.45f, 0x2A1446}, {0.8f, 0x6A2462}, {1, 0xE0508E}};
        constexpr Stop kSeaL[] = {{0, 0x7A2222}, {0.3f, 0x3A1428}, {1, 0x14102A}};
        constexpr Stop kSeaR[] = {{0, 0x4A1A56}, {0.3f, 0x2A1446}, {1, 0x0E0A22}};

        template <size_t N>
        Color stops(const Stop (&s)[N], float t)
        {
            t = core::clamp01(t);
            for (size_t i = 1; i < N; ++i)
                if (t <= s[i].at)
                    return mix(rgb(s[i - 1].color), rgb(s[i].color), (t - s[i - 1].at) / (s[i].at - s[i - 1].at));
            return rgb(s[N - 1].color);
        }

        float smoothstep(float u)
        {
            u = core::clamp01(u);
            return u * u * (3 - 2 * u);
        }

        // the sky's or the water's colour at grid (x, y): the left city's up to x 44, the right one's from 64
        Color skyAt(float x, float y)
        {
            const float u = smoothstep((x - 44.0f) / 20.0f);
            if (y < kHorizon)
                return mix(stops(kSkyL, y / kHorizon), stops(kSkyR, y / kHorizon), u);
            const float t = (y - kHorizon) / (108.0f - kHorizon);
            return mix(stops(kSeaL, t), stops(kSeaR, t), u);
        }

        struct Grid
        {
            float x0, y0, k; // grid unit -> pixels: x0 + (gx - 18) * k
            float x(float g) const { return x0 + (g - 18.0f) * k; }
            float y(float g) const { return y0 + (g - 18.0f) * k; }
            float d(float g) const { return g * k; }
            Rect r(float gx0, float gy0, float gx1, float gy1) const { return {x(gx0), y(gy0), d(gx1 - gx0), d(gy1 - gy0)}; }
        };

        // a band of the sky or the water from y0 to y1 (grid), its colour taken at its ends, in columns across the blend
        void band(const Grid &G, float y0, float y1, float rad, bool top, bool bottom)
        {
            const float cols[] = {18, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 90};
            constexpr int n = int(std::size(cols)) - 1;
            for (int i = 0; i < n; ++i)
            {
                const float a = cols[i], b = cols[i + 1], mid = (a + b) * 0.5f;
                const float cx = i == 0 ? 18 : i == n - 1 ? 90 : mid; // the flat ends: their own colour all the way
                Corners c{};
                if (i == 0)
                    c = Corners(top ? rad : 0, 0, 0, bottom ? rad : 0);
                if (i == n - 1)
                    c = Corners(0, top ? rad : 0, bottom ? rad : 0, 0);
                // a hair wider, so neighbouring columns never leave a seam
                Rect r = G.r(a, y0, b, y1);
                if (i < n - 1)
                    r.w += 0.75f;
                gradient(r, skyAt(cx, y0), skyAt(cx, y1), c);
            }
        }

        // a quadratic Bezier stroke, tapered from w0 to w1 (grid), as short lines
        void stroke(const Grid &G, float ax, float ay, float bx, float by, float cx, float cy, float w0, float w1, Color col,
                    int steps = 10)
        {
            float px0 = ax, py0 = ay;
            for (int i = 1; i <= steps; ++i)
            {
                const float t = float(i) / float(steps), s = 1 - t;
                const float qx = s * s * ax + 2 * s * t * bx + t * t * cx, qy = s * s * ay + 2 * s * t * by + t * t * cy;
                const float w = w0 + (w1 - w0) * (t - 0.5f / float(steps));
                line(G.x(px0), G.y(py0), G.x(qx), G.y(qy), std::max(1.0f, G.d(w)), col);
                px0 = qx;
                py0 = qy;
            }
        }

        // make_icon.py parabola(): the hanging cable through (xa, ya) and (xb, yb), lowest at ym, as a quadratic Bezier
        struct Cable
        {
            float ax, ay, bx, by, cx, cy, xm, a, ym;
        };
        Cable hang(float xa, float ya, float xb, float yb, float ym)
        {
            const float ra = std::sqrt(ym - ya), rb = std::sqrt(ym - yb);
            const float xm = (xa * rb + xb * ra) / (ra + rb);
            const float a = (ya - ym) / ((xa - xm) * (xa - xm));
            const float mx = (xa + xb) * 0.5f;
            return {xa, ya, mx, ya + 2 * a * (xa - xm) * (mx - xa), xb, yb, xm, a, ym};
        }
    }

    void storiesLogo(Rect box, bool rounded)
    {
        const float size = std::min(box.w, box.h);
        const Grid G{box.x + (box.w - size) * 0.5f, box.y + (box.h - size) * 0.5f, size / 72.0f};
        const bool small = size < px(40); // the title bar's: no stars, hangers, waves or windows' light
        const float rad = rounded ? 0.19f * 72.0f : 0.0f;

        // the sky and the water (the corners rounded on the outer columns)
        band(G, 18, 53.04f, rad * G.k, true, false);
        band(G, 53.04f, kHorizon, 0, false, false);
        band(G, kHorizon, 76.3f, 0, false, false);
        band(G, 76.3f, 90, rad * G.k, false, true);

        if (!small)
        {
            const float stars[][3] = {{23, 25, 0.5f}, {31, 22, 0.4f}, {52, 24, 0.45f}, {60, 31, 0.35f}, {86, 26, 0.45f}, {69, 21, 0.4f}};
            for (const auto &s : stars)
                circle(G.x(s[0]), G.y(s[1]), std::max(0.6f, G.d(s[2])), withAlpha(rgb(0xF4E8E0), 0.7f));
        }

        // the moon and its glow
        const float mx = 41.0f, my = 37.0f, mr = 7.5f;
        shadow({G.x(mx - mr - 2), G.y(my - mr - 2), G.d(2 * mr + 4), G.d(2 * mr + 4)}, withAlpha(rgb(0xF9B035), 0.45f), G.d(4.5f),
               G.d(mr + 2));
        gradient({G.x(mx - mr), G.y(my - mr), G.d(2 * mr), G.d(2 * mr)}, rgb(0xFFD98A), rgb(0xF08A1C), G.d(mr));

        // the neon sun: solid above its middle, then stripes thinning toward the horizon
        const float sx = 74.0f, sy = 59.5f, sr = 13.0f;
        shadow({G.x(sx - sr), G.y(sy - sr), G.d(2 * sr), G.d(2 * sr)}, withAlpha(rgb(0xFF5AAE), 0.40f), G.d(5), G.d(sr));
        {
            const Rect disc{G.x(sx - sr), G.y(sy - sr), G.d(2 * sr), G.d(2 * sr)};
            const float cuts[][2] = {{sy - sr, sy - 1.0f}, {sy + 0.4f, sy + 2.6f}, {sy + 3.6f, sy + 5.4f}, {sy + 6.3f, sy + 7.7f}};
            for (const auto &c : cuts)
            {
                const float b1 = std::min(c[1], kHorizon);
                pushClip({disc.x, G.y(c[0]), disc.w, G.d(b1 - c[0])});
                gradient({disc.x, disc.y, disc.w, G.d(kHorizon - (sy - sr))}, rgb(0xFFD27A), rgb(0xFF5AAE), Corners(G.d(sr)));
                popClip();
            }
        }

        // Liberty City's skyline, low behind the bridge
        {
            struct B
            {
                float x0, x1, top;
                int crown;
            };
            const B city[] = {{18.0f, 22.0f, 46.0f, 0}, {33.0f, 38.5f, 55.0f, 3}, {38.5f, 44.0f, 51.0f, 1},
                              {44.0f, 49.0f, 57.0f, 0}, {49.0f, 54.0f, 60.0f, 0}};
            const Color top = rgb(0x5A2648), bottom = rgb(0x2A1230), rim = withAlpha(rgb(0xF07A4A), 0.85f);
            for (const B &b : city)
            {
                const float w = b.x1 - b.x0;
                const float t0 = (b.top - 44.0f) / (kHorizon - 44.0f);
                gradient(G.r(b.x0, b.top, b.x1, kHorizon + 0.6f), mix(top, bottom, t0), bottom);
                if (b.crown == 1)
                {
                    rect(G.r(b.x0 + w * 0.2f, b.top - 2.4f, b.x1 - w * 0.2f, b.top + 0.1f), mix(top, bottom, t0));
                    rect(G.r(b.x0 + w * 0.2f, b.top - 2.4f, b.x1 - w * 0.2f, b.top - 1.65f), rim);
                }
                else if (b.crown == 3)
                {
                    const float q[8] = {G.x(b.x0), G.y(b.top + 0.1f), G.x(b.x0 + w * 0.5f), G.y(b.top - 3.4f),
                                        G.x(b.x0 + w * 0.5f), G.y(b.top - 3.4f), G.x(b.x1), G.y(b.top + 0.1f)};
                    quad(q, mix(top, bottom, t0));
                }
                rect(G.r(b.x0, b.top, b.x1, b.top + 0.75f), rim);
            }
        }

        // the light on the water: the moon's broken amber dashes, the sun's pink stripes, a teal wave
        {
            const float rows[][5] = {{0.0f, 16.0f, 1.5f, 0.9f, 0.62f}, {3.6f, 12.0f, 1.3f, 0.72f, 0.3f}, {7.0f, 8.5f, 1.15f, 0.55f, 0.7f},
                                     {10.2f, 5.5f, 1.0f, 0.42f, 0.4f}, {13.2f, 3.0f, 0.9f, 0.32f, -1}};
            for (const auto &rw : rows)
            {
                const float y0 = kHorizon + 2.2f + rw[0], x0 = mx - rw[1] * 0.5f, x1 = mx + rw[1] * 0.5f;
                const Color c = withAlpha(rgb(0xF9B035), rw[3] * 0.9f);
                if (rw[4] < 0)
                    rect(G.r(x0, y0, x1, y0 + rw[2]), c);
                else
                {
                    rect(G.r(x0, y0, x0 + rw[1] * rw[4] - 0.9f, y0 + rw[2]), c);
                    rect(G.r(x0 + rw[1] * rw[4] + 0.9f, y0, x1, y0 + rw[2]), c);
                }
            }
            const float lines[][3] = {{20.0f, 2.0f, 0.55f}, {15.0f, 5.0f, 0.42f}, {10.5f, 8.4f, 0.32f}, {6.5f, 12.0f, 0.24f}};
            for (const auto &l : lines)
                rect(G.r(sx - l[0] * 0.5f, kHorizon + l[1], sx + l[0] * 0.5f, kHorizon + l[1] + 1.2f), withAlpha(rgb(0xFF5AAE), l[2]));
            if (!small)
                for (int i = 0; i < 6; ++i)
                    stroke(G, 58.0f + 6 * i, 78.0f, 61.0f + 6 * i, i % 2 == 0 ? 76.0f : 80.0f, 64.0f + 6 * i, 78.0f, 1.3f, 1.3f,
                           withAlpha(rgb(0x3FE0CF), 0.85f), 4);
        }

        // the bridge: the near tower, its cables, the deck crossing the water to Vice City's shore
        {
            const Color steel = rgb(0x07061A);
            const float tx = kTowerX, T = kTowerTop;
            rect(G.r(18, kDeck, 72.0f, kHorizon + 0.6f), steel);
            rect(G.r(tx - 4.4f, T, tx - 1.6f, 84.0f), steel);
            rect(G.r(tx + 1.6f, T, tx + 4.4f, 84.0f), steel);
            rect(G.r(tx - 5.4f, T - 1.9f, tx + 5.4f, T + 1.5f), steel);
            rect(G.r(tx - 1.8f, T + 10.0f, tx + 1.8f, T + 12.2f), steel);
            rect(G.r(tx - 1.8f, T + 22.0f, tx + 1.8f, T + 24.0f), steel);
            const Cable main = hang(tx + 3.0f, T, 70.0f, kDeck + 0.2f, kDeck + 0.6f);
            const Cable side = hang(tx - 3.0f, T, -6.0f, 66.0f, 66.4f);
            stroke(G, main.ax, main.ay, main.bx, main.by, main.cx, main.cy, 1.7f, 1.7f, steel, 16);
            pushClip({G.x(18), G.y(18), G.d(72), G.d(72)});
            stroke(G, side.ax, side.ay, side.bx, side.by, side.cx, side.cy, 1.7f, 1.7f, steel, 10);
            popClip();
            if (!small)
            {
                for (int k = 0; k < 8; ++k)
                {
                    const float x = tx + 7.0f + 4.4f * float(k), y = main.a * (x - main.xm) * (x - main.xm) + main.ym;
                    if (y < kDeck - 0.8f)
                        rect(G.r(x - 0.4f, y, x + 0.4f, kDeck + 0.2f), steel);
                }
                for (int k = 0; k < 2; ++k)
                {
                    const float x = tx - 7.0f - 4.4f * float(k), y = side.a * (x - side.xm) * (x - side.xm) + side.ym;
                    rect(G.r(x - 0.4f, y, x + 0.4f, kDeck + 0.2f), steel);
                }
                circle(G.x(tx), G.y(T - 2.6f), std::max(0.8f, G.d(0.7f)), withAlpha(rgb(0xFF3B2B), 0.9f));
            }
        }

        // the palm against the sun (its trunk ends before the rounded corner)
        {
            const Color palm = rgb(0x0C0820);
            stroke(G, 80.5f, 88.0f, 81.5f, 70.0f, 77.5f, 50.5f, 2.6f, 1.5f, palm, small ? 4 : 10);
            const float fronds[][4] = {{64.5f, 55.5f, 69.5f, 44.5f}, {65.5f, 44.0f, 71.0f, 41.5f}, {73.5f, 37.0f, 75.0f, 40.5f},
                                       {85.5f, 39.5f, 82.0f, 39.0f}, {90.0f, 47.0f, 85.5f, 41.5f}, {87.5f, 55.5f, 85.5f, 48.0f}};
            pushClip({G.x(18), G.y(18), G.d(72), G.d(72)});
            for (const auto &f : fronds)
                stroke(G, 77.5f, 50.0f, f[2], f[3], f[0], f[1], 2.0f, 0.35f, palm, small ? 4 : 8);
            popClip();
            if (!small)
            {
                circle(G.x(76.8f), G.y(51.6f), G.d(1.0f), palm);
                circle(G.x(78.6f), G.y(51.9f), G.d(0.9f), palm);
            }
        }
    }

    // ---- the GTA Stories tab's Home --------------------------------------------------------------------------------
    namespace
    {
        // where one city stands: its disc, its build; the action that leads to what it needs
        struct City
        {
            const char *id, *title;
            bool disc = false, built = false, current = false, checking = false;
            std::string discLine, buildLine;
            bool ready() const { return disc && built; }
        };

        City lcsCity(const App &app)
        {
            City c{"lcs", "Liberty City Stories"};
            c.disc = app.disc.ok;
            c.checking = !app.doctorDone || app.doctorCached;
            const launcher::Json &b = app.doctor["build"];
            c.built = b["built"].boolean();
            c.current = b["current"].boolean();
            c.discLine = discLine(app);
            c.buildLine = gameLine(app);
            return c;
        }

        City vcsCity(const App &app)
        {
            City c{"vcs", "Vice City Stories"};
            const std::filesystem::path disc = app.knownDisc("vcs");
            c.disc = !disc.empty();
            c.checking = !app.storiesVcsDone;
            const launcher::Json &b = app.storiesVcs["build"];
            c.built = b["built"].boolean();
            c.current = b["current"].boolean();
            c.discLine = c.disc ? "USA, verified" : "Not chosen yet";
            c.buildLine = c.checking && !app.storiesVcs.isObject() ? "Checking"
                          : app.storiesVcs.isObject() ? (c.current ? "Built, up to date" : c.built ? "Needs an update" : "Not built yet")
                                                      : "Couldn't check";
            return c;
        }

        ui::Status cityStatus(const City &c)
        {
            if (c.checking && !c.built)
                return ui::Status::Checking;
            if (!c.disc)
                return ui::Status::Missing;
            return c.current ? ui::Status::Good : c.built ? ui::Status::Warning : ui::Status::Missing;
        }

        // a glass card: a status badge, the city, its disc and build; clicking it opens what it needs
        bool cityCard(const City &c, Rect r)
        {
            const ui::Interaction in = ui::interact(ui::id(c.id, 77), r, !c.disc);
            ui::Widget &w = *in.w;
            const Rect box = r.offset(0, -w.hover * px(2));
            shadow(box.offset(0, px(10)), theme::shade(0.45f + 0.15f * w.hover), px(20), px(theme::kRadius));
            rectEx(box, withAlpha(mix(theme::kSurface, theme::kSurfaceHi, w.hover), 0.80f), withAlpha(theme::kSurface, 0.72f),
                   px(theme::kRadius), theme::ink(0.07f + 0.05f * w.hover), ui::hairline());
            ui::statusBadge(ui::id(c.id, 78), box.x + px(30), box.y + px(30), px(11), cityStatus(c));
            ui::label(kFontSemibold, px(theme::kBody), {box.x + px(52), box.y + px(16), box.w - px(70), px(28)}, theme::kText, c.title);
            ui::label(kFontBody, px(theme::kSmall), {box.x + px(52), box.y + px(42), box.w - px(70), px(20)}, theme::kDim,
                      "Disc  \xC2\xB7  " + (c.disc ? c.discLine : std::string("Not chosen yet: click to choose")));
            ui::label(kFontBody, px(theme::kSmall), {box.x + px(52), box.y + px(62), box.w - px(70), px(20)}, theme::kDim,
                      "Game  \xC2\xB7  " + c.buildLine);
            if (!c.disc)
                drawIcon(kFontIcons, px(13), box.right() - px(24), box.y + px(30), withAlpha(theme::kPink, 0.6f + 0.4f * w.hover),
                         theme::icon::kFolder);
            if (w.focus > 0.01f)
                outline(box.inset(-px(3)), withAlpha(theme::kPink, w.focus), px(2), px(theme::kRadius + 3));
            return in.clicked;
        }

        std::string packsCardLine(const App &app, bool vcsDisc)
        {
            if (!vcsDisc)
                return "Made from your Vice City Stories disc once it is chosen";
            const launcher::Json &r = app.packsResult;
            if (r.isObject())
            {
                const size_t made = r["built"].size(), current = r["current"].size(), failed = r["failed"].size();
                if (failed)
                    return std::to_string(failed) + " couldn't be made (Play goes on)";
                if (made + current)
                    return std::to_string(made + current) + " ready  \xC2\xB7  checked before each Play";
            }
            return "Vic's swimming, Vice City's cars";
        }
    }

    void drawStoriesHome(Rect r)
    {
        App &app = *shell.app;
        const double since = shell.mode == Mode::Home ? shell.modeSince : ui::state.time;
        const float left = r.x + std::max(px(56), r.w * 0.07f);
        const float colW = std::min(px(540), r.w * 0.50f);
        float y = r.y + std::max(px(28), r.h * 0.09f);
        const bool demoReady = shell.options.demo == "storiesready"; // screenshots: both cities built

        City lcs = lcsCity(app), vcs = vcsCity(app);
        // screenshots: the tab's states without discs or builds (--demo storiesnone | storieslcs | storiesdiscs | storiesready)
        const std::string &demo = shell.options.demo;
        const bool demoState = demo == "storiesnone" || demo == "storieslcs" || demo == "storiesdiscs";
        if (demoState)
            for (City *c : {&lcs, &vcs})
            {
                const bool has = demo == "storiesdiscs" || (demo == "storieslcs" && c == &lcs);
                c->disc = has;
                c->built = c->current = c->checking = false;
                c->discLine = has ? "USA, verified" : "Not chosen yet";
                c->buildLine = has ? "Not built yet" : "Needs its disc";
            }
        if (demoReady)
            for (City *c : {&lcs, &vcs})
            {
                c->disc = c->built = c->current = true;
                c->checking = false;
                c->discLine = "USA, verified";
                c->buildLine = "Built, up to date";
            }

        // the logo, then the name beside it
        pushReveal(since, 0);
        const float logoSize = px(78);
        shadow({left, y + px(8), logoSize, logoSize}, theme::shade(0.5f), px(16), px(16));
        storiesLogo({left, y, logoSize, logoSize});
        const float tx = left + logoSize + px(22);
        ui::eyebrow(tx, y + px(4), theme::kPink, "GTA STORIES RECOMPILED  \xC2\xB7  EXPERIMENTAL");
        {
            const float size = fontFound(kFontBrand) ? px(58) : px(50);
            const FontMetrics m = metrics(kFontBrand, size);
            const float base = y + px(24) + m.ascent;
            shadow({tx, y + px(24) + m.ascent * 0.35f, measure(kFontBrand, size, "GTA Stories"), m.ascent * 0.6f},
                   withAlpha(theme::kPink, 0.16f), px(26), px(10));
            drawText(kFontBrand, size, tx - px(2), base, theme::kText, "GTA Stories");
            y = std::max(y + logoSize, base + m.descent) + px(14);
        }
        popReveal();
        pushReveal(since, 1);
        const bool toni = app.settings.storiesHero == "toni"; // [hero]
        const bool cityHero = app.settings.storiesHero == "city";
        y += ui::paragraph(kFontBody, px(16), {left, y, colW, 0}, theme::kDim,
                           std::string("Liberty City and Vice City at once. You play Liberty City; Vice City stands across "
                                       "the sea, east of Portland. Drive over the bridge (it ends at the airport in Vice "
                                       "City) or cross by boat, and ") +
                               (cityHero ? "Vic takes over in Vice City." :
                                           std::string("you are still ") + (toni ? "Toni" : "Vic") + ", in Vice City.")) +
             px(14);
        // [hero] one hero in both cities (src/game/stories_hero.h): the other game's hero is made from its disc before Play
        {
            int hero = cityHero ? 2 : toni ? 1 : 0;
            if (ui::segmented("#stories-hero", {left, y, std::min(colW, px(420)), px(36)},
                              {"Vic", "Toni", "Each city's own"}, hero))
            {
                app.settings.storiesHero = hero == 2 ? "city" : hero ? "toni" : "vic";
                app.saveSettings();
            }
            y += px(44);
            y += ui::paragraph(kFontBody, px(theme::kSmall), {left, y, colW, 0}, theme::kFaint,
                               cityHero ? "Toni in Liberty City, Vic in Vice City. Cutscenes keep each city's own hero."
                                        : "One hero in both cities, in his own outfits. Cutscenes keep each city's own hero.") +
                 px(18);
            // [savesync] Vice City saves made here go back to your own Vice City card (your card is backed up first)
            {
                bool on = app.settings.storiesSaveSync;
                ui::label(kFontBody, px(theme::kSmall), {left, y, std::min(colW, px(420)) - px(60), px(30)}, theme::kDim, "Save Vice City progress to my Vice City card");
                if (ui::toggle("#stories-save-sync", {left + std::min(colW, px(420)) - px(44), y, px(44), px(30)}, on))
                {
                    app.settings.storiesSaveSync = on;
                    app.saveSettings();
                }
                y += px(34);
                y += ui::paragraph(kFontBody, px(theme::kSmall), {left, y, colW, 0}, theme::kFaint,
                                   on ? "Vice City saves go back to your Vice City card (backed up first)."
                                      : "Vice City keeps a card of its own. Your Vice City saves stay as they are.") +
                     px(14);
            }
            // [missionborder] crossing the border during a mission: fails the mission (default) or a wall
            int mb = app.settings.missionBorder == "block" ? 1 : 0;
            if (ui::segmented("#stories-mission-border", {left, y, std::min(colW, px(420)), px(36)},
                              {"Crossing fails the mission", "Border blocked in missions"}, mb))
            {
                app.settings.missionBorder = mb ? "block" : "fail";
                app.saveSettings();
            }
            y += px(44);
            y += ui::paragraph(kFontBody, px(theme::kSmall), {left, y, colW, 0}, theme::kFaint,
                               "While a mission runs, crossing the border fails the mission, or the border holds you back so you stay in the city.") +
                 px(18);
        }
        popReveal();

        // the one thing to do next
        pushReveal(since, 2);
        const Rect big{left, y, px(232), px(56)};
        const float heroSize = fontFound(kFontBrand) ? px(31) : px(18);
        auto fit = [&](std::string_view label, char32_t icon) {
            return Rect{big.x, big.y, std::max(big.w, ui::buttonWidth(label, icon, heroSize, kFontBrand) + px(12)), big.h};
        };
        std::string below;
        Color belowColor = theme::kDim; // over the city: kept legible
        const bool enter = platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed();
        if (app.gameRunning)
        {
            rectEx({big.x, big.y, px(320), big.h}, withAlpha(theme::kPink, 0.12f), withAlpha(theme::kPink, 0.08f), px(14),
                   withAlpha(theme::kPink, 0.35f), ui::hairline());
            circle(big.x + px(28), big.cy(), px(5), theme::kPink);
            ui::label(kFontSemibold, px(17), {big.x + px(48), big.y, px(270), big.h}, theme::kText,
                      "Both cities  \xC2\xB7  " + duration(ui::state.wall - app.gameStart));
            stopGameButton(app, {big.x, big.y, px(320), big.h});
            below = "Vice City appears on the horizon after about a minute. The launcher comes back when Liberty City ends.";
            ui::wakeAt(std::floor(ui::state.wall) + 1.0);
        }
        else if (screens::packsLine(app, big, below))
            ;
        else if (app.storiesPlan.active)
        {
            ui::spinner(big.x + px(14), big.cy(), px(9), theme::kPink, px(2.5f));
            ui::label(kFontSemibold, px(16), {big.x + px(36), big.y, px(420), big.h}, theme::kText, "Getting the next build ready");
            ui::wakeAt(ui::state.wall + 0.25);
        }
        else if (!demoReady && !demoState && ((lcs.checking && !lcs.built) || (vcs.checking && !vcs.built) || app.doctorRunning ||
                                              app.storiesVcsRunning))
        {
            ui::spinner(big.x + px(14), big.cy(), px(9), theme::kDim, px(2.5f));
            ui::label(kFontBody, px(theme::kBody), {big.x + px(36), big.y, px(300), big.h}, theme::kDim, "Checking both cities");
        }
        else if (!demoState && !demoReady && app.doctorFailed())
        {
            if (ui::button("Check again", fit("Check again", theme::icon::kRefresh), ButtonStyle::Primary, theme::icon::kRefresh,
                           true, heroSize, kFontBrand) ||
                enter)
                app.refresh();
            below = "The check of this PC gave no answer.";
            belowColor = theme::kRed;
        }
        else if (!demoReady && !demoState && cpuUnsupported(app))
        {
            ui::label(kFontSemibold, px(16), {big.x, big.y, px(480), big.h}, theme::kRed, "This PC's processor is too old");
            below = "The game needs a newer processor (AVX2). Liberty City Stories' tab says more.";
            belowColor = theme::kRed;
        }
        else if (!lcs.disc || !vcs.disc)
        {
            // the missing disc is chosen right here: the usual picker and check (Liberty City's first)
            const City &need = !lcs.disc ? lcs : vcs;
            const std::string label = std::string("Choose ") + (std::string_view(need.id) == "lcs" ? "Liberty City" : "Vice City") + " disc";
            if (ui::button(label, fit(label, theme::icon::kFolder), ButtonStyle::Primary, theme::icon::kFolder, true, heroSize,
                           kFontBrand) ||
                enter)
                if (!demoState)
                    chooseStoriesDisc(need.id);
            below = (!lcs.disc && !vcs.disc)
                        ? "Both cities are built from your own two discs, once. Pick Liberty City Stories' first, or drop an .iso on this window."
                        : std::string(need.title) + "'s disc is next. Or drop its .iso on this window.";
        }
        else if (lcs.current && vcs.current)
        {
            if (ui::button("Play", fit("Play", theme::icon::kPlay), ButtonStyle::Primary, theme::icon::kPlay, true,
                           heroSize, kFontBrand) ||
                enter)
            {
                if (!demoReady)
                    play(app);
            }
            below = !app.packsNote.empty() ? app.packsNote : "Two games at once: a strong PC helps.";
            if (!app.packsNote.empty())
                belowColor = theme::kAmber;
        }
        else
        {
            // a city that is not built, or out of date: one button builds what is due (tools first if they are missing),
            // one after the other, here, then starts the game. Both built (one out of date): Play stays the big button
            const bool anyUnbuilt = !lcs.built || !vcs.built;
            const bool both = !lcs.current && !vcs.current;
            const std::string what = both ? "both cities" : std::string(!lcs.current ? lcs.title : vcs.title);
            const char *main = anyUnbuilt ? "Build and play" : "Play";
            const char *side = anyUnbuilt ? "Build only" : "Update the build";
            const char32_t mainIcon = anyUnbuilt ? theme::icon::kRepair : theme::icon::kPlay;
            const Rect mainRect = fit(main, mainIcon);
            if (ui::button(main, mainRect, ButtonStyle::Primary, mainIcon, true, heroSize, kFontBrand) || enter)
            {
                if (!demoState)
                {
                    if (anyUnbuilt)
                        storiesBuild(app, true);
                    else
                        play(app);
                }
            }
            const Rect sideRect{mainRect.right() + px(14), big.cy() - px(20), ui::buttonWidth(side, theme::icon::kRepair), px(40)};
            if (ui::button(side, sideRect, ButtonStyle::Secondary, theme::icon::kRepair, !app.busy()) && !demoState)
                storiesBuild(app, false);
            ui::tooltip(ui::id(side), sideRect, std::string("Builds ") + what + ", one after the other, and stays here.");
            below = anyUnbuilt ? std::string("Builds ") + what + " from your discs, one city after the other (minutes, once)" +
                                     (app.storiesToolsNeeded() ? ", after the build tools" : "") + ". The game starts when it is done."
                               : std::string(both ? "Both cities are" : what + " is") +
                                     " out of date: the game still plays; updating builds it again.";
        }
        y += big.h + px(16);
        if (!below.empty())
        {
            // on a glass strip, as Home's Game settings line: legible over the skyline in either palette
            const float tw = std::min(colW, measure(kFontBody, px(theme::kSmall), below) + px(4));
            const float th = ui::paragraphHeight(kFontBody, px(theme::kSmall), tw, below);
            rectEx({left - px(12), y - px(8), tw + px(24), th + px(16)}, withAlpha(theme::kSurface, 0.78f),
                   withAlpha(theme::kSurface, 0.70f), px(theme::kRadius), theme::ink(0.07f), ui::hairline());
            y += ui::paragraph(kFontBody, px(theme::kSmall), {left, y, tw, 0}, belowColor, below) + px(12);
        }
        // [stories] both cities in one program and one window (built with tools/build.py --oneproc): the switch
        if (app.oneProgramAvailable() && !app.gameRunning)
        {
            const Rect line{left, y, std::min(colW, px(420)), px(30)};
            rectEx({line.x - px(12), line.y - px(4), line.w + px(24), line.h + px(8)}, withAlpha(theme::kSurface, 0.78f),
                   withAlpha(theme::kSurface, 0.70f), px(theme::kRadius), theme::ink(0.07f), ui::hairline());
            ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText,
                      "One program (experimental)");
            bool one = app.settings.oneProgram;
            if (ui::toggle("#stories-one-program", {line.right() - px(44), line.y, px(44), line.h}, one))
            {
                app.settings.oneProgram = one;
                app.saveSettings();
            }
            ui::tooltip(ui::id("#stories-one-program"), line,
                        "Both cities in one window: no window swap at the border. Off: two programs, as before");
            y += line.h + px(16);
        }
        popReveal();

        // the two cities and the packs, along the bottom
        const float cardsH = px(92);
        const float gap = px(16);
        Rect strip{left, r.bottom() - px(36) - cardsH, r.right() - left - std::max(px(56), r.w * 0.07f), cardsH};
        if (strip.y < y + px(12))
            return;
        const float cw = (strip.w - 2 * gap) / 3;
        pushReveal(since, 4, 20);
        if (cityCard(lcs, {strip.x, strip.y, cw, cardsH}) && !demoState)
            chooseStoriesDisc("lcs");
        ui::tooltip(ui::id(lcs.id, 77), {strip.x, strip.y, cw, cardsH}, "In front: your window, your settings and saves");
        popReveal();
        pushReveal(since, 5, 20);
        if (cityCard(vcs, {strip.x + cw + gap, strip.y, cw, cardsH}) && !demoState)
            chooseStoriesDisc("vcs");
        ui::tooltip(ui::id(vcs.id, 77), {strip.x + cw + gap, strip.y, cw, cardsH},
                    app.settings.storiesSaveSync
                        ? "Behind it, hidden: the city across the sea. What you save there goes back to your Vice City card"
                        : "Behind it, hidden: the city across the sea, on a card of its own");
        popReveal();
        pushReveal(since, 6, 20);
        {
            const Rect pr{strip.x + 2 * (cw + gap), strip.y, cw, cardsH};
            const ui::Interaction in = ui::interact(ui::id("#stories-packs"), pr, true);
            ui::Widget &w = *in.w;
            const Rect box = pr.offset(0, -w.hover * px(2));
            shadow(box.offset(0, px(10)), theme::shade(0.45f + 0.15f * w.hover), px(20), px(theme::kRadius));
            rectEx(box, withAlpha(mix(theme::kSurface, theme::kSurfaceHi, w.hover), 0.80f), withAlpha(theme::kSurface, 0.72f),
                   px(theme::kRadius), theme::ink(0.07f + 0.05f * w.hover), ui::hairline());
            circle(box.x + px(30), box.y + px(30), px(14), withAlpha(theme::kPink, 0.14f));
            drawIcon(kFontIcons, px(14), box.x + px(30), box.y + px(30), theme::kPink, theme::icon::kList);
            ui::label(kFontSemibold, px(theme::kBody), {box.x + px(52), box.y + px(16), box.w - px(70), px(28)}, theme::kText,
                      "Stories packs");
            ui::label(kFontBody, px(theme::kSmall), {box.x + px(52), box.y + px(42), box.w - px(66), px(20)}, theme::kDim,
                      packsCardLine(app, vcs.disc));
            ui::label(kFontBody, px(theme::kSmall), {box.x + px(52), box.y + px(62), box.w - px(66), px(20)}, theme::kFaint,
                      "Made before Play, from your discs");
            if (w.focus > 0.01f)
                outline(box.inset(-px(3)), withAlpha(theme::kPink, w.focus), px(2), px(theme::kRadius + 3));
            if (in.clicked)
                openSheet(Sheet::Mods);
            ui::tooltip(ui::id("#stories-packs"), pr, "The other city's things for each game, in the Mods list (on / off there)");
        }
        popReveal();
    }
}
