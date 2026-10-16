# GPU performance measurements

## Draw-hook CPU work, 2026-10-15

The `perf/taa-draw-hooks` branch retains the installed MSAA-copy optimization,
camera reprojection and resolve shader. It fetches the native buffer bindings
once per draw, retains the fetched projection for restoration, and caches the
immutable layout descriptions. The cache retains six COM references to prevent
address reuse after buffer destruction. Contents are still read on every draw;
writing a new matrix into the same buffer never reuses stale projection data.

`DrawHookBenchmark` measures 3,000 draw preparations/restorations per synthetic
frame on a D3D11 hardware context, with 20 warmup frames and 120 measured frames.
It excludes actual native draws, buffer-write hooks, companion mods and the
fullscreen GPU pass. It is a CPU microbenchmark, not a gameplay FPS prediction.

On this PC, sequential baseline (`d086630`) and candidate runs measured:

| Layout | Baseline median CPU ms | Candidate median CPU ms | Baseline / candidate p95 ms |
| --- | ---: | ---: | ---: |
| Background | 0.3732 | 0.3155 | 0.3848 / 0.4628 |
| Solid world | 0.4374 | 0.3553 | 0.4496 / 0.3682 |

The solid-world median reduction is about 19%, or 0.0821 ms for this workload.
Background p95 was noisier in the candidate run. This small isolated saving
does not account for the user's full F9 FPS difference; live CPU/GPU logs and
a matched view comparison remain necessary. No resolve shader or settings
are changed by the draw-hook optimization.

The mod now logs `Draw setup/restore CPU` in `Mods/TemporalAA.log`. One frame in
60 is sampled and 30 completed samples are averaged. Native draw calls and
buffer-write hooks are outside this timing. GPU pass timings remain separate;
CPU and GPU timings overlap and must not simply be added into a frame cost.

Made with AI assistance. These measurements retain the existing visual settings.

The mod samples its GPU pass once every 60 executions using a four-slot timestamp-query ring. Results are polled with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; unfinished samples are skipped without waiting. Disjoint timestamps are rejected. Logs report averages of ten completed samples in milliseconds.

MoonandShadow writes `Mods/SteamShadows.log`: first is shadow-map generation; second is receiver application. TemporalAA writes `Mods/TemporalAA.log`: first is input copy and setup; second is resolve and output-to-history copy.

These measure GPU execution of the named passes, not total frame time, CPU capture overhead, or FPS. Sparse timestamps still have a small instrumentation cost. Compare the same location, camera, resolution, and settings after loading has settled. Test both enabled, shadows only (F9 off), TAA only (F7 off), and both disabled. Preserve ReShade settings throughout.

Projection uploads are reused when all 64 bytes are identical. Draw classification and projection/viewport inspection are shared between capture and jitter instead of repeated. Native perspective backgrounds share world jitter without becoming the world-history capture source.

`BlurPercent=12` blends a mild cross-neighborhood filter from samples already fetched by the resolve. `JitterPercent=75` reduces the eight-frame jitter amplitude without changing its sequence. Contrast-aware rejection on the edge-stability branch adds a small amount of arithmetic, reuses existing neighborhood bounds, and removes a duplicate center fetch. No extra texture samples, render targets or fullscreen passes are added.

## Edge-stability comparison, 2026-10-12

RX 6600, D3D11 hardware device, 1920 x 1080, HistoryPercent=65, BlurPercent=12 and JitterPercent=75. `TemporalStabilityTest --benchmark` measures the actual input copy, resolve and history copy on a controlled slanted-edge fixture. There are 24 warmup submissions and 80 timed samples per variant, with interleaved submission order. Scene uploads happen outside the timed intervals. Steam Cube World was running during this measurement.

