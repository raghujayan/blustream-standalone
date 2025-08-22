# TODOs · blustream-standalone

This document tracks performance work across the numbered steps.  
Each step has a **DONE** (implemented) and **TODO** (remaining) section.  
Move items from TODO → DONE as changes land in `main`.

---

## Perf Step #1 — Disable Debug File I/O in Hot Paths

### ✅ DONE
- Centralized flag & macros in `debug_config.h` (`DebugConfig` + atomics).
- Gated raw `.h264` and decoded `.ppm` writes in client hot paths:
  - `process_frame(...)` and `process_decoded_frame(...)`.
- Default behavior: **no disk writes** unless `BLUSTREAM_DEBUG_IO=1`.
- Added end-of-run counters and `DEBUG_IO_PERFORMANCE.md` usage notes.

### 🚧 TODO
- **Env init once (no hot-path getenv):** Ensure `BLUSTREAM_DEBUG_IO` is parsed once in `DebugConfig` ctor and cached.
- **Release safety guard:** Add `-DBLUSTREAM_RELEASE_BUILD=1` for release targets; force debug I/O OFF at compile time in release.
- **Perf HUD integration:** When Step #14 lands, display `blocked / permitted / opportunities` and % reductions.
- **CI zero-file assertion:** Headless run with `--save-frames` and no env var → assert output dir empty; with `BLUSTREAM_DEBUG_IO=1` → assert files exist.

---

## Perf Step #2 — Enable Hardware Video Decode & Frame Threading

### ✅ DONE
- _None yet_

### 🚧 TODO
- Detect OS and enable HW accel:
  - macOS: `videotoolbox`; Windows: `d3d11va`; Linux: `vaapi` (fallback to SW).
- Set FFmpeg options at codec-open: `threads=auto`, slice/threading if supported.
- Add `HW_DECODE=auto|off|force` runtime option; log chosen path.
- Telemetry: per-frame **decode ms** and process **CPU%**.

---

## Perf Step #3 — Avoid CPU YUV→RGB in Hot Path

### ✅ DONE
- _None yet_

### 🚧 TODO
- Remove CPU-side `sws_scale` conversions in decode/display path.
- Upload YUV planes to GPU; convert in fragment shader (multi-plane textures).
- Keep a fallback build flag to compare old/new path.
- Metrics: copy bytes/frame, conversion ms, upload ms.

---

## Perf Step #4 — Render Ring Buffer with Frame Skipping

### ✅ DONE
- _None yet_

### 🚧 TODO
- Add 3–5 frame circular buffer; render newest (drop stale).
- Counters: `frames_received`, `frames_rendered`, `frames_dropped`.
- Preference to disable dropping (QA mode).

---

## Perf Step #5 — Debounce Expensive Camera Operations

### ✅ DONE
- _None yet_

### 🚧 TODO
- Call `resetCamera()` only after dataset/geometry changes.
- Debounce auto-fit/bounds calculations (≈250–300 ms after interaction end).
- Verify identical visual behavior; fewer long frames.

---

## Perf Step #6 — One-Time vtk.js Pipeline, Zero Object Churn

### ✅ DONE
- _None yet_

### 🚧 TODO
- Create renderer, imageData, volume/mapper once in `init()`.
- Persist `vtkDataArray`; update via `setData(new TypedArray(buffer))`.
- Use `imageData.modified()`; avoid re-creating TFs/mappers.

---

## Perf Step #7 — Adaptive Sampling for Volume Raycasting

### ✅ DONE
- _None yet_

### 🚧 TODO
- On `onStartAnimation`: `mapper.setImageSampleDistance(≈2.0)`.
- On `onEndAnimation`: restore to `≈0.9–1.0`; optional `prop.setShade(false/true)`.
- Expose tunables in a dev config.

---

## Perf Step #8 — Slice Rendering During Navigation (Huge Volumes)

### ✅ DONE
- _None yet_

