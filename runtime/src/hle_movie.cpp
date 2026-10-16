// movie:: (subsdk1, the multimedia library) replaced by host code.
//
// The game's player (main 0x7100665000-0x710066f000, a port of the SDK's movie player sample)
// drives it like Android's MediaExtractor/MediaCodec:
//  - Extractor: opens the WebM through the game's ClientStreamReader, lists its tracks and reads
//    the samples of the selected tracks in file order through one cursor.
//  - Decoder (one per track): the game acquires input buffers, fills them from the extractor and
//    sends them; output buffers come back the same way. Four events per decoder wake the game's
//    MovieDecoderEventHandler: [0] input buffer available, [1] output buffer available,
//    [2] output format changed (playback waits for both decoders' formats), [3] error.
//  - Status 0 is success and 0x2000 the end of the stream.
// Object layouts (from subsdk1): Extractor and Decoder are {vtable, implementation} (0x10 bytes),
// MediaData is a pointer to its implementation, Buffer is the 0x30-byte GBuffer below.
//
// Decoding runs on the game thread that sends the input: VP9 through libvpx into NV12 pictures,
// Vorbis through libvorbis into 16-bit PCM in 1024-frame chunks (one of the game's AudioOut
// buffers each). The game paces playback: it presents a picture when its time comes and feeds
// audio as AudioOut drains, then releases the output. A decoder holds a bounded number of
// unreleased outputs and refuses input beyond that, so it stays a fraction of a second ahead.
#include <windows.h>

#include <vorbis/codec.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle.h"
#include "webm.h"

namespace rt {
namespace {

constexpr uint32_t kOk = 0, kEndOfStream = 0x2000, kTryAgain = 0x2001, kInvalidArg = 0x100B, kInvalidState = 0x100F,
                   kNotFound = 0x3000;
constexpr uint32_t kFlagEndOfStream = 4;

template <class T> T* G(uint64_t p) { return (T*)(uintptr_t)p; }

std::atomic<int> g_calls_logged{0};
void trace(const char* what) {
    if (g_calls_logged++ < 64) log("movie: %s", what);
}

int64_t host_us() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)((double)t.QuadPart * 1e6 / (double)freq.QuadPart);
}

void signal_event(Ctx* c, uint64_t ev) {
    static uint64_t fn = find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (ev) guest_call(c, fn, {ev});
}

// ---- Buffer -----------------------------------------------------------------------------------
struct GBuffer {
    int32_t offset, size, i32;
    uint32_t u32;
    uint8_t owns, pad[7];
    uint64_t capacity;
    int64_t i64;
    uint64_t base;
};
static_assert(sizeof(GBuffer) == 0x30, "movie::Buffer");

GBuffer* buf_of(Ctx* c) { return G<GBuffer>(c->x[0]); }
void buf_drop(GBuffer* b) {
    if (b->owns && b->base) free((void*)(uintptr_t)b->base);
    b->base = 0;
}
// subsdk1's copy: fields, then the data is duplicated if the source owns it, else shared
void buf_copy(GBuffer* d, const GBuffer* s) {
    d->offset = s->offset, d->size = s->size, d->i32 = s->i32, d->u32 = s->u32, d->i64 = s->i64;
    buf_drop(d);
    if (s->owns) {
        void* m = malloc(s->capacity ? s->capacity : 1);
        if (s->base && s->size > 0) memcpy((uint8_t*)m + s->offset, (const uint8_t*)(uintptr_t)s->base + s->offset, (size_t)s->size);
        d->base = (uint64_t)(uintptr_t)m;
    } else {
        d->base = s->base;
    }
    d->capacity = s->capacity;
    d->owns = s->owns;
}
void buf_set(GBuffer* b, void* data, uint64_t capacity) {
    buf_drop(b);
    b->owns = 0;
    b->base = (uint64_t)(uintptr_t)data;
    b->capacity = capacity;
    b->offset = 0;
    b->size = 0;
}

void BufferCtor(Ctx* c) { memset(buf_of(c), 0, sizeof(GBuffer)); }
void BufferCtorSize(Ctx* c) {
    GBuffer* b = buf_of(c);
    uint64_t n = c->x[1];
    memset(b, 0, sizeof *b);
    b->size = (int32_t)n;
    b->owns = 1;
    b->capacity = n;
    b->base = (uint64_t)(uintptr_t)malloc(n ? n : 1);
}
void BufferCtorData(Ctx* c) {
    GBuffer* b = buf_of(c);
    memset(b, 0, sizeof *b);
    b->size = (int32_t)c->x[2];
    b->capacity = c->x[2];
    b->base = c->x[1];
}
void BufferCtorCopy(Ctx* c) {
    GBuffer* b = buf_of(c);
    memset(b, 0, sizeof *b);
    buf_copy(b, G<GBuffer>(c->x[1]));
}
void BufferDtor(Ctx* c) { buf_drop(buf_of(c)); }
void BufferAssign(Ctx* c) {
    if (c->x[0] != c->x[1]) buf_copy(buf_of(c), G<GBuffer>(c->x[1]));
}
void BufferSize(Ctx* c) { c->x[0] = (uint64_t)(int64_t)buf_of(c)->size; }
void BufferOffset(Ctx* c) { c->x[0] = (uint64_t)(int64_t)buf_of(c)->offset; }
void BufferBase(Ctx* c) { c->x[0] = buf_of(c)->base; }
void BufferCapacity(Ctx* c) { c->x[0] = buf_of(c)->capacity; }
void BufferSetDataAndCapacity(Ctx* c) {
    GBuffer* b = buf_of(c);
    void* p = (void*)(uintptr_t)c->x[1];
    uint64_t n = c->x[2];
    buf_drop(b);
    b->owns = 0;
    b->base = (uint64_t)(uintptr_t)p;
    b->capacity = n;
}
void BufferSetRange(Ctx* c) { buf_of(c)->offset = (int32_t)c->x[1], buf_of(c)->size = (int32_t)c->x[2]; }
void BufferGetInt64(Ctx* c) { c->x[0] = (uint64_t)buf_of(c)->i64; }
void BufferSetInt64(Ctx* c) { buf_of(c)->i64 = (int64_t)c->x[1]; }
void BufferGetUint32(Ctx* c) { c->x[0] = buf_of(c)->u32; }
void BufferSetUint32(Ctx* c) { buf_of(c)->u32 = (uint32_t)c->x[1]; }
void BufferSetInt32(Ctx* c) { buf_of(c)->i32 = (int32_t)c->x[1]; }

