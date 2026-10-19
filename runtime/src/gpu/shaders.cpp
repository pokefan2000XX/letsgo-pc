#include "shaders.h"

#include <io.h>
#include <share.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../perf.h"
#include "../rt.h"
#include "gpu.h"
#include "maxwell.h"
#include "spirv.h"

// a hash of the translator's sources (CMakeLists.txt): translations from another version aren't used
#ifndef LETSGO_SHADER_VERSION
#define LETSGO_SHADER_VERSION 0ull
#endif

namespace gpu {

using rt::log;

namespace {

struct Hash {
    uint64_t a, b;
    bool operator==(const Hash& o) const { return a == o.a && b == o.b; }
};
struct HashOf {
    size_t operator()(const Hash& h) const { return (size_t)(h.a ^ h.b * 0x9E3779B97F4A7C15ull); }
};

uint64_t rotl(uint64_t x, int r) { return x << r | x >> (64 - r); }
// MurmurHash3's 64-bit lane, 8 bytes per step
uint64_t step(uint64_t h, uint64_t v) {
    v *= 0x87C37B91114253D5ull;
    v = rotl(v, 31);
    v *= 0x4CF5AD432745937Full;
    h ^= v;
    return rotl(h, 27) * 5 + 0x52DCE729;
}
uint64_t finish(uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    return h ^ h >> 33;
}
uint64_t hash_bytes(uint64_t h, const uint8_t* p, size_t n) {
    h = step(h, n);
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = step(h, v);
    }
    uint64_t v = 0;
    memcpy(&v, p, n);
    return step(h, v);
}

// the cache file: a header, then a record per variant in the order they were first built
constexpr char kMagic[4] = {'L', 'G', 'S', 'C'};
struct FileHeader {
    char magic[4];
    uint32_t format;
    uint64_t version;
    uint32_t refl_size;
    uint32_t reserved;
};
struct RecordHeader {
    Hash source;
    uint32_t key;
    uint32_t words;          // SPIR-V words after the reflection
};

}  // namespace

struct ShaderBuild : std::enable_shared_from_this<ShaderBuild> {
    Hash id{}, source{};
    uint32_t key = 0;
    std::shared_ptr<ShaderSource> src;       // null for a variant only the cache has seen so far
    std::mutex m;
    std::condition_variable cv;
    // Queued: waiting for a worker or a draw; Working: one thread is translating or building it
    enum State { Queued, Working, Done } state = Queued;
    bool from_cache = false, urgent = false;
    bool have_refl = false;
    nvasm::Reflection refl;
    std::string glsl, error;
    std::vector<uint32_t> spirv;             // until the shader object exists
    VkShaderEXT sh = VK_NULL_HANDLE;
};

namespace {

std::string g_path;
bool g_keep_glsl = false;
std::once_flag g_init;
bool g_cache_on = false;
std::mutex g_lock;                                                        // the maps and queues
std::unordered_map<Hash, std::shared_ptr<ShaderBuild>, HashOf> g_builds;  // by variant
std::unordered_map<Hash, std::vector<uint32_t>, HashOf> g_keys;           // by source: its variants
std::deque<std::shared_ptr<ShaderBuild>> g_now, g_warm;                   // for the workers
std::condition_variable g_wake;
std::mutex g_file_lock;
FILE* g_file = nullptr;
std::atomic<int> g_failures{0};

bool debug_fs() {
    static const bool on = getenv("LETSGO_DEBUG_FS") != nullptr;
    return on;
}

Hash variant_id(const Hash& source, uint32_t key) {
    return {finish(step(source.a, key)), finish(step(source.b, (uint64_t)key << 32 | 0x5A5A))};
}

std::wstring wide(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), n);
    return w;
}

FILE* open_utf8(const std::string& path, const wchar_t* mode, int share) { return _wfsopen(wide(path).c_str(), mode, share); }

