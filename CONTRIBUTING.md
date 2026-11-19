# Contributing to GTA Stories Recompiled

Thanks for helping. This project is a static recompilation of the PS2 versions of GTA: Liberty City Stories and
Vice City Stories, with GTA Stories (both cities in one world), a mod loader, a launcher and an Android port. It is built
on [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) and
[PS2Recomp](https://github.com/ran-j/PS2Recomp). Questions and ideas: use
[Discussions](https://github.com/batinium/gta-stories-recompiled/discussions).

## Hard rules (a pull request that breaks one is closed)

- **Never commit game data or code.** No ISOs, no ELF / `SLUS_*` files, no `generated/` or `generated-lcs/` C++, no
  extracted assets, packs, saves, disassembly listings, and no game libraries or APKs built with the game's code in them
  (a "personal build" contains the game; never attach one anywhere).
- **No Rockstar, Take-Two or Sony logos, artwork, fonts or sounds.**
- No proprietary third-party files (`Lossless.dll`, driver blobs) and no keystores or passwords.
- **Licence: GPL-3.0.** Your contribution is under the same licence ([LICENSE](LICENSE)). Code from elsewhere needs a
  compatible licence and its copyright lines.
- Before a push: `python tools/prepublish_check.py`.

## Pull requests

- Against the default branch. **Small, focused** PRs (one topic) get reviewed sooner.
- Say **what** changed and **why** (link the issue). Commit messages say why too.
- **Check the build** before you open it:
  ```
  python tools/build.py --game lcs --quick     # Liberty City Stories
  python tools/build.py --quick                # Vice City Stories
  ```
  and run a **smoke test** if you can (start the game, load a save, drive a bit). Say what you tested and on which
  OS / device, and what you could not test.
- Follow the project rules ([docs/UPSTREAM_AGENTS.md](docs/UPSTREAM_AGENTS.md), the working contract):
  - hooks on recompiled functions never rely on code after the original returns;
  - no floating-point contraction in generated or VU code;
  - a native replacement keeps a switch back to the reference path;
  - changes to `third_party/PS2Recomp` get a `[gta-recomp]` marker and a row in
    [docs/PS2RECOMP_PATCHES.md](docs/PS2RECOMP_PATCHES.md);
  - tools are Python 3, standard library only.
- Update the docs in `docs/` when behaviour, settings or files change.
- For the Android app see [docs/ANDROID.md](docs/ANDROID.md); test on a real device (emulators have no Vulkan 1.3).

## AI-assisted contributions

Welcome, but **say so** in the PR. You must understand the change and have tested it yourself; you are responsible for
it. Unreviewed generated code is closed.

## Upstream

A fix to the Vice City Stories game itself that also matters on the PC may be better sent to
[gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp), whose code we share. If unsure, ask first.

## Reporting bugs

Use the issue forms. In the launcher, *Help > Make a report* makes a zip of the logs (no game files, no saves) to attach.
