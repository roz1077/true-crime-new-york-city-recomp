#pragma once

// Sampling profiler for a thread other than the render thread. The render thread has its own sampler in
// the backends (TCNYC_SAMPLE -> logs/samples.txt); this one attaches to the calling thread and writes
// logs/samples-<label>.txt in the same "pc count" form, so out/resolve_samples.py reads it unchanged.
//
// Linux: a SIGRTMIN+2 thread timer, so it never collides with the render sampler's SIGPROF. The handler
// records the interrupted instruction (and its caller through the frame pointer when the build keeps one).
// Windows: a no-op for now; the render-thread sampler there has the mechanism, this needs its own refactor.
namespace sampler {

// attaches to the calling thread; no-op unless TCNYC_SAMPLE is set and the thread is already sampling
void StartCurrentThread(const char* label);

// writes every sampling thread's counts now (also done every 10 s from a reporter thread)
void DumpAll();

}  // namespace sampler
