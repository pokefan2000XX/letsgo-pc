#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>

#include "rt.h"

namespace rt {

int g_verbose = 0;
static FILE* g_log;
static std::mutex g_log_lock;
static uint64_t g_t0;

void log_init(const std::string& path) {
    g_log = fopen(path.c_str(), "w");
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_t0 = (uint64_t)t.QuadPart;
}

double uptime() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)((uint64_t)t.QuadPart - g_t0) / (double)f.QuadPart;
}

static void vlog(const char* prefix, const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    double sec = uptime();
    std::lock_guard<std::mutex> lk(g_log_lock);
    fprintf(stderr, "[%9.4f %5lu] %s%s\n", sec, GetCurrentThreadId(), prefix, buf);
    if (g_log) {
        fprintf(g_log, "[%9.4f %5lu] %s%s\n", sec, GetCurrentThreadId(), prefix, buf);
        fflush(g_log);
    }
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

static std::mutex g_act_lock;
static std::deque<std::string> g_act;

void activity(const char* fmt, ...) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%7.2f  ", uptime());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_act_lock);
    g_act.push_back(buf);
    if (g_act.size() > 64) g_act.pop_front();
}

std::vector<std::string> recent_activity(size_t n) {
    std::lock_guard<std::mutex> lk(g_act_lock);
    size_t k = g_act.size() < n ? g_act.size() : n;
    return std::vector<std::string>(g_act.end() - (long)k, g_act.end());
}

static void (*g_fatal_hook)(const char*);
void set_fatal_hook(void (*hook)(const char*)) { g_fatal_hook = hook; }

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    fflush(stderr);
    if (g_fatal_hook) g_fatal_hook(buf);
    TerminateProcess(GetCurrentProcess(), 3);
    __builtin_unreachable();
}

}  // namespace rt
