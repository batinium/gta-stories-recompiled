# License of this folder: GPL-3.0-or-later

The files here are Lossless Scaling frame generation (LSFG) for the Android build's Vulkan backend
(`src/rhi/vulkan/framegen.cpp`, docs/ANDROID.md "Frame generation (LSFG)"). They are a C port of the frame generation of [Eden](https://git.eden-emu.dev/eden-emu/eden)
(`src/video_core/frame_gen/{lossless_dll,lsfg_translate}.cpp`, `src/video_core/renderer_vulkan/present/{frame_gen_pacer,lsfg_*}.cpp`),
which builds on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk). All three are licensed GPL-3.0-or-later, and so is this
folder: each file keeps its SPDX copyright lines (Eden Emulator Project 2026, lsfg-vk 2025). The full license text is
[COPYING](COPYING). The rest of this repository is GPL too (upstream gta-vcs-recomp, LICENSE at the root); an APK built with
this folder is GPL-3.0-or-later as a whole.

Built only with the CMake option `VCS_LSFG` (default ON on Android, OFF elsewhere; Gradle `-PvcsLsfg=false` turns it off).

The shaders are not here and never are: they are read at run time from the user's own copy of `Lossless.dll` (Lossless
Scaling, proprietary, bought by the user), which is not part of this repository or of any APK.

| File | From |
|---|---|
| `lsfg_dll.c` | the earlier port's, unchanged (Eden's `frame_gen/lossless_dll.cpp`, `frame_gen/lsfg_translate.cpp`) |
| `lsfg_pacer.c` | the earlier port's, unchanged (Eden's `present/frame_gen_pacer.cpp`) |
| `lsfg_chain.c` | the earlier port's, its device calls through volk and `lsfg_vk.h` (Eden's `present/lsfg_common.cpp`, `lsfg_chain.cpp`, `lsfg_mipmaps.cpp`, `lsfg_alpha.cpp`, `lsfg_beta.cpp`, `lsfg_gamma.cpp`, `lsfg_delta.cpp`, `lsfg_generate.cpp`) |
| `lsfg.h` | the earlier port's (the headers of those), two comments changed |
| `lsfg_vk.h`, `lsfg_vk.c` | new: the device the chain works on (in place of the earlier port's `vka.h`) |