// the cache's variants, queued to create their shader objects in the background
void load_cache() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(wide(g_path)).parent_path(), ec);
    std::vector<uint8_t> data;
    if (FILE* f = open_utf8(g_path, L"rb", _SH_DENYNO)) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            data.resize((size_t)size);
            data.resize(fread(data.data(), 1, data.size(), f));
        }
        fclose(f);
    }
    FileHeader want{{kMagic[0], kMagic[1], kMagic[2], kMagic[3]}, 1, LETSGO_SHADER_VERSION, sizeof(nvasm::Reflection), 0};
    bool valid = data.size() >= sizeof want && !memcmp(data.data(), &want, sizeof want);
    size_t good = valid ? sizeof want : 0;
    int loaded = 0;
    bool warm = !getenv("LETSGO_SHADER_WARMUP") || atoi(getenv("LETSGO_SHADER_WARMUP")) != 0;
    if (valid)
        while (good + sizeof(RecordHeader) + sizeof(nvasm::Reflection) <= data.size()) {
            RecordHeader h;
            memcpy(&h, data.data() + good, sizeof h);
            size_t end = good + sizeof h + sizeof(nvasm::Reflection) + (size_t)h.words * 4;
            if (h.words == 0 || h.words > (64u << 20) || end > data.size()) break;
            Hash id = variant_id(h.source, h.key);
            auto& slot = g_builds[id];
            if (!slot) {
                slot = std::make_shared<ShaderBuild>();
                slot->id = id;
                slot->source = h.source;
                slot->key = h.key;
                slot->from_cache = slot->have_refl = true;
                memcpy(&slot->refl, data.data() + good + sizeof h, sizeof(nvasm::Reflection));
                const uint8_t* words = data.data() + good + sizeof h + sizeof(nvasm::Reflection);
                slot->spirv.resize(h.words);
                memcpy(slot->spirv.data(), words, (size_t)h.words * 4);
                g_keys[h.source].push_back(h.key);
                if (warm) g_warm.push_back(slot);
                loaded++;
            }
            good = end;
        }
    // append from here on; another instance of the game that has the file open only reads it
    g_file = open_utf8(g_path, valid ? L"r+b" : L"wb", _SH_DENYWR);
    if (!g_file) {
        log("gpu: shader cache %s: %d shaders (read-only: in use elsewhere)", g_path.c_str(), loaded);
        return;
    }
    if (!valid) {
        fwrite(&want, sizeof want, 1, g_file);
        good = sizeof want;
    } else if (good < data.size()) {
        _chsize_s(_fileno(g_file), (long long)good);    // a record cut short (the game was closed mid-write)
    }
    fseek(g_file, (long)good, SEEK_SET);
    log("gpu: shader cache %s: %d shaders", g_path.c_str(), loaded);
}

void append(const ShaderBuild& b, const std::vector<uint32_t>& spv) {
    std::lock_guard<std::mutex> lk(g_file_lock);
    if (!g_file) return;
    RecordHeader h{b.source, b.key, (uint32_t)spv.size()};
    fwrite(&h, sizeof h, 1, g_file);
    fwrite(&b.refl, sizeof b.refl, 1, g_file);
    fwrite(spv.data(), 4, spv.size(), g_file);
    fflush(g_file);
}

// translates the variant: its reflection and GLSL. By the thread that has it Working
bool translate(ShaderBuild& b) {
    std::shared_ptr<ShaderSource> src;
    {
        std::lock_guard<std::mutex> lk(b.m);
        if (!b.glsl.empty()) return true;
        if (!b.error.empty()) return false;
        src = b.src;
    }
    nvasm::Result r;
    if (!src) {
        r.error = "no source to translate";
    } else {
        rt::perf::Scope t(rt::perf::ShaderTranslate);
        nvasm::Options opt;
        if (src->maxwell) {
            opt.uint_inputs = b.key & 0xFFFF;
            opt.sint_inputs = b.key >> 16;
            r = maxwell::translate(src->control.data(), src->control.size(), src->code.data(), src->code.size(), opt);
        } else {
            opt.sint_inputs = b.key;
            r = nvasm::translate(src->control.data(), src->control.size(), opt);
        }
    }
    // LETSGO_DEBUG_FS=1: every fragment program writes magenta (shows what rasterizes)
    if (r.ok && debug_fs() && src->stage == 1 && r.glsl.find("out vec4 fo_0;") != std::string::npos) {
        size_t end = r.glsl.rfind('}');
        if (end != std::string::npos) r.glsl.insert(end, "    fo_0 = vec4(1.0, 0.0, 1.0, 1.0);\n");
        for (size_t at = 0; (at = r.glsl.find("return;", at)) != std::string::npos; at += 40)
            r.glsl.replace(at, 7, "{ fo_0 = vec4(1.0, 0.0, 1.0, 1.0); return; }");
    }
    std::lock_guard<std::mutex> lk(b.m);
    if (!r.ok) {
        b.error = r.error.empty() ? "translation failed" : r.error;
    } else {
        b.refl = r.refl;
        b.have_refl = true;
        b.glsl = std::move(r.glsl);
    }
    b.cv.notify_all();
    return r.ok;
}