// ---- MediaData ----------------------------------------------------------------------------------
struct Media {
    struct Val {
        int type;          // 0 int32, 1 int64, 2 string
        int64_t i;
        std::string s;
    };
    std::map<std::string, Val> kv;
    // CodecPrivate travels from the extractor's track format to Decoder::Configure under this key
    static constexpr const char* kCodecPrivate = "letsgo-codec-private";
    void set_i32(const char* k, int32_t v) { kv[k] = {0, v, {}}; }
    void set_i64(const char* k, int64_t v) { kv[k] = {1, v, {}}; }
    void set_str(const char* k, const std::string& v) { kv[k] = {2, 0, v}; }
};
Media* media(uint64_t md) {
    auto* slot = G<uint64_t>(md);
    if (!*slot) *slot = (uint64_t)(uintptr_t)new Media;
    return (Media*)(uintptr_t)*slot;
}
void MediaCtor(Ctx* c) { *G<uint64_t>(c->x[0]) = (uint64_t)(uintptr_t)new Media; }
void MediaDtor(Ctx* c) {
    delete (Media*)(uintptr_t)*G<uint64_t>(c->x[0]);
    *G<uint64_t>(c->x[0]) = 0;
}
void MediaClear(Ctx* c) {
    if (*G<uint64_t>(c->x[0])) media(c->x[0])->kv.clear();
}
void MediaFindInt32(Ctx* c) {
    Media* m = media(c->x[0]);
    auto it = m->kv.find(G<const char>(c->x[1]));
    bool ok = it != m->kv.end() && it->second.type <= 1;
    if (ok && c->x[2]) *G<int32_t>(c->x[2]) = (int32_t)it->second.i;
    c->x[0] = ok;
}
void MediaFindInt64(Ctx* c) {
    Media* m = media(c->x[0]);
    auto it = m->kv.find(G<const char>(c->x[1]));
    bool ok = it != m->kv.end() && it->second.type <= 1;
    if (ok && c->x[2]) *G<int64_t>(c->x[2]) = it->second.i;
    c->x[0] = ok;
}
void MediaFindString(Ctx* c) {
    Media* m = media(c->x[0]);
    auto it = m->kv.find(G<const char>(c->x[1]));
    bool ok = it != m->kv.end() && it->second.type == 2;
    if (ok && c->x[2]) *G<uint64_t>(c->x[2]) = (uint64_t)(uintptr_t)it->second.s.c_str();
    c->x[0] = ok;
}

// ---- Extractor ----------------------------------------------------------------------------------
// the tracks of the last file opened, in case the game configures a decoder with a format of its own
std::mutex g_last_lock;
std::vector<webm::Track> g_last_tracks;

struct GObj {
    uint64_t vtable, impl;
};

struct Extractor {
    std::mutex m;
    webm::File file;
    bool open = false;
    std::vector<bool> selected;
    size_t cursor = 0;            // the next sample (of a selected track)

    void skip() {
        while (cursor < file.samples.size() && !selected[file.samples[cursor].track]) cursor++;
    }
    bool load(std::vector<uint8_t> data, const std::string& what) {
        file = webm::File{};
        file.data = std::move(data);
        std::string err;
        open = webm::parse(file, err);
        if (!open) {
            log("movie: %s: %s", what.c_str(), err.c_str());
            return false;
        }
        selected.assign(file.tracks.size(), false);
        cursor = 0;
        {
            std::lock_guard<std::mutex> lk(g_last_lock);
            g_last_tracks = file.tracks;
        }
        log("movie: %s: %zu tracks, %zu samples, %.2f s", what.c_str(), file.tracks.size(), file.samples.size(),
            (double)file.duration_us / 1e6);
        for (auto& t : file.tracks)
            log("movie:   track %llu: %s %ux%u %.0f Hz %u ch", (unsigned long long)t.number, t.codec.c_str(), t.width, t.height,
                t.sample_rate, t.channels);
        return true;
    }
};
Extractor* ext_of(Ctx* c) { return (Extractor*)(uintptr_t)G<GObj>(c->x[0])->impl; }

uint64_t g_extractor_vtable[16];
uint64_t g_decoder_vtable[19];

