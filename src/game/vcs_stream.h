// VCS disc streaming without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // The EE side of CDSTREAM.IRX's stream server (SIF RPC 0x65686577): read / poll / fn 5 replaced by reads from the
    // disc image on a host thread straight into EE RAM, the server's init call answered on the host. False with
    // VCS_NATIVE_STREAM=0, which keeps the IOP's (the reference).
    bool installStreamHooks(PS2Runtime &runtime);
    // The same for another build of the engine (Liberty City Stories: game/lcs_overrides.cpp): its client functions,
    // and what its read returns for a started read
    bool installStreamHooks(PS2Runtime &runtime, uint32_t read, uint32_t poll, uint32_t fn5, uint32_t acceptedReply);
    // The server's sync (fn 3: wait until a channel's read is done) and last position (fn 4: the sector after the last
    // read) for a client that calls them (LCS). Through the IOP they answer at once - it never saw the host reads - so
    // the game could convert a model buffer still being filled. Only with installStreamHooks' reads;
    // VCS_STREAM_SYNC_HOOKS=0 leaves them to the IOP (the old behaviour).
    bool installStreamSyncHooks(PS2Runtime &runtime, uint32_t sync, uint32_t lastPosn);

    // Milestone 6 step 3: with streaming and sound both native the emulated IOP is switched off - no IRX module is
    // loaded or run - unless PS2X_IOP=1; PS2X_IOP=0 switches it off regardless.
    void retireIop(bool nativeComplete);
}
