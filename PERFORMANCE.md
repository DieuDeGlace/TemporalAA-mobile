# GPU performance measurements

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
