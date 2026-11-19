// [gta-recomp] The render submission worker: see runtime/ps2_render_worker.h.
#include "runtime/ps2_render_worker.h"
#include "runtime/ps2_perf.h"
#include "ThreadNaming.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <new>
#include <exception>
#include <mutex>
#include <thread>
#include <cstring>  // [gta-recomp] libstdc++ (Linux) does not bring it in
#include "runtime/ps2_host_std.h"

namespace ps2_render_worker
{
    namespace
    {
        constexpr size_t kReasons = static_cast<size_t>(FenceReason::Count);
        constexpr const char *kReasonNames[kReasons] = {"gs-priv", "gif-reg", "vif1-reg", "fifo", "vu1-mem", "gs-stub", "other"};
        constexpr size_t kMaxItems = 65536u; // a bound on queued work besides the frame bound

        struct Item
        {
            std::function<void()> work;
            bool frameEnd = false;
            ps2x::FpControl csr = 0u; // the game thread's MXCSR / FPCR (rounding, flush-to-zero): VU1 and GS work runs in it
        };

        struct State
        {
            std::mutex mutex;
            std::condition_variable workCv; // the worker waits for work
            std::condition_variable doneCv; // the game thread waits for the worker to get on
            std::deque<Item> queue;
            bool busy = false, stopping = false, started = false;
            uint32_t framesQueued = 0u; // frameEnd items not finished yet
            uint32_t waiters = 0u;      // game-thread waits in progress (doneCv is only signalled for them)
            ps2x::Thread thread; // [gta-recomp] a guest-sized stack (ps2_host_std.h)

            std::mutex poolMutex;
            std::vector<std::vector<uint8_t>> pool;

            // statistics (mutex)
            uint64_t items = 0u, frames = 0u, postWaits = 0u, postWaitNs = 0u;
            uint64_t fences[kReasons]{}, fenceWaits[kReasons]{}, fenceWaitNs = 0u;
            std::chrono::steady_clock::time_point lastLog = ps2x::hostNow();
        };

        // Never destroyed: the worker may still be parked on it while static destructors run at exit.
        State &state()
        {
            static State *s = new State();
            return *s;
        }

