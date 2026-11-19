// See dualworld.h.
#include "game/dualworld.h"

#include "game/game_table.h"
#include "game/lcs_crossover.h"
#include "game/lcs_drawdist.h"
#include "game/lcs_freecam.h"
#include "game/vcs_crossover.h"
#include "game/lcs_frontend.h"
#include "game/lcs_trainer.h"
#include "game/vcs_frontend.h"
#include "game/stories_bridge.h"
#include "game/vcs_display.h"
#include "game/vcs_freecam.h"
#include "game/vcs_save_anywhere.h"
#include "game/vcs_trainer.h"
#include "platform/platform.h"
#include "render/presenter.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_dualworld.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_host_std.h"
#include "settings/settings.h"

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dualworld
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kMagic = 0x44574C44u; // "DLWD"
        constexpr uint32_t kVersion = 6; // [parity] 4: the settings sync and the liveness stamps; [dwlag] 5: camTag, overscanBits, picCam; 6: + parity's pid
        constexpr uint32_t kMaxW = 3840, kMaxH = 2160, kSlots = 3; // [parity] up to 4K: the front's own frame size (1:1)

        // LCS camera (lcs_freecam.cpp): TheCamera 0x43BBF0, active CCam byte +0x10C, Cams +0x220 x 0x2B0; CCam Front
        // +0x1C0, Source +0x1D0, Up +0x1F0, FOV +0xE8. Clock (lcs_trainer.cpp): hours u8 0x3D9B74, minutes u8 0x3D9B80.
        constexpr uint32_t kLcsCamera = 0x0043BBF0u, kLcsActive = 0x10Cu, kLcsCams = 0x220u, kLcsCamSize = 0x2B0u;
        constexpr uint32_t kLcsFront = 0x1C0u, kLcsSource = 0x1D0u, kLcsUp = 0x1F0u, kLcsFov = 0xE8u;
        constexpr uint32_t kLcsHours = 0x003D9B74u, kLcsMinutes = 0x003D9B80u;
        // VCS camera (vcs_freecam.cpp, game-internals.md 12b): CCam Front +0x10, Source +0x20, Up +0x60, FOV +0x134,
        // Target +0x1A0. Clock (vcs_trainer.cpp): CClock hours, minutes bytes 0x4CD148 / 0x4CD149.
        constexpr uint32_t kVcsFront = 0x10u, kVcsSource = 0x20u, kVcsUp = 0x60u, kVcsFov = 0x134u, kVcsTarget = 0x1A0u;
        constexpr uint32_t kVcsHours = 0x004CD148u, kVcsMinutes = 0x004CD149u;
        // VCS TheCamera 0x6F44D0: the active CCam's index byte +0x50, Cams[3] from +0x70, 0x270 each (vcs_freecam.cpp)
        constexpr uint32_t kVcsCamera = 0x006F44D0u, kVcsActive = 0x50u, kVcsCams = 0x70u, kVcsCamSize = 0x270u;
        constexpr uint32_t kLcsTarget = 0x160u;
        enum : uint32_t
        {
            kGameLcs = 1,
            kGameVcs = 2
        };

        struct Shared
        {
            uint32_t magic, version;
            // front -> back: the camera (seqlock: odd while written)
            std::atomic<uint32_t> camSeq;
            float pos[3], front[3], up[3], fov;
            int32_t hours, minutes;
            std::atomic<uint64_t> frontFrames;
            std::atomic<int64_t> frontNs; // host time of the last camera write
            std::atomic<uint32_t> frontGame; // kGameLcs / kGameVcs: which game is in front (shown, played); 0 = not set
            std::atomic<uint32_t> camGame;   // whose coordinates pos is in
            // back -> front: pictures, kSlots buffers; `latest` the newest complete one, `reading` the one being copied
            std::atomic<uint32_t> latest, reading;
            std::atomic<uint64_t> frameSeq;
            std::atomic<uint64_t> backFrames;
            std::atomic<int64_t> backNs;
            uint32_t w[kSlots], h[kSlots];
            uint64_t seqOf[kSlots];
            // the border handover: the front game that saw its player cross writes this, then hands the front over
            std::atomic<uint32_t> hoSeq;
            uint32_t hoTo;           // the game taking the player (kGameLcs / kGameVcs)
            float hoPos[3];          // in the taking game's coordinates
            float hoHeading, hoSpeed; // degrees (the game's heading: 0 = +y), m/s
            int32_t hoModel;         // the taking game's vehicle model (0: on foot)
            int32_t hoMoney, hoHealth;
            int32_t hoArmour, hoCurrent;  // armour; the weapon in hand (the taking game's type, 0 none)
            int32_t hoWeapons[10][2];     // the taking game's weapon types and ammo (type 0: none)
            std::atomic<uint32_t> ready[3]; // per game: in play and its picture handed over (it may take the player)
            // the front game's window (desktop coordinates of the client area), for the one shown next
            std::atomic<uint32_t> winSeq;
            int32_t winX, winY, winW, winH, winFull;
            // the front game's weather (its own types), for the background game's sky to match (written each game frame)
            std::atomic<uint32_t> wxGame; // whose types (0: none yet)
            int32_t wxOld, wxNew;
            // the front game's aspect widening (vcs::aspectWidening: display aspect / (4:3)), as float bits; 0: not set
            std::atomic<uint32_t> aspectBits;
            // [parity] the settings sync: each change made in the front game (its F2 menu, Alt+Enter, a preset) as a
            // record {key, value}; the other game applies the records it has not seen through settings::set, the F2
            // menu's own path. setCount is the version (records written so far); record i is in setRecs[i % kSetRecs]
            // and complete when its seq is i + 1. setLock: one writer at a time (the two games swap roles)
            std::atomic<uint32_t> setLock, setCount;
            struct SetRec
            {
                std::atomic<uint32_t> seq;
                uint32_t game;     // the writer
                int32_t value;
                int32_t isDefault; // the writer's default: the reader takes its own (LCS's effects volume 76 is VCS's 88)
                char key[56];
            } setRecs[64];
            // [parity] per game: host time of its last host frame (a stopped or crashed game is seen within seconds)
            std::atomic<int64_t> aliveNs[3];
            // [parity] the front's composite rect (its frame's picture in target pixels, gsdw::Stats rectW / rectH): the
            // background game hands its picture over at this size, so it is drawn 1:1 (no upscale blur)
            std::atomic<uint32_t> wantW, wantH;
            // [parity] the handover's vehicle motion: the whole move speed (m/s, x y z: climbing / falling too) and the turn
            // speed (the games' own units, per 1/50 s step), given to the arriving vehicle on its first frames
            float hoVel[3], hoTurn[3];
            // [parity] per game: its window (HWND on Windows, 0 elsewhere): the game leaving the front hands the
            // foreground (keyboard focus, mouse capture) to the other's window before it hides its own
            std::atomic<uint64_t> window[3];
            std::atomic<int64_t> gameNs[3]; // [parity] per game: host time of its last game frame (a dead game thread)
            std::atomic<uint32_t> pid[3]; // [parity] per game: its process (a stop is a process gone, not a long frame)
            // [dwlag] the frame lag fix (reprojection, gsdw::PictureCamera). front -> back: the front frame's tag of the
            // camera above (in the camera's seqlock), and the overscan the background renders with (float bits; 0: none,
            // the front draws the picture as it is). back -> front, per picture slot: the camera it was rendered with
            uint64_t camTag;
            std::atomic<uint32_t> overscanBits;
            struct PicCam
            {
                uint32_t on;
                float cam[3], front[3], right[3], up[3]; // the front game's coordinates; right / up * tan(half the view)
                uint64_t frontTag;
            } picCam[kSlots];
            uint8_t pixels[kSlots][kMaxW * kMaxH * 4];
            // [stories] the GPU hand-over (both games in one process, stories.exe): the background game's picture in one of
            // two textures shared with the front game's renderer (gsdw::setGpuSink / setGpuSource); only for a front game
            // of the same process (gpuPid). After the pixels: the layout before them is the two-process one, unchanged
            std::atomic<uint32_t> gpuPid;
            std::atomic<uint64_t> gpuSeq; // seqlock: odd while written
            uint64_t gpuHandle;
            uint32_t gpuW, gpuH, gpuSlot;
            PicCam gpuCam; // [dwlag] the camera the GPU picture was rendered with (the reprojection), as picCam[]
        };

        Role g_role = Role::Off;
        std::atomic<int> g_hostRole{0}; // the role as the main thread last applied it (window shown / hidden)
        bool g_enabled = false;
        // the handover (game thread)
        float g_border = 1800.0f;      // STORIES_DW_BORDER: the border, an LCS x (east of Portland; LCS stops boats at x 1900); VCS x = it + offset
        bool g_handover = true;        // STORIES_DW_HANDOVER=0: look only
        bool g_windows = true;         // STORIES_DW_WINDOWS=0: the windows left as they are (hidden test runs)
        float g_lastX = 0.0f;
        bool g_lastValid = false;
        int64_t g_cooldownUntil = 0;
        uint32_t g_hoApplied = 0;
        int64_t g_frontSinceNs = 0;
        // the crossing's fade (STORIES_DW_FADE_MS, default 400; 0: none): the game left fades to black, the handover
        // happens at black, the game arriving shows black until it has the player in place, then fades in. Drawn by our
        // renderer over the picture and its HUD (gsdw::setFade), not with the games' own camera / script fades: those
        // belong to the mission scripts, which may be failing at that moment (a fail fades and restarts VCS's opening)
        int64_t g_fadeNs = 400000000ll;
        std::atomic<int64_t> g_fadeOutStart{0}; // leaving: the fade out's start (0: none)
        std::atomic<int64_t> g_fadeInStart{0};  // arriving: the fade in's start (may be ahead: a short hold at black)
        std::atomic<bool> g_fadeHold{false};    // arriving: black until the player is in place
        int64_t g_holdSince = 0;
        std::atomic<int64_t> g_blackUntil{0};   // left: black until the other window has come up
        std::atomic<float> g_fadeNow{0.0f};
        // [bridge2] the vehicle the player crossed the line in (0: on foot), kept for the handover at black: a car
        // crossing arrives in a car even if the ped left it (or its state flickered) during the fade
        uint32_t g_crossVeh = 0;
        // arriving in a vehicle the game puts its drive-by weapon in hand (LCS slot 5, the SMG); the weapon carried across
        // goes back in hand when the player gets out (front, game thread)
        int g_restoreWeapon = 0;      // the arriving game's type (0: nothing to do)
        bool g_restoreSeenIn = false; // the player was seen in the vehicle
        int64_t g_restoreOutSince = 0;
        int g_onFootWeapon = 0; // this game's type last in hand on foot (in a vehicle the hand holds the drive-by weapon)
        std::vector<std::pair<int, int>> g_carMap; // STORIES_DW_CARMAP "mine:theirs,...": this game's vehicle -> the other's
        uint32_t g_me = 0; // kGameLcs / kGameVcs
        Shared *g_shm = nullptr;
        constexpr float kSeaDelta = 6.0f; // [farsea] VCS sea (water z 6.0) - LCS sea (0.0: CWaterLevel's levels)
        float g_off[3] = {-3800.0f, 0.0f, 0.0f};
        uint32_t g_sinkW = 960, g_sinkH = 672;
        uint32_t g_backW = 0, g_backH = 0; // the picture's size as the live aspect made it (0: g_sinkW x g_sinkH)
        bool g_clock = true;

        // front: the copy the renderer reads
        std::vector<uint8_t> g_copy;
        const uint8_t *g_direct = nullptr; // [robust] the picture read in place in the shared memory (source)
        uint64_t g_copySeq = 0;
        uint32_t g_copyW = 0, g_copyH = 0;
        gsdw::PictureCamera g_copyCam; // [dwlag] the camera it was rendered with

        // [dwlag] The frame lag. The background game renders with the front's camera of a frame or two before, and the
        // front composites its newest picture a frame or two later still: the far city trailed the camera when it turned.
        // Each game frame gets a tag (gsdw::setFrameTag) that its world's draws carry to the render thread; the
        // background game hands each picture over with the camera of its frame, the front reprojects it into the camera
        // of the frame it composes (gs_util.hlsl ps_dw_backdrop). The background renders a little wider than the view
        // (the overscan), so a fast turn still finds picture at the screen's edges.
        uint64_t g_tag = 0;                  // game thread: this game's frame tag
        std::atomic<bool> g_reproject{true}; // STORIES_DW_REPROJECT=0 / `dualworld reproject 0|1`
        std::atomic<bool> g_hideBackdrop{false}; // tests (`dualworld backdrop 0|1`): the front without the other city
        float g_overscan = 1.05f;            // STORIES_DW_OVERSCAN: the background's view widened by this (tan)
        struct BackCam
        {
            bool on = false;
            float pos[3]{}, front[3]{}, up[3]{}, fov = 70.0f; // the front game's coordinates, the FOV rendered with
            uint64_t frontTag = 0;
        };
        BackCam g_backPending;                // game thread: set by backCamera in this frame's camera update
        std::mutex g_backCamsMutex;           // game thread writes, render thread (sink) reads
        std::pair<uint64_t, gsdw::PictureCamera> g_backCams[16]; // by this game's frame tag
        // tests (`dualworld sweep`): the front's free camera swung up / down and sideways
        struct Sweep
        {
            bool on = false;
            int64_t start = 0, end = 0;
            float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, fov = 70, ampPitch = 0, ampYaw = 0, period = 1;
        };
        std::mutex g_sweepMutex;
        Sweep g_sweep;

        std::atomic<uint64_t> g_camWrites{0}, g_camSkips{0};
        std::atomic<uint64_t> g_sinkTag{0}, g_gameTag{0};
        std::atomic<uint64_t> g_picMiss{0}, g_picOff{0}; // [farsea] pictures handed over with no camera: its tag not kept / the frame had no camera
        // front, set each game frame: the backdrop's opacity (0 hides it: an interior, thick fog)
        std::atomic<float> g_opacity{1.0f};
        // back: pictures handed over (STORIES_DW_WAIT=1: not until `dualworld resume` - the boot, the intro cutscene)
        std::atomic<bool> g_sinkOn{true};
        std::atomic<uint32_t> g_area{0};
        std::atomic<float> g_farClip{0.0f};
        std::atomic<bool> g_fogAlpha{false}; // [tone] this game's far fog handed over as alpha while behind (install)
        std::atomic<int64_t> g_lastLog{0};

        int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow().time_since_epoch()).count(); }

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        Shared *openShared(const std::string &name)
        {
            const size_t size = sizeof(Shared);
            // [stories] both games in one process (Android: src/android/stories_host.cpp): the host's block, plain memory,
            // handed to each game as STORIES_DW_BLOCK=<address>:<size>, hex (docs/ANDROID.md "Both cities on Android")
            if (const char *b = std::getenv("STORIES_DW_BLOCK"); b && *b)
            {
                unsigned long long addr = 0, have = 0;
                if (std::sscanf(b, "%llx:%llx", &addr, &have) != 2 || !addr || have < size)
                {
                    std::fprintf(stderr, "[dualworld] STORIES_DW_BLOCK '%s' unusable (needs 0x%zx bytes)\n", b, size);
                    return nullptr;
                }
                std::fprintf(stderr, "[dualworld] the host's block, in process (0x%zx bytes)\n", size);
                return reinterpret_cast<Shared *>(static_cast<uintptr_t>(addr));
            }
#if defined(_WIN32)
            std::wstring wname(name.begin(), name.end());
            HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(uint64_t(size) >> 32),
                                          static_cast<DWORD>(size & 0xFFFFFFFFu), wname.c_str());
            if (!h)
                return nullptr;
            void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
            return static_cast<Shared *>(p); // the handle stays open for the process's life