void ExtractorCtor(Ctx* c) {
    GObj* o = G<GObj>(c->x[0]);
    o->vtable = (uint64_t)(uintptr_t)g_extractor_vtable;
    o->impl = (uint64_t)(uintptr_t)new Extractor;
    trace("Extractor created");
}
void ExtractorDtor(Ctx* c) {
    GObj* o = G<GObj>(c->x[0]);
    delete (Extractor*)(uintptr_t)o->impl;
    o->impl = 0;
}
void ExtractorSetDataSourcePath(Ctx* c) {
    Extractor* x = ext_of(c);
    std::string guest = G<const char>(c->x[1]);
    std::vector<uint8_t> data;
    fsx::read_whole_file(guest.c_str(), &data);
    std::lock_guard<std::mutex> lk(x->m);
    c->x[0] = !data.empty() && x->load(std::move(data), guest) ? kOk : kNotFound;
}
void ExtractorSetDataSourceMemory(Ctx* c) {
    Extractor* x = ext_of(c);
    const uint8_t* p = G<uint8_t>(c->x[1]);
    int n = (int)c->x[2];
    std::lock_guard<std::mutex> lk(x->m);
    c->x[0] = p && n > 0 && x->load(std::vector<uint8_t>(p, p + n), "(memory)") ? kOk : kNotFound;
}
// SetDataSource(ClientStreamReader*, path): the game's reader (vtable: [2] Open(path, &handle) -> bool,
// [3] ReadAt(handle, offset, buffer, size) -> bytes, [4] Close(handle), [5] GetSize(handle, &size))
void ExtractorSetDataSourceReader(Ctx* c) {
    Extractor* x = ext_of(c);
    uint64_t reader = c->x[1], path = c->x[2];
    std::string name = path ? G<const char>(path) : "";
    const uint64_t* vt = G<uint64_t>(*G<uint64_t>(reader));
    uint64_t handle = 0;
    std::vector<uint8_t> data;
    if (guest_call(c, vt[2], {reader, path, (uint64_t)(uintptr_t)&handle}) & 1) {
        int64_t size = 0;
        guest_call(c, vt[5], {reader, handle, (uint64_t)(uintptr_t)&size});
        if (size > 0 && size < (1ll << 31)) {
            data.resize((size_t)size);
            uint64_t done = 0;
            while (done < (uint64_t)size) {
                uint64_t n = std::min<uint64_t>((uint64_t)size - done, 8u << 20);
                uint64_t got = guest_call(c, vt[3], {reader, handle, done, (uint64_t)(uintptr_t)(data.data() + done), n});
                if (!got || got > n) break;
                done += got;
            }
            data.resize((size_t)done);
        }
        guest_call(c, vt[4], {reader, handle});
    }
    std::lock_guard<std::mutex> lk(x->m);
    c->x[0] = !data.empty() && x->load(std::move(data), name) ? kOk : kNotFound;
}
void ExtractorGetTrackCount(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    if (!x->open) {
        c->x[0] = kInvalidState;
        return;
    }
    *G<int32_t>(c->x[1]) = (int32_t)x->file.tracks.size();
    c->x[0] = kOk;
}
void ExtractorGetTrackConfiguration(Ctx* c) {
    Extractor* x = ext_of(c);
    int i = (int)c->x[1];
    std::lock_guard<std::mutex> lk(x->m);
    if (!x->open || i < 0 || i >= (int)x->file.tracks.size()) {
        c->x[0] = kInvalidArg;
        return;
    }
    const webm::Track& t = x->file.tracks[(size_t)i];
    Media* m = media(c->x[2]);
    m->set_str("mime", webm::mime_of(t.codec));
    m->set_i64("durationUs", x->file.duration_us);
    if (t.type == 1) {
        m->set_i32("width", (int32_t)t.width);
        m->set_i32("height", (int32_t)t.height);
    } else if (t.type == 2) {
        m->set_i32("sample-rate", (int32_t)t.sample_rate);
        m->set_i32("channel-count", (int32_t)t.channels);
    }
    if (!t.codec_private.empty())
        m->set_str(Media::kCodecPrivate, std::string(t.codec_private.begin(), t.codec_private.end()));
    c->x[0] = kOk;
}
void ExtractorSelectTrack(Ctx* c) {
    Extractor* x = ext_of(c);
    int i = (int)c->x[1];
    std::lock_guard<std::mutex> lk(x->m);
    bool ok = x->open && i >= 0 && i < (int)x->selected.size();
    if (ok) x->selected[(size_t)i] = true;
    c->x[0] = ok ? kOk : kInvalidArg;
}
void ExtractorUnselectTrack(Ctx* c) {
    Extractor* x = ext_of(c);
    int i = (int)c->x[1];
    std::lock_guard<std::mutex> lk(x->m);
    bool ok = x->open && i >= 0 && i < (int)x->selected.size();
    if (ok) x->selected[(size_t)i] = false;
    c->x[0] = ok ? kOk : kInvalidArg;
}
// SeekTo(us): to the last video keyframe at or before the time (or the first sample at it)
void ExtractorSeekTo(Ctx* c) {
    Extractor* x = ext_of(c);
    int64_t t = (int64_t)c->x[1];
    std::lock_guard<std::mutex> lk(x->m);
    size_t pos = 0;
    const auto& s = x->file.samples;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i].time_us > t) break;
        if (s[i].key && x->file.tracks[s[i].track].type == 1) pos = i;
    }
    x->cursor = pos;
    c->x[0] = kOk;
}
void ExtractorGetTrackIndexForAvailableData(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    x->skip();
    if (x->cursor >= x->file.samples.size()) {
        c->x[0] = kEndOfStream;
        return;
    }
    *G<uint64_t>(c->x[1]) = x->file.samples[x->cursor].track;
    c->x[0] = kOk;
}
void ExtractorRead(Ctx* c) {
    Extractor* x = ext_of(c);
    GBuffer* b = G<GBuffer>(c->x[1]);
    std::lock_guard<std::mutex> lk(x->m);
    x->skip();
    if (x->cursor >= x->file.samples.size()) {
        b->size = 0;
        c->x[0] = kEndOfStream;
        return;
    }
    const webm::Sample& s = x->file.samples[x->cursor];
    if (!b->base || s.size > b->capacity) {
        log("movie: Read: sample of %u bytes does not fit a %llu-byte buffer", s.size, (unsigned long long)b->capacity);
        c->x[0] = kInvalidArg;
        return;
    }
    memcpy((void*)(uintptr_t)b->base, x->file.data.data() + s.offset, s.size);
    b->offset = 0;
    b->size = (int32_t)s.size;
    c->x[0] = kOk;
}
void ExtractorAdvance(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    x->skip();
    if (x->cursor < x->file.samples.size()) x->cursor++;
    x->skip();
    c->x[0] = x->cursor < x->file.samples.size() ? kOk : kEndOfStream;
}
void ExtractorGetSampleTime(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    x->skip();
    if (x->cursor >= x->file.samples.size()) {
        c->x[0] = kEndOfStream;
        return;
    }
    *G<int64_t>(c->x[1]) = x->file.samples[x->cursor].time_us;
    c->x[0] = kOk;
}
void ExtractorGetSampleMetaData(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    x->skip();
    if (x->cursor >= x->file.samples.size()) {
        c->x[0] = kEndOfStream;
        return;
    }
    media(c->x[1])->set_i32("is-sync-frame", x->file.samples[x->cursor].key);
    c->x[0] = kOk;
}
void ExtractorGetBufferedRange(Ctx* c) {
    Extractor* x = ext_of(c);
    std::lock_guard<std::mutex> lk(x->m);
    if (c->x[1]) *G<int64_t>(c->x[1]) = 0;
    if (c->x[2]) *G<int64_t>(c->x[2]) = x->file.duration_us;
    c->x[0] = kOk;
}

