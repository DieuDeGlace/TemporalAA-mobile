# TemporalAA mobile

Derived from DieuDeGlace/TemporalAA with source history and MIT attribution retained. This is a Windows x64 mod used through GameNative, not an Android native library.

## Current version

Baseline 2e0d10b (superseded by the coordinated update below): shares world-draw preparation, skips identical projection uploads, adds sparse nonblocking GPU timing, and softens resolve/jitter. MobileBuild.h names the inspected executable identity and game pointer RVA. NativeScene validates the immutable executable once, but reads the game pointer each time so world reloads remain safe. Upstream source formatting is preserved to simplify future merges.

Settings: Enabled=1, HistoryPercent=65, BlurPercent=12, JitterPercent=75. F9 toggles TAA. Install TemporalAA.dll and TemporalAA.ini into the actual Steam game Mods folder with the game closed. Keep a backup; removing the DLL disables it. Update MoonandShadow-mobile and alpha-lookalike-mobile too: their completion exports coordinate lighting, grading and the TAA resolve before the HUD.

## Validation

TemporalTest passed on Windows WARP at 1280x720, 1920x1080 and 512x512, including projection reuse, preparation equivalence, blur, history rejection, target resizing and state restoration. Installed on Pixel 10 Pro / Android17 / GameNative1.2.1 with System driver, DXVK2.7.1 and the established Proton/ALSA setup. Device DLL hash verified; actual Steam world and HUD render. GPU logs confirm real resolve/history work. Measurements describe these passes, not FPS improvement; no controlled before/after benchmark was performed.

No motion vectors; fast camera/creature/foliage movement can trail. Long-session and all-weather testing remains incomplete. See PERFORMANCE.md for sparse timing details.

## Build

Run build-mobile.cmd from a Windows x64 Visual Studio tools prompt with clang-cl available. It builds the DLL and runs the WARP test. CMake remains available as documented upstream. Game files, saves, runtimes and Steam credentials are not included.

## Coordinated rendering update (2026-10-12)
Merged upstream test/cascaded-shadows at 5084188, TemporalAA test/taa-edge-stability at 8f3c561 (code 44b12e0), and WorldMotion test/cascaded-shadows at 59c8a89. Palette has no newer source update and its installed companion stays unchanged.

Shadows retain the phone strength 45, full far 4096/radius 120, cloud and moon settings. Upstream now enables a near 2048/radius 32 map by default, even with older INIs; it adds roughly 16 MiB depth storage. Cascades=0 in [Shadows] disables the near map. The new model receiver preserves ambient and local illumination, fixes close contact/self-shadow comparisons, and stabilizes grazing model faces. WorldMotion shares animation time across native rendering and shadow capture. TAA aligns native backgrounds with world jitter and improves edge history rejection without adding samples or fullscreen passes. Phone wind settings and all INIs were preserved byte for byte during installation.

Built Windows x64 DLLs with clang-cl /O2. All nine shadow tests passed, including160 model-plane cases, native shader clip equivalence (4608 values, zero ULP discrepancy on the local PC fixture), ambient/point/lantern preservation, cascade filtering and state restoration. TemporalTest passed 720p/1080p/square cases; TemporalStabilityTest static-edge variance changed from 67.8996 to 14.7805 on WARP, with moving silhouette/uniform-color checks passing. This synthetic variance reduction is not an in-game percentage claim. Wind CPU/GPU tests passed including frame-time synchronization.

Installed into the actual GameNative Steam folder with a rollback installer; three DLLs byte-compared successfully. Pixel 10 Pro / Android 17 / GameNative 1.2.1 loaded into Groron Hills; HUD and terrain rendered. Logs confirm model receiver/cascades active and TAA resolves. User reported: "Looks steadier; speed feels similar." No matched gameplay FPS benchmark or long-session test was performed. Wine's shader compiler reports a potential-uninitialized warning for modelPlaneVisibility; all return paths are defined, compilation completes, and rendering tests/device initialization succeed. This warning is retained in diagnostics rather than suppressed.

Rollback: close GameNative and restore all three backed-up DLLs together. Keep the existing INIs. The normal Steam-folder launcher was restored after the one-time installer. Geo/.NET, saves, Steam auth, driver/DXVK/Proton and other installed mods were not replaced.

## Camera reprojection and MSAA depth update (2026-10-15)

Merged upstream TemporalAA `perf/taa-msaa-copies`. This includes camera reprojection: since 2026-10-14 the phone already ran that PC build through the parity installer. It also adds:

- shader-readable scene depth, so the multisampled depth buffer is read in place instead of copied every frame;
- double-buffered history in place of the per-frame MSAA history resolve;
- swapped depth-history textures.

`MobileBuild.h` and the phone-only identity check are kept, and `NativeScene.h` keeps the mobile version.

On the PC RX 6600 benchmark, at 1080p with 4x/8x MSAA, the pass went from 1.20/2.02 ms to 0.36/0.53 ms. The image is unchanged. DXVK on the phone has not been measured. `ReadableDepth=0` in `TemporalAA.ini` restores the copy path.
