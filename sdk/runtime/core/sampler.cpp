#include "core/sampler.h"

#if defined(_WIN32)

namespace sampler {
void StartCurrentThread(const char*) {}
void DumpAll() {}
}  // namespace sampler

#else  // Linux

#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <link.h>

#include "app/app.h"
#include "core/log.h"

namespace sampler {

namespace {

constexpr size_t kMax = 1 << 20;  // samples held per thread
constexpr int kPeriodMs = 1;
// glibc's SIGRTMIN is a function call, not a constant: resolve it once
int Signal() {
    static const int s = SIGRTMIN + 2;  // the render sampler owns SIGPROF
    return s;
}

struct Sink {
    const char* label = nullptr;
    std::atomic<size_t> count{0};
    std::unique_ptr<uint64_t[]> pcs{new uint64_t[kMax]};
    std::unique_ptr<uint64_t[]> callers{new uint64_t[kMax]};
    std::atomic<uint64_t> stack_lo{0}, stack_hi{0};
};

std::mutex g_lock;
std::vector<Sink*> g_sinks;
std::atomic<bool> g_handler_installed{false};
std::atomic<bool> g_reporter_started{false};
thread_local Sink* t_sink = nullptr;
std::atomic<uintptr_t> g_base{0};

void OnTick(int, siginfo_t*, void* ctx) {
    Sink* s = t_sink;
    if (!s) return;
    const size_t i = s->count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMax) return;
    const auto& r = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
    const uint64_t rbp = uint64_t(r[REG_RBP]), rsp = uint64_t(r[REG_RSP]);
    s->pcs[i] = uint64_t(r[REG_RIP]);
    // release builds omit frame pointers: follow RBP only when the caller slot is inside this thread's stack
    const uint64_t lo = s->stack_lo.load(std::memory_order_relaxed), hi = s->stack_hi.load(std::memory_order_relaxed);
    s->callers[i] = rbp >= rsp && rbp >= lo && rbp + 16 <= hi && !(rbp & 7)
                        ? *reinterpret_cast<const uint64_t*>(rbp + 8)
                        : 0;
}

int FindBase(dl_phdr_info* info, size_t, void*) {
    if (!g_base.load(std::memory_order_relaxed)) g_base.store(info->dlpi_addr);
    return 1;
}

void DumpSink(Sink& s) {
    if (!s.count.load(std::memory_order_relaxed)) return;
    const size_t n = std::min(s.count.load(std::memory_order_acquire), kMax);
    const uintptr_t base = g_base.load();
    // counts by address, relative to the executable, as the render sampler writes them
    std::unordered_map<uint64_t, uint32_t> counts;
    for (size_t i = 0; i < n; i++)
        if (s.pcs[i] > base) counts[s.pcs[i] - base]++;
    std::error_code ec;
    std::filesystem::create_directories(app::ExeDir() / "logs", ec);
    const std::string name = std::string("samples-") + s.label + ".txt";
    if (FILE* f = std::fopen((app::ExeDir() / "logs" / name).string().c_str(), "w")) {
        for (const auto& [pc, c] : counts) std::fprintf(f, "%llx %u\n", (unsigned long long)pc, c);
        std::fclose(f);
        LOGR("sampler: %s thread: %zu samples in %zu addresses, logs/%s", s.label, n, counts.size(), name.c_str());
    }
}

void Reporter() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard lock(g_lock);
        for (Sink* s : g_sinks) DumpSink(*s);
    }
}

}  // namespace

void StartCurrentThread(const char* label) {
    // read directly rather than through DevEnv: profiling a *release* build is the point, and DevEnv is
    // compiled out of those. This is the only diagnostic that deliberately keeps its switch there.
    if (!std::getenv("TCNYC_SAMPLE") || t_sink) return;
    if (!g_base.load()) dl_iterate_phdr(FindBase, nullptr);
    if (!g_handler_installed.exchange(true)) {
        struct sigaction sa {};
        sa.sa_sigaction = OnTick;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(Signal(), &sa, nullptr);
    }
    auto* sink = new Sink;
    sink->label = label;
    t_sink = sink;
    {
        std::lock_guard lock(g_lock);
        g_sinks.push_back(sink);
    }
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* lo = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &lo, &size) == 0) {
            sink->stack_lo.store(uint64_t(reinterpret_cast<uintptr_t>(lo)));
            sink->stack_hi.store(sink->stack_lo.load() + size);
        }
        pthread_attr_destroy(&attr);
    }
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return;
    sigevent ev {};
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = Signal();
    ev._sigev_un._tid = pid_t(syscall(SYS_gettid));
    timer_t timer;
    if (timer_create(clock, &ev, &timer) != 0) return;
    itimerspec spec {};
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = kPeriodMs * 1000000L;
    if (timer_settime(timer, 0, &spec, nullptr) != 0) return;
    LOGR("sampler: %s thread sampled every %d ms of its CPU time (TCNYC_SAMPLE)", label, kPeriodMs);
    if (!g_reporter_started.exchange(true)) std::thread(Reporter).detach();
}

void DumpAll() {
    if (!g_base.load()) return;
    std::lock_guard lock(g_lock);
    for (Sink* s : g_sinks) DumpSink(*s);
}

}  // namespace sampler

#endif