// ---- Decoder ------------------------------------------------------------------------------------
// Xiph lacing (Vorbis CodecPrivate): a count byte, the sizes of all but the last packet, the packets
bool xiph_split(const std::vector<uint8_t>& cp, std::vector<std::pair<size_t, size_t>>& parts) {
    if (cp.empty()) return false;
    size_t n = (size_t)cp[0] + 1, pos = 1, used = 0;
    std::vector<size_t> sizes;
    for (size_t i = 0; i + 1 < n; i++) {
        size_t sz = 0;
        while (pos < cp.size() && cp[pos] == 255) sz += 255, pos++;
        if (pos >= cp.size()) return false;
        sz += cp[pos++];
        sizes.push_back(sz);
        used += sz;
    }
    if (pos + used > cp.size()) return false;
    sizes.push_back(cp.size() - pos - used);
    for (size_t sz : sizes) parts.push_back({pos, sz}), pos += sz;
    return true;
}

struct Decoder {
    static constexpr int kInputs = 4;
    static constexpr size_t kInputSize = 4u << 20;
    static constexpr int kAudioChunk = 1024;        // frames per audio output (one AudioOut buffer)
    std::mutex m;                    // queues: the game's threads
    std::mutex codec_m;              // codec state: decoding happens in SendInputBufferForDecode
    uint32_t type = 0;               // DecoderType: 1 AVC, 2 VP8, 3 VP9, 4 AAC, 5 Vorbis
    bool video = false;
    uint64_t events[4] = {};         // guest EventType*: input, output, format changed, error
    int32_t width = 0, height = 0, sample_rate = 0, channels = 0;
    std::vector<uint8_t> codec_private;
    std::vector<uint8_t> in[kInputs];
    std::deque<int> free_in;
    struct Out {
        int64_t time = 0;
        uint32_t flags = 0;
        int32_t width = 0, height = 0;
        int state = 0;               // 0 free, 1 ready, 2 held by the game
        std::vector<uint8_t> data;   // NV12 picture or interleaved 16-bit PCM
    };
    std::deque<Out> slots;           // output buffer index -> output (a deque keeps references stable)
    std::vector<int> free_slots;
    std::deque<int> ready;           // decoded, not yet acquired by the game
    int held = 0;                    // acquired by the game, not yet released
    int limit = 8;                   // unreleased outputs before input is refused
    bool input_refused = false;      // the game was refused an input buffer because of the limit
    bool started = false;
    bool eos = false;                // end of stream queued: no more input until Flush/Start
    int64_t last_time = 0;

