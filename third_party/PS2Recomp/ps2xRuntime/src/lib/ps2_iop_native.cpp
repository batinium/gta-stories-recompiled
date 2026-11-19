// [gta-recomp] Native IOP services - see runtime/ps2_iop_native.h.
#include "runtime/ps2_iop_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ps2_iop_native
{
    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::unordered_map<uint32_t, Server> servers;
        };

        State &state()
        {
            static State *s = new State(); // never destroyed: servers may be called while the runtime shuts down
            return *s;
        }

        std::atomic<bool> g_emulatedIop{[]
                                        {
                                            const char *v = std::getenv("PS2X_IOP");
                                            return !(v && v[0] == '0');
                                        }()};
        std::atomic<bool> g_emulatedAudioMuted{false};
        std::atomic<Clock> g_clock{nullptr};
        std::atomic<uint64_t> g_eeCycles{0};
    }

    void registerServer(uint32_t sid, Server server)
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().servers[sid] = std::move(server);
    }

    bool hasServer(uint32_t sid)
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        return state().servers.count(sid) != 0;
    }

    bool dispatch(uint8_t *rdram, const ps2x::iop::RpcRequest &request, ps2x::iop::RpcResult &result)
    {
        Server server;
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            const auto it = state().servers.find(request.sid);
            if (it == state().servers.end())
                return false;
            server = it->second;
        }
        server(rdram, request);
        // as the emulated IOP answers a server that returned (ps2xIOP iop_rpc.cpp IopRpcBridge::handleRpc)
        result = {};
        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ps2x::iop::ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return true;
    }

    namespace
    {
        // [gta-recomp] the audio keeper (header)
        std::atomic<int64_t> g_lastTickNs{0};      // host time of the EE thread's last tick
        std::atomic<uint64_t> g_keeperCycles{0};   // the credit: given by the keeper, not yet taken off the EE thread's ticks
        std::mutex g_keeperMutex;                  // the credit and its time
        int64_t g_keeperCreditNs = 0;              // host time of the keeper's last step
        std::atomic<uint64_t> g_keeperGiven{0};    // (stats) all it gave
        std::atomic<uint64_t> g_keeperStalls{0};   // (stats) stalls it covered

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void keeperLoop()
        {
            constexpr int64_t kStallNs = 20'000'000; // no tick for 20 ms: the EE thread is stalled
            constexpr double kEeCyclesPerNs = 294912000.0 / 1e9;
            int64_t coveredTo = 0; // host time the keeper's IOP time reaches in the current stall
            bool inStall = false;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const Clock clock = g_clock.load(std::memory_order_acquire);
                const int64_t last = g_lastTickNs.load(std::memory_order_acquire);
                const int64_t now = nowNs();
                if (!clock || !last || now - last < kStallNs)
                {
                    inStall = false;
                    continue;
                }
                if (!inStall || coveredTo < last)
                {
                    inStall = true;
                    coveredTo = last; // the EE thread's catch-up brought IOP time to the host clock at its last tick
                    g_keeperStalls.fetch_add(1, std::memory_order_relaxed);
                }
                const uint64_t cycles = static_cast<uint64_t>(static_cast<double>(now - coveredTo) * kEeCyclesPerNs);
                coveredTo = now;
                if (!cycles)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(g_keeperMutex);
                    g_keeperCycles.fetch_add(cycles, std::memory_order_relaxed);
                    g_keeperCreditNs = now;
                }
                g_keeperGiven.fetch_add(cycles, std::memory_order_relaxed);
                g_eeCycles.fetch_add(cycles, std::memory_order_relaxed);
                clock(cycles); // the driver's own lock orders it with the EE thread's calls
                static uint64_t logged = 0;
                if (std::getenv("PS2X_AUDIO_STATS") && (logged++ % 50u) == 0u)
                    std::fprintf(stderr, "[audio] keeper: %llu stalls covered, %.0f ms given\n",
                                 (unsigned long long)g_keeperStalls.load(), g_keeperGiven.load() / 294912.0);
            }
        }
    }

    void setClock(Clock clock)
    {
        g_clock.store(clock);
        static const bool keeper = [] {
            const char *e = std::getenv("PS2X_AUDIO_KEEPER");
            return !(e && e[0] == '0');
        }();
        static std::once_flag started;
        if (clock && keeper)
            std::call_once(started, [] { std::thread(keeperLoop).detach(); });
    }

    void tick(uint64_t eeCycles)
    {
        const int64_t now = nowNs();
        g_lastTickNs.store(now, std::memory_order_release);
        if (g_keeperCycles.load(std::memory_order_relaxed))
        {
            // the keeper's credit (header): off these cycles, or dropped once 250 ms old
            std::lock_guard<std::mutex> lock(g_keeperMutex);
            if (now - g_keeperCreditNs > 250'000'000)
                g_keeperCycles.store(0, std::memory_order_relaxed);
            else
            {
                const uint64_t credit = g_keeperCycles.load(std::memory_order_relaxed);
                const uint64_t take = std::min(credit, eeCycles);
                g_keeperCycles.store(credit - take, std::memory_order_relaxed);
                eeCycles -= take;
            }
            if (!eeCycles)
                return;
        }
        g_eeCycles.fetch_add(eeCycles, std::memory_order_relaxed);
        if (const Clock clock = g_clock.load(std::memory_order_acquire))
            clock(eeCycles);
    }

    uint64_t eeCycles() { return g_eeCycles.load(std::memory_order_relaxed); }

    bool recording()
    {
        static const bool on = std::getenv("PS2X_RPC_RECORD") != nullptr;
        return on;
    }

    namespace
    {
        struct Recorder
        {
            std::mutex mutex;
            std::FILE *file = nullptr;
            std::vector<uint32_t> sids;

            Recorder()
            {
                if (const char *path = std::getenv("PS2X_RPC_RECORD"))
                    file = std::fopen(path, "wb");
                const char *v = std::getenv("PS2X_RPC_RECORD_SIDS");
                for (const char *p = v; p && *p;)
                {
                    char *end = nullptr;
                    const unsigned long sid = std::strtoul(p, &end, 16);
                    if (end == p)
                        break;
                    sids.push_back(static_cast<uint32_t>(sid));
                    p = *end == ',' ? end + 1 : end;
                }
            }

            bool wanted(uint32_t sid) const
            {
                return file && (sids.empty() || std::find(sids.begin(), sids.end(), sid) != sids.end());
            }
        };

        Recorder &recorder()
        {
            static Recorder *r = new Recorder();
            return *r;
        }

        const uint8_t *guestBytes(const uint8_t *rdram, uint32_t address, uint32_t size)
        {
            address &= 0x1FFFFFFFu;
            return rdram && address && size && address + size <= 0x2000000u ? rdram + address : nullptr;
        }
    }

    // a call: IOP time (u64), sid, function, send size, receive size, the bytes sent; recordReply adds the reply's
    // first word (u32, 0 without a receive buffer) once the call returned
    void record(const uint8_t *rdram, const ps2x::iop::RpcRequest &request, uint64_t iopTimeEeCycles)
    {
        Recorder &r = recorder();
        if (!r.wanted(request.sid))
            return;
        std::lock_guard<std::mutex> lock(r.mutex);
        const uint8_t *send = guestBytes(rdram, request.send.address, request.send.size);
        const uint32_t header[4] = {request.sid, request.function, send ? request.send.size : 0u, request.receive.size};
        std::fwrite(&iopTimeEeCycles, 8, 1, r.file);
        std::fwrite(header, 4, 4, r.file);
        if (send)
            std::fwrite(send, 1, request.send.size, r.file);
    }

    void recordReply(const uint8_t *rdram, const ps2x::iop::RpcRequest &request)
    {
        Recorder &r = recorder();
        if (!r.wanted(request.sid))
            return;
        std::lock_guard<std::mutex> lock(r.mutex);
        uint32_t reply = 0;
        if (const uint8_t *receive = guestBytes(rdram, request.receive.address, 4))
            std::memcpy(&reply, receive, 4);
        std::fwrite(&reply, 4, 1, r.file);
        std::fflush(r.file); // the game leaves through _Exit
    }

    void setEmulatedIop(bool enabled) { g_emulatedIop.store(enabled); }
    bool emulatedIop() { return g_emulatedIop.load(); }
    void setEmulatedAudioMuted(bool muted) { g_emulatedAudioMuted.store(muted); }
    bool emulatedAudioMuted() { return g_emulatedAudioMuted.load(); }
}