// translation, glslang and the shader object, whichever are still missing. By the Working thread
void build(ShaderBuild& b) {
    if (b.spirv.empty()) {
        if (!translate(b)) {
            if (g_failures++ < 20) log("gpu: shader translation failed: %s", b.error.c_str());
            return;
        }
        std::vector<uint32_t> spv;
        std::string msg;
        bool ok;
        {
            rt::perf::Scope t(rt::perf::ShaderCompile);
            ok = compile_glsl(b.refl.stage, b.glsl, spv, msg);
        }
        if (!ok) {
            if (g_failures++ < 20) log("gpu: shader compile failed: %s", msg.c_str());
            std::lock_guard<std::mutex> lk(b.m);
            b.error = "compile failed";
            return;
        }
        if (g_cache_on) append(b, spv);
        std::lock_guard<std::mutex> lk(b.m);
        b.spirv = std::move(spv);
        if (!g_keep_glsl) std::string().swap(b.glsl);
    }
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.stage = b.refl.stage == nvasm::Stage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
               : b.refl.stage == nvasm::Stage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                        : VK_SHADER_STAGE_COMPUTE_BIT;
    ci.nextStage = b.refl.stage == nvasm::Stage::Vertex ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = b.spirv.size() * 4;
    ci.pCode = b.spirv.data();
    ci.pName = "main";
    VkDescriptorSetLayout sets[2] = {g.set0_layout, g.set1_layout};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    VkShaderEXT sh = VK_NULL_HANDLE;
    VkResult res;
    {
        rt::perf::Scope t(rt::perf::ShaderDriver);
        res = vk::vkCreateShadersEXT(g.dev, 1, &ci, nullptr, &sh);
    }
    if (res != VK_SUCCESS) log("gpu: vkCreateShadersEXT failed: %s", vk::result_name(res));
    std::lock_guard<std::mutex> lk(b.m);
    b.sh = res == VK_SUCCESS ? sh : VK_NULL_HANDLE;
    std::vector<uint32_t>().swap(b.spirv);
}

void worker() {
    for (;;) {
        std::shared_ptr<ShaderBuild> b;
        bool warm = false;
        {
            std::unique_lock<std::mutex> lk(g_lock);
            g_wake.wait(lk, [] { return !g_now.empty() || !g_warm.empty(); });
            if (!g_now.empty()) {
                b = std::move(g_now.front());
                g_now.pop_front();
            } else {
                b = std::move(g_warm.front());
                g_warm.pop_front();
                warm = true;
            }
        }
        {
            std::lock_guard<std::mutex> lk(b->m);
            if (b->state != ShaderBuild::Queued) continue;      // done, or taken by a draw
            b->state = ShaderBuild::Working;
        }
        // shaders the game asked for compete with its threads; the warm-up only takes idle time
        SetThreadPriority(GetCurrentThread(), warm ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
        build(*b);
        {
            std::lock_guard<std::mutex> lk(b->m);
            b->state = ShaderBuild::Done;
        }
        b->cv.notify_all();
    }
}

void shaders_init() {
    std::call_once(g_init, [] {
        g_cache_on = !g_path.empty() && !g_keep_glsl && !debug_fs();
        if (g_cache_on) load_cache();
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = std::clamp(hw / 2, 2u, 6u);
        for (unsigned i = 0; i < n; i++) std::thread(worker).detach();
        log("gpu: %u shader build threads", n);
    });
}

}  // namespace

