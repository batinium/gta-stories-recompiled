// [lcs] Liberty City Stories' side of Enhancements > Button prompts (VCS's: game/vcs_prompts.h). The setting is VCS's
// (registered by vcs_prompts.cpp); this reads its value. LCS's help text names controls by `~k~ ~ACTION~`, which the
// game's key expander turns into the PS2's words and its font's four face-button characters; here the expander writes
// the player's own controls instead, as text: the keys and mouse buttons bound, an Xbox pad's buttons by name, or a
// PlayStation pad's (its face buttons as the font's own symbols). The game then lays the text out as any other.
#pragma once

class PS2Runtime;

namespace lcs::prompts
{
    // At start-up (applyLcs): replaces the key expander (0x2F8F58). VCS_PROMPTS_HOOKS=0 installs nothing. The
    // `lcsprompts` command: state, and `lcsprompts text <KEY>` / `lcsprompts expand <ACTION>` for tests.
    void install(PS2Runtime &runtime);
}
