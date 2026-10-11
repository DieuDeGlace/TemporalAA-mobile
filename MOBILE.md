# TemporalAA mobile

Derived from DieuDeGlace/TemporalAA with source history and MIT attribution retained. This is a Windows x64 mod used through GameNative, not an Android native library.

## Current version

Upstream 2e0d10b: shares world-draw preparation, skips identical projection uploads, adds sparse nonblocking GPU timing, and softens resolve/jitter. MobileBuild.h names the inspected executable identity and game pointer RVA. NativeScene validates the immutable executable once, but reads the game pointer each time so world reloads remain safe. Upstream source formatting is preserved to simplify future merges.

Settings: Enabled=1, HistoryPercent=65, BlurPercent=12, JitterPercent=75. F9 toggles TAA. Install TemporalAA.dll and TemporalAA.ini into the actual Steam game Mods folder with the game closed. Keep a backup; removing the DLL disables it. Update MoonandShadow-mobile and alpha-lookalike-mobile too: their completion exports coordinate lighting, grading and the TAA resolve before the HUD.

## Validation

TemporalTest passed on Windows WARP at 1280x720, 1920x1080 and 512x512, including projection reuse, preparation equivalence, blur, history rejection, target resizing and state restoration. Installed on Pixel 10 Pro / Android17 / GameNative1.2.1 with System driver, DXVK2.7.1 and the established Proton/ALSA setup. Device DLL hash verified; actual Steam world and HUD render. GPU logs confirm real resolve/history work. Measurements describe these passes, not FPS improvement; no controlled before/after benchmark was performed.

No motion vectors; fast camera/creature/foliage movement can trail. Long-session and all-weather testing remains incomplete. See PERFORMANCE.md for sparse timing details.

## Build

Run build-mobile.cmd from a Windows x64 Visual Studio tools prompt with clang-cl available. It builds the DLL and runs the WARP test. CMake remains available as documented upstream. Game files, saves, runtimes and Steam credentials are not included.