void shaders_set_cache(const std::string& path) { g_path = path; }
void shaders_keep_glsl() { g_keep_glsl = true; }

std::shared_ptr<ShaderSource> make_source(int stage, bool maxwell, const uint8_t* control, size_t control_size,
                                         const uint8_t* code, size_t code_size) {
    auto s = std::make_shared<ShaderSource>();
    s->stage = stage;
    s->maxwell = maxwell;
    s->control.assign(control, control + control_size);
    if (code) s->code.assign(code, code + code_size);
    for (int i = 0; i < 2; i++) {
        uint64_t h = (uint64_t)LETSGO_SHADER_VERSION ^ (i ? 0x6A09E667F3BCC909ull : 0xBB67AE8584CAA73Bull);
        h = step(h, (uint64_t)stage | (uint64_t)maxwell << 8);
        h = hash_bytes(h, s->control.data(), s->control.size());
        h = hash_bytes(h, s->code.data(), s->code.size());
        s->hash[i] = finish(h);
    }
    return s;
}

std::shared_ptr<ShaderBuild> shader_build(const std::shared_ptr<ShaderSource>& src, uint32_t key, bool prefetch) {
    shaders_init();
    Hash source{src->hash[0], src->hash[1]};
    std::shared_ptr<ShaderBuild> result;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto get = [&](uint32_t k) {
            Hash id = variant_id(source, k);
            auto& slot = g_builds[id];
            if (!slot) {
                slot = std::make_shared<ShaderBuild>();
                slot->id = id;
                slot->source = source;
                slot->key = k;
                slot->src = src;
                auto& keys = g_keys[source];
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
                g_now.push_back(slot);
                return slot;
            }
            {
                std::lock_guard<std::mutex> l2(slot->m);
                if (!slot->src) slot->src = src;
                if (slot->from_cache && !slot->urgent && slot->state == ShaderBuild::Queued) {
                    slot->urgent = true;
                    g_now.push_back(slot);                      // needed now: ahead of the warm-up
                }
            }
            return slot;
        };
        result = get(key);
        if (prefetch)
            if (auto it = g_keys.find(source); it != g_keys.end()) {
                std::vector<uint32_t> keys = it->second;
                for (uint32_t k : keys)
                    if (k != key) get(k);
            }
    }
    g_wake.notify_all();
    return result;
}

bool shader_reflection(ShaderBuild& b, nvasm::Reflection& refl, std::string& error) {
    std::unique_lock<std::mutex> lk(b.m);
    for (;;) {
        if (b.have_refl) {
            refl = b.refl;
            return true;
        }
        if (!b.error.empty()) {
            error = b.error;
            return false;
        }
        if (b.state == ShaderBuild::Done) {
            error = "no reflection";
            return false;
        }
        if (b.state == ShaderBuild::Working) {          // a worker is translating it
            b.cv.wait(lk);
            continue;
        }
        // translate here (the quick part); glslang and the driver stay with the workers
        b.state = ShaderBuild::Working;
        lk.unlock();
        translate(b);
        lk.lock();
        b.state = ShaderBuild::Queued;
        b.cv.notify_all();
        lk.unlock();
        {
            std::lock_guard<std::mutex> q(g_lock);
            g_now.push_front(b.shared_from_this());
        }
        g_wake.notify_one();
        lk.lock();
    }
}

VkShaderEXT shader_object(ShaderBuild& b) {
    std::unique_lock<std::mutex> lk(b.m);
    if (b.state == ShaderBuild::Done) return b.sh;
    if (b.state == ShaderBuild::Queued) {
        b.state = ShaderBuild::Working;
        lk.unlock();
        build(b);
        lk.lock();
        b.state = ShaderBuild::Done;
        b.cv.notify_all();
        return b.sh;
    }
    rt::perf::Scope t(rt::perf::ShaderWait);
    b.cv.wait(lk, [&] { return b.state == ShaderBuild::Done; });
    return b.sh;
}

std::string shader_glsl(ShaderBuild& b) {
    std::lock_guard<std::mutex> lk(b.m);
    return b.glsl;
}

}  // namespace gpu
