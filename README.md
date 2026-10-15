# TemporalAA (prototype)

Native Steam Cube World temporal anti-aliasing, made by DieuDeGlace with AI assistance (OpenAI ChatGPT/Codex). MIT licensed; SDK attribution in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and inspected Steam build timestamp 0x5d93ab0d/image size 0x5b7000. ReShade is untouched.

The supplied Omega videos guide the softer voxel-edge appearance, but do not establish Omega's implementation. This is a prototype, not an exact recreation. Eight-frame Halton subpixel projection jitter, linear reconstruction, neighborhood-clamped color history and change-based rejection resolve the world before the HUD. History resets on resolution changes, missed scene boundaries, large camera jumps, zoom changes and toggling. F9 toggles. Mods/TemporalAA.ini defaults to Enabled=1, HistoryPercent=65, EdgeHistoryPercent=90, BlurPercent=12 and JitterPercent=75.

## Camera reprojection

History is fetched where each surface was on the previous frame, not at the same screen pixel. The resolve reads the native scene depth and maps every pixel through the current and previous jittered camera (previous view-projection times the inverse of the current one, built in double precision on the CPU). Without this, any camera movement blended in whatever used to be at that pixel, which showed as doubled or striped edges, smeared characters and shimmering distant faces.

- Silhouettes use the nearest depth of a small cross, so edge pixels follow the foreground surface instead of smearing it over the background.
- History is released when the previous frame's depth shows a different surface there (newly uncovered terrain, objects passing in front). Sky and far-plane pixels still reproject, which corrects camera rotation.
- History is sampled with a five-tap Catmull-Rom filter, so camera motion does not soften the image a little more every frame.
- The neighborhood clip, edge accumulation and Karis weighting are unchanged from the accepted shimmer fix.
- MSAA scene targets now resolve into history; the previous CopyResource across sample counts was invalid and would have left history stale. Multisampled depth uses the nearest sample.

Characters, fish and waving foliage still have no per-object motion vectors; their own movement relies on the neighborhood clip and change rejection. `CameraReprojection=0` in `Mods/TemporalAA.ini` restores same-pixel history. The log reports whether reprojection is active, or why not.

Cost: two full-size depth textures and one depth copy per frame, plus five depth loads and four extra history taps in the existing resolve pass. No extra fullscreen pass and no CPU readback.

The native depth convention was checked in the executable: depth test enabled with LESS_EQUAL (standard depth, near 0, far 1).

## Build

Windows x64, Visual Studio 2022 C++ Build Tools, Windows SDK, LLVM clang-cl, CMake 3.20+ and Ninja required. In an x64 Native Tools prompt:

```bat
git clone --branch test/taa-edge-stability https://github.com/DieuDeGlace/TemporalAA.git
cd TemporalAA
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional hardware comparison, run from the repository directory:

```bat
build\TemporalStabilityTest.exe --benchmark
```

The benchmark submits interleaved baseline/candidate samples at 1080p. Its bounded GPU readback is test-only. The mod never waits for a performance query.

## Install

Close Steam Cube World; copy build/TemporalAA.dll and optional config/TemporalAA.ini into Mods, then restart. Keep an existing configuration to preserve your settings. Remove this DLL to uninstall. MoonandShadow, alpha-lookalike and motion mods remain separate.

Fine-detail stabilization retains more bounded history on thin, alternating pixel detail such as distant voxel steps. This is a screen-space feature-size adjustment, not a depth-based distance mask; nearby subpixel grass can also benefit. Broad edges use the configured history weight with contrast-aware rejection. Flat regions retain the original rejection thresholds. Stale history is still neighborhood-clamped and strongly changed pixels are rejected.
