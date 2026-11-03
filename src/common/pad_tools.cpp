// Optional scripted input and frame-indexed controller recording/replay.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pad_tools.h"
#include "xdk/d3d8.h"

namespace {

// XBE_PAD_SCRIPT=<seconds>:<button>[,<seconds>:<button>...] presses a button on
// controller 1 for 0.2 s at each time (from the first poll), to reach a scene
// without someone at the controller. Buttons: a b x y start back up down left
// right. Controller 1 also counts as plugged in while a script is set.
struct Press {
    double at;
    uint16_t buttons;
    int analog; // -1 for none
};

std::vector<Press> LoadScript() {
    std::vector<Press> out;
    const char *v = std::getenv("XBE_PAD_SCRIPT");
    if (!v) return out;
    struct Name {
        const char *name;
        uint16_t buttons;
        int analog;
    };
    constexpr Name kNames[] = {{"up", 1, -1},    {"down", 2, -1}, {"left", 4, -1}, {"right", 8, -1}, {"start", 16, -1},
                               {"back", 32, -1}, {"a", 0, 0},     {"b", 0, 1},     {"x", 0, 2},      {"y", 0, 3}};
    std::string s = v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            double at = std::atof(item.c_str());
            std::string b = item.substr(colon + 1);
            for (const Name &n : kNames)
                if (b == n.name) out.push_back({at, n.buttons, n.analog});
        }
        pos = end + 1;
    }
    return out;
}

// Full controller recording and replay, timed by presented pictures
// (d3d::PresentCount) rather than seconds, so a slower or faster load does not
// shift the inputs that follow it. XBE_PAD_RECORD=<file>: writes "pad2" then,
// whenever controller 1 changes, one line
//   <picture> <buttons hex> <8 analog buttons> <lx> <ly> <rx> <ry>
// XBE_PAD_REPLAY=<file>: plays such a file back on controller 1 (the state
// holds until the next line); the real controller is ignored while it runs and
// gets back control after the last line plus 2 s.
struct PadState {
    uint32_t picture;
    input::Pad pad;
};

bool SameState(const input::Pad &a, const input::Pad &b) {
    return a.buttons == b.buttons && !std::memcmp(a.analog, b.analog, 8) && a.lx == b.lx && a.ly == b.ly &&
           a.rx == b.rx && a.ry == b.ry;
}

void Record(const input::Pad &p) {
    static FILE *f = [] {
        const char *v = std::getenv("XBE_PAD_RECORD");
        FILE *out = v ? std::fopen(v, "w") : nullptr;
        if (out) std::fputs("pad2\n", out);
        return out;
    }();
    static input::Pad last;
    static bool first = true;
    if (!f || (!first && SameState(p, last))) return;
    first = false;
    last = p;
    std::fprintf(f, "%u %04X", d3d::PresentCount(), p.buttons);
    for (int i = 0; i < 8; i++)
        std::fprintf(f, " %u", p.analog[i]);
    std::fprintf(f, " %d %d %d %d\n", p.lx, p.ly, p.rx, p.ry);
    std::fflush(f);
}

std::vector<PadState> LoadReplay() {
    std::vector<PadState> out;
    const char *v = std::getenv("XBE_PAD_REPLAY");
    FILE *f = v ? std::fopen(v, "r") : nullptr;
    if (!f) return out;
    char line[256];
    if (!std::fgets(line, sizeof(line), f) || std::strncmp(line, "pad2", 4)) {
        std::fclose(f);
        return out;
    }
    while (std::fgets(line, sizeof(line), f)) {
        PadState st{};
        unsigned buttons, a[8];
        int lx, ly, rx, ry;
        if (std::sscanf(line, "%u %x %u %u %u %u %u %u %u %u %d %d %d %d", &st.picture, &buttons, &a[0], &a[1], &a[2],
                        &a[3], &a[4], &a[5], &a[6], &a[7], &lx, &ly, &rx, &ry) != 14)
            continue;
        st.pad.connected = true;
        st.pad.buttons = uint16_t(buttons);
        for (int i = 0; i < 8; i++)
            st.pad.analog[i] = uint8_t(a[i]);
        st.pad.lx = int16_t(lx), st.pad.ly = int16_t(ly), st.pad.rx = int16_t(rx), st.pad.ry = int16_t(ry);
        out.push_back(st);
    }
    std::fclose(f);
    return out;
}

} // namespace

void game::ApplyPadTools(int port, input::Pad &p) {
    static const std::vector<Press> script = LoadScript();
    static const std::vector<PadState> replay = LoadReplay();
    static const auto start = std::chrono::steady_clock::now();
    if (port != 0) return;
    if (!replay.empty()) {
        const uint32_t now = d3d::PresentCount();
        if (now <= replay.back().picture + 120) { // 2 s at 60 pictures past the last change
            size_t lo = 0, hi = replay.size();
            while (hi - lo > 1) {
                size_t mid = (lo + hi) / 2;
                (replay[mid].picture <= now ? lo : hi) = mid;
            }
            p = replay[lo].picture <= now ? replay[lo].pad : input::Pad{}; // before the first line: not plugged in yet
            return;
        }
    }
    if (!script.empty()) {
        p.connected = true;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (const Press &press : script) {
            if (t < press.at || t >= press.at + 0.2) continue;
            p.buttons |= press.buttons;
            if (press.analog >= 0) p.analog[press.analog] = 0xFF;
        }
    }
    if (p.connected) Record(p); // what the game gets, scripted presses included
}
