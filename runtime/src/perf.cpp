#include "perf.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#include "rt.h"

namespace rt::perf {

namespace {

const char* const kNames[kWorkCount] = {"shader translate", "shader glslang", "shader driver", "shader wait",
                                        "texture decode", "texture upload"};
std::atomic<uint64_t> g_frame_ticks[kWorkCount];   // this frame
std::atomic<uint32_t> g_frame_count[kWorkCount];
std::atomic<uint64_t> g_total_ticks[kWorkCount];   // since the last summary
std::atomic<uint32_t> g_total_count[kWorkCount];
uint64_t g_last = 0, g_frames = 0;
// frame times since the last summary
uint64_t g_sum = 0, g_max = 0, g_min = ~0ull, g_n = 0, g_over34 = 0, g_over50 = 0, g_over100 = 0;

uint64_t freq() {
    static const uint64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return (uint64_t)q.QuadPart;
    }();
    return f;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* v = getenv("LETSGO_PERF");
        return v && *v && *v != '0';
    }();
    return on;
}

uint64_t ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

double ms(uint64_t t) { return (double)t * 1000.0 / (double)freq(); }

void add(Work w, uint64_t t) {
    g_frame_ticks[w] += t;
    g_frame_count[w]++;
}

void frame() {
    uint64_t now = ticks();
    uint64_t work[kWorkCount];
    uint32_t count[kWorkCount];
    for (int i = 0; i < kWorkCount; i++) {
        work[i] = g_frame_ticks[i].exchange(0);
        count[i] = g_frame_count[i].exchange(0);
        g_total_ticks[i] += work[i];
        g_total_count[i] += count[i];
    }
    g_frames++;
    if (!g_last) {
        g_last = now;
        return;
    }
    uint64_t dt = now - g_last;
    g_last = now;
    if (!enabled()) return;
    double d = ms(dt);
    g_sum += dt;
    g_max = std::max(g_max, dt);
    g_min = std::min(g_min, dt);
    g_n++;
    g_over34 += d > 34.5;
    g_over50 += d > 50;
    g_over100 += d > 100;
    if (d > 40) {
        std::string what;
        for (int i = 0; i < kWorkCount; i++)
            if (count[i]) {
                char buf[96];
                snprintf(buf, sizeof buf, ", %s %.1f ms (%u)", kNames[i], ms(work[i]), count[i]);
                what += buf;
            }
        log("perf: frame %llu took %.1f ms%s", (unsigned long long)g_frames, d, what.c_str());
    }
    if (g_n == 600) {
        std::string what;
        for (int i = 0; i < kWorkCount; i++) {
            uint32_t n = g_total_count[i].exchange(0);
            uint64_t t = g_total_ticks[i].exchange(0);
            if (n) {
                char buf[96];
                snprintf(buf, sizeof buf, ", %s %.0f ms (%u)", kNames[i], ms(t), n);
                what += buf;
            }
        }
        log("perf: last %llu frames: mean %.2f ms, min %.2f ms, max %.2f ms, over 34.5 ms %llu, over 50 ms %llu, over 100 ms %llu%s",
            (unsigned long long)g_n, ms(g_sum) / (double)g_n, ms(g_min), ms(g_max), (unsigned long long)g_over34,
            (unsigned long long)g_over50, (unsigned long long)g_over100, what.c_str());
        g_sum = g_max = g_n = g_over34 = g_over50 = g_over100 = 0;
        g_min = ~0ull;
    }
}

}  // namespace rt::perf
