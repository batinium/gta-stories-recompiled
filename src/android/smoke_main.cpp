// [android] Bring-up program for the Android build, used while there is no generated game code (no disc recompiled):
// the same SDL3 platform layer, Vulkan RHI, presenter and Dear ImGui overlay the game uses, drawing a moving test
// picture at the PS2's 640x448. It checks the NDK toolchain, the device's Vulkan driver, texture uploads, the UI
// shaders, touch / gamepad input and the background -> foreground surface swap (rhi::Device::recreateSurface).
// Log: adb logcat -s vcs
#include <SDL3/SDL_main.h>

#include "android/android_log.h"
#include "platform/platform.h"
#include "render/presenter.h"
#include "rhi/rhi.h"

#include "imgui.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    const ImVec4 kHeld(0.45f, 1.0f, 0.45f, 1.0f), kIdle(0.45f, 0.45f, 0.45f, 1.0f);

    // The pad as the game reads it (the on-screen controls merged with a real gamepad): each PS2 button lit while held,
    // the sticks and triggers as numbers and dots, so the overlay can be checked without the game.
    void drawPadState(std::chrono::steady_clock::time_point &lookTestUntil, float &lookX, float &lookY)
    {
        using namespace platform;
        static constexpr const char *kNames[GAMEPAD_BUTTON_COUNT] = {
            "Cross", "Circle", "Square", "Triangle", "Select", "Start", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right",
        };
        for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
        {
            if (b % 7)
                ImGui::SameLine();
            ImGui::TextColored(gamepadButton(static_cast<GamepadButton>(b)) ? kHeld : kIdle, "%s", kNames[b]);
        }
        const float lt = gamepadAxis(AXIS_LEFT_TRIGGER), rt = gamepadAxis(AXIS_RIGHT_TRIGGER);
        ImGui::TextColored(lt > 0.5f ? kHeld : kIdle, "L2 %.2f", lt);
        ImGui::SameLine();
        ImGui::TextColored(rt > 0.5f ? kHeld : kIdle, "R2 %.2f", rt);
        const float lx = gamepadAxis(AXIS_LEFT_X), ly = gamepadAxis(AXIS_LEFT_Y);
        const float rx = gamepadAxis(AXIS_RIGHT_X), ry = gamepadAxis(AXIS_RIGHT_Y);
        ImGui::Text("Left stick %+.2f %+.2f   Right stick %+.2f %+.2f", lx, ly, rx, ry);
        // the sticks as dots in their squares
        const float side = ImGui::GetFrameHeight() * 3.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int s = 0; s < 2; ++s)
        {
            if (s)
                ImGui::SameLine();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(side, side));
            const float x = s ? rx : lx, y = s ? ry : ly;
            dl->AddRect(p, ImVec2(p.x + side, p.y + side), IM_COL32(160, 160, 160, 255));
            dl->AddCircleFilled(ImVec2(p.x + side * 0.5f * (1.0f + x), p.y + side * 0.5f * (1.0f + y)), side * 0.08f,
                                IM_COL32(120, 255, 120, 255));
        }
        // The camera drag works while the game has the mouse captured (playing, no menu): the test captures it for 10 s,
        // so the overlay's camera area can be tried, then lets go (every touch is the overlay's while it lasts, this
        // window's included)
        const auto now = std::chrono::steady_clock::now();
        if (now < lookTestUntil)
            ImGui::Text("Camera drag on for %.0f s: drag on the right half", std::chrono::duration<float>(lookTestUntil - now).count());
        else if (ImGui::Button("Test the camera drag (10 s)"))
        {
            lookTestUntil = now + std::chrono::seconds(10);
            lookX = lookY = 0.0f;
        }
        ImGui::Text("Camera motion (mouse counts): %+.0f %+.0f", lookX, lookY);
    }
}

