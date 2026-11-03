# Possible optimisations

**First pass implemented:** profiler gating, allocation-free audio lane scratch, and persistent DXBC shader
bytecode caching. See [PERFORMANCE.md](PERFORMANCE.md) for measured cache reuse, regression results and the
limits of the replay comparison. Adapter-specific pipeline caching and the other candidates remain open.

Reviewed 2026-11-02 after the file-lifetime, timing and locker fixes. These are candidates found in the
source at review time, not measured speedup claims. The cleanup did not alter rendering quality, simulation
speed, audio rate, frame pacing or resource readiness. Existing historical performance logs span older
builds and should not be treated as a benchmark of the cleaned build.

The presentation interval deliberately limits this title to roughly 30 presented frames per second.
The useful targets are fewer hitches, lower CPU use, stable frame times and more GPU headroom, while
preserving the game's simulation timing. Removing the cap would invalidate that comparison.

## Suggested order

| Priority | Candidate and evidence in the code | Expected benefit | Constraints and validation |
| --- | --- | --- | --- |
| 1 | **Disable profiler work when profiling is off.** `sdk/runtime/gpu/render_thread.cpp` records clock timestamps and updates atomic counters in Draw/Clear/Copy/Present even without `diag::Perf()`. `BindGuestTexture` in `sdk/runtime/gpu/d3d12.cpp` also times signature checks unconditionally. Most other D3D12 timers already use a gated `PerfScope`. | Reduce CPU overhead on the many small draw calls without changing game output. | Gate both timer collection and counter updates consistently. Compare CPU frame time with profiling off; separately confirm enabled logs still work. No estimated gain until measured. |
| 2 | **Persist compiled shaders and pipeline state.** `D3D12Backend::Compile` caches blobs in memory but runs `D3DCompile` synchronously on a miss; pipeline creation also happens on demand. | Reduce first-encounter and repeat-launch hitches. | A disk cache needs source/profile/compiler-version keys, corruption fallback and pipeline adapter/driver invalidation. Compare cold/warm runs and shader/PSO creation counts. Keep synchronous correct fallback; do not skip a draw while compiling. |
| 3 | **Reduce repeated texture-change checks.** `BindGuestTexture` checks a signature once per texture per frame. `MemSig` hashes the whole allocation up to64 KiB and samples larger allocations, alongside dirty epochs. | Less CPU memory traffic in texture-heavy street scenes. | First measure signature time and checked bytes. Epoch-only invalidation is unsafe while the title can write guest memory directly. Any stronger write tracking must cover those writes, aliasing, palette changes, text, movie frames and render-target views. |
| 4 | **Reuse audio mixing scratch storage.** `AudibleLanes` constructs and grows a new vector for each audible voice on every mix block. `Render` runs under the shared sound mutex, so allocations also extend time spent holding that lock. | Fewer small allocations and less audio-thread jitter. | Reuse per-voice capacity or a bounded scratch array while preserving gain ramps, packet completion order, DSP routing and channel behavior. Validate output samples and callback order, then measure underruns and mix-block time. Do not change the48 kHz clock. |
| 5 | **Track dirty render state more precisely.** `Threaded::SendState` compares state ranges in64-byte pieces; `UploadConstants` compares the full192-register vertex constant bank to reuse the last upload. | Reduce repeated state comparisons and copies across many similar draws. | Existing delta sending and constant reuse already avoid some work. Add generation/dirty-range tracking only if comparison cost is material, covering HLE setters and direct push-buffer writes. Validate capture/state equivalence, including immediate attributes and shader defaults. |
| 6 | **Reduce avoidable readback synchronisation.** `Threaded::ReadBack` can submit and wait for a GPU-written surface; resource waits and presentation also constrain overlap. Existing code already tracks writes and supports late copies. | Improve CPU/GPU overlap where streaming or locks force waits. | Use the existing per-kind wait counters to identify actual waits first. Preserve Lock/NOOVERWRITE/resource/fence semantics and guest-visible data freshness. Do not broadly remove waits or delay callbacks. Recheck road masks/reflections, locker rendering and streaming transitions. |
| 7 | **Cache vertex declaration metadata.** `RetainVertexAttributes` resolves a layout and decodes the last submitted vertex; assembly separately collects inputs and scans indexed ranges. | Reduce repeated layout work and, where inputs are immutable, index scanning. | The last submitted vertex must still update retained attributes, including indexed draws and base_vertex. The title and render worker operate at different times, so sharing mutable decoded data casually would introduce races. Cache only with resource/layout versions and preserve the attribute regression. |
| 8 | **Measure upload-ring pressure before changing allocation policy.** `Upload` uses a64 MiB per-frame ring, creates committed overflow buffers and can force submission when little space remains. Counters already expose uploaded bytes and overflow. | Reduce allocation/submission hitches if large scenes actually exhaust the ring. | Do not simply increase memory budgets. First establish overflow frequency and allocation lifetime; then consider pooled overflow buffers or appropriately sized rings with fence-safe reuse. Track peak GPU/host memory as well as frame times. |

## Baseline and acceptance

1. Use the cleaned build with ordinary gameplay timing. Disable deterministic replay, F3 captures and
   controller recording for the baseline; the convenience launcher currently enables recording.
2. Capture several runs of the intro, a busy street, the locker and the former parking-ramp route. Compare
   similar weather/time of day when assessing rendering. Record CPU/GPU time, frame-time percentiles,
   shader misses, texture-check/upload cost, wait categories, allocation counts and audio underruns.
3. Use `XBE_PERF=1` with `XBE_VERBOSE=1` for existing CPU-side breakdowns. The current counters do not
   provide a full GPU execution-time breakdown; use a graphics profiler or GPU timestamps for that.
4. Implement one candidate at a time, run `scripts/run_checks.py`, compare captures and repeat the relevant
   gameplay route. Ordinary input replay is useful, but asynchronous streaming means it is not fully deterministic.
5. Keep the change only if measurements improve and compatibility checks still pass. Never substitute
   missing-resource guards, reduced scene content or altered game speed for an optimisation.

My first implementation choice would be **gating unused profiler work**, followed by **shader-cache
persistence** if cold-run captures show compilation hitches. Use measurements to choose between texture,
audio and synchronisation work after that.