#elif defined(__ANDROID__)
            (void)name; // [android] no shm_open in bionic: only the one-process host's block above (STORIES_DW_BLOCK)
            (void)size;
            return nullptr;
#else
            const std::string path = "/" + name;
            int fd = shm_open(path.c_str(), O_CREAT | O_RDWR, 0600);
            if (fd < 0)
                return nullptr;
            if (ftruncate(fd, static_cast<off_t>(size)) != 0)
                return nullptr;
            void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            return p == MAP_FAILED ? nullptr : static_cast<Shared *>(p);
#endif
        }

        // [dwlag] the camera of the frame a picture shows (its tag), for the front's reprojection
        Shared::PicCam picCamOf(uint64_t tag)
        {
            Shared::PicCam pc{};
            std::lock_guard<std::mutex> lock(g_backCamsMutex);
            const auto &bc = g_backCams[tag % 16u];
            if (tag && bc.first == tag && bc.second.on)
            {
                pc.on = 1;
                std::memcpy(pc.cam, bc.second.cam, sizeof(pc.cam));
                std::memcpy(pc.front, bc.second.front, sizeof(pc.front));
                std::memcpy(pc.right, bc.second.right, sizeof(pc.right));
                std::memcpy(pc.up, bc.second.up, sizeof(pc.up));
                pc.frontTag = bc.second.frontTag;
            }
            else
                (tag && bc.first == tag ? g_picOff : g_picMiss).fetch_add(1, std::memory_order_relaxed);
            return pc;
        }

        gsdw::PictureCamera pictureCamera(const Shared::PicCam &pc)
        {
            gsdw::PictureCamera c{};
            c.on = pc.on != 0;
            std::memcpy(c.cam, pc.cam, sizeof(pc.cam));
            std::memcpy(c.front, pc.front, sizeof(pc.front));
            std::memcpy(c.right, pc.right, sizeof(pc.right));
            std::memcpy(c.up, pc.up, sizeof(pc.up));
            c.frontTag = pc.frontTag;
            return c;
        }

        // back: the renderer's frame (its sky alpha 0) into a free slot
        void sink(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t tag)
        {
            g_sinkTag.store(tag, std::memory_order_relaxed);
            if (!g_shm || w > kMaxW || h > kMaxH || !g_sinkOn.load(std::memory_order_relaxed))
                return;
            const uint32_t latest = g_shm->latest.load(std::memory_order_acquire);
            const uint32_t reading = g_shm->reading.load(std::memory_order_acquire);
            uint32_t slot = 0;
            while (slot == latest || slot == reading)
                ++slot;
            if (slot >= kSlots)
                return;
            std::memcpy(g_shm->pixels[slot], rgba, size_t(w) * h * 4u);
            g_shm->w[slot] = w;
            g_shm->h[slot] = h;
            {
                // [dwlag] the camera of the frame this picture shows (its tag), for the front's reprojection
                Shared::PicCam pc{};
                std::lock_guard<std::mutex> lock(g_backCamsMutex);
                const auto &bc = g_backCams[tag % 16u];
                if (tag && bc.first == tag && bc.second.on)
                {
                    pc.on = 1;
                    std::memcpy(pc.cam, bc.second.cam, sizeof(pc.cam));
                    std::memcpy(pc.front, bc.second.front, sizeof(pc.front));
                    std::memcpy(pc.right, bc.second.right, sizeof(pc.right));
                    std::memcpy(pc.up, bc.second.up, sizeof(pc.up));
                    pc.frontTag = bc.second.frontTag;
                }
                else
                    (tag && bc.first == tag ? g_picOff : g_picMiss).fetch_add(1, std::memory_order_relaxed);
                g_shm->picCam[slot] = pc;
            }
            g_shm->seqOf[slot] = g_shm->frameSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            g_shm->latest.store(slot, std::memory_order_release);
            g_shm->backFrames.fetch_add(1, std::memory_order_relaxed);
            g_shm->backNs.store(nowNs(), std::memory_order_relaxed);
        }

        // [stories] the GPU hand-over: on in the one-process host (STORIES_HOST_HWND) unless STORIES_DW_GPU=0
        bool g_gpu = false;

        uint32_t thisPid()
        {
#if defined(_WIN32)
            return static_cast<uint32_t>(GetCurrentProcessId());
#else
            return static_cast<uint32_t>(getpid());
#endif
        }

        // the background game handed its picture over on the GPU, in this process, lately
        bool gpuPictureLive()
        {
            return g_shm && g_shm->gpuPid.load(std::memory_order_acquire) == thisPid() &&
                   g_shm->gpuSeq.load(std::memory_order_acquire) != 0 &&
                   nowNs() - g_shm->backNs.load(std::memory_order_relaxed) <= 3000000000ll;
        }

        // back, render thread: the texture just filled (unlocked)
        void gpuSink(uint64_t handle, uint32_t w, uint32_t h, uint32_t slot, uint64_t tag)
        {
            if (!g_shm || !g_sinkOn.load(std::memory_order_relaxed))
                return;
            const Shared::PicCam pc = picCamOf(tag);
            const uint64_t seq = g_shm->gpuSeq.load(std::memory_order_relaxed);
            g_shm->gpuSeq.store(seq | 1u, std::memory_order_release); // odd: being written
            g_shm->gpuHandle = handle;
            g_shm->gpuW = w, g_shm->gpuH = h, g_shm->gpuSlot = slot;
            g_shm->gpuCam = pc;
            g_shm->gpuPid.store(thisPid(), std::memory_order_relaxed);
            g_shm->gpuSeq.store((seq | 1u) + 1u, std::memory_order_release);
            g_shm->backFrames.fetch_add(1, std::memory_order_relaxed);
            g_shm->backNs.store(nowNs(), std::memory_order_relaxed);
        }

        // [stories] same frame (experimental, STORIES_DW_SYNC_MS=ms, default 0: off): the front's composite waits up to
        // that long for the picture rendered with the camera of the frame it composes (frontTag + STORIES_DW_SYNC_LAG >=
        // the frame's tag), instead of taking the newest one there is
        int g_syncUs = 0;
        uint64_t g_syncLag = 0;

        // front, render thread: the newest picture on the GPU
        bool gpuSource(gsdw::GpuPicture &out)
        {
            if (!gpuPictureLive())
                return false;
            if (g_syncUs > 0 && out.composing)
            {
                const int64_t until = nowNs() + int64_t(g_syncUs) * 1000;
                while (nowNs() < until)
                {
                    const uint64_t a = g_shm->gpuSeq.load(std::memory_order_acquire);
                    if (!(a & 1u) && g_shm->gpuCam.on && g_shm->gpuCam.frontTag + g_syncLag >= out.composing)
                        break;
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            const float opacity = g_opacity.load(std::memory_order_relaxed);
            if (opacity <= 0.01f || g_hideBackdrop.load(std::memory_order_relaxed))
                return false;
            for (int tries = 0; tries < 4; ++tries)
            {
                const uint64_t a = g_shm->gpuSeq.load(std::memory_order_acquire);
                if (a & 1u)
                    continue;
                out.handle = g_shm->gpuHandle;
                out.w = g_shm->gpuW, out.h = g_shm->gpuH, out.slot = g_shm->gpuSlot;
                out.camera = pictureCamera(g_shm->gpuCam);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (g_shm->gpuSeq.load(std::memory_order_acquire) == a)
                {
                    out.seq = a;
                    out.opacity = opacity;
                    return true;
                }
            }
            out.contended = true; // [farsea] the writer had it every try
            return false;
        }

        // front: the newest background picture (copied, so the other process may write on)
        bool source(gsdw::Picture &out)
        {
            if (!g_shm || g_shm->backFrames.load(std::memory_order_relaxed) == 0)
                return false;
            if (gpuPictureLive())
                return false; // [stories] handed over on the GPU (a moment the GPU one is held: no older CPU picture)
            if (nowNs() - g_shm->backNs.load(std::memory_order_relaxed) > 3000000000ll)
                return false; // the background game stopped: no stale picture
            const float opacity = g_opacity.load(std::memory_order_relaxed);
            if (opacity <= 0.01f || g_hideBackdrop.load(std::memory_order_relaxed))
                return false;
            uint32_t slot = g_shm->latest.load(std::memory_order_acquire);
            if (slot < kSlots && g_shm->seqOf[slot] != g_copySeq)
            {
                // [robust] the picture read in place: its slot stays marked `reading` (the background game writes the
                // other two) until a newer one is taken, and the renderer uploads straight from the shared memory into
                // its mapped texture - no copy of 14 MB a frame at 1440p in between. A slot that stopped being the newest
                // while it was marked may be being written: the newest is taken instead. The haze (opacity < 1) still
                // needs a copy of its own (its alpha scaled).
                for (int tries = 0; tries < 3; ++tries)
                {
                    g_shm->reading.store(slot, std::memory_order_release);
                    const uint32_t now = g_shm->latest.load(std::memory_order_acquire);
                    if (now == slot || now >= kSlots)
                        break;
                    slot = now;
                }
                const uint32_t w = g_shm->w[slot], h = g_shm->h[slot];
                if (w && h && w <= kMaxW && h <= kMaxH)
                {
                    if (opacity < 0.99f) // haze: the other city fades out as this one's far plane closes in
                    {
                        g_copy.resize(size_t(w) * h * 4u);
                        std::memcpy(g_copy.data(), g_shm->pixels[slot], g_copy.size());
                        const uint32_t k = static_cast<uint32_t>(opacity * 256.0f);
                        for (size_t i = 3; i < g_copy.size(); i += 4)
                            g_copy[i] = static_cast<uint8_t>((g_copy[i] * k) >> 8);
                        g_direct = nullptr;
                    }
                    else
                        g_direct = g_shm->pixels[slot];
                    g_copyW = w, g_copyH = h, g_copySeq = g_shm->seqOf[slot];
                    const Shared::PicCam &pc = g_shm->picCam[slot]; // [dwlag]
                    g_copyCam = {};
                    g_copyCam.on = pc.on != 0;
                    std::memcpy(g_copyCam.cam, pc.cam, sizeof(pc.cam));
                    std::memcpy(g_copyCam.front, pc.front, sizeof(pc.front));
                    std::memcpy(g_copyCam.right, pc.right, sizeof(pc.right));
                    std::memcpy(g_copyCam.up, pc.up, sizeof(pc.up));
                    g_copyCam.frontTag = pc.frontTag;
                }
                else
                    g_direct = nullptr, g_copy.clear();
                if (!g_direct)
                    g_shm->reading.store(kSlots, std::memory_order_release);
            }
            if (!g_direct && g_copy.empty())
                return false;
            out.rgba = g_direct ? g_direct : g_copy.data();
            out.w = g_copyW;
            out.h = g_copyH;
            out.seq = g_copySeq;
            out.camera = g_copyCam;
            return true;
        }

        int mapCar(int model)
        {
            // [crossover] an LCS pool slot holding one of VCS's own vehicles (game/lcs_crossover.h) goes back as that one
            if (game::isLcs())
                if (const int other = lcs::crossover::boundOther(model))
                    return other;
            if (!game::isLcs()) // [crossover] a VCS new id holding one of LCS's own vehicles (game/vcs_crossover.h)
                if (const int other = vcs::crossover::boundOther(model))
                    return other;
            for (const auto &[a, b] : g_carMap)
                if (a == model)
                    return b;
            return 0;
        }

        // The weapon in the other game closest to this one: the same slot (melee, thrown, pistol, shotgun, SMG,
        // rifle, heavy, sniper, camera), the nearest kind in it. 0: none (left behind). Types: lcs_trainer.cpp's and
        // vcs_trainer.cpp's kWeapons (LCS 16 and 35 are no player weapons; VCS's binoculars have no LCS twin)
        int mapWeapon(bool fromLcs, int type)
        {
            static const int kLcsToVcs[37] = {0,
                1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, // brass knuckles .. chainsaw (screwdriver: butterfly knife, hammer:
                                                   // axe, cleaver: gaff hook)
                12, 13, 16, 17, 0,                 // grenades, remote grenades, tear gas, molotovs
                19, 20,                            // pistol, .357: pistol, Equalizer
                21, 22, 23,                        // shotgun, combat: Spaz, stubby
                25, 24, 26, 27,                    // Tec-9: Scorpion, micro SMG, MAC-10, MP5: SMG
                28, 29,                            // M4: assault rifle, AK
                30, 31,                            // sniper, laser-scoped
                32, 33, 34, 35, 0,                 // rocket launcher, flame-thrower, M60: M249, minigun
                38};                               // camera
            static const int kVcsToLcs[40] = {0,
                1, 5, 3, 4, 5, 6, 7, 8, 9, 10, 11, // butterfly knife: knife, axe: hammer, gaff hook: cleaver
                12, 13, 13, 0, 14, 15, 0,          // grenades, remote, land mines: remote, tear gas, molotovs
                17, 18,                            // pistol, Equalizer: .357
                19, 20, 21,                        // shotguns
                23, 22, 24, 25,                    // micro SMG, Scorpion: Tec-9, Mac 10, SMG: MP5
                26, 27, 28, 29,                    // assault rifle: M4, AK, snipers
                30, 31, 32, 33, 0, 0,              // rocket launcher, flame-thrower, M249: M60, minigun
                36, 0};                            // camera; binoculars
            if (fromLcs)
                return type > 0 && type < 37 ? kLcsToVcs[type] : 0;
            return type > 0 && type < 40 ? kVcsToLcs[type] : 0;
        }

        // ---- A mission running in the game left is failed, the game's own way ------------------------------------
        // Both games' CRunningScript::Process (LCS 0x10A678, VCS 0x255FA8) call DoDeatharrestCheck (LCS 0x2FF130, VCS
        // 0x258C18) for mission scripts (LCS script +0x20E, VCS +0x20A). It goes on only with deatharrest on (LCS
        // +0x217, VCS +0x20D; Init sets it) and the OnAMission global (*ScriptSpace + *0x3D9C10 / *0x4CD3D4) == 1, then
        // asks IsRestartingAfterDeath (LCS 0x107578, VCS 0x24A650: CPlayerInfo +0xE8 == 1) and ...AfterArrest. When
        // the player is out it unwinds the GOSUB stack to the mission's first frame and RETURNs (LCS 0x111EC0, VCS
        // 0x256180), clears the script text (LCS 0x2F85B0, VCS 0x3AF468), OnAMission = 0, deatharrest-executed = 1 (LCS
        // +0x218, VCS +0x20E), wake time 0. The mission then takes its HAS_DEATHARREST_BEEN_EXECUTED branch: "MISSION
        // FAILED", its cleanup, MISSION_HAS_FINISHED (missionRunning 0), and the marker comes back - as after a death.
        // We answer "yes" to that one question (the call from DoDeatharrestCheck only: ra LCS 0x2FF190, VCS 0x258C94)
        // for a few frames after the crossing; nothing else of a death happens (no hospital, nothing taken).
        // Also FAIL_CURRENT_MISSION's flag (LCS 0x3D9C34, VCS 0x4CD3F0; the command sets 2, CTheScripts::Process counts
        // it down, the mission script RETURNs from its first frame at 1): the cleanup without the fail text, for a
        // mission that turned deatharrest off. After the check's unwinding the stack depth is 0: it does nothing more.
        constexpr uint32_t kLcsRestarting = 0x00107578u, kLcsRestartingRa = 0x002FF190u, kLcsFailFlag = 0x003D9C34u;
        constexpr uint32_t kVcsRestarting = 0x0024A650u, kVcsRestartingRa = 0x00258C94u, kVcsFailFlag = 0x004CD3F0u;
        constexpr uint32_t kLcsMissionRunning = 0x003D9C29u, kVcsMissionRunning = 0x004CD3E7u; // u8 (lcs/vcs_trainer)
        PS2Runtime::RecompiledFunction g_restartingNext = nullptr;
        std::atomic<int> g_failFrames{0}; // game frames left in which the mission fails
        std::atomic<uint64_t> g_failsDone{0};
        std::atomic<bool> g_failQueued{false}; // `dualworld failmission` (tests)

        PS2_GUEST_CC void restartingHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_failFrames.load(std::memory_order_relaxed) > 0 && ra == (game::isLcs() ? kLcsRestartingRa : kVcsRestartingRa))
            {
                // once: as at a death, the first mission script asked sets OnAMission 0 and the rest of the pass is not
                // asked; a mission the fail starts again at once (VCS's opening "Soldier" does) is left to run
                g_failFrames.store(0, std::memory_order_relaxed);
                SET_GPR_U64(ctx, 2, 1);
                ctx->pc = ra;
                g_failsDone.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[dualworld] the mission fails (the player left the city)\n");
                return;
            }
            g_restartingNext(rdram, ctx, runtime);
        }

        // [robust] Vice City in both-cities mode never plays its opening mission (Soldier, JER_A1 = mission 7): on a new
        // card the main script launches it (MAIN.SCM 0x7A96: `IF g4 == 0 AND g2 == 0` -> g0x2AC = 1, g0x5D4 = 0,
        // LOAD_AND_LAUNCH_MISSION_INTERNAL 7) and launches it again whenever it fails (0x7B28: g3 == 1 -> g3 = 0, launch 7;
        // Soldier's fail branch sets g3 = 1, mission +0x5AEE). Behind Liberty City every crossing failed it, so it came back
        // with its intro cutscene each time. LOAD_AND_LAUNCH_MISSION_INTERNAL is opcode 0x289 (the opcode table 0x4BEAF0
        // -> handler 0x3B2208, which CollectParameters the index and returns 0). The hook answers that command from the main
        // script with index 7 by stepping over its argument: the main script then waits in its loop (it leaves it on g6 = 1,
        // the save flag) and the player is in free roam where INITPLA put him (Fort Baxter). A loaded save never takes
        // that path (g2 / g4 set). STORIES_DW_OPENING=1 (tools/stories.py sets 0) plays it as usual.
        constexpr uint32_t kVcsLaunchMission = 0x003B2208u, kVcsScriptSpace = 0x0048741Cu, kVcsOpening = 7u;
        constexpr uint32_t kVcsTheCamera = 0x006F44D0u, kVcsCameraFade = 0x00376D90u;
        PS2Runtime::RecompiledFunction g_launchNext = nullptr;
        std::atomic<uint32_t> g_openingSkipped{0};

        PS2_GUEST_CC void launchMissionHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t script = GPR_U32(ctx, 4);
            uint32_t space = 0, ip = 0;
            std::memcpy(&space, rdram + (kVcsScriptSpace & kRamMask), 4);
            std::memcpy(&ip, rdram + ((script + 0x10u) & kRamMask), 4);
            const uint8_t *arg = rdram + ((space + ip) & kRamMask);
            const bool mainScript = rdram[(script + 0x217u) & kRamMask] == 0; // not a mission (missions never launch it)
            if (ctx->pc == kVcsLaunchMission && mainScript && arg[0] == 7 && arg[1] == kVcsOpening)
            {
                std::memcpy(rdram + ((script + 0x10u) & kRamMask), &(ip += 2u), 4); // past `07 07` (int8 7)
                // The new game's screen is faded out (the loading picture stays up, nothing drawn) until Soldier fades
                // in: instead, what DO_FADE 0 1 (opcode 0xC9, handler 0x287AA0) does - CCamera::Fade (0x376D90:
                // TheCamera 0x6F44D0, f12 seconds, a1 1 = in) - called in the command's place; it returns to
                // ProcessOneCommand (a nonzero v0 only ends the main script's turn for this frame)
                SET_GPR_U32(ctx, 4, kVcsTheCamera);
                SET_GPR_U32(ctx, 5, 1);
                ctx->f[12] = 0.0f;
                ctx->pc = kVcsCameraFade;
                if (g_openingSkipped.fetch_add(1, std::memory_order_relaxed) == 0)
                    std::fprintf(stderr, "[dualworld] Vice City's opening mission (Soldier) not started: free roam\n");
                return;
            }
            g_launchNext(rdram, ctx, runtime);
        }

        bool missionOn(const uint8_t *ram) { return ram[(game::isLcs() ? kLcsMissionRunning : kVcsMissionRunning) & kRamMask] != 0; }

        // [missionborder] STORIES_DW_MISSION_BORDER: fail (default) = crossing fails the mission (failMission, above); block = the border is a wall while a
        // mission runs in this city (wallAtBorder, checkBorder)
        bool g_blockBorder = false;
        std::atomic<int> g_fakeMission{0}; // 1 held, 2 clear once; // tests: `dualworld fakemission 1` sets this game's mission-running byte for good

        void failMission(uint8_t *ram)
        {
            if (!g_restartingNext || !missionOn(ram))
                return;
            g_failFrames = 3;
            ram[(game::isLcs() ? kLcsFailFlag : kVcsFailFlag) & kRamMask] = 2;
            std::fprintf(stderr, "[dualworld] a mission runs: failing it\n");
        }

        // The player as this game has them: position (of the vehicle when in one), forward, move speed (m/s), the
        // vehicle and its model, money, health. LCS: CPlayerInfo[0] 0x408E40 (ped; money +0xBC), ped +0x534 the vehicle
        // when the state +0x34C is 0x32 (driving), model u16 +0x58, move speed +0x70 per 1/50 s, health float +0x4B8.
        // VCS: ped *0x4E4910, money 0x4E49CC, vehicle +0x480 with state +0x8C4 0x37, model u16 +0x56, move speed
        // +0x140, health +0x4E4 (vcs_trainer.cpp, vcs_carcam.cpp, vcs_physics_timing.cpp).
        struct Player
        {
            bool ok = false;
            uint32_t ped = 0, veh = 0;
            float pos[3]{}, fwd[3]{}, speed = 0.0f;
            float vel[3]{}, turn[3]{}; // [parity] the vehicle's move speed (m/s) and turn speed (per step)
            int model = 0, money = 0, health = 0, armour = 0;
            int weapons[10][2]{}; // this game's weapon type and ammo (the clip included) per slot
            int currentSlot = 0;
        };
        int vehicleModel(const uint8_t *ram, uint32_t veh)
        {
            uint16_t m;
            std::memcpy(&m, ram + ((veh + (game::isLcs() ? 0x58u : 0x56u)) & kRamMask), 2);
            return m;
        }
        // the vehicle's place, heading, speed and model as the player's
        void readVehicle(const uint8_t *ram, uint32_t veh, Player &pl)
        {
            const bool lcs = game::isLcs();
            pl.veh = veh;
            for (int i = 0; i < 3; ++i)
            {
                pl.pos[i] = rdf(ram, veh + 0x30u + 4u * i);
                pl.fwd[i] = rdf(ram, veh + 0x10u + 4u * i);
            }
            const uint32_t ms = veh + (lcs ? 0x70u : 0x140u);
            pl.speed = std::hypot(rdf(ram, ms), rdf(ram, ms + 4u)) * 50.0f;
            // [parity] CPhysical m_vecTurnSpeed: LCS +0x80 (ACTIVATE_SAVE_MENU zeroes +0x70 / +0x80: 0x22A878 /
            // 0x22A8A8, lcs_save.cpp), VCS +0x70 (the collision log in vcs_overrides.cpp: v +0x140, w +0x70)
            const uint32_t ts = veh + (lcs ? 0x80u : 0x70u);
            for (int i = 0; i < 3; ++i)
            {
                pl.vel[i] = rdf(ram, ms + 4u * i) * 50.0f;
                pl.turn[i] = rdf(ram, ts + 4u * i);
            }
            pl.model = vehicleModel(ram, veh);
        }

        Player readPlayer(const uint8_t *ram)
        {
            Player pl;
            const bool lcs = game::isLcs();
            pl.ped = rd32(ram, lcs ? 0x00408E40u : 0x004E4910u);
            if (!pl.ped || pl.ped >= 0x02000000u)
                return pl;
            const uint32_t v = rd32(ram, pl.ped + (lcs ? 0x534u : 0x480u));
            const bool driving = v && v < 0x02000000u && rd32(ram, pl.ped + (lcs ? 0x34Cu : 0x8C4u)) == (lcs ? 0x32u : 0x37u);
            pl.veh = driving ? v : 0u;
            const uint32_t at = pl.veh ? pl.veh : pl.ped;
            for (int i = 0; i < 3; ++i)
            {
                pl.pos[i] = rdf(ram, at + 0x30u + 4u * i);
                pl.fwd[i] = rdf(ram, at + 0x10u + 4u * i);
            }
            if (pl.veh)
                readVehicle(ram, pl.veh, pl);
            pl.money = static_cast<int>(rd32(ram, lcs ? 0x00408EFCu : 0x004E49CCu));
            pl.health = static_cast<int>(rdf(ram, pl.ped + (lcs ? 0x4B8u : 0x4E4u)));
            // armour float LCS +0x4BC (ADD_ARMOUR_TO_CHAR), VCS +0x4E8; weapon slots LCS +0x594, VCS +0x578, 10 x 0x1C
            // {type, state, clip, total}; the slot in hand s8 LCS +0x6B8, VCS +0x789 (lcs_save.cpp, game-internals 12)
            pl.armour = static_cast<int>(rdf(ram, pl.ped + (lcs ? 0x4BCu : 0x4E8u)));
            for (int s = 0; s < 10; ++s)
            {
                const uint32_t slot = pl.ped + (lcs ? 0x594u : 0x578u) + 0x1Cu * s;
                pl.weapons[s][0] = static_cast<int>(rd32(ram, slot));
                pl.weapons[s][1] = static_cast<int>(rd32(ram, slot + 0xCu));
            }
            pl.currentSlot = static_cast<int8_t>(ram[(pl.ped + (lcs ? 0x6B8u : 0x789u)) & kRamMask]);
            pl.ok = std::isfinite(pl.pos[0]) && std::fabs(pl.pos[0]) < 5000.0f;
            return pl;
        }

        void handOver(uint8_t *ram, int64_t now);

        bool gameReady() { return game::isLcs() ? lcs::trainer::ready() : vcs::trainer::ready(); }

        const char *cityName(uint32_t g) { return g == kGameLcs ? "Liberty City" : "Vice City"; }

        // [parity] the other game is running: its host frames go on (aliveNs) - a crash or a quit is seen in 5 s
        bool processRunning(uint32_t pid)
        {
            if (!pid)
                return false;
#if defined(_WIN32)
            const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h)
                return GetLastError() == ERROR_ACCESS_DENIED; // there, but not ours to look at
            DWORD code = 0;
            const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            return running;