int main(int, char **)
{
    vcs_android::redirectStdioToLogcat("vcs");
    std::fprintf(stderr, "[smoke] start\n");

    platform::WindowDesc wd;
    wd.title = "VCS Recomp (Android smoke test)";
    wd.width = 1280;
    wd.height = 720;
    wd.vulkan = true;
    if (!platform::init(wd))
    {
        std::fprintf(stderr, "[smoke] platform::init failed\n");
        return 1;
    }
    int w = 0, h = 0;
    platform::windowSize(w, h);
    present::setBackend(static_cast<int>(rhi::Backend::Vulkan));
    if (!present::init(w, h))
    {
        std::fprintf(stderr, "[smoke] present::init failed\n");
        platform::shutdown();
        return 1;
    }
    std::fprintf(stderr, "[smoke] %s, window %dx%d\n", present::device()->adapterName().c_str(), w, h);

    constexpr int kW = 640, kH = 448;
    present::Picture picture = present::createPicture(kW, kH, true);
    std::vector<uint32_t> pixels(kW * kH);

    const auto start = std::chrono::steady_clock::now();
    auto lastReport = start;
    uint64_t frames = 0, framesAtReport = 0;
    bool wasBackground = false;
    // the camera-drag test: the mouse captured for a while (as the game does while playing), its motion summed
    std::chrono::steady_clock::time_point lookTestUntil{};
    float lookX = 0.0f, lookY = 0.0f;
    while (!platform::quitRequested())
    {
        platform::pumpEvents();
        if (platform::keyPressed(platform::KEY_ESCAPE)) // Back (platform_sdl3.cpp): leave the test
            break;
        if (platform::inBackground())
        {
            if (!wasBackground)
                std::fprintf(stderr, "[smoke] background\n");
            wasBackground = true;
            platform::delayPrecise(16000000ull);
            continue;
        }
        if (wasBackground)
            std::fprintf(stderr, "[smoke] foreground (surface generation %u)\n", platform::surfaceGeneration());
        wasBackground = false;

        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
            {
                const uint32_t r = static_cast<uint32_t>(127.5f + 127.5f * std::sin(t + x * 0.02f));
                const uint32_t g = static_cast<uint32_t>(127.5f + 127.5f * std::sin(t * 1.3f + y * 0.03f));
                const uint32_t b = ((x / 32 + y / 32) & 1) ? 255u : 64u;
                pixels[y * kW + x] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
        present::updatePicture(picture, pixels.data());

        platform::setMouseCaptured(std::chrono::steady_clock::now() < lookTestUntil);
        {
            float dx = 0.0f, dy = 0.0f;
            platform::takeMouseMotion(dx, dy);
            lookX += dx;
            lookY += dy;
        }

        present::beginFrame();
        platform::beginFrame();
        // top centre: clear of the on-screen controls (shoulders in the top corners, sticks and buttons at the sides)
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.14f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
        ImGui::Begin("VCS Recomp - Android smoke test");
        ImGui::Text("GPU: %s", present::device()->adapterName().c_str());
        ImGui::Text("Window: %dx%d   %.1f fps", w, h, ImGui::GetIO().Framerate);
        ImGui::Text("Surface generation: %u", platform::surfaceGeneration());
        ImGui::Text("Gamepad: %s", platform::gamepadConnected() ? platform::gamepadName() : "(none)");
        drawPadState(lookTestUntil, lookX, lookY);
        float mx = 0, my = 0;
        if (platform::mousePosition(mx, my))
            ImGui::Text("Pointer: %.0f, %.0f", mx, my);
        static int taps = 0;
        if (ImGui::Button("Tap me"))
            ++taps;
        ImGui::SameLine();
        ImGui::Text("%d", taps);
        ImGui::End();
        platform::endUiFrame();

        platform::windowSize(w, h);
        present::drawWindow(picture, kW, kH, 4.0f / 3.0f, w, h);
        present::present();
        platform::waitNextFrame();

        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(5))
        {
            const double secs = std::chrono::duration<double>(now - lastReport).count();
            std::fprintf(stderr, "[smoke] %.1f fps, window %dx%d\n", (frames - framesAtReport) / secs, w, h);
            lastReport = now;
            framesAtReport = frames;
        }
    }
    std::fprintf(stderr, "[smoke] quit\n");
    present::shutdown();
    platform::shutdown();
    return 0;
}
