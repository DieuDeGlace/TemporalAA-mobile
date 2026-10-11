# GPU performance measurements

Made with AI assistance. These measurements retain the existing visual settings.

The mod samples its GPU pass once every 60 executions using a four-slot timestamp-query ring. Results are polled with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; unfinished samples are skipped without waiting. Disjoint timestamps are rejected. Logs report averages of ten completed samples in milliseconds.

MoonandShadow writes `Mods/SteamShadows.log`: first is shadow-map generation; second is receiver application. TemporalAA writes `Mods/TemporalAA.log`: first is input copy and setup; second is resolve and output-to-history copy.

These measure GPU execution of the named passes, not total frame time, CPU capture overhead, or FPS. Sparse timestamps still have a small instrumentation cost. Compare the same location, camera, resolution, and settings after loading has settled. Test both enabled, shadows only (F9 off), TAA only (F7 off), and both disabled. Preserve ReShade settings throughout.

Projection uploads are reused when all 64 bytes are identical. World-draw classification and projection/viewport inspection are shared between capture and jitter instead of repeated. The fallback path remains for draws outside normal capture.

`BlurPercent=12` blends a mild cross-neighborhood filter from samples already fetched by the resolve. `JitterPercent=75` reduces the eight-frame jitter amplitude without changing its sequence. No extra texture samples or fullscreen passes are added. GPU tests cover upload reuse, changed projection readback, equivalent preparation paths, blur, uniform colors, history rejection, resizing, and state restoration. Actual FPS improvement still requires a game comparison.