    // codecs (codec_m)
    vpx_codec_ctx_t vpx{};
    bool vpx_ok = false;
    vorbis_info vi{};
    vorbis_comment vc{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool vorbis_ok = false;
    int64_t packetno = 0;
    std::vector<int16_t> pcm;        // decoded audio short of a full chunk
    int64_t pcm_time = INT64_MIN;    // time of pcm's first frame
    int errors_logged = 0;
    // statistics for the end-of-stream log line
    int64_t first_input = 0;         // host time (us) of the first input
    int inputs = 0, outputs = 0, fetched = 0;
    double decode_ms = 0, decode_ms_max = 0;

    ~Decoder() { close_codecs(); }
    void close_codecs() {
        if (vpx_ok) vpx_codec_destroy(&vpx);
        vpx_ok = false;
        if (vorbis_ok) {
            vorbis_block_clear(&vb);
            vorbis_dsp_clear(&vd);
            vorbis_comment_clear(&vc);
            vorbis_info_clear(&vi);
        }
        vorbis_ok = false;
    }
    void open_codecs() {
        close_codecs();
        if (type == 3) {
            vpx_codec_dec_cfg_t cfg{};
            cfg.threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
            cfg.w = (unsigned)width, cfg.h = (unsigned)height;
            vpx_codec_err_t e = vpx_codec_dec_init(&vpx, vpx_codec_vp9_dx(), &cfg, 0);
            vpx_ok = e == VPX_CODEC_OK;
            if (vpx_ok) vpx_codec_control(&vpx, VP9D_SET_ROW_MT, 1);
            else log("movie: VP9 decoder: %s", vpx_codec_err_to_string(e));
        } else if (type == 5) {
            std::vector<std::pair<size_t, size_t>> parts;
            if (!xiph_split(codec_private, parts) || parts.size() != 3) {
                log("movie: Vorbis decoder: no headers in the track's codec data (%zu bytes)", codec_private.size());
                return;
            }
            vorbis_info_init(&vi);
            vorbis_comment_init(&vc);
            int r = 0;
            for (int i = 0; i < 3 && r >= 0; i++) {
                ogg_packet op{};
                op.packet = codec_private.data() + parts[(size_t)i].first;
                op.bytes = (long)parts[(size_t)i].second;
                op.b_o_s = i == 0;
                op.packetno = i;
                r = vorbis_synthesis_headerin(&vi, &vc, &op);
            }
            if (r < 0 || vorbis_synthesis_init(&vd, &vi) != 0) {
                log("movie: Vorbis decoder: bad headers (%d)", r);
                vorbis_comment_clear(&vc);
                vorbis_info_clear(&vi);
                return;
            }
            vorbis_block_init(&vd, &vb);
            vorbis_ok = true;
            packetno = 3;
            if (vi.channels != channels || vi.rate != sample_rate)
                log("movie: Vorbis stream is %ld Hz %d ch (track says %d Hz %d ch)", vi.rate, vi.channels, sample_rate, channels);
            channels = vi.channels;
            sample_rate = (int32_t)vi.rate;
        }
    }
    void fill_format(Media* f, int32_t w = 0, int32_t h = 0) const {
        if (video) {
            // NV12: the luma plane, then interleaved chroma at half resolution
            if (!w) w = width, h = height;
            f->set_i32("width", w);
            f->set_i32("height", h);
            f->set_i32("nv12-y-stride", w);
            f->set_i32("nv12-y-offset", 0);
            f->set_i32("nv12-uv-offset", w * h);
        } else {
            f->set_i32("sample-rate", sample_rate);
            f->set_i32("channel-count", channels);
        }
    }
    // all outputs back to the decoder and every input buffer free (Start, Flush, Stop)
    void reset_queues() {
        free_in.clear();
        for (int i = 0; i < kInputs; i++) free_in.push_back(i);
        ready.clear();
        free_slots.clear();
        for (int i = (int)slots.size() - 1; i >= 0; i--) free_slots.push_back(i), slots[(size_t)i].state = 0;
        held = 0;
        input_refused = false;
        eos = false;
    }
    // a free output slot (m held)
    int new_slot() {
        if (free_slots.empty()) {
            slots.emplace_back();
            return (int)slots.size() - 1;
        }
        int i = free_slots.back();
        free_slots.pop_back();
        return i;
    }
    bool accepting() const { return (int)ready.size() + held < limit; }
    uint32_t frame_bytes() const { return (uint32_t)(width * height * 3 / 2); }
};
Decoder* dec_of(Ctx* c) { return (Decoder*)(uintptr_t)G<GObj>(c->x[0])->impl; }

void DecoderCtor(Ctx* c) {
    GObj* o = G<GObj>(c->x[0]);
    auto* d = new Decoder;
    d->type = (uint32_t)c->x[1];
    d->video = d->type >= 1 && d->type <= 3;
    d->limit = d->video ? 8 : 64;           // ~0.27 s of 30 fps video, ~1.4 s of 48 kHz audio
    for (auto& b : d->in) b.resize(Decoder::kInputSize);
    o->vtable = (uint64_t)(uintptr_t)g_decoder_vtable;
    o->impl = (uint64_t)(uintptr_t)d;
    char msg[96];
    snprintf(msg, sizeof msg, "Decoder created (type %u, output format %u, mode %u)", d->type, (uint32_t)c->x[2], (uint32_t)c->x[3]);
    trace(msg);
}
void DecoderDtor(Ctx* c) {
    GObj* o = G<GObj>(c->x[0]);
    delete (Decoder*)(uintptr_t)o->impl;
    o->impl = 0;
}
void DecoderConfigure(Ctx* c) {
    Decoder* d = dec_of(c);
    Media* f = media(c->x[1]);
    std::lock_guard<std::mutex> ck(d->codec_m);
    std::lock_guard<std::mutex> lk(d->m);
    auto get = [&](const char* k, int32_t& v) {
        auto it = f->kv.find(k);
        if (it != f->kv.end() && it->second.type <= 1) v = (int32_t)it->second.i;
    };
    get("width", d->width);
    get("height", d->height);
    get("sample-rate", d->sample_rate);
    get("channel-count", d->channels);
    auto it = f->kv.find(Media::kCodecPrivate);
    if (it != f->kv.end() && it->second.type == 2) {
        d->codec_private.assign(it->second.s.begin(), it->second.s.end());
    } else {
        const char* want = d->type == 3 ? "V_VP9" : d->type == 5 ? "A_VORBIS" : "";
        std::lock_guard<std::mutex> tl(g_last_lock);
        for (auto& t : g_last_tracks)
            if (t.codec == want) d->codec_private = t.codec_private;
    }
    d->open_codecs();
    c->x[0] = kOk;
}
void DecoderRegisterClientEvents(Ctx* c) {
    Decoder* d = dec_of(c);
    const uint64_t* ev = G<uint64_t>(c->x[1]);
    std::lock_guard<std::mutex> lk(d->m);
    for (int i = 0; i < 4; i++) d->events[i] = ev ? ev[i] : 0;
    c->x[0] = kOk;
}
void DecoderGetOutputFormat(Ctx* c) {
    Decoder* d = dec_of(c);
    std::lock_guard<std::mutex> lk(d->m);
    d->fill_format(media(c->x[1]));
    c->x[0] = kOk;
}
// codec state back to the start of a stream (Start, Flush: the next input may be anywhere)
void restart_codec(Decoder* d) {
    if (d->vorbis_ok) vorbis_synthesis_restart(&d->vd);
    d->pcm.clear();
    d->pcm_time = INT64_MIN;
}
void DecoderStart(Ctx* c) {
    Decoder* d = dec_of(c);
    uint64_t in_ev, fmt_ev;
    {
        std::lock_guard<std::mutex> ck(d->codec_m);
        restart_codec(d);
        std::lock_guard<std::mutex> lk(d->m);
        d->started = true;
        d->reset_queues();
        in_ev = d->events[0], fmt_ev = d->events[2];
    }
    signal_event(c, fmt_ev);
    signal_event(c, in_ev);
    c->x[0] = kOk;
}
void DecoderStop(Ctx* c) {
    Decoder* d = dec_of(c);
    std::lock_guard<std::mutex> ck(d->codec_m);
    restart_codec(d);
    std::lock_guard<std::mutex> lk(d->m);
    d->started = false;
    d->reset_queues();
    c->x[0] = kOk;
}
void DecoderFlush(Ctx* c) {
    Decoder* d = dec_of(c);
    uint64_t in_ev;
    bool started;
    {
        std::lock_guard<std::mutex> ck(d->codec_m);
        restart_codec(d);
        std::lock_guard<std::mutex> lk(d->m);
        d->reset_queues();
        started = d->started;
        in_ev = d->events[0];
    }
    if (started) signal_event(c, in_ev);
    c->x[0] = kOk;
}
void DecoderAcquireInputBufferIndex(Ctx* c) {
    Decoder* d = dec_of(c);
    std::lock_guard<std::mutex> lk(d->m);
    if (!d->started || d->eos || d->free_in.empty() || !d->accepting()) {
        if (d->started && !d->eos && !d->accepting()) d->input_refused = true;
        c->x[0] = kTryAgain;
        return;
    }
    *G<int32_t>(c->x[1]) = d->free_in.front();
    d->free_in.pop_front();
    if (c->x[2]) *G<int32_t>(c->x[2]) = (int32_t)d->free_in.size();
    c->x[0] = kOk;
}
void DecoderGetInputBuffer(Ctx* c) {
    Decoder* d = dec_of(c);
    int i = (int)c->x[1];
    if (i < 0 || i >= Decoder::kInputs) {
        c->x[0] = kInvalidArg;
        return;
    }
    buf_set(G<GBuffer>(c->x[2]), d->in[i].data(), Decoder::kInputSize);
    c->x[0] = kOk;
}

// one output, filled by `fill(Out&)` outside the queue lock (codec_m held)
template <class Fill> void emit(Decoder* d, int64_t time, uint32_t flags, Fill fill) {
    int i;
    Decoder::Out* o;
    {
        std::lock_guard<std::mutex> lk(d->m);
        i = d->new_slot();
        o = &d->slots[(size_t)i];
    }
    o->time = time, o->flags = flags, o->width = d->width, o->height = d->height;
    fill(*o);
    std::lock_guard<std::mutex> lk(d->m);
    o->state = 1;
    d->ready.push_back(i);
    d->last_time = time;
    d->outputs += flags == 0;
}

// a decoded VP9 picture as NV12 (the game's texture layout: luma rows, then interleaved chroma)
void emit_picture(Decoder* d, const vpx_image_t* img, int64_t time) {
    emit(d, time, 0, [&](Decoder::Out& o) {
        int w = (int)img->d_w, h = (int)img->d_h, cw = (w + 1) / 2, chh = (h + 1) / 2;
        o.width = w, o.height = h;
        o.data.resize((size_t)w * h + (size_t)cw * 2 * chh);
        uint8_t* y = o.data.data();
        uint8_t* uv = y + (size_t)w * h;
        bool yv12 = img->fmt == VPX_IMG_FMT_YV12;
        if (img->fmt != VPX_IMG_FMT_I420 && !yv12) {
            if (d->errors_logged++ < 3) log("movie: VP9 picture format 0x%x is not 8-bit 4:2:0", (unsigned)img->fmt);
            memset(y, 16, (size_t)w * h);
            memset(uv, 128, (size_t)cw * 2 * chh);
            return;
        }
        const uint8_t* U = img->planes[yv12 ? VPX_PLANE_V : VPX_PLANE_U];
        const uint8_t* V = img->planes[yv12 ? VPX_PLANE_U : VPX_PLANE_V];
        for (int r = 0; r < h; r++) memcpy(y + (size_t)r * w, img->planes[VPX_PLANE_Y] + (size_t)r * img->stride[VPX_PLANE_Y], (size_t)w);
        for (int r = 0; r < chh; r++) {
            const uint8_t* u = U + (size_t)r * img->stride[VPX_PLANE_U];
            const uint8_t* v = V + (size_t)r * img->stride[VPX_PLANE_V];
            uint8_t* dst = uv + (size_t)r * cw * 2;
            for (int x = 0; x < cw; x++) dst[2 * x] = u[x], dst[2 * x + 1] = v[x];
        }
    });
}
// a black picture (no VP9 decoder): the movie still runs its length
void emit_black(Decoder* d, int64_t time) {
    emit(d, time, 0, [&](Decoder::Out& o) {
        size_t luma = (size_t)o.width * o.height;
        o.data.assign(luma, 0);
        o.data.resize(luma * 3 / 2, 128);
    });
}
// whole chunks of the decoded audio (or all of it at the end of the stream)
void emit_audio(Decoder* d, bool all) {
    size_t ch = (size_t)std::max(d->channels, 1), chunk = (size_t)Decoder::kAudioChunk * ch, pos = 0;
    while (d->pcm.size() - pos >= chunk || (all && pos < d->pcm.size())) {
        size_t n = std::min(chunk, d->pcm.size() - pos);
        int64_t t = d->pcm_time + (int64_t)(pos / ch) * 1000000 / std::max(d->sample_rate, 1);
        emit(d, t, 0, [&](Decoder::Out& o) {
            o.data.resize(n * 2);
            memcpy(o.data.data(), d->pcm.data() + pos, n * 2);
        });
        pos += n;
    }
    if (pos) {
        d->pcm_time += (int64_t)(pos / ch) * 1000000 / std::max(d->sample_rate, 1);
        d->pcm.erase(d->pcm.begin(), d->pcm.begin() + (ptrdiff_t)pos);
    }
}
void decode_vorbis(Decoder* d, const uint8_t* data, size_t size, int64_t time) {
    if (d->pcm_time == INT64_MIN) d->pcm_time = time;
    ogg_packet op{};
    op.packet = (unsigned char*)data;
    op.bytes = (long)size;
    op.packetno = d->packetno++;
    op.granulepos = -1;
    if (vorbis_synthesis(&d->vb, &op) == 0) vorbis_synthesis_blockin(&d->vd, &d->vb);
    else if (d->errors_logged++ < 3) log("movie: Vorbis packet of %zu bytes did not decode", size);
    float** pcm;
    int n;
    int ch = d->vi.channels;
    // Vorbis orders 5.1 as FL FC FR RL RR LFE; the Switch as FL FR FC LFE RL RR
    static const int k51[6] = {0, 2, 1, 5, 3, 4};
    while ((n = vorbis_synthesis_pcmout(&d->vd, &pcm)) > 0) {
        size_t base = d->pcm.size();
        d->pcm.resize(base + (size_t)n * (size_t)ch);
        int16_t* out = d->pcm.data() + base;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) {
                float v = pcm[ch == 6 ? k51[k] : k][i] * 32768.0f;
                out[(size_t)i * ch + k] = (int16_t)std::clamp(v, -32768.0f, 32767.0f);
            }
        vorbis_synthesis_read(&d->vd, n);
    }
}
// SendInputBufferForDecode(index, offset, size, time us, flags)
void DecoderSendInputBufferForDecode(Ctx* c) {
    Decoder* d = dec_of(c);
    int i = (int)c->x[1];
    int32_t off = (int32_t)c->x[2], size = (int32_t)c->x[3];
    int64_t t = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    if (i < 0 || i >= Decoder::kInputs || off < 0 || size < 0 || (size_t)off + (size_t)size > Decoder::kInputSize) {
        c->x[0] = kInvalidArg;
        return;
    }
    bool eos = (flags & kFlagEndOfStream) != 0, first_eos = false;
    size_t before;
    {
        std::lock_guard<std::mutex> ck(d->codec_m);
        {
            std::lock_guard<std::mutex> lk(d->m);
            before = d->ready.size();
            first_eos = eos && !d->eos;
            if (eos) d->eos = true;
        }
        const uint8_t* data = d->in[i].data() + off;
        int64_t t0 = host_us();
        if (!d->first_input) d->first_input = t0;
        d->inputs += size > 0;
        if (d->video) {
            if (size > 0) {
                if (d->vpx_ok) {
                    if (vpx_codec_decode(&d->vpx, data, (unsigned)size, nullptr, 0) != VPX_CODEC_OK && d->errors_logged++ < 3)
                        log("movie: VP9 frame at %.3f s: %s", (double)t / 1e6, vpx_codec_error(&d->vpx));
                    vpx_codec_iter_t it = nullptr;
                    while (const vpx_image_t* img = vpx_codec_get_frame(&d->vpx, &it)) emit_picture(d, img, t);
                } else if (d->width > 0 && d->height > 0) {
                    emit_black(d, t);
                }
            }
        } else if (d->vorbis_ok) {
            if (size > 0) decode_vorbis(d, data, (size_t)size, t);
            emit_audio(d, eos);
        }
        double ms = (double)(host_us() - t0) / 1000.0;
        d->decode_ms += ms;
        d->decode_ms_max = std::max(d->decode_ms_max, ms);
        // the game marks every input buffer it holds as the end; one end-of-stream output answers them
        if (first_eos) emit(d, std::max(t, d->last_time), kFlagEndOfStream, [](Decoder::Out&) {});
    }
    uint64_t in_ev, out_ev;
    bool more_input, output;
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->free_in.push_back(i);
        output = d->ready.size() != before;
        more_input = !d->eos && d->accepting();
        if (!more_input && !d->eos) d->input_refused = true;
        in_ev = d->events[0], out_ev = d->events[1];
    }
    if (first_eos)
        log("movie: %s decoder reached the end of the stream after %.2f s", d->video ? "video" : "audio",
            (double)(host_us() - d->first_input) / 1e6);
    if (output) signal_event(c, out_ev);
    if (more_input) signal_event(c, in_ev);
    c->x[0] = kOk;
}
void DecoderAcquireOutputBufferIndex(Ctx* c) {
    Decoder* d = dec_of(c);
    std::lock_guard<std::mutex> lk(d->m);
    if (d->ready.empty()) {
        c->x[0] = kTryAgain;
        return;
    }
    int i = d->ready.front();
    d->ready.pop_front();
    d->held++;
    Decoder::Out& o = d->slots[(size_t)i];
    o.state = 2;
    *G<int32_t>(c->x[1]) = i;
    if (c->x[2]) *G<int64_t>(c->x[2]) = o.time;
    if (c->x[3]) *G<uint32_t>(c->x[3]) = o.flags;
    if (c->x[4]) *G<int32_t>(c->x[4]) = (int32_t)d->ready.size();
    c->x[0] = kOk;
}
// GetOutputBuffer(index, buffer): into the caller's buffer (for video, the game's texture memory)
uint32_t write_output(Decoder* d, int i, GBuffer* b) {
    const Decoder::Out* o;
    {
        std::lock_guard<std::mutex> lk(d->m);
        if (i < 0 || i >= (int)d->slots.size()) return kInvalidArg;
        o = &d->slots[(size_t)i];
    }
    size_t n = o->data.size();
    if (!b->base) {
        // no buffer of its own: lend the output (valid until it is released)
        buf_set(b, (void*)o->data.data(), n);
    } else if (n > b->capacity) {
        if (d->errors_logged++ < 3) log("movie: output of %zu bytes does not fit a %llu-byte buffer", n, (unsigned long long)b->capacity);
        n = 0;
    } else if (n) {
        memcpy((void*)(uintptr_t)b->base, o->data.data(), n);
    }
    if (n) d->fetched++;
    b->offset = 0;
    b->size = (int32_t)n;
    b->i64 = o->time;
    b->u32 = o->flags;
    return kOk;
}
void DecoderGetOutputBuffer(Ctx* c) { c->x[0] = write_output(dec_of(c), (int)c->x[1], G<GBuffer>(c->x[2])); }
void DecoderGetOutputBufferWithFormat(Ctx* c) {
    Decoder* d = dec_of(c);
    int i = (int)c->x[1];
    c->x[0] = write_output(d, i, G<GBuffer>(c->x[2]));
    if (c->x[3] && c->x[0] == kOk) {
        std::lock_guard<std::mutex> lk(d->m);
        d->fill_format(media(c->x[3]), d->slots[(size_t)i].width, d->slots[(size_t)i].height);
    }
}
void DecoderGetOutputBufferSize(Ctx* c) {
    Decoder* d = dec_of(c);
    uint32_t n = d->video ? d->frame_bytes() : (uint32_t)(Decoder::kAudioChunk * std::max(d->channels, 1) * 2);
    if (c->x[1]) *G<int32_t>(c->x[1]) = (int32_t)n;
    if (c->x[2]) *G<int32_t>(c->x[2]) = 1;
    c->x[0] = kOk;
}
void DecoderGetOutputBufferFormat(Ctx* c) {
    Decoder* d = dec_of(c);
    int i = (int)c->x[1];
    std::lock_guard<std::mutex> lk(d->m);
    if (i >= 0 && i < (int)d->slots.size()) d->fill_format(media(c->x[2]), d->slots[(size_t)i].width, d->slots[(size_t)i].height);
    else d->fill_format(media(c->x[2]));
    c->x[0] = kOk;
}
// ReleaseOutputBufferIndex(index): the output goes back to the decoder, which may take input again
void DecoderReleaseOutputBufferIndex(Ctx* c) {
    Decoder* d = dec_of(c);
    int i = (int)c->x[1];
    uint64_t in_ev = 0;
    {
        std::lock_guard<std::mutex> lk(d->m);
        if (i < 0 || i >= (int)d->slots.size() || d->slots[(size_t)i].state != 2) {
            c->x[0] = kInvalidArg;
            return;
        }
        d->held--;
        d->slots[(size_t)i].state = 0;
        d->free_slots.push_back(i);
        if (d->slots[(size_t)i].flags & kFlagEndOfStream)       // the game has played the whole stream
            log("movie: %s decoder done: %d inputs, %d outputs, %d fetched by the game, decoding %.2f ms per input (%.2f ms at most)",
                d->video ? "video" : "audio", d->inputs, d->outputs, d->fetched, d->decode_ms / std::max(d->inputs, 1),
                d->decode_ms_max);
        if (d->input_refused && d->accepting() && !d->eos && d->started) {
            d->input_refused = false;
            in_ev = d->events[0];
        }
    }
    signal_event(c, in_ev);
    c->x[0] = kOk;
}

