"""Exercise the actual PulseEvent/Pulsed logic without threading the guest."""
from pathlib import Path
from native_test import run_native

root = Path(__file__).resolve().parents[1]
kh = (root / 'sdk/runtime/kernel/kernel.h').read_text()
obj = (root / 'sdk/runtime/kernel/objects.cpp').read_text()


def slice_from(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


object_type = slice_from(kh, 'enum class ObjType', 'struct Waiter;')
object = slice_from(kh, 'struct Object {', 'using ObjectRef')
waiter = slice_from(obj, 'struct Waiter {', 'namespace {')
pulse = slice_from(obj, 'static bool Pulsed(', 'static uint32_t WaitFor(')
pulse_event = slice_from(obj, 'int32_t PulseEvent(', 'int32_t ReleaseSemaphore(')
wake = slice_from(obj, 'static void Wake(', '// a Wait')

source = r'''
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
''' + object_type + waiter + object + r'''
std::mutex g_dispatch;
using ObjectRef = std::shared_ptr<Object>;
struct Queued {
    std::vector<uint64_t> pulse_seen;
};
''' + wake + pulse + pulse_event + r'''
extern "C" __declspec(dllexport) int run() {
    Object auto_event(ObjType::Event);
    auto_event.notification = false;
    Queued auto_waiter;
    auto_waiter.pulse_seen = {0};
    if (PulseEvent(auto_event) != 0) return 1;
    if (auto_event.signal != 0 || auto_event.pulse != 1) return 2;
    if (!Pulsed(auto_event, 0, auto_waiter)) return 3;
    if (Pulsed(auto_event, 0, auto_waiter)) return 4;

    Object manual(ObjType::Event);
    manual.notification = true;
    Queued manual_waiter;
    manual_waiter.pulse_seen = {0};
    PulseEvent(manual);
    if (!Pulsed(manual, 0, manual_waiter) || !Pulsed(manual, 0, manual_waiter)) return 5;

    Object late(ObjType::Event);
    late.notification = false;
    late.pulse = 2;
    Queued late_waiter;
    late_waiter.pulse_seen = {2};
    if (Pulsed(late, 0, late_waiter)) return 6;

    Object twice(ObjType::Event);
    twice.notification = false;
    twice.pulse = 2;
    Queued twice_waiter;
    twice_waiter.pulse_seen = {0};
    if (!Pulsed(twice, 0, twice_waiter) || !Pulsed(twice, 0, twice_waiter)) return 7;
    if (Pulsed(twice, 0, twice_waiter)) return 8;
    return 0;
}
'''
rc = run_native(source, prefix='tcnyc-pulse-', needs_crt=True)
assert rc == 0, f'PulseEvent regression {rc}'
print('PASS: auto-reset pulses release once, notification pulses release every existing waiter')