### 🚧 TODO
- Two modes: `sliceMode` (3× `vtkImageSlice`) vs `volumeMode` (raycast).
- Switch to slices on interaction; revert to volume after ~250 ms idle.
- Maintain TF/opacity parity across modes.

---

## Perf Step #9 — Quantize on the Wire (8–12 bits) + Percentiles

### ✅ DONE
- _None yet_

### 🚧 TODO
- Producer: compute p2/p98; map to `Uint8`/`Uint16`; attach `{scaleMin, scaleMax}`.
- Client: set TF domain from metadata; symmetric TF around 0.
- Provide a “precision” toggle to request float32 for analysis.

---

## Perf Step #10 — Symmetric Seismic Transfer Functions + Throttling

### ✅ DONE
- _None yet_

### 🚧 TODO
- Define symmetric color/opacity curves about amplitude 0.
- Throttle TF updates to ≤10 Hz; commit final on pointer up.
- Mutate TF control points vs. re-creating objects.

---

## Perf Step #11 — Correct Grid Metadata (Spacing/Origin/Directions)

### ✅ DONE
- _None yet_

### 🚧 TODO
- Apply inline/xline/z spacing + origin from survey metadata.
- Ensure consistent axis directions (document convention).
- Single `resetCamera()` after geometry is set.

---

## Perf Step #12 — WebGL Canvas Tuning (AA/DPR/Power Pref)

### ✅ DONE
- _None yet_

### 🚧 TODO
- Canvas init: `{ antialias:false, preserveDrawingBuffer:false, powerPreference:'high-performance' }`.
- Auto-cap DPR to 1 if FPS < target; size canvas to display pixels.
- Provide user toggle for high-quality screenshots.

---

## Perf Step #13 — ESM + Tree-Shaking vtk.js Profiles

### ✅ DONE
- _None yet_

### 🚧 TODO
- Replace broad imports with `Rendering/Profiles/Volume` (or `/Image`).
- Remove unused modules; verify treeshaking; record bundle size delta.

---

## Perf Step #14 — Lightweight Perf HUD (Decode/Upload/Render/FPS)

### ✅ DONE
- _None yet_

### 🚧 TODO
- Implement `PerfHUD` with per-stage ms and FPS.
- Toggle via `?perf=1`; keep overhead <0.5 ms/frame.
- Integrate Step #1 counters (blocked/permitted/opportunities).

---

## Perf Step #15 — Spector.js Capture (Dev Only)

### ✅ DONE
- _None yet_

### 🚧 TODO
- `?capture=1` loads Spector and captures one steady-state frame.
- Console instructions to open Spector UI; exclude from prod bundles.

---

## Perf Step #16 — Crop Boxes / Clipping to Shorten Rays

### ✅ DONE
- _None yet_

### 🚧 TODO
- ROI UI (drag box / numeric inputs).
- `mapper.setCroppingRegionPlanes([xmin,xmax,ymin,ymax,zmin,zmax])`.
- “Reset crop” action; persist ROI across mode switches.

---

## Perf Step #17 — Decode in Browser Using WebCodecs (Optional Path)

### ✅ DONE
- _None yet_

### 🚧 TODO
- Implement `VideoDecoder` output → direct WebGL texture (avoid `createImageBitmap()`).
- Transport via WebRTC DataChannel/WebTransport; feature-detect and fallback.
- Telemetry: decode latency and CPU% vs. existing path.

---

## Perf Step #18 — Guardrail Tests & CI Checks

### ✅ DONE
- _None yet_

### 🚧 TODO
- Headless perf test on representative datasets (e.g., 128³ / 256³); assert avg frame time & p95 thresholds.
- Assert `DEBUG_IO` is OFF by default and that no files are written in default runs.
- Publish perf artifacts for trend charts.

---

### Notes
- When you complete any TODO, move it up to the corresponding **DONE** list and link the PR/commit.
- Keep this file short and actionable; long design docs belong in separate markdowns.
