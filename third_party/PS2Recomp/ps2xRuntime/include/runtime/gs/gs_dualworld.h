// [gta-recomp] Dual world (src/game/dualworld.h): two games' pictures stitched into one view.
//
// The HW executor (gs_hw.cpp) offers two seams, both called on the render (main) thread while a frame is composed for
// display, before the native HUD layer is drawn over it:
//  - a backdrop source (the front game): a picture drawn into the frame's target where the frame's depth buffer is
//    still clear (nothing of the game's own world covers it: the sky and beyond the far plane), blended by its alpha;
//  - a frame sink (the background game): the frame's 3D picture without the HUD layer, alpha 1 where its world drew and
//    0 where its depth stayed clear (its sky), scaled to the sink's size and read back to the CPU.
// Neither set (the default): nothing changes.
#pragma once

#include <cstdint>

namespace gsdw
{
    // The camera a picture was rendered with, in the front game's coordinates (the frame lag fix: the front reprojects
    // the picture into the camera of the frame it composes, so the other city stays locked to the world while the
    // camera turns, however many frames old the picture is). right / up: unit vectors * tan(half the view), as View's.
    struct PictureCamera
    {
        bool on = false;
        float cam[3]{}, front[3]{}, right[3]{}, up[3]{};
        uint64_t frontTag = 0; // the front game's frame tag (setFrameTag) whose camera the background game took: the lag
    };
    struct Picture
    {
        const uint8_t *rgba = nullptr; // w * h * 4 bytes, rows top down
        uint32_t w = 0, h = 0;
        uint64_t seq = 0; // changes when the picture does
        PictureCamera camera; // [dwlag] off: drawn as it is (no reprojection)
    };
    // Fills `out` with the newest picture; false: none (nothing drawn). The pointer stays valid until the next call.
    using BackdropSource = bool (*)(Picture &out);
    void setBackdropSource(BackdropSource source);

    // tag: the frame tag (setFrameTag) of the game frame whose world the picture shows
    using FrameSink = void (*)(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t tag);
    void setFrameSink(FrameSink sink, uint32_t w, uint32_t h);

    // [stories] The GPU hand-over (both games in one process, src/stories): the background game's frame copied into a
    // texture shared with the front game's device instead of being read back, at the frame's own size (sharp), and
    // the front game samples it directly (no upload). Tried before the CPU sink / source; each falls back to them
    // when the renderer can't share (rhi::Device::createSharedTexture null) or nothing is there.
    // The background game: called on the render thread after the copy (the texture unlocked), `slot` 0..1, `tag` as
    // FrameSink's (the frame whose world it shows: its camera for the reprojection).
    using GpuSink = void (*)(uint64_t handle, uint32_t w, uint32_t h, uint32_t slot, uint64_t tag);
    void setGpuSink(GpuSink sink);
    struct GpuPicture
    {
        uint64_t handle = 0; // a shared texture of this process (rhi::Device::openSharedTexture)
        uint32_t w = 0, h = 0;
        uint64_t seq = 0;
        uint32_t slot = 0;    // which of the background game's two textures (each opened once)
        float opacity = 1.0f; // the picture's alpha times this (haze: the other city fading out)
        PictureCamera camera; // as Picture's (the reprojection)
        uint64_t composing = 0; // in: the frame tag being composed (a source may wait for the picture of that frame)
        bool contended = false; // out, when it returns false: a picture exists but was being written all the tries ([farsea] the last good one is drawn again)
    };
    // The front game: the newest picture; false: none (the CPU source is tried)
    using GpuSource = bool (*)(GpuPicture &out);
    void setGpuSource(GpuSource source);

    // Frame tags (game thread, each game frame before its display list is kicked): the native draws recorded after it
    // carry the tag, so the render thread knows which game frame it is composing (it runs a frame or two behind the game
    // thread). setView's view is kept per tag; the sink is told the tag of the picture it gets.
    void setFrameTag(uint64_t tag);

    // The depth-correct composite (the front game): the frame's camera and the border plane. Where a pixel's view ray
    // crosses the plane x = borderX towards the other city (side +1: at +x), the picture also covers what the frame drew
    // beyond that crossing (its far sea), not only where its depth stayed clear: the crossing's depth in the frame's
    // own Z mapping (taken from its native draws) against the frame's depth. on = false (or no native draws): the depth
    // clear only. Game thread, each game frame.
    struct View
    {
        bool on = false;
        float cam[3]{}, front[3]{};
        float right[3]{}, up[3]{}; // * tan(half the horizontal / vertical view)
        float borderX = 0.0f, side = 1.0f;
        float seaZ = 0.0f, blend = 0.0f; // the sea level; past the border the picture blends in over `blend` m of sea
        bool depth = true;      // [dwlag] the border's depth (false: the depth-clear rule only; on = the camera is valid)
        bool reproject = true;  // [dwlag] the picture reprojected into this view (Picture::camera)
        float proxyPast = 1200.0f; // [dwlag] reprojection's translation: the other city taken at this many m past the border
    };
    // For the frame of the last setFrameTag (the composite uses the view of the frame it composes).
    void setView(const View &view);

    // [tone] The background game's far fog handed over as transparency (its picture only; its own screen is unchanged).
    // A game fogs its far world into its own fog colour, which matches its own sky but not the front game's: Liberty
    // City's haze put the far city on Vice City's horizon as a flat silhouette in LCS's fog colour (dark grey-green in
    // rain and storms, at dusk). On: the picture's alpha is the fog factor (from the frame's depth and the fog its
    // native draws used) and the fog colour is taken out of its colour, so the far city fades into the front game's sky
    // as it faded into its own fog. Any thread.
    void setFogToAlpha(bool on);

    // The crossing's fade (the front game): 0 none .. 1 black, drawn over the whole displayed picture, its HUD included.
    // Any thread; applies from the next frame shown.
    void setFade(float amount);

    // statistics for the logs: frames composed / sunk, the last composite's and readback's cost (microseconds)
    struct Stats
    {
        uint64_t composed = 0, sunk = 0;
        uint32_t composeUs = 0, sinkUs = 0;
        // [gta-recomp] the frame rect (target pixels) the last composite drew the backdrop into (front) / the last sunk
        // frame's rect (back): the front's is the picture size that is drawn 1:1 (Stories: no upscale blur)
        uint32_t rectW = 0, rectH = 0;
        // [dwlag] the lag (front composites since the last statsReset): frames between the frame composed and the front
        // frame whose camera the picture was rendered with (lagHist[7]: 7 or more), and how far off a far point at the
        // picture's centre lands in the frame composed, in display pixels (without reprojection: the visible error)
        uint64_t lagHist[8]{};
        uint64_t lagN = 0;
        double errPxSum = 0.0, errPxMax = 0.0, errYSum = 0.0;
        uint64_t held = 0, lockFail = 0, noGpu = 0; // [farsea] composites that drew the held picture / GPU lock missed / GPU picture not offered
        uint64_t viewMisses = 0; // frames composed whose own view was not found (the newest one used)
    };
    void statsReset();
    Stats stats();
}
