// [android] A ring of the last draws / markers the Vulkan RHI recorded, dumped when the device is lost.
//
// VK_ERROR_DEVICE_LOST says nothing about which draw faulted. This keeps the last kSize events (draw: pipeline name,
// counts, blend, depth, target, buffer sizes; marker: pass / push name) in plain structs, always on: a record is a few
// integer stores and two short copies (no allocation, no formatting); the text is made only by dump(). Written by the
// thread that records commands; dump() may run on it too (vkQueueSubmit is called there), a racing reader at worst sees
// a half-written entry.
// Header only and free of Vulkan types, so tools/ tests can include it.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rhi
{
    class DrawRing
    {
    public:
        static constexpr uint32_t kSize = 128; // power of two
        static constexpr uint32_t kName = 28;

        enum Kind : uint8_t { None, Draw, DrawIndexed, Push, Pop, PassBegin };

        struct Event
        {
            uint32_t seq = 0;
            Kind kind = None;
            uint8_t topology = 0, blendOn = 0, writeMask = 0;
            uint8_t srcColor = 0, dstColor = 0, colorOp = 0, srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
            uint8_t depthTest = 0, depthWrite = 0, depthFunc = 0, samples = 0, texMask = 0;
            uint16_t stride = 0, attrs = 0;
            uint32_t count = 0, first = 0, frame = 0;
            int32_t base = 0;
            uint32_t width = 0, height = 0, colorFormat = 0, depthFormat = 0;
            uint32_t vtxOffset = 0, vtxSize = 0, idxSize = 0; // the vertex buffer's offset / size, the index buffer's size
            char pipeline[kName] = {};                        // draw: pipeline name; marker: its name goes in `marker`
            char marker[kName] = {};                          // draw: marker stack top; push / pass: the name
        };

        void record(const Event &e)
        {
            const uint32_t n = m_head.fetch_add(1, std::memory_order_relaxed);
            Event &slot = m_events[n & (kSize - 1)];
            slot = e;
            slot.seq = n;
        }

        static void copyName(char (&dst)[kName], const char *src)
        {
            if (!src)
            {
                dst[0] = '\0';
                return;
            }
            std::strncpy(dst, src, kName - 1);
            dst[kName - 1] = '\0';
        }

        // the marker stack (pushMarker / popMarker): the top names the draws that follow
        void pushName(const char *name)
        {
            if (m_sp < kDepth)
                copyName(m_stack[m_sp], name);
            ++m_sp;
        }
        void popName()
        {
            if (m_sp)
                --m_sp;
        }
        void resetNames() { m_sp = 0; }
        const char *top() const { return m_sp && m_sp <= kDepth ? m_stack[m_sp - 1] : (m_sp ? m_stack[kDepth - 1] : ""); }

        uint32_t count() const { return m_head.load(std::memory_order_relaxed); }

        // Oldest first. `sink(const char *line)` gets one line each (line buffer is reused).
        template <class Sink> void dump(Sink &&sink) const
        {
            const uint32_t head = m_head.load(std::memory_order_relaxed);
            const uint32_t n = head < kSize ? head : kSize;
            char line[512];
            std::snprintf(line, sizeof(line), "[rhi] last %u of %u recorded draws / markers, oldest first:", n, head);
            sink(line);
            for (uint32_t i = head - n; i != head; ++i)
            {
                const Event &e = m_events[i & (kSize - 1)];
                switch (e.kind)
                {
                case Push:
                case PassBegin:
                    std::snprintf(line, sizeof(line), "[rhi]  #%u f%u %s '%s' %ux%u", e.seq, e.frame,
                                  e.kind == PassBegin ? "pass" : "push", e.marker, e.width, e.height);
                    break;
                case Pop:
                    std::snprintf(line, sizeof(line), "[rhi]  #%u f%u pop", e.seq, e.frame);
                    break;
                case Draw:
                case DrawIndexed:
                    std::snprintf(line, sizeof(line),
                                  "[rhi]  #%u f%u %s '%s' in '%s' n=%u first=%u base=%d topo=%u stride=%u attrs=%u "
                                  "blend=%s %u/%u op%u a%u/%u op%u mask=%x depth=%u/%u/f%u tex=%02x "
                                  "rt=%ux%u fmt=%u/%u x%u vb@%u/%u ib=%u",
                                  e.seq, e.frame, e.kind == DrawIndexed ? "drawIndexed" : "draw", e.pipeline, e.marker,
                                  e.count, e.first, e.base, e.topology, e.stride, e.attrs, e.blendOn ? "on" : "off",
                                  e.srcColor, e.dstColor, e.colorOp, e.srcAlpha, e.dstAlpha, e.alphaOp, e.writeMask,
                                  e.depthTest, e.depthWrite, e.depthFunc, e.texMask, e.width, e.height, e.colorFormat,
                                  e.depthFormat, e.samples, e.vtxOffset, e.vtxSize, e.idxSize);
                    break;
                default:
                    continue;
                }
                sink(line);
            }
            sink("[rhi] (blend factors: 0 zero 1 one 2 srcA 3 1-srcA 4 dstA 5 1-dstA 6 src1A 7 1-src1A 8 const 9 1-const; "
                 "depth=test/write/func; tex=bound texture slot mask; fmt=VkFormat colour/depth)");
        }

    private:
        static constexpr uint32_t kDepth = 8;
        std::atomic<uint32_t> m_head{0};
        char m_stack[kDepth][kName] = {};
        uint32_t m_sp = 0;
        Event m_events[kSize];
    };
} // namespace rhi