// ---- library ------------------------------------------------------------------------------------
void SetAllocator(Ctx* c) { c->x[0] = 0; }
void GetThreadPriority(Ctx* c) { c->x[0] = 16; }

}  // namespace

void register_movie_overrides(void (*reg)(const char*, rc_fn)) {
    struct Slot { const char* name; rc_fn fn; };
    const Slot ext[16] = {
        {"movie::Extractor::~Extractor", ExtractorDtor}, {"movie::Extractor::~Extractor(deleting)", ExtractorDtor},
        {"movie::Extractor::SetDataSource(path)", ExtractorSetDataSourcePath},
        {"movie::Extractor::SetDataSource(memory)", ExtractorSetDataSourceMemory},
        {"movie::Extractor::GetTrackCount", ExtractorGetTrackCount},
        {"movie::Extractor::GetTrackConfiguration", ExtractorGetTrackConfiguration},
        {"movie::Extractor::SelectTrack", ExtractorSelectTrack}, {"movie::Extractor::UnselectTrack", ExtractorUnselectTrack},
        {"movie::Extractor::SeekTo", ExtractorSeekTo},
        {"movie::Extractor::GetTrackIndexForAvailableData", ExtractorGetTrackIndexForAvailableData},
        {"movie::Extractor::Read", ExtractorRead}, {"movie::Extractor::Advance", ExtractorAdvance},
        {"movie::Extractor::GetSampleTime", ExtractorGetSampleTime},
        {"movie::Extractor::GetSampleMetaData", ExtractorGetSampleMetaData},
        {"movie::Extractor::GetBufferedRange", ExtractorGetBufferedRange},
        {"movie::Extractor::SetDataSource(reader)", ExtractorSetDataSourceReader},
    };
    const Slot dec[19] = {
        {"movie::Decoder::~Decoder", DecoderDtor}, {"movie::Decoder::~Decoder(deleting)", DecoderDtor},
        {"movie::Decoder::Configure", DecoderConfigure}, {"movie::Decoder::RegisterClientEvents", DecoderRegisterClientEvents},
        {"movie::Decoder::GetOutputFormat", DecoderGetOutputFormat}, {"movie::Decoder::Start", DecoderStart},
        {"movie::Decoder::Stop", DecoderStop}, {"movie::Decoder::Reset", DecoderStop}, {"movie::Decoder::Flush", DecoderFlush},
        {"movie::Decoder::AcquireInputBufferIndex", DecoderAcquireInputBufferIndex},
        {"movie::Decoder::GetInputBuffer", DecoderGetInputBuffer},
        {"movie::Decoder::SendInputBufferForDecode", DecoderSendInputBufferForDecode},
        {"movie::Decoder::AcquireOutputBufferIndex", DecoderAcquireOutputBufferIndex},
        {"movie::Decoder::GetOutputBuffer", DecoderGetOutputBuffer},
        {"movie::Decoder::GetOutputBufferSize", DecoderGetOutputBufferSize},
        {"movie::Decoder::ReleaseOutputBufferIndex", DecoderReleaseOutputBufferIndex}, {"movie::Decoder::GetLastError", hle_ok},
        {"movie::Decoder::GetOutputBuffer(format)", DecoderGetOutputBufferWithFormat},
        {"movie::Decoder::GetOutputBufferFormat", DecoderGetOutputBufferFormat},
    };
    for (int i = 0; i < 16; i++) g_extractor_vtable[i] = host_fn(ext[i].name, ext[i].fn);
    for (int i = 0; i < 19; i++) g_decoder_vtable[i] = host_fn(dec[i].name, dec[i].fn);

    reg("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", SetAllocator);
    reg("_ZN5movie17GetThreadPriorityEv", GetThreadPriority);
    reg("_ZN5movie11SetCoreMaskEm", hle_void);
    reg("_ZN5movie9MediaDataC1Ev", MediaCtor);
    reg("_ZN5movie9MediaDataD1Ev", MediaDtor);
    reg("_ZN5movie9MediaData5ClearEv", MediaClear);
    reg("_ZNK5movie9MediaData9FindInt32EPKcPi", MediaFindInt32);
    reg("_ZNK5movie9MediaData9FindInt64EPKcPl", MediaFindInt64);
    reg("_ZNK5movie9MediaData10FindStringEPKcPS2_", MediaFindString);
    reg("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", ExtractorCtor);
    reg("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", DecoderCtor);
    reg("_ZN5movie6BufferC1Ev", BufferCtor);
    reg("_ZN5movie6BufferC1Em", BufferCtorSize);
    reg("_ZN5movie6BufferC1EPvm", BufferCtorData);
    reg("_ZN5movie6BufferC1ERKS0_", BufferCtorCopy);
    reg("_ZN5movie6BufferD1Ev", BufferDtor);
    reg("_ZN5movie6BufferaSERKS0_", BufferAssign);
    reg("_ZNK5movie6Buffer4SizeEv", BufferSize);
    reg("_ZNK5movie6Buffer6OffsetEv", BufferOffset);
    reg("_ZNK5movie6Buffer4BaseEv", BufferBase);
    reg("_ZNK5movie6Buffer8CapacityEv", BufferCapacity);
    reg("_ZN5movie6Buffer18SetDataAndCapacityEPvm", BufferSetDataAndCapacity);
    reg("_ZN5movie6Buffer8SetRangeEii", BufferSetRange);
    reg("_ZNK5movie6Buffer12GetInt64DataEv", BufferGetInt64);
    reg("_ZN5movie6Buffer12SetInt64DataEl", BufferSetInt64);
    reg("_ZNK5movie6Buffer13GetUint32DataEv", BufferGetUint32);
    reg("_ZN5movie6Buffer13SetUint32DataEj", BufferSetUint32);
    reg("_ZN5movie6Buffer12SetInt32DataEi", BufferSetInt32);
}

}  // namespace rt