| Resolver | Median GPU ms | Mean GPU ms | 95th percentile GPU ms |
| --- | ---: | ---: | ---: |
| Accepted main baseline (2e0d10b) | 0.1847 | 0.1961 | 0.2988 |
| Edge-stability candidate | 0.1884 | 0.1988 | 0.3025 |
| Candidate minus baseline | +0.0037 | +0.0027 | +0.0037 |

The difference is small in this run; it is not a guaranteed zero-cost change or a gameplay FPS comparison. The fixture excludes per-draw CPU hook overhead and native background rendering. The candidate keeps classification shared and projection uploads cached; live timing should confirm the complete game behavior.

The static slanted-edge test measures per-pixel variance across eight jitter phases after history warms up. WARP variance fell from 67.8996 to 14.7805 (78.23% lower); hardware results were similar. This measures a synthetic edge, not a percentage reduction of every in-game shimmer. Moving silhouettes leave no stale trail outside the current neighborhood in the test, and uniform colors remain unchanged.

After installing build 44b12e0, the user reported "Steadier, FPS similar" in the requested gameplay comparison. This confirms an initial visible improvement without a reported FPS regression. No exact FPS readings or matched gameplay timestamp measurements were supplied; the hardware fixture above remains the quantitative cost comparison. The tested DLL stays installed, and main remains unchanged.

Current gameplay logs for the accepted MoonandShadow build averaged 1.3324 ms GPU across 44 reported groups, including both shadow maps and receiver application. Those observations do not isolate the incremental cost of its latest model-shading correction: a comparison at the same camera and location would be required. The correction adds no pass and skips depth lookups on grazing model faces.

## Edge shimmer fix, 2026-10-13

The edge-stability resolve still rejected history whenever it differed from the current pixel by more than a contrast-scaled threshold. On a strong edge, the jitter alone changes the pixel by up to the full edge contrast each frame, so those edges dropped history and showed raw jittered coverage.

History is now rejected only by how far the neighborhood clamp had to move it. History inside the current 3x3 range is a valid jittered average, and history outside it is a real change. Edges and thin details accumulate up to `EdgeHistoryPercent` (default 90). Flat areas keep `HistoryPercent`. Current and history are blended with luminance weights (Karis), so bright samples do not dominate across jitter phases. No textures, passes or samples are added.

WARP static slanted-edge phase variance: 14.7805 (edge-stability) to 0.7492 (94.9% lower), and 67.8996 for the original main shader. The moving-silhouette (no stale trail beyond the neighborhood) and uniform-color checks pass unchanged. These are synthetic measurements; in-game appearance needs live confirmation.

## MSAA depth and history copies, 2026-10-15

Live logs showed about 1.8 ms per frame for the pass. `Tests/MsaaBenchmark` times one complete `apply()` on the game's configuration: RX 6600, D3D11 hardware device, 1920 x 1080, multisampled colour target and multisampled D24S8 depth, camera reprojection on, 120 timed frames.

With reprojection off, the pass took about 0.19 ms at every sample count. Nearly all the remaining cost was copying the full multisampled depth buffer each frame, because the game's depth target cannot be bound as a shader resource.

| Change | 4x MSAA median | 8x MSAA median |
| --- | ---: | ---: |
| Before | 1.204 ms | 2.019 ms |
| History rendered into a second history texture, then blitted to the scene; depth textures swapped instead of copied | 1.137 ms | 1.952 ms |
| Scene depth read in place (shader-readable depth target) | 0.359 ms | 0.531 ms |

- Depth targets are created typeless with a shader-resource binding, as ReShade does, and depth-stencil views get the matching typed format. When the depth is already readable, reprojection reads it in place and the full multisampled copy is skipped. Depth values, tests and stencil are unchanged; `Tests/ReadableDepthTest` covers the hooks.
- `ReadableDepth=0` keeps the previous copy path.
- The resolve's inputs and arithmetic are unchanged, so the image is the same: every MSAA sample receives the resolved colour, exactly what resolving the target back into history used to read.
- These are fixture timings, not gameplay FPS. Live `TemporalAA.log` timing should confirm them.
