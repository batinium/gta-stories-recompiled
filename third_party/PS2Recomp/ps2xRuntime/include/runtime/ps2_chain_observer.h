// [gta-recomp] Observers of the VIF1 DMA chains (game code: docs/RSL_SUBMISSION.md, src/debug/rsl_shadow.cpp,
// src/game/vcs_rsl_world.cpp).
// - Tags: called on the thread that kicks a chain (it is copied then), once per tag, in chain order; chainStart on a
//   chain's first tag. dataAddr / qwc: the payload (0 when none); vifCodes: the tag's upper 64 bits (the two VIFcodes
//   sent with it); flatOffset: where this tag's bytes start in the copied chain - the stream processVIF1Data walks.
//   The return value marks a REF's payload or a CALL's packet: 0 copies the chain as it is; kReplaceWithMark | imm
//   copies one qword before the payload (a REF) or after the CALL's own payload, before the CALLed packet's tags (a
//   CALL; the packet is everything up to its RET): VIF MARK imm, a NOP whose low 24 bits hold the marked bytes' length
//   in 8-byte units (0: not known - a CALL whose RET was not reached), and two NOPs. The marked bytes follow the
//   qword as they would have; the MARK handler may do their work and skip them. (The bytes stay in the copy: they are
//   what the guest memory held when the chain was kicked, which is not what it holds by the time the copy is processed.)
// - Queued: the copied chain was queued for processing; key = its buffer's data pointer, the same pointer the
//   processing sees (the buffer is moved, not copied, to the render worker).
// - Processing: a copied chain's processing begins / ends on the calling thread (key as above).
// - Mark: the VIF1 interpreter ran a MARK on the processing thread (after setting the MARK register); next / avail: the
//   stream after the MARK code. Returns the bytes after the MARK code to skip (0: run on).
// - streamOffset(): on the processing thread, the offset in that stream of the VIFcode being run (set at MSCAL, so the
//   MSCAL hooks can tell which part of the chain started the program).
// Kept out of ps2_memory.h: the generated code includes that one.
#pragma once

#include <cstdint>

class PS2Memory;
struct GSImageBytes;

namespace ps2chain
{
    constexpr uint32_t kReplaceWithMark = 0x80000000u;
    // with kReplaceWithMark: copy none of the marked bytes - the MARK qword's length word is kSkippedLength, and after a
    // CALL the walk goes on at its return address as if its packet had returned (docs/FULLY_NATIVE.md step 4: a block
    // known unchanged is replayed from its recording)
    constexpr uint32_t kSkipWithMark = 0x40000000u;
    constexpr uint32_t kSkippedLength = 0x00FFFFFFu;
    // [gta-recomp] A VIFcode no hardware has, only in the chain copies the walker makes (ps2_memory.cpp, chain images): a REF
    // whose payload is exactly one DIRECT / DIRECTHL's data goes into the copy as this code (IMMEDIATE = the DIRECT's qword
    // count, NUM bit 0 = DIRECTHL) and a pointer (8 bytes) to the interned payload (GSImageBytes, one reference) instead of
    // the DIRECT code and its bytes; the VIF1 interpreter hands it on (to the GS by gsSubmitImageShared).
    constexpr uint32_t kVifChainImage = 0x5Eu;

