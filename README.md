# TemporalAA (prototype)

Native Steam Cube World temporal anti-aliasing, made by DieuDeGlace with AI assistance (OpenAI ChatGPT/Codex). MIT licensed; SDK attribution in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and inspected Steam build timestamp 0x5d93ab0d/image size 0x5b7000. ReShade is untouched.

The supplied Omega videos guide the softer voxel-edge appearance, but do not establish Omega's implementation. This is a prototype, not an exact recreation. Eight-frame Halton subpixel projection jitter, linear reconstruction, neighborhood-clamped color history and change-based rejection resolve the world before the HUD. History resets on resolution changes, missed scene boundaries, large camera jumps, zoom changes and toggling. F9 toggles. Mods/TemporalAA.ini defaults to Enabled=1, HistoryPercent=65, BlurPercent=12 and JitterPercent=75.

## Edge-stability test branch

`test/taa-edge-stability` starts from the accepted main build and preserves its pass count, texture dimensions, settings and projection-upload reuse. Local contrast now guides history rejection at broad edges, reducing jitter-phase coverage changes while retaining raw color-change rejection and neighborhood clamping. The already fetched center sample is reused. Native perspective backgrounds receive the same projection jitter as the world; orthographic HUD draws stay excluded. World history capture still requires the solid world layout, a translated camera and a depth-backed target.

The compiled resolver uses at most nine scene samples and one history load. WARP tests compare an independent main shader baseline against the candidate over all eight jitter phases, check moving-edge trails and uniform colors, and cover projection alignment, history rejection, target resizing, state restoration and binding release at 720p, 1080p and 512 x 512. See PERFORMANCE.md for hardware timings. Tests do not establish Omega parity or gameplay frame rate; camera motion, clouds, foliage, HUD and minimap still need live confirmation.

There is currently no depth reprojection or per-object motion-vector buffer on this branch. Fast camera/character/foliage motion may lose temporal smoothing or retain some trails. Color rejection limits this but cannot replace motion vectors. Projection jitter applies to the identified native perspective matrix layouts; other custom rendering pipelines may not share it.

One full-size input copy or MSAA resolve, one output-to-history copy and one fullscreen resolve pass are added. GPU memory/work increase with resolution. There is no CPU GPU readback in the render path. World grading and MoonandShadow coordinate through optional completion exports; update alpha-lookalike alongside this mod. No ReShade files, proprietary shaders, game assets or saves are bundled.

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