        std::atomic<int> g_enabled{-1}; // -1 until configure()
        thread_local bool t_onWorker = false;
    thread_local uint64_t t_idleNs = 0u; // [gta-recomp] noteIdle: waits inside the current item, not busy time   // the worker or the GS stage
        thread_local bool t_onGsStage = false;  // the GS stage

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow().time_since_epoch()).count());
        }

        uint32_t framesAhead()
        {
            static const uint32_t ahead = [] {
                const char *v = std::getenv("PS2X_RENDER_THREAD_AHEAD");
                const int n = v ? std::atoi(v) : 1;
                return static_cast<uint32_t>(n < 1 ? 1 : n);
            }();
            return ahead;
        }

        // ---- [gta-recomp] the GS stage (see the header) ----
        // One producer (the worker), one consumer (the GS stage): records of a 64-byte header and a payload padded to 64
        // bytes in a ring; a record that would wrap is preceded by a skip record to the ring's end.
        constexpr uint64_t kGsRingBytes = uint64_t(64) << 20;
        constexpr uint64_t kGsAlign = 64u;
        constexpr uint64_t kGsHeaderBytes = 64u;
        constexpr uint32_t kGsSkip = 0xFFFFFFFFu, kGsRun = 0xFFFFFFFEu;
        static_assert(sizeof(GsCommand) <= kGsHeaderBytes, "header");

        struct GsStage
        {
            uint8_t *ring = nullptr;
            alignas(64) std::atomic<uint64_t> head{0}; // the consumer's position: everything before it has run
            alignas(64) std::atomic<uint64_t> tail{0}; // the producer's committed end
            // [gta-recomp] the producer's own, on their own cache line: they shared `tail`'s, which the GS stage reads, and
            // every gsReserve / gsSend wrote them (the line went back and forth between the two threads per record)
            alignas(64) uint64_t reserved = 0;         // the producer's reserved record end (gsSend commits it)
            uint64_t committed = 0;                    // [gta-recomp] the producer's committed end, published to `tail` in batches
            uint64_t cachedHead = 0;                   // [gta-recomp] `head` when last read: room known without reading it
            uint32_t unpublished = 0;                  // [gta-recomp] records committed since the last publish
            alignas(64) std::atomic<uint32_t> framesPending{0}; // frame ends sent, not run yet
            std::atomic<bool> consumerWaiting{false}, producerWaiting{false};
            std::atomic<bool> producerWaitingFrame{false}; // [gta-recomp] the producer waits for the frame bound (gsWaitProducer)
            std::atomic<uint32_t> drainWaiters{0};
            std::atomic<uint64_t> drains{0}, drainWaits{0}, drainWaitNs{0}; // [gta-recomp] PS2X_RENDER_THREAD_LOG
            std::mutex mutex;
            std::condition_variable workCv, spaceCv, doneCv;
            bool started = false, stopping = false;
            ps2x::Thread thread; // [gta-recomp] a guest-sized stack (ps2_host_std.h)
            GsHandler handler = nullptr;
            GsPauseHandler pause = nullptr; // [gta-recomp] setGsPauseHandler
        };

        GsStage &gsState()
        {
            static GsStage *g = new GsStage(); // never destroyed (as State)
            return *g;
        }

        std::atomic<int> g_gsStageOn{0};

        void gsNotify(GsStage &g, std::condition_variable &cv)
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            cv.notify_all();
        }

        // [gta-recomp] Records committed by gsSend reach the GS stage in batches: LCS sends ~50k records a frame (a GIF
        // packet each), and a seq_cst tail store, a consumer-waiting check and often a wake-up per record were ~14% of the
        // render worker, the GS stage's matching head store / empty-ring spin and timing another ~10%. Published when
        // kGsBatchRecords are waiting, at every run-on-stage command (frame ends), before the producer waits or drains,
        // and when the worker finishes an item (so nothing is left behind while it waits for the game).
        // PS2X_GS_BATCH=1: every record published as it is sent (as before).
        uint32_t gsBatchRecords()
        {
            static const uint32_t n = [] {
                const char *e = std::getenv("PS2X_GS_BATCH");
                const long v = e && *e ? std::strtol(e, nullptr, 10) : 32;
                return static_cast<uint32_t>(v < 1 ? 1 : v > 4096 ? 4096 : v);
            }();
            return n;
        }

        void gsPublish(GsStage &g)
        {
            g.unpublished = 0;
            if (g.tail.load(std::memory_order_relaxed) == g.committed)
                return;
            g.tail.store(g.committed, std::memory_order_seq_cst);
            if (g.consumerWaiting.load(std::memory_order_seq_cst))
                gsNotify(g, g.workCv);
        }

        void gsRun()
        {
            ThreadNaming::SetCurrentThreadName("GsStage");
            t_onWorker = true;
            t_onGsStage = true;
            GsStage &g = gsState();
            uint64_t busySince = 0;
            uint64_t knownTail = 0; // [gta-recomp] `tail` when last read: records up to it run without reading it again
            for (;;)
            {
                const uint64_t head = g.head.load(std::memory_order_relaxed);
                if (knownTail == head)
                    knownTail = g.tail.load(std::memory_order_acquire);
                if (knownTail == head)
                {
                    if (g.pause) // [gta-recomp] before anything waits on this stage (drainers, a full ring's producer)
                        g.pause();
                    if (busySince)
                    {
                        ps2perf::addGsStageTime(nowNs() - busySince);
                        busySince = 0;
                    }
                    if (g.drainWaiters.load(std::memory_order_seq_cst))
                        gsNotify(g, g.doneCv);
                    std::atomic_thread_fence(std::memory_order_seq_cst); // [gta-recomp] (the release head store above)
                    if (g.producerWaiting.load(std::memory_order_seq_cst))
                        gsNotify(g, g.spaceCv);
                    bool more = false;
                    for (int i = 0; i < 4000 && !more; ++i) // the next command is usually microseconds away
                    {
                        ps2x::cpuRelax();
                        more = g.tail.load(std::memory_order_acquire) != head;
                    }
                    if (!more)
                    {
                        std::unique_lock<std::mutex> lock(g.mutex);
                        g.consumerWaiting.store(true, std::memory_order_seq_cst);
                        g.workCv.wait(lock, [&] { return g.tail.load(std::memory_order_seq_cst) != head || g.stopping; });
                        g.consumerWaiting.store(false, std::memory_order_relaxed);
                        if (g.tail.load(std::memory_order_seq_cst) == head)
                            break; // stopping, nothing left
                    }
                    continue;
                }
                if (!busySince)
                    busySince = nowNs();
                const uint8_t *record = g.ring + (head & (kGsRingBytes - 1u));
                GsCommand c;
                std::memcpy(&c, record, sizeof(c));
                const uint64_t size = kGsHeaderBytes + ((c.bytes + kGsAlign - 1u) & ~(kGsAlign - 1u));
                if (c.type != kGsSkip)
                {
                    if (ps2x::fpControl() != c.csr)
                        ps2x::setFpControl(c.csr);
                    try
                    {
                        if (c.type == kGsRun)
                        {
                            if (g.pause) // [gta-recomp] work items run without the lock held for a run of records
                                g.pause();
                            auto *work = reinterpret_cast<std::function<void()> *>(static_cast<uintptr_t>(c.a));
                            (*work)();
                            delete work;
                        }
                        else if (g.handler)
                            g.handler(c, record + kGsHeaderBytes);
                    }
                    catch (const std::exception &e)
                    {
                        static int reported = 0;
                        if (reported++ < 20)
                            std::fprintf(stderr, "[gs-stage] command failed: %s\n", e.what());
                    }
                    catch (...)
                    {
                        static int reported = 0;
                        if (reported++ < 20)
                            std::fprintf(stderr, "[gs-stage] command failed (unknown exception)\n");
                    }
                    if (c.type == kGsRun && c.b)
                        g.framesPending.fetch_sub(1u, std::memory_order_seq_cst);
                }
                // [gta-recomp] release, not seq_cst (an xchg per record): a producer waiting for space whose wake this
                // check misses is woken at the next record (it waits only with the ring nearly full) or at the empty-ring
                // check above; a frame end's fetch_sub already orders its check. PS2X_GS_BATCH=1: seq_cst as before.
                g.head.store(head + size, gsBatchRecords() > 1u ? std::memory_order_release : std::memory_order_seq_cst);
                // [gta-recomp] a producer waiting for the frame bound only after a frame end: woken (lock, notify, its
                // wait re-locking and checking again) after every record while the worker sat on the frame bound, the
                // two threads took turns on the mutex (~10% of the GS stage in LCS's Fort Staunton)
                if (g.producerWaiting.load(std::memory_order_seq_cst) &&
                    (!g.producerWaitingFrame.load(std::memory_order_relaxed) || (c.type == kGsRun && c.b)))
                    gsNotify(g, g.spaceCv);
            }
        }

        // the producer waits until `ready()` (space in the ring, the frame bound)
        template <typename Ready>
        void gsWaitProducer(GsStage &g, Ready ready, bool frameBound = false)
        {
            for (int i = 0; i < 2000; ++i)
            {
                if (ready())
                    return;
                ps2x::cpuRelax();
            }
            std::unique_lock<std::mutex> lock(g.mutex);
            g.producerWaitingFrame.store(frameBound, std::memory_order_relaxed); // [gta-recomp] (before the seq_cst store)
            g.producerWaiting.store(true, std::memory_order_seq_cst);
            g.spaceCv.wait(lock, ready);
            g.producerWaiting.store(false, std::memory_order_relaxed);
            g.producerWaitingFrame.store(false, std::memory_order_relaxed);
        }

        void gsStart(GsStage &g)
        {
            if (g.started)
                return;
            g.started = true;
            g.ring = static_cast<uint8_t *>(::operator new(static_cast<size_t>(kGsRingBytes), std::align_val_t(64)));
            g.thread = ps2x::Thread(ps2x::kGuestThreadStack, true, gsRun);
        }

        void run()
        {
            ThreadNaming::SetCurrentThreadName("RenderWorker");
            t_onWorker = true;
            State &s = state();
            std::unique_lock<std::mutex> lock(s.mutex);
            for (;;)
            {
                s.workCv.wait(lock, [&] { return !s.queue.empty() || s.stopping; });
                if (s.queue.empty())
                    break;
                Item item = std::move(s.queue.front());
                s.queue.pop_front();
                s.busy = true;
                lock.unlock();
                if (ps2x::fpControl() != item.csr)
                    ps2x::setFpControl(item.csr);
                const uint64_t start = nowNs();
                try
                {
                    item.work();
                }
                catch (const std::exception &e)
                {
                    static int reported = 0;
                    if (reported++ < 20)
                        std::fprintf(stderr, "[render-worker] work failed: %s\n", e.what());
                }
                catch (...)
                {
                    static int reported = 0;
                    if (reported++ < 20)
                        std::fprintf(stderr, "[render-worker] work failed (unknown exception)\n");
                }
                if (g_gsStageOn.load(std::memory_order_relaxed) == 1 && gsState().started)
                    gsPublish(gsState()); // [gta-recomp] the item's batched GS records (gsPublish)
                item.work = nullptr; // its captures (chain buffers) go outside the lock
                const uint64_t busyNs = nowNs() - start;
                ps2perf::addWorkerTime(busyNs > t_idleNs ? busyNs - t_idleNs : 0u); // [gta-recomp] (noteIdle)
                t_idleNs = 0u;
                lock.lock();
                s.busy = false;
                if (item.frameEnd)
                    --s.framesQueued;
                if (s.waiters)
                    s.doneCv.notify_all();
            }
        }

        // Caller holds the lock.
        void maybeLog(State &s)
        {
            static const bool log = [] {
                const char *v = std::getenv("PS2X_RENDER_THREAD_LOG");
                return v && *v && *v != '0';
            }();
            if (!log)
                return;
            const auto now = ps2x::hostNow();
            if (now - s.lastLog < std::chrono::seconds(5))
                return;
            const double secs = std::chrono::duration<double>(now - s.lastLog).count();
            s.lastLog = now;
            std::fprintf(stderr, "[render-worker] %.0f items/s, %.1f frames/s, post waits %llu (%.1f ms/s); fences (waited):",
                         s.items / secs, s.frames / secs, static_cast<unsigned long long>(s.postWaits), s.postWaitNs / 1e6 / secs);
            for (size_t r = 0; r < kReasons; ++r)
                if (s.fences[r])
                    std::fprintf(stderr, " %s %llu (%llu)", kReasonNames[r], static_cast<unsigned long long>(s.fences[r]),
                                 static_cast<unsigned long long>(s.fenceWaits[r]));
            std::fprintf(stderr, ", fence wait %.1f ms/s", s.fenceWaitNs / 1e6 / secs);
            {
                GsStage &g = gsState(); // [gta-recomp] the GS stage drained by the worker (front-end calls that need it idle)
                std::fprintf(stderr, "; GS stage drains %llu (%llu waited, %.1f ms/s)\n", (unsigned long long)g.drains.exchange(0),
                             (unsigned long long)g.drainWaits.exchange(0), g.drainWaitNs.exchange(0) / 1e6 / secs);
            }
            s.items = s.frames = s.postWaits = s.postWaitNs = s.fenceWaitNs = 0u;
            for (size_t r = 0; r < kReasons; ++r)
                s.fences[r] = s.fenceWaits[r] = 0u;
        }
    }

    void configure(bool defaultOn)
    {
        const char *v = std::getenv("PS2X_RENDER_THREAD");
        const bool on = v && *v ? *v != '0' : defaultOn;
        g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
        std::fprintf(stderr, "[render-worker] %s\n", on ? "on: render submission runs on its own thread" : "off");
    }

    bool enabled()
    {
        return g_enabled.load(std::memory_order_relaxed) == 1;
    }

    bool onWorker()
    {
        return t_onWorker;
    }

    void noteIdle(uint64_t ns) { t_idleNs += ns; } // [gta-recomp]

    void post(std::function<void()> work, bool frameEnd)
    {
        if (!enabled() || t_onWorker)
        {
            work();
            return;
        }
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.started)
        {
            s.started = true;
            s.thread = ps2x::Thread(ps2x::kGuestThreadStack, true, run);
        }
        auto full = [&] { return s.queue.size() >= kMaxItems || (frameEnd && s.framesQueued >= framesAhead()); };
        if (full())
        {
            const uint64_t start = nowNs();
            ++s.waiters;
            s.doneCv.wait(lock, [&] { return !full(); });
            --s.waiters;
            ++s.postWaits;
            s.postWaitNs += nowNs() - start;
        }
        s.queue.push_back({std::move(work), frameEnd, ps2x::fpControl()});
        if (frameEnd)
        {
            ++s.framesQueued;
            ++s.frames;
        }
        ++s.items;
        maybeLog(s);
        lock.unlock();
        s.workCv.notify_one();
    }

    void fence(FenceReason reason)
    {
        if (!enabled() || t_onGsStage)
            return;
        if (t_onWorker) // [gta-recomp] the worker reading what the GS stage makes
        {
            gsDrain();
            return;
        }
        State &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        const size_t r = static_cast<size_t>(reason);
        ++s.fences[r];
        const bool gsIdle = gsState().head.load(std::memory_order_seq_cst) == gsState().tail.load(std::memory_order_seq_cst);
        if (s.queue.empty() && !s.busy && gsIdle)
            return;
        const uint64_t start = nowNs();
        ++s.waiters;
        s.doneCv.wait(lock, [&] { return s.queue.empty() && !s.busy; });
        --s.waiters;
        lock.unlock();
        gsDrain(); // [gta-recomp] and the GS stage behind it
        lock.lock();
        ++s.fenceWaits[r];
        s.fenceWaitNs += nowNs() - start;
    }

    void configureGsStage(bool defaultOn)
    {
        const char *v = std::getenv("PS2X_GS_THREAD");
        const bool on = enabled() && (v && *v ? *v != '0' : defaultOn);
        g_gsStageOn.store(on ? 1 : 0, std::memory_order_relaxed);
        std::fprintf(stderr, "[render-worker] GS stage %s\n", on ? "on: the GS front end and frame building on a second thread" : "off");
    }

    void setGsHandler(GsHandler handler)
    {
        gsState().handler = handler;
    }

    void setGsPauseHandler(GsPauseHandler handler) // [gta-recomp]
    {
        gsState().pause = handler;
    }

    bool gsStage()
    {
        return t_onWorker && !t_onGsStage && g_gsStageOn.load(std::memory_order_relaxed) == 1;
    }

    bool onGsStage()
    {
        return t_onGsStage;
    }

    uint8_t *gsReserve(const GsCommand &command)
    {
        GsStage &g = gsState();
        gsStart(g);
        const uint64_t need = kGsHeaderBytes + ((command.bytes + kGsAlign - 1u) & ~(kGsAlign - 1u));
        if (need > kGsRingBytes / 4u)
        {
            gsDrain();
            return nullptr;
        }
        uint64_t at = g.committed; // the producer's own ([gta-recomp] committed, maybe not published yet)
        const uint64_t offset = at & (kGsRingBytes - 1u);
        const uint64_t toEnd = kGsRingBytes - offset;
        const uint64_t total = need > toEnd ? toEnd + need : need;
        // [gta-recomp] the room known from the last read of `head` first (it only grows)
        auto space = [&] {
            if (at + total - g.cachedHead <= kGsRingBytes)
                return true;
            g.cachedHead = g.head.load(std::memory_order_seq_cst);
            return at + total - g.cachedHead <= kGsRingBytes;
        };
        if (!space())
        {
            gsPublish(g); // [gta-recomp] the stage must see what it is to make room from
            gsWaitProducer(g, space);
        }
        if (need > toEnd) // a skip to the ring's end (records are multiples of 64 bytes, so toEnd holds a header)
        {
            GsCommand skip;
            skip.type = kGsSkip;
            skip.bytes = static_cast<uint32_t>(toEnd - kGsHeaderBytes);
            std::memcpy(g.ring + offset, &skip, sizeof(skip));
            at += toEnd;
        }
        GsCommand header = command;
        header.csr = ps2x::fpControl();
        uint8_t *record = g.ring + (at & (kGsRingBytes - 1u));
        std::memcpy(record, &header, sizeof(header));
        g.reserved = at + need;
        return record + kGsHeaderBytes;
    }

    void gsSend()
    {
        GsStage &g = gsState();
        g.committed = g.reserved;
        if (++g.unpublished >= gsBatchRecords()) // [gta-recomp] in batches (gsPublish)
            gsPublish(g);
    }

    void runOnGsStage(std::function<void()> work, bool frameEnd)
    {
        if (!gsStage())
        {
            work();
            return;
        }
        GsStage &g = gsState();
        if (frameEnd) // at most one frame end waiting on the GS stage
        {
            gsStart(g);
            gsPublish(g); // [gta-recomp] (batched records first)
            gsWaitProducer(g, [&] { return g.framesPending.load(std::memory_order_seq_cst) < 1u; }, true);
            g.framesPending.fetch_add(1u, std::memory_order_seq_cst);
        }
        GsCommand c;
        c.type = kGsRun;
        c.a = reinterpret_cast<uintptr_t>(new std::function<void()>(std::move(work)));
        c.b = frameEnd ? 1u : 0u;
        gsReserve(c); // (no payload: never too large)
        gsSend();
        gsPublish(g); // [gta-recomp] work to run there: now
    }

    void gsDrain()
    {
        if (t_onGsStage)
            return;
        GsStage &g = gsState();
        if (t_onWorker && g.started) // [gta-recomp] the producer: its batched records first
            gsPublish(g);
        g.drains.fetch_add(1u, std::memory_order_relaxed);
        const uint64_t target = g.tail.load(std::memory_order_seq_cst);
        auto done = [&] { return g.head.load(std::memory_order_seq_cst) >= target; };
        if (done())
            return;
        const uint64_t start = nowNs();
        g.drainWaits.fetch_add(1u, std::memory_order_relaxed);
        struct Account
        {
            GsStage &g;
            uint64_t start;
            ~Account() { g.drainWaitNs.fetch_add(nowNs() - start, std::memory_order_relaxed); }
        } account{g, start};
        for (int i = 0; i < 4000; ++i)
        {
            if (done())
                return;
            ps2x::cpuRelax();
        }
        std::unique_lock<std::mutex> lock(g.mutex);
        g.drainWaiters.fetch_add(1u, std::memory_order_seq_cst);
        while (!done())
            ps2x::waitFor(g.doneCv, lock, std::chrono::milliseconds(1)); // (the stage notifies when idle; the timeout is a guard)
        g.drainWaiters.fetch_sub(1u, std::memory_order_seq_cst);
    }

    std::vector<uint8_t> takeBuffer(size_t want)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.poolMutex);
        if (s.pool.empty())
            return {};
        size_t pick = 0; // [gta-recomp] the smallest that fits, else the largest
        for (size_t i = 1; i < s.pool.size(); ++i)
        {
            const size_t c = s.pool[i].capacity(), best = s.pool[pick].capacity();
            const bool fits = c >= want, bestFits = best >= want;
            if ((fits && (!bestFits || c < best)) || (!fits && !bestFits && c > best))
                pick = i;
        }
        std::vector<uint8_t> buffer = std::move(s.pool[pick]);
        s.pool[pick] = std::move(s.pool.back());
        s.pool.pop_back();
        buffer.clear();
        return buffer;
    }

    void recycleBuffer(std::vector<uint8_t> &&buffer)
    {
        if (buffer.capacity() == 0u)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.poolMutex);
        if (s.pool.size() < 8u)
        {
            s.pool.push_back(std::move(buffer));
            return;
        }
        // [gta-recomp] full: a larger buffer replaces the smallest (the chain buffers must not be the ones dropped)
        auto smallest = std::min_element(s.pool.begin(), s.pool.end(),
                                         [](const auto &a, const auto &b) { return a.capacity() < b.capacity(); });
        if (smallest->capacity() < buffer.capacity())
            *smallest = std::move(buffer);
    }

    void shutdown()
    {
        State &s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.started || s.stopping)
                return;
            s.stopping = true;
        }
        s.workCv.notify_one();
        if (s.thread.joinable())
            s.thread.join();
        GsStage &g = gsState(); // [gta-recomp] then the GS stage, once it ran what the worker sent
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (!g.started)
                return;
            g.stopping = true;
            g.workCv.notify_all();
        }
        if (g.thread.joinable())
            g.thread.join();
    }
}