    // [gta-recomp] Chain images while a tag observer is installed (off by default: VCS's RSL world names rasters from the
    // DIRECTs instead). The walker never interns inside a marked packet; a marked REF's payload is never a pixel REF the
    // observer marks (LCS marks CALLs only).
    void setChainImagesWithObserver(bool on);
    // [gta-recomp] PS2X_CHAIN_ASYNC (ps2_memory.cpp): the VIF1 chain kicked last may still be walked (read from guest
    // memory) on its own thread. Game code that frees or moves memory a chain may reference (a model heap's free, its
    // defragmentation) calls this first, on the game thread: it returns once that walk is done (at once when none).
    void waitAsyncWalk();
    // [gta-recomp] The observers installed can run on the asynchronous walk's thread (LCS's RSL replay without draw
    // items: its tag / queued observers touch only the walk's own state, and its frame-kick hook calls waitAsyncWalk
    // before it hands the next frame's draw list over). Off: an observed chain is walked on the kicking thread.
    void setAsyncWithObserver(bool on);
    bool chainImagesWithObserver();
    // [gta-recomp] A chain image of these bytes (one reference, the walker's hash): an observer's recording hands a DIRECT
    // payload to the VIF1 interpreter as kVifChainImage + this pointer (retained per use) instead of copying the bytes
    GSImageBytes *newChainImage(const uint8_t *data, uint32_t size);

    using TagObserver =uint32_t (*)(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                                     uint32_t flatOffset, bool chainStart);
    using QueuedObserver = void (*)(const void *key, uint32_t sizeBytes);
    using ProcessingObserver = void (*)(const void *key, bool begin);
    using MarkHandler = uint32_t (*)(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail);

    void addTagObserver(TagObserver observer);
    void addChainObservers(QueuedObserver queued, ProcessingObserver processing);
    void setMarkHandler(MarkHandler handler); // nullptr = off
    // DIRECT / DIRECTHL on the processing thread: its data (a pointer into the stream being processed) before it goes to
    // PATH2 - where a tag observer saw it come from can be found by its offset in the copied chain
    using DirectHandler = void (*)(const uint8_t *data, uint32_t bytes);
    void setDirectHandler(DirectHandler handler); // nullptr = off

    uint32_t streamOffset();
    void setStreamOffset(uint32_t offset); // the VIF1 interpreter

    // Recording what a VIF stream does (a REF's block recorded once, then replayed: copy the UNPACKs' results, run the
    // MSCALs with PS2Memory::vu1Mscal): while set on a thread, the VIF1 interpreter reports each UNPACK after it wrote VU
    // memory (qwords VU addresses vuAddr .. + qwords, wrapping at 1024; topRelative: the address had TOPS added; lanes: the
    // fields written - bit n = field n - for a write cycle of CL = WL = 1 (the others keep what VU memory held: fields the
    // format does not supply, write-protected mask fields); lanesFromCol: a mask field took the COL register), each
    // MSCAL before it runs, and every other VIFcode (the recorder decides which ones a replay can do without).
    class VifRecorder
    {
    public:
        virtual ~VifRecorder() = default;
        virtual void unpack(uint32_t vuAddr, uint32_t qwords, bool topRelative, uint32_t tops, uint32_t cycle, bool masked,
                            uint32_t mode, uint32_t lanes, bool lanesFromCol, const uint8_t *vuData) = 0;
        virtual void mscal(uint32_t startPc) = 0;
        virtual void other(uint32_t vifcode) = 0;
        // DIRECT / DIRECTHL: its GS data, before it goes to PATH2 (complete: not cut off by the end of the stream)
        virtual void direct(const uint8_t *data, uint32_t qwords, bool directHl, bool complete)
        {
            (void)data, (void)qwords, (void)directHl, (void)complete;
        }
        // [gta-recomp] MPG: its VIFcode and the microcode after it (complete: the stream held all of it), after other()
        virtual void mpg(uint32_t vifcode, const uint8_t *data, uint32_t bytes, bool complete)
        {
            (void)vifcode, (void)data, (void)bytes, (void)complete;
        }
    };
    void setVifRecorder(VifRecorder *recorder); // this thread
    VifRecorder *vifRecorder();

    // runtime side
    bool tagObserved();
    uint32_t notifyTag(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                       uint32_t flatOffset, bool chainStart);
    void notifyQueued(const void *key, uint32_t sizeBytes);
    void notifyProcessing(const void *key, bool begin);
    uint32_t notifyMark(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail);
    void notifyDirect(const uint8_t *data, uint32_t bytes);
}