#elif defined(__ANDROID__)
            return true;
#else
            return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
        }

        // the other game runs: its host frames go on, or (a long frame: loading, a 4K target made) its process is still
        // there. Only a process gone counts as stopped (a 5 s hitch once handed the front over by itself in a test)
        bool otherAlive(uint32_t other, int64_t now)
        {
            // a game thread that died (stress test: a null call in VCS's 0x18F9D4 left the process, its window and its
            // host frames running, its game frozen for good): no game frame for 15 s is a stop too (loads take less)
            if (const int64_t g = g_shm->gameNs[other].load(std::memory_order_relaxed); g != 0 && now - g > 15000000000ll)
                return false;
            const int64_t t = g_shm->aliveNs[other].load(std::memory_order_relaxed);
            if (t != 0 && now - t < 5000000000ll)
                return true;
            return t != 0 && processRunning(g_shm->pid[other].load(std::memory_order_relaxed));
        }

        // [parity] a note on screen (the trainer's notices, both games) and in the log, at most every 4 s per kind
        void note(int kind, const char *text, int64_t now)
        {
            static int64_t last[8] = {};
            if (kind < 0 || kind >= 8 || now - last[kind] < 4000000000ll)
                return;
            last[kind] = now;
            vcs::trainer::notify(text);
            std::fprintf(stderr, "[dualworld] note: %s\n", text);
        }

        // front, each game frame: the player over the border line (LCS: x past it eastwards; VCS: x past it + offset
        // westwards) hands the front to the other game, which puts the player at the matching place.
        // [parity] Armed / fired (not a single frame's edge): the crossing fires on the first frame the player is past the
        // line, having been seen on this side since arriving here, and nothing stops it. It waits (the player may sail
        // on past the line; it happens once the reason is gone) while a crossing's fade runs, in the cooldown after one
        // (STORIES_DW_COOLDOWN_MS, default 4000: rapid back-and-forth can't wedge it), in a cutscene or scripted scene, in
        // the pause menu, and while the other city is not ready (loading, stopped). A short note says why.
        bool g_armed = false;
        int64_t g_cooldownNs = 4000000000ll;
        std::atomic<uint64_t> g_crossings{0}, g_refusals{0};

        // [parity] a cutscene or a scripted camera holds the view: the active CCam's mode is one of the fixed / fly-by /
        // cutscene ones, 14-17 (player_control.cpp's camera read: LCS TheCamera +0x10C active, +0x220 + i x 0x2B0, mode
        // +0x40; VCS +0x50, +0x70 + i x 0x270, mode +0). Not CPad's DisablePlayerControls: Vice City's opening mission,
        // restarted after a crossing fails it, leaves bits 0x80 | 0x20 set there for good while the player drives on
        // (seen in the stress test: 160 with the player in a helicopter, camera mode 18), so no crossing would ever go.
        bool cutsceneCamera(const uint8_t *ram)
        {
            const bool lcs = game::isLcs();
            const uint32_t cam = lcs ? kLcsCamera : kVcsCamera;
            const uint32_t active = ram[(cam + (lcs ? kLcsActive : kVcsActive)) & kRamMask];
            if (active > 2)
                return false;
            uint16_t mode;
            std::memcpy(&mode, ram + ((lcs ? cam + kLcsCams + active * kLcsCamSize + 0x40u : cam + kVcsCams + active * kVcsCamSize) & kRamMask), 2);
            return mode >= 14 && mode <= 17;
        }

        bool fadeActive(int64_t now)
        {
            return g_fadeOutStart.load(std::memory_order_relaxed) || g_fadeInStart.load(std::memory_order_relaxed) ||
                   g_fadeHold.load(std::memory_order_relaxed) || now < g_blackUntil.load(std::memory_order_relaxed);
        }

        // [missionborder] A mission runs here: the border line is a wall. The player (the vehicle when driving) is put back
        // just inside the line and the part of his motion going out is bounced (30%, at most 3 m/s back), so a car, boat,
        // helicopter or plane meets it like a soft wall instead of being stuck on it; the rest of the motion (along the
        // border, up, down) is kept. Position: matrix translation +0x30 (readPlayer); motion: CPhysical move speed (LCS
        // +0x70, VCS +0x140, per 1/50 s; readVehicle). On foot the ped is only set back (the pad drives his speed).
        void wallAtBorder(uint8_t *ram, const Player &pl, float line, bool lcs, int64_t now)
        {
            const float out = lcs ? 1.0f : -1.0f; // the direction of the other city along x
            const uint32_t at = pl.veh ? pl.veh : pl.ped;
            wrf(ram, at + 0x30u, line - out * 0.5f);
            if (pl.veh)
            {
                const uint32_t ms = pl.veh + (lcs ? 0x70u : 0x140u);
                const float vx = rdf(ram, ms) * 50.0f * out; // m/s going out
                if (vx > 0.0f)
                    wrf(ram, ms, -std::min(vx * 0.3f, 3.0f) * out / 50.0f);
            }
            g_refusals.fetch_add(1, std::memory_order_relaxed);
            note(4, "Finish the mission before crossing the border", now);
        }

        void checkBorder(uint8_t *ram, int64_t now)
        {
            if (!g_handover || !gameReady())
            {
                g_lastValid = false;
                return;
            }
            const Player pl = readPlayer(ram);
            if (!pl.ok)
            {
                g_lastValid = false;
                return;
            }
            if (!pl.veh && pl.currentSlot >= 0 && pl.currentSlot < 10)
                g_onFootWeapon = pl.weapons[pl.currentSlot][0];
            const bool lcs = game::isLcs();
            const float line = lcs ? g_border : g_border + g_off[0];
            const float x = pl.pos[0];
            const bool past = lcs ? x > line : x < line;
            g_lastX = x;
            g_lastValid = true;
            if (!past)
            {
                g_armed = true; // seen on this side: the next time past the line is a crossing
                return;
            }
            const uint32_t other = lcs ? kGameVcs : kGameLcs;
            if (!g_armed || fadeActive(now))
                return;
            if (g_blockBorder && missionOn(ram))
            {
                wallAtBorder(ram, pl, line, lcs, now);
                return;
            }
            if (now < g_cooldownUntil)
            {
                g_refusals.fetch_add(1, std::memory_order_relaxed);
                return note(0, "Crossing in a moment...", now);
            }
            if (cutsceneCamera(ram) || (lcs ? lcs::frontend::open() : vcs::frontend::open()))
            {
                g_refusals.fetch_add(1, std::memory_order_relaxed);
                return note(1, "No crossing during a cutscene or a menu: it goes on once you have control", now);
            }
            if (!otherAlive(other, now))
            {
                g_refusals.fetch_add(1, std::memory_order_relaxed);
                char text[128];
                std::snprintf(text, sizeof(text), "%s is not running: no crossing (it is being restarted)", cityName(other));
                return note(2, text, now);
            }
            if (!g_shm->ready[other].load(std::memory_order_acquire) ||
                now - g_shm->backNs.load(std::memory_order_relaxed) > 2000000000ll)
            {
                g_refusals.fetch_add(1, std::memory_order_relaxed);
                char text[128];
                std::snprintf(text, sizeof(text), "%s is still loading: crossing when it is ready", cityName(other));
                return note(3, text, now);
            }
            g_armed = false;
            g_crossings.fetch_add(1, std::memory_order_relaxed);
            g_cooldownUntil = now + g_fadeNs + g_cooldownNs;
            g_crossVeh = pl.veh; // [bridge2] handed over in it (handOver), whatever the ped does during the fade
            if (g_fadeNs <= 0)
                return handOver(ram, now);
            g_fadeOutStart = now; // handOver() at black
            std::fprintf(stderr, "[dualworld] border crossed at (%.0f, %.0f) %s (ped state 0x%x): fading out\n", pl.pos[0], pl.pos[1],
                         pl.veh ? "in a vehicle" : "on foot", rd32(ram, pl.ped + (lcs ? 0x34Cu : 0x8C4u)));
        }

        // front, at the end of the fade out: the player handed to the other game, which comes to the front
        void handOver(uint8_t *ram, int64_t now)
        {
            g_fadeOutStart = 0;
            Player pl = readPlayer(ram);
            const bool lcs = game::isLcs();
            // [bridge2] crossed in a vehicle but not driving it now (the ped's state left 0x32 / 0x37 during the fade: the
            // exit button, a jolt): handed over in that vehicle all the same, if it is still the one (same model, in RAM)
            const uint32_t crossVeh = std::exchange(g_crossVeh, 0u);
            if (pl.ok && !pl.veh && crossVeh && crossVeh < 0x02000000u)
            {
                const int m = vehicleModel(ram, crossVeh);
                Player v = pl;
                readVehicle(ram, crossVeh, v);
                if (m > 0 && std::isfinite(v.pos[0]) && std::fabs(v.pos[0] - pl.pos[0]) < 30.0f && std::fabs(v.pos[1] - pl.pos[1]) < 30.0f)
                {
                    std::fprintf(stderr, "[dualworld] the player crossed in vehicle model %d but is not driving it now (ped state 0x%x): "
                                         "handed over in it\n", m, rd32(ram, pl.ped + (lcs ? 0x34Cu : 0x8C4u)));
                    pl = v;
                }
            }
            const uint32_t other = lcs ? kGameVcs : kGameLcs;
            if (!pl.ok || !g_shm->ready[other].load(std::memory_order_acquire))
            {
                std::fprintf(stderr, "[dualworld] the crossing is called off (%s)\n", pl.ok ? "the other city is not ready" : "no player");
                g_fadeInStart = now;
                return;
            }
            const float sign = lcs ? 1.0f : -1.0f; // LCS -> VCS: + offset
            float to[3];
            for (int i = 0; i < 3; ++i)
                to[i] = pl.pos[i] + sign * g_off[i];
            // 25 m past the line in the other game: travelling on, never straight back over it
            const float otherLine = lcs ? g_border + g_off[0] : g_border;
            // (LCS -> VCS the player goes on east, +x; VCS -> LCS west)
            to[0] = lcs ? std::max(pl.pos[0] + g_off[0], otherLine + 25.0f) : std::min(pl.pos[0] - g_off[0], otherLine - 25.0f);
            // [guard] a vehicle on the water (a boat, a seaplane: within 3 m of this city's sea) keeps its height above
            // the sea, not above zero: the seas differ (boats float at z ~1 in LCS, ~6.6 in VCS: publishView's seaZ).
            // Kept as it was, a Speeder arrived 5.8 m under Vice City's sea (28.7 -> 8.5 m/s at +0.1 s, surfacing for
            // 2 s) and dropped 5 m into Liberty City's (27.2 -> 14.6 m/s at +1 s): guard's vel.py, rows 5 / 6
            {
                const float mySea = lcs ? 1.0f : 6.6f, theirSea = lcs ? 6.6f : 1.0f;
                if (pl.veh && std::fabs(pl.pos[2] - mySea) < 3.0f)
                    to[2] = pl.pos[2] - mySea + theirSea;
            }
            to[2] = std::max(to[2], 0.5f) + 1.0f;
            g_shm->hoTo = other;
            std::memcpy(g_shm->hoPos, to, sizeof(to));
            g_shm->hoHeading = std::fmod(std::atan2(-pl.fwd[0], pl.fwd[1]) * 57.29578f + 360.0f, 360.0f);
            g_shm->hoSpeed = pl.veh ? pl.speed : 0.0f;
            for (int i = 0; i < 3; ++i) // [parity] the whole motion (both worlds have the same axes)
            {
                g_shm->hoVel[i] = pl.veh ? pl.vel[i] : 0.0f;
                g_shm->hoTurn[i] = pl.veh ? pl.turn[i] : 0.0f;
            }
            g_shm->hoModel = pl.veh ? mapCar(pl.model) : 0;
            // [parity] no vehicle to arrive in (a model the car map lacks): never on foot in mid-air - at the other game's
            // sea level (LCS ~0, VCS ~6: publishView's seaZ), the motion dropped
            // [bridge4] the road bends (stories_bridge.cpp centerY): on the deck, keep the place across the road, not the
            // same y 25 m further on (that is 10 m off the centre line where the deck is slanting)
            const float lcsXTo = lcs ? to[0] - g_off[0] : to[0], lcsXFrom = lcs ? pl.pos[0] : pl.pos[0] - g_off[0];
            // [farsea] heights in LCS terms (the offset's z lifts Vice City's: its deck at the border is 13 + the offset)
            const float zFromL = lcs ? pl.pos[2] : pl.pos[2] - g_off[2];
            const bool wasOnDeck = std::fabs(pl.pos[1] - bridge::centerY(lcsXFrom)) < 9.0f && zFromL < 20.0f && zFromL > 8.0f;
            if (wasOnDeck)
                to[1] = bridge::centerY(lcsXTo) + (pl.pos[1] - bridge::centerY(lcsXFrom));
            const bool onDeck = std::fabs(to[1] - bridge::centerY(lcsXTo)) < 12.0f && (lcs ? to[2] - g_off[2] : to[2]) < 20.0f; // stories_bridge.cpp, 14 m wide, z 13
            if (pl.veh && !g_shm->hoModel && !onDeck && to[2] > (lcs ? 6.0f : 0.0f) + 4.0f)
            {
                std::fprintf(stderr, "[dualworld] no vehicle to arrive in at %.0f m: at sea level\n", to[2]);
                to[2] = (lcs ? 6.0f : 0.0f) + 1.0f;
                std::memcpy(g_shm->hoPos, to, sizeof(to));
            }
            g_shm->hoMoney = pl.money;
            g_shm->hoHealth = pl.health;
            g_shm->hoArmour = pl.armour;
            g_shm->hoCurrent = 0;
            // the weapon "in hand": in a vehicle the one held on foot before getting in (the game put its drive-by weapon
            // in the hand; the arriving game gives that back once the player gets out: restoreWeapon), if still had
            int hand = pl.currentSlot >= 0 && pl.currentSlot < 10 ? pl.weapons[pl.currentSlot][0] : 0;
            for (int s = 0; s < 10 && pl.veh && g_onFootWeapon; ++s)
                if (pl.weapons[s][0] == g_onFootWeapon && pl.weapons[s][1] > 0)
                    hand = g_onFootWeapon;
            for (int s = 0; s < 10; ++s)
            {
                const int t = mapWeapon(lcs, pl.weapons[s][0]);
                g_shm->hoWeapons[s][0] = t;
                g_shm->hoWeapons[s][1] = pl.weapons[s][1];
                if (pl.weapons[s][0] == hand && hand)
                    g_shm->hoCurrent = t;
            }
            // what the player leaves behind: a mission fails, the wanted level is gone (crossing counts as an escape)
            failMission(ram);
            if (lcs)
                lcs::trainer::setWanted(0);
            else
                vcs::trainer::setWanted(0);
            const uint32_t seq = g_shm->hoSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_hoApplied = seq; // ours: not taken back by us
            g_cooldownUntil = now + g_cooldownNs;
            g_blackUntil = now + 600000000ll; // the window stays up ~250 ms more (onHostFrame): black
            platform::allowOtherForeground(); // the shown window changes process (Windows: a background process may not
                                              // take the foreground unless the foreground one allows it)
            g_shm->frontGame.store(other, std::memory_order_release);
            std::fprintf(stderr, "[dualworld] handed over at (%.0f, %.0f, %.0f) (%s model %d -> %d, %.0f m/s, velocity %.1f %.1f %.1f, "
                                 "turn %.4f %.4f %.4f, $%d)\n",
                         pl.pos[0], pl.pos[1], pl.pos[2], pl.veh ? "vehicle" : "on foot", pl.model, g_shm->hoModel, g_shm->hoSpeed,
                         pl.vel[0], pl.vel[1], pl.vel[2], pl.turn[0], pl.turn[1], pl.turn[2], pl.money);
        }

        // [parity] the arriving vehicle keeps its motion: the arrival script makes it with SET_CAR_FORWARD_SPEED (level,
        // along the heading); once the player sits in it (a vehicle other than the one held here while behind), its move
        // and turn speed are set to the crossing's for its first frames (kVelFrames) - a climbing or falling helicopter,
        // a car in a turn. Given up after 5 s (no vehicle came: on foot, or the arrival failed).
        constexpr int kVelFrames = 4;
        struct VelPending
        {
            bool on = false;
            float vel[3]{}, turn[3]{};
            uint32_t oldVeh = 0, veh = 0;
            int frames = 0;
            int64_t since = 0;
        } g_vel;

        void applyArrivalVelocity(uint8_t *ram, int64_t now)
        {
            if (!g_vel.on)
                return;
            if (now - g_vel.since > 5000000000ll) // [guard] 5 s: the arrival may wait ~2 s for a vehicle pool slot
            {
                g_vel.on = false;
                std::fprintf(stderr, "[dualworld] arrival velocity: no vehicle in 5 s\n");
                return;
            }
            const Player pl = readPlayer(ram);
            if (!pl.ok || !pl.veh || pl.veh == g_vel.oldVeh || (g_vel.veh && pl.veh != g_vel.veh))
                return;
            const bool lcs = game::isLcs();
            if (!g_vel.veh)
            {
                g_vel.veh = pl.veh;
                std::fprintf(stderr, "[dualworld] arrival velocity: in the vehicle (model %d, %.1f m/s level as made), set to %.1f %.1f %.1f\n",
                             pl.model, pl.speed, g_vel.vel[0], g_vel.vel[1], g_vel.vel[2]);
            }
            const uint32_t ms = pl.veh + (lcs ? 0x70u : 0x140u), ts = pl.veh + (lcs ? 0x80u : 0x70u);
            for (int i = 0; i < 3; ++i)
            {
                wrf(ram, ms + 4u * i, g_vel.vel[i] / 50.0f);
                wrf(ram, ts + 4u * i, g_vel.turn[i]);
            }
            // VCS helicopters: the rotor at full speed, or a helicopter made in mid-air drops (user report: "my helicopter
            // stopped and I fell into the water"). The rotor's speed is CVehicle +0x700 (game-internals.md: the blade test
            // cuts it to 0.15); a spawned Maverick starts at 0.187, its top is 0.22 (read in a run, stress test notes).
            // Models: tools/modslots/mod_slots.txt's VCS heli class.
            static const int kVcsHelis[] = {173, 188, 189, 213, 260, 261, 262, 263, 275};
            if (!lcs && std::find(std::begin(kVcsHelis), std::end(kVcsHelis), pl.model) != std::end(kVcsHelis))
                wrf(ram, pl.veh + 0x700u, 0.22f);
            // [guard] LCS helicopters likewise: LCS's own (Hunter 213, Maverick 214, Police / VCN Maverick 215 / 216:
            // tools/modslots/mod_slots.txt) or a VCS one bound to an LCS model id (lcs_crossover.h). The rotor's speed is
            // CAutomobile +0x670: CAutomobile::ProcessControl 0x128408 ramps it by 0.003 / 0.001 a step up to 0.22
            // (0x12C398..0x12C408: lwc1 0x670($s4) / c.lt.s 0.22 / swc1) and gives lift above 0.15 (0x12C410); a
            // helicopter made at the arrival starts at 0 and drops.
            if (lcs)
            {
                const lcs::crossover::Vehicle *v = lcs::crossover::find(lcs::crossover::boundOther(pl.model));
                if (v ? v->cls == "heli" : (pl.model >= 213 && pl.model <= 216))
                    wrf(ram, pl.veh + 0x670u, 0.22f);
            }
            if (++g_vel.frames >= kVelFrames)
                g_vel.on = false;
        }

        // the new front game: the player put where the other game's player crossed
        void takeHandover(const uint8_t *ram, int64_t now)
        {
            const uint32_t seq = g_shm->hoSeq.load(std::memory_order_acquire);
            if (seq == g_hoApplied || g_shm->hoTo != g_me)
                return;
            if (!gameReady())
                return; // next frame
            {
                // [bridge2] this game's player dying or dead (left here dead earlier: the game respawns them at a
                // hospital first): the arrival waits for the respawn, else its warp into the car is lost (hidden pair
                // run: "no vehicle in 3 s", the player on foot at the hospital). LCS state +0x34C 0x36 / 0x37, VCS +0x8C4
                // 0x39 / 0x3A (lcs_trainer.cpp / vcs_trainer.cpp alive())
                const bool lcs = game::isLcs();
                const uint32_t ped = rd32(ram, lcs ? 0x00408E40u : 0x004E4910u);
                const uint32_t st = ped && ped < 0x02000000u ? rd32(ram, ped + (lcs ? 0x34Cu : 0x8C4u)) : 0u;
                if (!ped || ped >= 0x02000000u || (lcs ? (st == 0x36u || st == 0x37u) : (st == 0x39u || st == 0x3Au)))
                {
                    static int64_t told = 0;
                    if (now - told > 2000000000ll)
                    {
                        told = now;
                        std::fprintf(stderr, "[dualworld] arrival: this game's player is dead / respawning: waiting\n");
                    }
                    return;
                }
            }
            g_hoApplied = seq;
            const float *p = g_shm->hoPos;
            const int model = g_shm->hoModel;
            if (game::isLcs())
            {
                lcs::trainer::arrive(p[0], p[1], p[2], g_shm->hoHeading, model, g_shm->hoSpeed);
                lcs::trainer::carry(g_shm->hoMoney, g_shm->hoHealth);
                lcs::trainer::carryKit(g_shm->hoArmour, g_shm->hoWeapons, 10, g_shm->hoCurrent);
            }
            else
            {
                vcs::trainer::arrive(p[0], p[1], p[2], g_shm->hoHeading, model, g_shm->hoSpeed, false);
                vcs::trainer::carry(g_shm->hoMoney, g_shm->hoHealth);
                vcs::trainer::carryKit(g_shm->hoArmour, g_shm->hoWeapons, 10, g_shm->hoCurrent);
            }
            g_cooldownUntil = now + g_cooldownNs;
            g_lastValid = false;
            g_restoreWeapon = model ? g_shm->hoCurrent : 0;
            g_vel = {};
            if (model) // [parity] the crossing's motion, for the vehicle the arrival makes
            {
                g_vel.on = true;
                std::memcpy(g_vel.vel, g_shm->hoVel, sizeof(g_vel.vel));
                std::memcpy(g_vel.turn, g_shm->hoTurn, sizeof(g_vel.turn));
                g_vel.oldVeh = readPlayer(ram).veh;
                g_vel.since = now;
            }
            g_onFootWeapon = g_shm->hoCurrent; // what this player held on foot, until seen on foot here
            g_restoreSeenIn = false;
            g_restoreOutSince = 0;
            if (g_fadeHold.exchange(false))
                g_fadeInStart = now + 150000000ll; // a moment at black: the arrival's placement and streaming settle
            std::fprintf(stderr, "[dualworld] took the player at (%.0f, %.0f, %.0f), model %d, %.0f m/s\n", p[0], p[1], p[2], model,
                         g_shm->hoSpeed);
        }

        // ---- The weather: the background game's follows the front game's -------------------------------------------
        // CWeather's types (s16): Old / New, blended by the minute (Interpolation = (minutes + seconds / 60) / 60: the
        // clocks are in step already), and Forced (-1: none; when set, New takes it at the next hour). LCS: Old 0x3D9CAA,
        // New 0x3D9CAC, Forced 0x3DA190 (FORCE_WEATHER_NOW 0x325728 writes all three, RELEASE_WEATHER 0x325748 Forced =
        // -1; CWeather::Update 0x324C50 copies New to Old at the hour). VCS: Old 0x4CD29E, New 0x4CD2A0, Forced 0x4CD608
        // (CWeather::Update 0x3E3720, the same code). The background game gets the front's Old / New mapped to its
        // nearest types each frame, and Forced = New (its own weather list never runs); released when it comes to the
        // front. STORIES_DW_WEATHER=0: left alone.
        constexpr uint32_t kLcsWxOld = 0x003D9CAAu, kLcsWxNew = 0x003D9CACu, kLcsWxForced = 0x003DA190u;
        constexpr uint32_t kVcsWxOld = 0x004CD29Eu, kVcsWxNew = 0x004CD2A0u, kVcsWxForced = 0x004CD608u;
        bool g_weather = true;
        bool g_weatherForced = false;

        // LCS: 0 sunny, 1 cloudy, 2 rainy, 3 foggy, 4 extra sunny, 5 stormy (lcs_trainer kWeatherNames). VCS: the same 0-5
        // (5 hurricane), 6 extra colours (interiors / cutscenes: sunny's look outside), 7 ultra sunny
        int mapWeather(bool fromLcs, int w)
        {
            if (w < 0)
                return 0;
            if (fromLcs)
                return w <= 5 ? w : 0;
            static const int kVcsToLcs[8] = {0, 1, 2, 3, 4, 5, 0, 4};
            return w < 8 ? kVcsToLcs[w] : 0;
        }

        int16_t rd16(const uint8_t *ram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        void wr16(uint8_t *ram, uint32_t a, int16_t v) { std::memcpy(ram + (a & kRamMask), &v, 2); }

        // front: publish; back: follow
        void syncWeather(uint8_t *ram, bool front)
        {
            const bool lcs = game::isLcs();
            const uint32_t aOld = lcs ? kLcsWxOld : kVcsWxOld, aNew = lcs ? kLcsWxNew : kVcsWxNew, aForced = lcs ? kLcsWxForced : kVcsWxForced;
            if (front)
            {
                if (g_weatherForced)
                {
                    g_weatherForced = false; // this game's own weather again (it goes on from the matched one)
                    wr16(ram, aForced, -1);
                }
                g_shm->wxOld = rd16(ram, aOld);
                g_shm->wxNew = rd16(ram, aNew);
                g_shm->wxGame.store(g_me, std::memory_order_release);
                return;
            }
            const uint32_t from = g_shm->wxGame.load(std::memory_order_acquire);
            if (!g_weather || !from || from == g_me)
                return;
            const bool fromLcs = from == kGameLcs;
            const int16_t o = static_cast<int16_t>(mapWeather(fromLcs, g_shm->wxOld));
            const int16_t n = static_cast<int16_t>(mapWeather(fromLcs, g_shm->wxNew));
            if (rd16(ram, aOld) != o || rd16(ram, aNew) != n)
            {
                static int logged = 0;
                if (logged++ < 20)
                    std::fprintf(stderr, "[dualworld] weather: %d -> %d (front's %d -> %d)\n", o, n, g_shm->wxOld, g_shm->wxNew);
            }
            wr16(ram, aOld, o);
            wr16(ram, aNew, n);
            wr16(ram, aForced, n);
            g_weatherForced = true;
        }

        // ---- The aspect ratio, live: the background game's view follows the front game's widening ------------------
        // (the setting changed in the menu, "Match window" and a resized window). The picture's shape is the frame's
        // (640 x 448 PS2 pixels, the view widened by the game's camera, game/vcs_display.h); its size in the shared
        // block is set to the aspect so the pixels stay square: g_sinkW wide (or kMaxH tall)
        float g_backAspect = 0.0f; // the widening applied here as the background game (0: none yet)
        // [parity] the background game's own aspect / HUD setting was just applied (settings sync): its applyAspect set
        // the widening for its hidden window, so the front's is put back over it
        std::atomic<bool> g_aspectDirty{false};

        // [parity] the picture's pixel budget (STORIES_DW_MAX=WxH, default 2560x1440): a front frame larger than this (4K
        // internal resolution) is handed over scaled down to it, the readback's cost (bandwidth) bounded
        uint32_t g_capW = 2560, g_capH = 1440;
        uint32_t g_wantW = 0, g_wantH = 0; // back: the front's rect last sized for

        void fitPicture(uint32_t &w, uint32_t &h)
        {
            double s = 1.0;
            const double pix = double(w) * h, cap = double(g_capW) * g_capH;
            if (pix > cap)
                s = std::sqrt(cap / pix);
            s = std::min({s, double(kMaxW) / w, double(kMaxH) / h});
            if (s < 1.0)
            {
                w = static_cast<uint32_t>(w * s) & ~1u;
                h = static_cast<uint32_t>(h * s) & ~1u;
            }
            w = std::max(w, 64u), h = std::max(h, 64u);
        }

        void syncAspect(bool front)
        {
            if (front)
            {
                const float r = vcs::aspectWidening();
                uint32_t bits;
                std::memcpy(&bits, &r, 4);
                g_shm->aspectBits.store(bits, std::memory_order_relaxed);
                g_backAspect = 0.0f;
                g_wantW = g_wantH = 0;
                const gsdw::Stats st = gsdw::stats();
                if (st.rectW >= 64 && st.rectH >= 64)
                {
                    g_shm->wantW.store(st.rectW, std::memory_order_relaxed);
                    g_shm->wantH.store(st.rectH, std::memory_order_relaxed);
                }
                return;
            }
            const uint32_t bits = g_shm->aspectBits.load(std::memory_order_relaxed);
            float r;
            std::memcpy(&r, &bits, 4);
            const bool dirty = g_aspectDirty.exchange(false);
            const uint32_t ww = g_shm->wantW.load(std::memory_order_relaxed), wh = g_shm->wantH.load(std::memory_order_relaxed);
            const bool wantChanged = ww != g_wantW || wh != g_wantH;
            if (!bits || !(r >= 0.5f && r <= 3.0f) || (std::fabs(r - g_backAspect) < 1e-3f && !dirty && !wantChanged))
                return;
            g_wantW = ww, g_wantH = wh;
            const bool first = g_backAspect == 0.0f && !dirty;
            g_backAspect = r;
            if (!first || std::fabs(r - vcs::aspectWidening()) > 1e-3f)
            {
                vcs::setAspectWidening(r);
                gsHwSetAspectWidening(r);
                present::setDisplayAspect(r * (4.0f / 3.0f));
            }
            const float aspect = r * (4.0f / 3.0f);
            uint32_t w = g_sinkW, h = static_cast<uint32_t>(std::lround(g_sinkW / aspect / 2.0f)) * 2u;
            if (ww >= 64 && wh >= 64)
                w = ww, h = wh; // [parity] the front's frame size: drawn there 1:1
            fitPicture(w, h);
            if (w != g_backW || h != g_backH)
            {
                g_backW = w, g_backH = h;
                gsdw::setFrameSink(&sink, w, h);
            }
            std::fprintf(stderr, "[dualworld] aspect: the front's %.3f (widening %.3f), its frame %ux%u, picture %ux%u\n", aspect,
                         r, ww, wh, w, h);
        }

        // front: the view for the depth-correct composite (gsdw::View): the frame's camera, its view's half-angles and the
        // border plane. The CCam FOV is the 4:3 picture's horizontal angle (tan 35 deg * 4/3 = 0.9335 = 320 / fx, the focal
        // length read from LCS's native draws at 16:9: fx 342.8, fy 426.5 PS2 pixels), widened as vcs_display does: Hor+
        // (r >= 1) widens across, Vert+ (r < 1) up and down. STORIES_DW_DEPTH=0 (renderer): the depth-clear composite only.
        float g_depthBlend = 150.0f; // STORIES_DW_SEAM=m: the two seas blended over this much past the border (0: a hard line)
        std::atomic<bool> g_depth{true}; // `dualworld depth 0|1` (tests: the band with and without)

        // the view's unit front and its right / up scaled by tan(half the view) (as the frame's projection), false: no view
        bool viewBasis(const float *front, const float *up, float fov, float *f, float *right, float *upOut)
        {
            for (int i = 0; i < 3; ++i)
                f[i] = front[i];
            const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (!(fl > 0.5f && fl < 2.0f) || !(fov >= 10.0f && fov <= 130.0f))
                return false;
            for (int i = 0; i < 3; ++i)
                f[i] /= fl;
            float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]}; // front x up
            const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (!(rl > 1e-3f))
                return false;
            for (float &c : r)
                c /= rl;
            const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]}; // right x front
            const float t = std::tan(fov * 0.5f * 0.01745329f), w = vcs::aspectWidening();
            const float tx = w >= 1.0f ? t * w : t, ty = w >= 1.0f ? t * 0.75f : t * 0.75f / std::max(w, 0.1f);
            for (int i = 0; i < 3; ++i)
            {
                right[i] = r[i] * tx;
                upOut[i] = u[i] * ty;
            }
            return true;
        }

        void publishView(const float *pos, const float *front, const float *up, float fov)
        {
            gsdw::View v;
            if (!viewBasis(front, up, fov, v.front, v.right, v.up))
                return gsdw::setView(v);
            const bool lcs = game::isLcs();
            v.on = true;
            v.depth = g_depth.load(std::memory_order_relaxed);
            v.reproject = g_reproject.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; ++i)
                v.cam[i] = pos[i];
            v.borderX = lcs ? g_border : g_border + g_off[0];
            v.side = lcs ? 1.0f : -1.0f; // LCS in front: Vice City at +x (offset -3800: it is at LCS x + 3800)
            v.seaZ = lcs ? 0.0f : 6.0f; // this game's sea (boats measured floating at z ~1 in LCS, ~6.6 in VCS)
            v.blend = g_depthBlend;
            gsdw::setView(v);
        }

        // the fade's amount now (game thread each game frame and main thread each host frame: smooth at any frame rate)
        void updateFade(int64_t now)
        {
            float a = 0.0f;
            if (g_fadeNs > 0)
            {
                if (g_fadeHold.load(std::memory_order_relaxed) || now < g_blackUntil.load(std::memory_order_relaxed))
                    a = 1.0f;
                else if (const int64_t out = g_fadeOutStart.load(std::memory_order_relaxed))
                    a = std::min(1.0f, static_cast<float>(now - out) / static_cast<float>(g_fadeNs));
                else if (const int64_t in = g_fadeInStart.load(std::memory_order_relaxed))
                {
                    a = 1.0f - std::clamp(static_cast<float>(now - in) / static_cast<float>(g_fadeNs), 0.0f, 1.0f);
                    if (a <= 0.0f)
                        g_fadeInStart.store(0, std::memory_order_relaxed);
                }
            }
            g_fadeNow.store(a, std::memory_order_relaxed);
            gsdw::setFade(a);
        }

        // front: after a crossing in a vehicle, the weapon carried across back in hand once the player is out of it (the
        // game chose the drive-by weapon on the way in). 0.5 s after getting out: the exit animation done
        void restoreWeapon(const uint8_t *ram, int64_t now)
        {
            if (!g_restoreWeapon)
                return;
            const Player pl = readPlayer(ram);
            if (!pl.ok)
                return;
            if (pl.veh)
            {
                g_restoreSeenIn = true;
                g_restoreOutSince = 0;
                return;
            }
            if (!g_restoreSeenIn)
                return; // not in it yet (the arrival is queued)
            if (!g_restoreOutSince)
                g_restoreOutSince = now;
            if (now - g_restoreOutSince < 500000000ll)
                return;
            const int want = g_restoreWeapon;
            g_restoreWeapon = 0;
            const int inHand = pl.currentSlot >= 0 && pl.currentSlot < 10 ? pl.weapons[pl.currentSlot][0] : 0;
            bool has = false;
            for (int s = 0; s < 10; ++s)
                has = has || (pl.weapons[s][0] == want && pl.weapons[s][1] > 0);
            if (inHand == want || !has)
                return;
            if (game::isLcs())
                lcs::trainer::selectWeapon(want);
            else
                vcs::trainer::selectWeapon(want);
            std::fprintf(stderr, "[dualworld] out of the vehicle: weapon %d back in hand (was %d)\n", want, inHand);
        }

        // back: the player out of play - a vehicle held still where it stopped (its driver has no input: the window is
        // hidden)
        // [robust] ... and kept alive: the health (ped LCS +0x4B8, VCS +0x4E4) and the vehicle's (LCS +0x268,
        // lcs_trainer.cpp kCarHealth; VCS +0x278) held at what they were when the game went behind. Toni left in a car
        // he drove into the sea drowned in 5 s (health 95 -> 0, WASTED) and was at the hospital when the player came
        // back over the line (hidden pair, 2026-11-19). Not for a player already dying (LCS state 0x36 / 0x37, VCS
        // 0x39 / 0x3A): the game finishes that as usual.
        float g_heldHealth = -1.0f, g_heldCarHealth = -1.0f;
        uint32_t g_heldPed = 0, g_heldCar = 0;
        void holdAlive(uint8_t *ram, const Player &pl)
        {
            const bool lcs = game::isLcs();
            const uint32_t state = rd32(ram, pl.ped + (lcs ? 0x34Cu : 0x8C4u));
            if (lcs ? (state == 0x36u || state == 0x37u) : (state == 0x39u || state == 0x3Au))
                return;
            const uint32_t hp = pl.ped + (lcs ? 0x4B8u : 0x4E4u);
            if (pl.ped != g_heldPed)
                g_heldPed = pl.ped, g_heldHealth = std::max(rdf(ram, hp), 1.0f);
            if (rdf(ram, hp) < g_heldHealth)
                wrf(ram, hp, g_heldHealth);
            if (!pl.veh)
                return;
            const uint32_t ch = pl.veh + (lcs ? 0x268u : 0x278u);
            if (pl.veh != g_heldCar)
                g_heldCar = pl.veh, g_heldCarHealth = std::max(rdf(ram, ch), 300.0f);
            if (rdf(ram, ch) < g_heldCarHealth)
                wrf(ram, ch, g_heldCarHealth);
        }

        void park(uint8_t *ram)
        {
            const Player pl = readPlayer(ram);
            if (!pl.ok)
                return;
            holdAlive(ram, pl);
            if (!pl.veh)
                return;
            const uint32_t ms = pl.veh + (game::isLcs() ? 0x70u : 0x140u);
            for (int i = 0; i < 3; ++i)
                wrf(ram, ms + 4u * i, 0.0f);
        }

        void parseFloats(const char *s, float *out, int n)
        {
            for (int i = 0; i < n && s && *s; ++i)
            {
                char *end = nullptr;
                out[i] = std::strtof(s, &end);
                s = end && *end == ',' ? end + 1 : end;
            }
        }

        void logState(const char *why)
        {
            const gsdw::Stats st = gsdw::stats();
            std::fprintf(stderr, "[dualworld] %s: role %s cam writes %llu skips %llu, front frames %llu back frames %llu, composed %llu (%u us) sunk %llu (%u us)\n",
                         why, g_role == Role::Front ? "front" : "back", (unsigned long long)g_camWrites.load(),
                         (unsigned long long)g_camSkips.load(), g_shm ? (unsigned long long)g_shm->frontFrames.load() : 0ull,
                         g_shm ? (unsigned long long)g_shm->backFrames.load() : 0ull, (unsigned long long)st.composed, st.composeUs,
                         (unsigned long long)st.sunk, st.sinkUs);
        }

        // ---- [parity] One set of settings: the front game's F2 changes reach the other game live -------------------
        // Main thread (onHostFrame), as the F2 menu. Each host frame:
        // 1. the records the other game wrote since the last look are applied here with settings::set - the menu's own
        //    path (live settings at once; restart ones saved for the next start, the menu says so). An Integer is
        //    clamped to this game's range (LCS's draw distance goes to 1000%, VCS's to 300%); a Choice this game lacks is
        //    skipped; the writer's default becomes this game's default. Keys this game doesn't have (LCS's colour filter
        //    resolution, far fog, user tracks ...) or locks for the run (VCS's drive-by rate in LCS) are left alone.
        // 2. in front: every setting whose value changed here since the last look (the F2 menu, Alt+Enter, a preset, a
        //    hotkey) is written as a record. Only changes travel, never a whole snapshot: a clamped value (the 300%)
        //    never comes back to overwrite the other game's 1000%.
        // Applied in either role, before this game publishes: a change made just before a swap is not undone by it.
        // The start: tools/stories.py gives the background game the front's settings in a generated vcs.ini
        // (VCS_SETTINGS_INI), so both start alike; this keeps them so.
        // The background game's draw distance is at least 300% (or its maximum) while behind - the far city as seen
        // from across the sea, as before - and its own (the synced) value again in front.
        constexpr uint32_t kSetRecs = 64;
        constexpr int kHorizonDraw = 300;
        std::unordered_map<std::string, int> g_setSnap; // main thread: each setting as last seen / published here
        uint32_t g_setSeen = 0;
        bool g_setInit = false;
        int g_setHostRole = 0; // the role the host-side settings were last applied for (1 front, 2 back)
        std::atomic<uint64_t> g_setPublished{0}, g_setApplied{0}, g_setRefused{0};

        const settings::Spec *findSpec(const std::string &key)
        {
            for (const settings::Spec &s : settings::all())
                if (s.key == key)
                    return &s;
            return nullptr;
        }

        bool lockedHere(const std::string &key)
        {
            static const std::vector<std::string> locked = settings::lockedForRun();
            return std::find(locked.begin(), locked.end(), key) != locked.end();
        }

        // the draw distance's apply path with a value of its own (the saved value unchanged): the horizon's floor behind,
        // the setting in front
        void applyDrawDistance(bool back)
        {
            const settings::Spec *s = findSpec("enhance.draw_distance");
            if (!s || !s->onChange)
                return;
            const int v = settings::value(s->key);
            s->onChange(back ? std::max(v, std::min(kHorizonDraw, s->max)) : v);
            if (game::isLcs()) // [tone] LCS reads the setting itself each frame (lcs_drawdist.cpp): the floor goes there
                lcs::drawdist::setFloor(back ? kHorizonDraw : 0);
        }

        void publishSetting(const std::string &key, int v, bool isDefault)
        {
            uint32_t expected = 0;
            for (int spin = 0; !g_shm->setLock.compare_exchange_weak(expected, 1u, std::memory_order_acquire); ++spin)
            {
                expected = 0;
                if (spin > 100000)
                    return; // the other process died holding it: drop this one record rather than hang
            }
            const uint32_t i = g_shm->setCount.load(std::memory_order_relaxed);
            Shared::SetRec &r = g_shm->setRecs[i % kSetRecs];
            r.seq.store(0, std::memory_order_relaxed);
            r.game = g_me;
            r.value = v;
            r.isDefault = isDefault ? 1 : 0;
            std::snprintf(r.key, sizeof(r.key), "%s", key.c_str());
            r.seq.store(i + 1, std::memory_order_release);
            g_shm->setCount.store(i + 1, std::memory_order_release);
            g_shm->setLock.store(0, std::memory_order_release);
            if (g_setSeen == i)
                g_setSeen = i + 1; // ours: nothing to apply (one of the other's written meanwhile is read next frame)
            g_setPublished.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[dualworld] setting %s = %d sent to the other city (version %u)\n", key.c_str(), v, i + 1);
        }

        void applyRecord(const Shared::SetRec &r)
        {
            const std::string key(r.key, strnlen(r.key, sizeof(r.key)));
            const settings::Spec *s = findSpec(key);
            if (!s || s->type == settings::Type::Action || lockedHere(key))
                return;
            int v = r.isDefault ? s->defaultValue : r.value;
            if (s->type == settings::Type::Integer)
                v = std::clamp(v, s->min, s->max);
            const int before = settings::value(key);
            if (v != before && !settings::set(key, v))
            {
                g_setRefused.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[dualworld] setting %s = %d from the other city not taken here (%s)\n", key.c_str(), v,
                             settings::overridden(key) ? "set by its environment variable" : "not valid here");
            }
            else if (v != before)
            {
                g_setApplied.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[dualworld] setting %s = %d from the other city (was %d)%s\n", key.c_str(), v, before,
                             s->apply == settings::Apply::Restart ? ", applies at the next start" : "");
                if (key == "display.aspect" || key == "display.hud")
                    g_aspectDirty = true;
                if (key == "enhance.draw_distance" && g_role == Role::Back)
                    applyDrawDistance(true);
            }
            g_setSnap[key] = settings::value(key);
        }

        void syncSettings()
        {
            if (!g_setInit)
            {
                g_setInit = true;
                for (const settings::Spec &s : settings::all())
                    if (s.type != settings::Type::Action)
                        g_setSnap[s.key] = settings::value(s.key);
                g_setSeen = g_shm->setCount.load(std::memory_order_acquire); // a restarted game: the start's ini has them
            }
            // 1. the other game's records
            const uint32_t count = g_shm->setCount.load(std::memory_order_acquire);
            if (count - g_setSeen > kSetRecs)
                g_setSeen = count - kSetRecs; // fell behind (64 changes in one frame): the newest ones
            for (; g_setSeen != count; ++g_setSeen)
            {
                const Shared::SetRec &r = g_shm->setRecs[g_setSeen % kSetRecs];
                if (r.seq.load(std::memory_order_acquire) != g_setSeen + 1)
                    break; // being written: next frame
                if (r.game != g_me)
                    applyRecord(r);
            }
            // the role's own draw distance (the saved value stays)
            const int hostRole = g_role == Role::Front ? 1 : 2;
            if (hostRole != g_setHostRole)
            {
                if (g_setHostRole != 0 || hostRole == 2)
                    applyDrawDistance(hostRole == 2);
                g_setHostRole = hostRole;
            }
            // 2. in front: this game's own changes
            if (g_role != Role::Front)
                return;
            for (const settings::Spec &s : settings::all())
            {
                if (s.type == settings::Type::Action)
                    continue;
                const int v = settings::value(s.key);
                auto it = g_setSnap.find(s.key);
                if (it == g_setSnap.end() || it->second == v)
                    continue;
                it->second = v;
                if (!lockedHere(s.key))
                    publishSetting(s.key, v, v == s.defaultValue);
            }
        }
    }

    Role role() { return g_role; }

    void offset(float out[3])
    {
        for (int i = 0; i < 3; ++i)
            out[i] = g_off[i];
    }

    namespace
    {
        // [parity] The foreground at a crossing (user report: "the game loses focus and I see my Windows cursor; I need
        // to Alt+Tab back"). Windows lets only the foreground process give the foreground away: the game leaving the
        // front (it has the focus while the player plays it) waits for the other game's window to be shown, puts that
        // window in the foreground (SetForegroundWindow; AttachThreadInput when refused), and only then hides its own.
        // The arriving game captures the mouse again as on any focus gain (platform: capture follows the focus). Not
        // when another program has the foreground (the player switched away): nothing is taken from it.
        uint64_t otherWindow()
        {
            return g_shm->window[g_me == kGameLcs ? kGameVcs : kGameLcs].load(std::memory_order_acquire);
        }

        bool otherWindowShown()
        {
#if defined(_WIN32)
            const HWND w = reinterpret_cast<HWND>(static_cast<uintptr_t>(otherWindow()));
            return w && IsWindow(w) && IsWindowVisible(w);
#else
            return false;
#endif
        }

        void giveForeground()
        {
            if (platform::hosted())
                return; // [stories] one frame for both games: its foreground stays; the keyboard focus moves with the child shown
#if defined(_WIN32)
            const HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(otherWindow()));
            const HWND mine = static_cast<HWND>(platform::nativeWindow());
            const HWND fg = GetForegroundWindow();
            if (!target || !IsWindow(target))
            {
                std::fprintf(stderr, "[dualworld] foreground: no window of the other game\n");
                return;
            }
            if (fg != mine)
            {
                std::fprintf(stderr, "[dualworld] foreground: another program has it (%p, not ours %p): left alone\n",
                             static_cast<void *>(fg), static_cast<void *>(mine));
                return;
            }
            DWORD pid = 0;
            const DWORD targetThread = GetWindowThreadProcessId(target, &pid);
            AllowSetForegroundWindow(pid);
            bool ok = SetForegroundWindow(target) && GetForegroundWindow() == target;
            const char *how = "SetForegroundWindow";
            if (!ok)
            {
                // the input queues joined for a moment: a foreground change within one input state is always allowed
                const DWORD me = GetCurrentThreadId();
                const bool attached = AttachThreadInput(me, targetThread, TRUE) != 0;
                BringWindowToTop(target);
                SetForegroundWindow(target);
                SetFocus(target);
                if (attached)
                    AttachThreadInput(me, targetThread, FALSE);
                ok = GetForegroundWindow() == target;
                how = "AttachThreadInput";
            }
            std::fprintf(stderr, "[dualworld] foreground: handed to the other game's window (%s): %s\n", how, ok ? "ok" : "REFUSED");
#endif
        }

        // [parity] the other game stopped (crashed, or closed by itself): seen from its host frames (aliveNs). In front:
        // a note, crossing off until it is back (tools/stories.py restarts it). Behind: the front is gone, so this game
        // takes the front - its window shown, the player where they crossed over (held there since).
        bool g_otherSeen = false, g_otherGoneNoted = false;

        void watchOther(int64_t now)
        {
            const uint32_t other = g_me == kGameLcs ? kGameVcs : kGameLcs;
            if (otherAlive(other, now))
            {
                g_otherSeen = true;
                g_otherGoneNoted = false;
                return;
            }
            if (!g_otherSeen || g_otherGoneNoted)
                return;
            g_otherGoneNoted = true;
            char text[160];
            if (g_role == Role::Front)
            {
                std::snprintf(text, sizeof(text), "%s stopped: %s goes on alone, no crossing until it is back", cityName(other),
                              cityName(g_me));
                note(4, text, now);
                return;
            }
            if (g_shm->frontGame.load(std::memory_order_acquire) == other)
            {
                g_shm->frontGame.store(g_me, std::memory_order_release); // onGameFrame takes the front role
                std::snprintf(text, sizeof(text), "%s stopped: back in %s, where you crossed over", cityName(other), cityName(g_me));
                note(5, text, now);
            }
        }
    }

    void onHostFrame()
    {
        if (!g_enabled || !g_shm)
            return;
        {
            const int64_t now = nowNs();
            g_shm->aliveNs[g_me].store(now, std::memory_order_relaxed);
            g_shm->window[g_me].store(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(platform::nativeWindow())),
                                      std::memory_order_release);
            syncSettings(); // [parity] the F2 changes, both ways (the front's drive)
            watchOther(now);
        }
        updateFade(nowNs());
        if (!g_windows)
        {
            // [parity] hidden runs never move the foreground: the handoff's inputs logged once per role change instead
            static Role logged = Role::Off;
            if (g_role != logged)
            {
                logged = g_role;
#if defined(_WIN32)
                std::fprintf(stderr, "[dualworld] foreground (hidden run, left alone): this window %p, the other's %p shown %d, "
                                     "the foreground ours %d\n",
                             platform::nativeWindow(), reinterpret_cast<void *>(static_cast<uintptr_t>(otherWindow())),
                             otherWindowShown() ? 1 : 0, GetForegroundWindow() == static_cast<HWND>(platform::nativeWindow()) ? 1 : 0);
#endif
            }
            return;
        }
        // [parity] the window by the shared front, not this game's role: a game whose game thread died never applies a
        // role change, but its main thread still hides its window when the other game takes the front
        const int want = g_shm->frontGame.load(std::memory_order_acquire) == g_me ? 1 : 2;
        static int64_t since = 0, published = 0;
        const int64_t now = nowNs();
        if (want == 1)
        {
            if (g_hostRole.load() != 1)
            {
                // shown where the other game's window was, at its size, full screen or not
                const bool have = g_shm->winSeq.load(std::memory_order_acquire) != 0 && g_shm->winW > 0;
                platform::setWindowShown(true, have ? g_shm->winX : 0, have ? g_shm->winY : 0, have ? g_shm->winW : 0,
                                         have ? g_shm->winH : 0, have && g_shm->winFull);
                g_hostRole = 1;
                std::fprintf(stderr, "[dualworld] window shown\n");
            }
            since = 0;
            if (now - published > 500000000ll && !platform::windowHidden())
            {
                published = now;
                int x = 0, y = 0, w = 0, h = 0;
                platform::windowRect(x, y, w, h);
                g_shm->winX = x, g_shm->winY = y, g_shm->winW = w, g_shm->winH = h;
                g_shm->winFull = platform::fullscreen() ? 1 : 0;
                g_shm->winSeq.fetch_add(1, std::memory_order_release);
            }
            return;
        }
        // background: hidden once the other window is up and has the foreground from us (no flash of the desktop, no
        // focus lost: [parity] giveForeground); at most 1 s waiting for it
        static bool handed = false;
        static int64_t handedAt = 0;
        if (g_hostRole.load() == 1)
        {
            if (!since)
            {
                since = now;
                handed = false;
            }
            if (!handed && (otherWindowShown() || now - since > 1000000000ll))
            {
                handed = true;
                handedAt = now;
                giveForeground();
            }
            if (!handed || now - handedAt < 50000000ll || now - since < 150000000ll)
                return;
        }
        if (g_hostRole.load() != 2)
        {
            if (!platform::windowHidden() || platform::hosted()) // [stories] hosted: hidden inside a hidden frame too
                platform::setWindowShown(false, 0, 0, 0, 0, false);
            g_hostRole = 2;
            since = 0;
            std::fprintf(stderr, "[dualworld] window hidden\n");
        }
        // [parity] the hidden window kept at the shown one's size: an internal resolution of "Match window" renders this
        // game's picture at the front's resolution too (the picture is drawn there 1:1)
        static int32_t sizedW = 0, sizedH = 0;
        if (g_shm->winSeq.load(std::memory_order_acquire) != 0 && g_shm->winW > 0 && g_shm->winH > 0 &&
            (g_shm->winW != sizedW || g_shm->winH != sizedH))
        {
            sizedW = g_shm->winW, sizedH = g_shm->winH;
            platform::setWindowShown(false, 0, 0, sizedW, sizedH, false);
        }
    }

    namespace
    {
        void applyRole(Role r)
        {
            g_role = r;
            g_heldPed = g_heldCar = 0; // [robust] holdAlive takes the values anew
            if (r == Role::Front)
            {
                gsdw::setFrameSink(nullptr, g_sinkW, g_sinkH);
                gsdw::setGpuSink(nullptr);
                gsdw::setBackdropSource(&source);
                gsdw::setGpuSource(g_gpu ? &gpuSource : nullptr);
            }
            else
            {
                gsdw::setBackdropSource(nullptr);
                gsdw::setGpuSource(nullptr);
                gsdw::setFrameSink(&sink, g_backW ? g_backW : g_sinkW, g_backH ? g_backH : g_sinkH);
                gsdw::setGpuSink(g_gpu && g_sinkOn.load() ? &gpuSink : nullptr);
            }
            std::fprintf(stderr, "[dualworld] now %s\n", r == Role::Front ? "in front (shown, played)" : "in the background");
            { // [smooth] the played city before the one drawn behind it for CPU time (VCS_PRIORITY overrides both)
                const char *p = std::getenv("VCS_PRIORITY");
                if (!(p && *p))
                    platform::setProcessPriority(r == Role::Front ? 1 : -1);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *r = std::getenv("STORIES_DUALWORLD");
        if (!r || !*r)
            return;
        const std::string rs(r);
        g_role = rs == "front" ? Role::Front : rs == "back" ? Role::Back : Role::Off;
        if (g_role == Role::Off)
            return;
        if (const char *o = std::getenv("STORIES_DW_OFFSET"))
            parseFloats(o, g_off, 3);
        // [farsea] the seas line up: Vice City's sea is 6 m above Liberty City's (water at z 0 / 6; a boat floats at ~1.0 / ~6.6), so with no vertical offset the far picture's sea showed above the near one's. The offset gets that height
        // (VCS = LCS + offset: the background camera, a crossing's arrival, the bridge deck all follow it).
        // STORIES_DW_SEAMATCH=0 puts it back to 0 (STORIES_DW_OFFSET's own z, if it has one, is always used as given)
        {
            const char *sm = std::getenv("STORIES_DW_SEAMATCH");
            const bool match = !(sm && sm[0] == '0');
            const bool hasZ = g_off[2] != 0.0f;
            if (match && !hasZ)
                g_off[2] = kSeaDelta;
        }
        if (const char *z = std::getenv("STORIES_DW_SIZE"))
        {
            unsigned w = 0, h = 0;
            if (std::sscanf(z, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= kMaxW && h <= kMaxH)
                g_sinkW = w, g_sinkH = h;
        }
        if (const char *mx = std::getenv("STORIES_DW_MAX"))
        {
            unsigned w = 0, h = 0;
            if (std::sscanf(mx, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64)
                g_capW = w, g_capH = h;
        }
        if (const char *c = std::getenv("STORIES_DW_CLOCK"); c && c[0] == '0')
            g_clock = false;
        if (const char *wt = std::getenv("STORIES_DW_WAIT"); wt && wt[0] == '1')
            g_sinkOn = false;
        if (const char *b = std::getenv("STORIES_DW_BORDER"); b && *b)
            g_border = std::strtof(b, nullptr);
        if (const char *f = std::getenv("STORIES_DW_FADE_MS"); f && *f)
            g_fadeNs = std::max(0ll, static_cast<long long>(std::atoll(f))) * 1000000ll;
        if (const char *sm = std::getenv("STORIES_DW_SEAM"); sm && *sm)
            g_depthBlend = std::max(0.0f, std::strtof(sm, nullptr));
        if (const char *rp = std::getenv("STORIES_DW_REPROJECT"); rp && rp[0] == '0') // [dwlag]
            g_reproject = false;
        if (const char *os = std::getenv("STORIES_DW_OVERSCAN"); os && *os)
            g_overscan = std::clamp(std::strtof(os, nullptr), 1.0f, 1.3f);
        if (const char *wx = std::getenv("STORIES_DW_WEATHER"); wx && wx[0] == '0')
            g_weather = false;
        if (const char *h = std::getenv("STORIES_DW_HANDOVER"); h && h[0] == '0')
            g_handover = false;
        // [tone] the far fog as transparency in the picture this game hands over while behind (gs_dualworld.h
        // setFogToAlpha): Liberty City's by default (its haze put the far city on Vice City's horizon as a flat silhouette in
        // LCS's fog colour, dark in rain, storm and dusk); STORIES_DW_FOGALPHA=0 never, =2 Vice City's too
        {
            const char *fa = std::getenv("STORIES_DW_FOGALPHA");
            const int mode = fa && *fa ? std::atoi(fa) : 1;
            g_fogAlpha.store(mode >= 2 || (mode == 1 && game::isLcs()));
            gsdw::setFogToAlpha(g_fogAlpha.load());
        }
        // [stories] in the one-process host the windows are children of its one frame (hidden or not): switching them is
        // what makes the front, in hidden test runs too
        const char *hostHwnd = std::getenv("STORIES_HOST_HWND");
        const bool hosted = hostHwnd && *hostHwnd;
        if (const char *gpu = std::getenv("STORIES_DW_GPU"); hosted && !(gpu && gpu[0] == '0'))
            g_gpu = true; // [stories] one process: the picture handed over on the GPU (gs_dualworld.h setGpuSink)
        if (const char *sy = std::getenv("STORIES_DW_SYNC_MS"); sy && *sy)
            g_syncUs = static_cast<int>(std::strtof(sy, nullptr) * 1000.0f);
        if (const char *sl = std::getenv("STORIES_DW_SYNC_LAG"); sl && *sl)
            g_syncLag = static_cast<uint64_t>(std::max(0, std::atoi(sl)));
        if (const char *wn = std::getenv("STORIES_DW_WINDOWS"); (wn && wn[0] == '0') || (ps2control::hiddenMode() && !hosted))
            g_windows = false;
        if (const char *m = std::getenv("STORIES_DW_CARMAP"))
            for (const char *c = m; *c;)
            {
                int a = 0, b = 0, n = 0;
                if (std::sscanf(c, "%d:%d%n", &a, &b, &n) != 2)
                    break;
                g_carMap.push_back({a, b});
                c += n;
                if (*c == ',')
                    ++c;
            }
        const char *n = std::getenv("STORIES_DW_NAME");
        g_shm = openShared(n && *n ? n : "GTAStoriesDualWorld");
        if (!g_shm)
        {
            std::fprintf(stderr, "[dualworld] shared memory not opened: off\n");
            g_role = Role::Off;
            return;
        }
        if (g_shm->magic != kMagic)
        {
            // first of the pair (a new mapping is zero-filled)
            g_shm->version = kVersion;
            g_shm->latest.store(kSlots);
            g_shm->reading.store(kSlots);
            g_shm->magic = kMagic;
        }
        g_me = game::isLcs() ? kGameLcs : kGameVcs;
        g_enabled = true;
        // [parity] a (re)started game: not ready yet, alive; an old handover (a crash's) not taken; a game started behind
        // never claims the front its earlier run held (the other one takes it: watchOther)
        if (const char *cd = std::getenv("STORIES_DW_COOLDOWN_MS"); cd && *cd)
            g_cooldownNs = std::max(0ll, static_cast<long long>(std::atoll(cd))) * 1000000ll;
        g_shm->ready[g_me].store(0, std::memory_order_release);
        g_shm->aliveNs[g_me].store(nowNs(), std::memory_order_relaxed);
#if defined(_WIN32)
        g_shm->pid[g_me].store(GetCurrentProcessId(), std::memory_order_relaxed);
#elif !defined(__ANDROID__)
        g_shm->pid[g_me].store(static_cast<uint32_t>(getpid()), std::memory_order_relaxed);
#endif
        g_hoApplied = g_shm->hoSeq.load(std::memory_order_acquire);
        if (g_role == Role::Back && g_shm->frontGame.load() == g_me)
            g_shm->frontGame.store(g_me == kGameLcs ? kGameVcs : kGameLcs);
        bridge::install(g_off[0], g_off[1], g_off[2]); // [bridge] the deck between the cities (game/stories_bridge.h)
        if (const char *mb = std::getenv("STORIES_DW_MISSION_BORDER"); mb && std::strcmp(mb, "block") == 0) // [missionborder]
            g_blockBorder = true;
        std::fprintf(stderr, "[dualworld] mission at the border: %s\n", g_blockBorder ? "block (a wall)" : "fail (default)");
        if (const char *mf = std::getenv("STORIES_DW_MISSIONFAIL"); !mf || mf[0] != '0')
        {
            const uint32_t restarting = game::isLcs() ? kLcsRestarting : kVcsRestarting;
            g_restartingNext = runtime.lookupFunction(restarting);
            if (g_restartingNext)
                runtime.replaceFunction(restarting, &restartingHook);
            else
                std::fprintf(stderr, "[dualworld] 0x%x not in the function map: missions are not failed at the border\n", restarting);
        }
        if (const char *op = std::getenv("STORIES_DW_OPENING"); !game::isLcs() && op && op[0] == '0')
        {
            g_launchNext = runtime.lookupFunction(kVcsLaunchMission);
            if (g_launchNext)
                runtime.replaceFunction(kVcsLaunchMission, &launchMissionHook);
        }
        if (g_role == Role::Front)
            g_shm->frontGame.store(g_me);
        applyRole(g_role);
        std::fprintf(stderr, "[dualworld] role %s (%s), offset %.0f %.0f %.0f, picture %ux%u, %zu MB shared\n", r,
                     game::isLcs() ? "LCS" : "VCS", g_off[0], g_off[1], g_off[2], g_sinkW, g_sinkH, sizeof(Shared) >> 20);
        ps2control::addCommand("dualworld", [](const std::string &args, ps2control::Reply reply) {
            if (args.rfind("pause", 0) == 0 || args.rfind("resume", 0) == 0) // the background picture held / handed over
            {
                g_sinkOn.store(args[0] == 'r');
                if (g_role == Role::Back) // [stories] a held picture is held on the GPU too (no new frames into its textures)
                    gsdw::setGpuSink(g_gpu && g_sinkOn.load() ? &gpuSink : nullptr);
                return reply(args[0] == 'r' ? "ok resumed" : "ok paused");
            }
            if (args.rfind("depth ", 0) == 0) // tests: the depth-correct composite off / on
            {
                g_depth.store(args.size() > 6 && args[6] != '0');
                return reply(g_depth.load() ? "ok depth on" : "ok depth off");
            }
            if (args.rfind("reproject ", 0) == 0) // [dwlag] tests: the picture reprojected into the frame's camera, or not
            {
                g_reproject.store(args.size() > 10 && args[10] != '0');
                return reply(g_reproject.load() ? "ok reproject on" : "ok reproject off");
            }
            if (args.rfind("backdrop ", 0) == 0) // [dwlag] tests: the other city shown / left out (masks)
            {
                g_hideBackdrop.store(args.size() > 9 && args[9] == '0');
                return reply(g_hideBackdrop.load() ? "ok backdrop hidden" : "ok backdrop shown");
            }
            if (args.rfind("fogalpha ", 0) == 0) // [tone] tests: the far fog as alpha in the picture handed over, or as drawn
            {
                g_fogAlpha.store(args.size() > 9 && args[9] != '0');
                gsdw::setFogToAlpha(g_fogAlpha.load());
                return reply(g_fogAlpha.load() ? "ok fogalpha on" : "ok fogalpha off");
            }
            if (args.rfind("lagreset", 0) == 0)
            {
                gsdw::statsReset();
                return reply("ok");
            }
            if (args.rfind("lag", 0) == 0) // [dwlag] the composites since `lagreset`: frames behind, the picture's offset
            {
                const gsdw::Stats st = gsdw::stats();
                char buf[560];
                const double n = st.lagN ? static_cast<double>(st.lagN) : 1.0;
                std::snprintf(buf, sizeof(buf),
                              "ok n=%llu lag0=%llu lag1=%llu lag2=%llu lag3=%llu lag4=%llu lag5=%llu lag6=%llu lag7plus=%llu "
                              "mean_lag=%.2f err_px_mean=%.2f err_px_max=%.2f err_y_mean=%.2f view_misses=%llu reproject=%d "
                              "composed=%llu held=%llu lock_fail=%llu no_gpu=%llu",
                              (unsigned long long)st.lagN, (unsigned long long)st.lagHist[0], (unsigned long long)st.lagHist[1],
                              (unsigned long long)st.lagHist[2], (unsigned long long)st.lagHist[3], (unsigned long long)st.lagHist[4],
                              (unsigned long long)st.lagHist[5], (unsigned long long)st.lagHist[6], (unsigned long long)st.lagHist[7],
                              [&] {
                                  double m = 0;
                                  for (int k = 0; k < 8; ++k)
                                      m += k * static_cast<double>(st.lagHist[k]);
                                  return m / n;
                              }(),
                              st.errPxSum / n, st.errPxMax, st.errYSum / n, (unsigned long long)st.viewMisses, g_reproject.load() ? 1 : 0, (unsigned long long)st.composed,
                              (unsigned long long)st.held, (unsigned long long)st.lockFail, (unsigned long long)st.noGpu);
                return reply(buf);
            }
            if (args.rfind("sweep", 0) == 0) // [dwlag] tests: sweep x y z yaw pitch fov ampPitchDeg ampYawDeg periodS seconds
            {
                Sweep sw;
                const int got = std::sscanf(args.c_str() + 5, "%f %f %f %f %f %f %f %f %f", &sw.x, &sw.y, &sw.z, &sw.yaw, &sw.pitch,
                                            &sw.fov, &sw.ampPitch, &sw.ampYaw, &sw.period);
                float secs = 0.0f;
                if (const char *last = std::strrchr(args.c_str(), ' '))
                    secs = std::strtof(last + 1, nullptr);
                if (got != 9 || sw.period <= 0.05f || secs <= 0.0f)
                    return reply("err sweep x y z yaw pitch fov ampPitchDeg ampYawDeg periodS seconds");
                sw.on = true;
                sw.start = nowNs();
                sw.end = sw.start + static_cast<int64_t>(secs * 1e9);
                std::lock_guard<std::mutex> lock(g_sweepMutex);
                g_sweep = sw;
                return reply("ok sweeping");
            }
            if (args.rfind("fakemission", 0) == 0) // tests: "fakemission 1|0": a mission running (the byte held) or not
            {
                const bool on = args.size() > 12 && args[12] == '1';
                g_fakeMission = on ? 1 : 2;
                return reply(on ? "ok mission faked" : "ok mission unfaked");
            }
            if (args.rfind("failmission", 0) == 0) // tests: what a crossing does to a running mission
            {
                g_failQueued = true;
                return reply("ok fail queued");
            }
            if (args.rfind("front", 0) == 0) // this game to the front (the other one goes to the background)
            {
                g_shm->frontGame.store(g_me);
                return reply("ok front requested");
            }
            const gsdw::Stats st = gsdw::stats();
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                          "ok role=%s crossings=%llu refused=%llu set_version=%u set_sent=%llu set_taken=%llu set_refused=%llu fails=%llu front_frames=%llu back_frames=%llu cam_writes=%llu composed=%llu compose_us=%u sunk=%llu sink_us=%u cam=%.1f,%.1f,%.1f aspect=%d area=%u far=%.0f opacity=%.2f fade=%.2f weather=%d,%d widen=%.3f muted=%d armed=%d gpu=%d gpu_seq=%llu gpu_size=%ux%u opening_skipped=%u fogalpha=%d picmiss=%llu picoff=%llu sinktag=%llu gametag=%llu offset=%.1f,%.1f,%.1f",
                          g_role == Role::Front ? "front" : "back", (unsigned long long)g_crossings.load(),
                          (unsigned long long)g_refusals.load(), g_shm->setCount.load(), (unsigned long long)g_setPublished.load(),
                          (unsigned long long)g_setApplied.load(), (unsigned long long)g_setRefused.load(),
                          (unsigned long long)g_failsDone.load(),
                          (unsigned long long)g_shm->frontFrames.load(),
                          (unsigned long long)g_shm->backFrames.load(), (unsigned long long)g_camWrites.load(),
                          (unsigned long long)st.composed, st.composeUs, (unsigned long long)st.sunk, st.sinkUs, g_shm->pos[0],
                          g_shm->pos[1], g_shm->pos[2], settings::value("display.aspect"), g_area.load(), g_farClip.load(),
                          g_opacity.load(), g_fadeNow.load(), g_shm->wxOld, g_shm->wxNew, vcs::aspectWidening(),
                          ps2AudioMuted() ? 1 : 0, g_armed ? 1 : 0, gpuPictureLive() ? 1 : 0,
                          (unsigned long long)g_shm->gpuSeq.load(), g_shm->gpuW, g_shm->gpuH, g_openingSkipped.load(), g_fogAlpha.load() ? 1 : 0,
                          (unsigned long long)g_picMiss.load(), (unsigned long long)g_picOff.load(), (unsigned long long)g_sinkTag.load(),
                          (unsigned long long)g_gameTag.load(), g_off[0], g_off[1], g_off[2]);
            reply(buf);
        });
    }

    void onGameFrame(uint8_t *ram)
    {
        if (!g_enabled || !g_shm)
            return;
        g_gameTag.store(g_tag + 1, std::memory_order_relaxed);
        gsdw::setFrameTag(++g_tag); // [dwlag] this frame's world draws (kicked after this) carry it to the render thread
        bridge::onGameFrame(ram, g_tag); // [bridge] built once the world is up, kept linked, projected for the renderer
        const int64_t now = nowNs();
        g_shm->gameNs[g_me].store(now, std::memory_order_relaxed); // [parity]
        if (g_failFrames.load(std::memory_order_relaxed) > 0)
            g_failFrames.fetch_sub(1, std::memory_order_relaxed);
        if (const int fm = g_fakeMission.load(); fm) // [missionborder] tests
        {
            ram[(game::isLcs() ? kLcsMissionRunning : kVcsMissionRunning) & kRamMask] = fm == 1 ? 1 : 0;
            if (fm == 2)
                g_fakeMission = 0;
        }
        if (g_failQueued.exchange(false))
            failMission(ram);
        if (const uint32_t front = g_shm->frontGame.load(std::memory_order_acquire); front)
        {
            const Role want = front == g_me ? Role::Front : Role::Back;
            if (want != g_role)
            {
                if (want == Role::Front && g_fadeNs > 0 && g_shm->hoTo == g_me && g_shm->hoSeq.load(std::memory_order_acquire) != g_hoApplied)
                {
                    g_fadeHold = true; // black from the first frame shown, until takeHandover has the player in place
                    g_holdSince = now;
                    gsdw::setFade(1.0f);
                }
                if (want == Role::Back)
                {
                    g_restoreWeapon = 0;
                    g_fadeHold = false;
                    g_fadeOutStart = 0;
                    g_fadeInStart = 0;
                }
                applyRole(want);
                g_lastValid = false;
                if (want == Role::Front)
                    g_frontSinceNs = now;
            }
        }
        // [starterfix] Vice City is not ready while a save is being loaded (the boot's load comes after a new game has started
        // and the first frames of play: crossing then put the player in the new game's Fort Baxter and its text)
        const bool loading = !game::isLcs() && vcs::saveanywhere::loading();
        g_shm->ready[g_me].store(gameReady() && !loading && g_sinkOn.load(std::memory_order_relaxed) ? 1u : 0u, std::memory_order_release);
        if (now - g_lastLog.load(std::memory_order_relaxed) > 10000000000ll)
        {
            g_lastLog.store(now, std::memory_order_relaxed);
            logState("every 10 s");
        }
        if (g_fadeHold.load(std::memory_order_relaxed) && now - g_holdSince > 3000000000ll)
        {
            g_fadeHold = false; // never black for good (the player not taken in 3 s)
            g_fadeInStart = now;
            std::fprintf(stderr, "[dualworld] the player was not taken in 3 s: fading in anyway\n");
        }
        if (const int64_t out = g_fadeOutStart.load(std::memory_order_relaxed); out && g_role == Role::Front && now - out >= g_fadeNs)
            handOver(ram, now);
        updateFade(now);
        if (g_role == Role::Front)
        {
            const bool lcs = game::isLcs();
            const uint32_t cam = lcs ? kLcsCamera + kLcsCams + ram[(kLcsCamera + kLcsActive) & kRamMask] * kLcsCamSize
                                     : kVcsCamera + kVcsCams + ram[(kVcsCamera + kVcsActive) & kRamMask] * kVcsCamSize;
            const uint32_t oSource = lcs ? kLcsSource : kVcsSource, oFront = lcs ? kLcsFront : kVcsFront, oUp = lcs ? kLcsUp : kVcsUp;
            float pos[3], front[3], up[3];
            for (int i = 0; i < 3; ++i)
            {
                pos[i] = rdf(ram, cam + oSource + 4u * i);
                front[i] = rdf(ram, cam + oFront + 4u * i);
                up[i] = rdf(ram, cam + oUp + 4u * i);
            }
            const float fov = rdf(ram, cam + (lcs ? kLcsFov : kVcsFov));
            publishView(pos, front, up, fov);
            const bool reproject = g_reproject.load(std::memory_order_relaxed);
            const float overscan = reproject ? g_overscan : 0.0f;
            uint32_t overscanBits;
            std::memcpy(&overscanBits, &overscan, 4);
            g_shm->overscanBits.store(overscanBits, std::memory_order_relaxed);
            g_shm->camSeq.fetch_add(1, std::memory_order_acq_rel); // odd: being written
            g_shm->camTag = g_tag; // [dwlag] the frame this camera is drawn in
            std::memcpy(g_shm->pos, pos, sizeof(pos));
            std::memcpy(g_shm->front, front, sizeof(front));
            std::memcpy(g_shm->up, up, sizeof(up));
            g_shm->fov = fov;
            g_shm->hours = ram[(lcs ? kLcsHours : kVcsHours) & kRamMask];
            g_shm->minutes = ram[(lcs ? kLcsMinutes : kVcsMinutes) & kRamMask];
            g_shm->camGame.store(g_me, std::memory_order_relaxed);
            // the backdrop only outdoors (an interior's open sky shows the other game's world at the same place), and
            // faded by the timecycle's far clip: none in fog (LCS 116 m), whole from 1 km (sunny noon 1,168, midnight
            // 859). LCS: CGame::currArea 0x3D8430, CTimeCycle *0x3D4AE8 + 0x2B8C far clip (lcs_sky.cpp);
            // VCS: currArea 0x489F7C, far clip as the original (no fade)
            const uint32_t area = rd32(ram, lcs ? 0x003D8430u : 0x00489F7Cu);
            float farClip = 2000.0f;
            if (lcs)
            {
                const uint32_t tc = rd32(ram, 0x003D4AE8u);
                if (tc && tc < 0x02000000u)
                    farClip = rdf(ram, tc + 0x2B8Cu);
            }
            g_area.store(area, std::memory_order_relaxed);
            g_farClip.store(farClip, std::memory_order_relaxed);
            float opacity = std::clamp((farClip - 350.0f) / 550.0f, 0.0f, 1.0f);
            if (area != 0u)
                opacity = 0.0f;
            g_opacity.store(opacity, std::memory_order_relaxed);
            g_shm->camSeq.fetch_add(1, std::memory_order_acq_rel);
            g_shm->frontFrames.fetch_add(1, std::memory_order_relaxed);
            g_shm->frontNs.store(now, std::memory_order_relaxed);
            syncWeather(ram, true);
            syncAspect(true);
            takeHandover(ram, now);
            applyArrivalVelocity(ram, now); // [parity]
            restoreWeapon(ram, now);
            checkBorder(ram, now);
            {
                // tests (`dualworld sweep`): the free camera swung, its pose for the next camera update
                std::lock_guard<std::mutex> lock(g_sweepMutex);
                if (g_sweep.on && now >= g_sweep.end)
                    g_sweep.on = false;
                if (g_sweep.on)
                {
                    const float ph = 6.2831853f * static_cast<float>(static_cast<double>(now - g_sweep.start) * 1e-9 / g_sweep.period);
                    const float yaw = g_sweep.yaw + g_sweep.ampYaw * 0.01745329f * std::sin(ph);
                    const float pitch = g_sweep.pitch + g_sweep.ampPitch * 0.01745329f * std::sin(ph);
                    if (lcs)
                        lcs::freecam::setPose(g_sweep.x, g_sweep.y, g_sweep.z, yaw, pitch, g_sweep.fov);
                    else
                        vcs::freecam::setPose(g_sweep.x, g_sweep.y, g_sweep.z, yaw, pitch, g_sweep.fov);
                }
            }
            return;
        }
        park(ram);
        {
            // [dwlag] the camera this frame was drawn with (backCamera, in this frame's camera update), kept by its tag
            // for the picture the renderer hands over (sink)
            gsdw::PictureCamera pc;
            if (g_backPending.on)
            {
                pc.on = viewBasis(g_backPending.front, g_backPending.up, g_backPending.fov, pc.front, pc.right, pc.up);
                std::memcpy(pc.cam, g_backPending.pos, sizeof(pc.cam));
                pc.frontTag = g_backPending.frontTag;
            }
            g_backPending.on = false;
            std::lock_guard<std::mutex> lock(g_backCamsMutex);
            g_backCams[g_tag % 16u] = {g_tag, pc};
        }
        syncWeather(ram, false);
        syncAspect(false);
        // back: the clock follows the front game's
        if (g_clock && g_shm->frontFrames.load(std::memory_order_relaxed) &&
            now - g_shm->frontNs.load(std::memory_order_relaxed) < 2000000000ll)
        {
            const int32_t h = g_shm->hours, m = g_shm->minutes;
            if (h >= 0 && h < 24 && m >= 0 && m < 60)
            {
                ram[(game::isLcs() ? kLcsHours : kVcsHours) & kRamMask] = static_cast<uint8_t>(h);
                ram[(game::isLcs() ? kLcsMinutes : kVcsMinutes) & kRamMask] = static_cast<uint8_t>(m);
            }
        }
    }

    bool backCamera(uint8_t *ram, uint32_t cam)
    {
        if (g_role != Role::Back || !g_shm || g_shm->camGame.load(std::memory_order_relaxed) == g_me)
            return false;
        if (!g_shm->frontFrames.load(std::memory_order_relaxed) || nowNs() - g_shm->frontNs.load(std::memory_order_relaxed) > 2000000000ll)
        {
            g_camSkips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        float pos[3], front[3], up[3], fov = 70.0f;
        uint64_t frontTag = 0;
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t s0 = g_shm->camSeq.load(std::memory_order_acquire);
            if (s0 & 1u)
                continue;
            std::memcpy(pos, g_shm->pos, sizeof(pos));
            std::memcpy(front, g_shm->front, sizeof(front));
            std::memcpy(up, g_shm->up, sizeof(up));
            fov = g_shm->fov;
            frontTag = g_shm->camTag;
            if (g_shm->camSeq.load(std::memory_order_acquire) == s0)
                break;
        }
        if (!(fov >= 10.0f && fov <= 130.0f))
            fov = 70.0f;
        // [dwlag] the front reprojects the picture into its newer camera: a little wider than its view (the overscan), so
        // a turn since then still finds picture at the screen's edges; the camera is handed over with the picture
        {
            const uint32_t bits = g_shm->overscanBits.load(std::memory_order_relaxed);
            float k;
            std::memcpy(&k, &bits, 4);
            if (k > 1.0f && k < 1.5f)
                fov = std::min(150.0f, 2.0f * std::atan(k * std::tan(fov * 0.5f * 0.01745329f)) * 57.29578f);
            g_backPending.on = true;
            std::memcpy(g_backPending.pos, pos, sizeof(pos));
            std::memcpy(g_backPending.front, front, sizeof(front));
            std::memcpy(g_backPending.up, up, sizeof(up));
            g_backPending.fov = fov;
            g_backPending.frontTag = frontTag;
        }
        // offset: VCS = LCS + offset
        const bool lcs = game::isLcs();
        const float sign = lcs ? -1.0f : 1.0f;
        const uint32_t oSource = lcs ? kLcsSource : kVcsSource, oFront = lcs ? kLcsFront : kVcsFront, oUp = lcs ? kLcsUp : kVcsUp,
                       oTarget = lcs ? kLcsTarget : kVcsTarget;
        for (int i = 0; i < 3; ++i)
        {
            const float p = pos[i] + sign * g_off[i];
            wrf(ram, cam + oSource + 4u * i, p);
            wrf(ram, cam + oFront + 4u * i, front[i]);
            wrf(ram, cam + oUp + 4u * i, up[i]);
            wrf(ram, cam + oTarget + 4u * i, p + front[i]);
        }
        wrf(ram, cam + (lcs ? kLcsFov : kVcsFov), fov);
        g_camWrites.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}
