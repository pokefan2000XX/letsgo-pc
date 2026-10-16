// Audio-side overrides. The hardware Opus decoder (Wwise's streams are the
// Switch's Opus) decodes with libopus. Audio output (audout) plays through the PC's
// audio device, which also paces it: an appended buffer is released (and the
// buffer event signalled) once the device has played it, as the service does.
// Without a device (headless, LETSGO_MUTE) a clock paces it instead.
// LETSGO_AUDIO_WAV=file.wav records each output stream (file.wav, file-2.wav, ...).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <opus/opus.h>

#include "hle.h"

namespace rt {
namespace audio {

// HardwareOpusDecoder object: vtable at +0; our state at +8
struct OpusState { uint32_t magic, sample_rate, channels, pad; };
constexpr uint32_t kMagic = 0x4F505553;   // "OPUS"
static OpusState* st(uint64_t self) { return (OpusState*)(uintptr_t)(self + 8); }

// samples per channel in one Opus packet (RFC 6716, section 3.1), at 48 kHz
static int opus_samples(const uint8_t* p, size_t n) {
    if (n < 1) return 0;
    int config = p[0] >> 3, code = p[0] & 3;
    static const int silk[] = {480, 960, 1920, 2880};       // 10, 20, 40, 60 ms
    static const int celt[] = {120, 240, 480, 960};         // 2.5, 5, 10, 20 ms
    int per_frame = config < 12 ? silk[config & 3] : config < 16 ? ((config & 1) ? 960 : 480) : celt[config & 3];
    int frames = code == 0 ? 1 : code == 3 ? (n >= 2 ? (p[1] & 0x3F) : 0) : 2;
    return per_frame * frames;
}

// one libopus decoder per HardwareOpusDecoder object
static std::mutex g_opus_lock;
static std::unordered_map<uint64_t, OpusDecoder*> g_opus;

// GetWorkBufferSize(int sampleRate, int channels) const
static void GetWorkBufferSize(Ctx* c) { c->x[0] = 0x8000; }
// Initialize(int sampleRate, int channels, void* work, size_t size)
static void Initialize(Ctx* c) {
    OpusState* s = st(c->x[0]);
    s->magic = kMagic;
    s->sample_rate = (uint32_t)c->x[1];
    s->channels = (uint32_t)c->x[2];
    int err = 0;
    OpusDecoder* d = opus_decoder_create((opus_int32)s->sample_rate, (int)s->channels, &err);
    if (!d) log("opus: decoder for %u Hz, %u channels: %s", s->sample_rate, s->channels, opus_strerror(err));
    std::lock_guard<std::mutex> lk(g_opus_lock);
    OpusDecoder*& slot = g_opus[c->x[0]];
    if (slot) opus_decoder_destroy(slot);
    slot = d;
    c->x[0] = 0;
}
static void Finalize(Ctx* c) {
    st(c->x[0])->magic = 0;
    std::lock_guard<std::mutex> lk(g_opus_lock);
    auto it = g_opus.find(c->x[0]);
    if (it != g_opus.end()) {
        if (it->second) opus_decoder_destroy(it->second);
        g_opus.erase(it);
    }
}
static void IsInitialized(Ctx* c) { c->x[0] = st(c->x[0])->magic == kMagic; }
static void GetSampleRate(Ctx* c) { c->x[0] = st(c->x[0])->sample_rate; }
static void GetChannelCount(Ctx* c) { c->x[0] = st(c->x[0])->channels; }
// DecodeInterleaved(size_t* consumed, int* samples, int16* out, size_t outSize, void const* in, size_t inSize):
// one packet per call; Nintendo packets carry an 8-byte header (big-endian payload size, final range)
static void DecodeInterleaved(Ctx* c) {
    OpusState* s = st(c->x[0]);
    const uint8_t* in = (const uint8_t*)(uintptr_t)c->x[5];
    size_t in_size = (size_t)c->x[6];
    int16_t* out = (int16_t*)(uintptr_t)c->x[3];
    size_t out_size = (size_t)c->x[4];
    size_t payload = in_size;
    const uint8_t* pkt = in;
    if (in_size >= 8) {
        uint32_t be = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
        if (be + 8 <= in_size) { payload = be; pkt = in + 8; }
    }
    uint32_t ch = s->channels ? s->channels : 1;
    OpusDecoder* d = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_opus_lock);
        auto it = g_opus.find(c->x[0]);
        if (it != g_opus.end()) d = it->second;
    }
    int samples = d ? opus_decode(d, pkt, (opus_int32)payload, out, (int)(out_size / (2 * ch)), 0) : -1;
    if (samples < 0) {
        // undecodable: silence of the packet's length keeps Wwise's timing
        static std::atomic<int> logged{0};
        if (logged++ < 5) log("opus: packet of %zu bytes: %s", payload, d ? opus_strerror(samples) : "no decoder");
        samples = opus_samples(pkt, payload) * (int)(s->sample_rate ? s->sample_rate : 48000) / 48000;
        size_t bytes = std::min<size_t>((size_t)samples * ch * 2, out_size);
        memset(out, 0, bytes);
    }
    *(uint64_t*)(uintptr_t)c->x[1] = (uint64_t)(pkt - in) + payload;
    *(int32_t*)(uintptr_t)c->x[2] = samples;
    c->x[0] = 0;
}

// ---- audout --------------------------------------------------------------------------------
// AudioOut (game-owned): +0 sample rate, +4 channel count, +8 SampleFormat (read by inline getters)
// AudioOutBuffer: +0 link, +8 data, +0x10 buffer size, +0x18 data size
// AudioOutParameter: +0 sample rate (0 = default), +4 u16 channel count (0 = default), +6 magic
enum { kFmtInt16 = 2 };
enum { kStarted = 0, kStopped = 1 };
struct AudioOutDev {
    uint32_t rate = 48000, channels = 2, format = kFmtInt16, event_w = 0;
    bool started = false, quit = false;
    std::deque<uint64_t> queued, released;       // queued: appended, not yet played (front first)
    std::deque<uint64_t> ends;                   // stream offset where each submitted queued buffer ends
    size_t submitted = 0;                        // queued buffers already handed to the device
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
};
static std::mutex g_devs_lock;
static std::unordered_map<uint64_t, AudioOutDev*> g_devs;

static uint32_t sample_bytes(uint32_t fmt) { return fmt == 1 ? 1 : fmt == 3 ? 3 : (fmt == 4 || fmt == 5) ? 4 : 2; }

// LETSGO_AUDIO_WAV=x.wav: a PCM16 WAV of each AudioOut's output (the first to x.wav, then x-2.wav, ...)
struct WavFile {
    FILE* f = nullptr;
    uint32_t bytes = 0;
    ~WavFile() {
        if (f) fclose(f);
    }
    void open(uint32_t rate, uint32_t channels, int index) {
        const char* env = getenv("LETSGO_AUDIO_WAV");
        if (!env) return;
        std::string path = env;
        if (index > 0) {
            size_t dot = path.rfind('.');
            if (dot == std::string::npos || path.find_first_of("/\\", dot) != std::string::npos) dot = path.size();
            path.insert(dot, "-" + std::to_string(index + 1));
        }
        if (!(f = fopen(path.c_str(), "wb"))) return;
        header(rate, channels);
        log("audout: recording to %s", path.c_str());
    }
    void header(uint32_t rate, uint32_t channels) {
        uint32_t block = channels * 2;
        uint8_t h[44];
        memcpy(h, "RIFF", 4);
        uint32_t v = 36 + bytes;
        memcpy(h + 4, &v, 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        v = 16;
        memcpy(h + 16, &v, 4);
        uint16_t pcm = 1, ch = (uint16_t)channels, bits = 16, ba = (uint16_t)block;
        memcpy(h + 20, &pcm, 2);
        memcpy(h + 22, &ch, 2);
        memcpy(h + 24, &rate, 4);
        v = rate * block;
        memcpy(h + 28, &v, 4);
        memcpy(h + 32, &ba, 2);
        memcpy(h + 34, &bits, 2);
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &bytes, 4);
        fseek(f, 0, SEEK_SET);
        fwrite(h, 1, 44, f);
        fseek(f, 0, SEEK_END);
    }
    void write(const void* p, uint32_t n, uint32_t rate, uint32_t channels) {
        if (!f) return;
        fwrite(p, 1, n, f);
        uint32_t before = bytes;
        bytes += n;
        if (bytes / (rate * 4) != before / (rate * 4)) header(rate, channels);     // keep it playable while running
        fflush(f);
    }
};
static std::atomic<int> g_wav_count{0};

static void device_thread(AudioOutDev* d) {
    using clk = std::chrono::steady_clock;
    void* stream = platform_audio_open(d->rate, d->channels);   // waits for the platform layer
    WavFile wav;
    wav.open(d->rate, d->channels, g_wav_count++);
    log("audout: %s", stream ? "playing through the PC's audio device" : "no audio device, paced by a clock");
    uint32_t frame = d->channels * sample_bytes(d->format);
    uint64_t pushed = 0;                       // bytes handed to the device
    // clock pacing (no device): what has played by now, from `base_played` at `base_t`
    uint64_t base_played = 0;
    clk::time_point base_t = clk::now();
    bool was_started = false;
    std::unique_lock<std::mutex> lk(d->m);
    for (;;) {
        if (d->quit) break;
        if (stream && d->started != was_started) platform_audio_pause(stream, !d->started);
        if (!stream && d->started && !was_started) base_t = clk::now();
        was_started = d->started;
        // hand newly appended buffers to the device
        while (d->started && d->submitted < d->queued.size()) {
            uint64_t buf = d->queued[d->submitted];
            const void* data = (const void*)(uintptr_t)*(uint64_t*)(uintptr_t)(buf + 8);
            uint32_t size = (uint32_t)*(uint64_t*)(uintptr_t)(buf + 0x18);
            size -= size % frame;
            if (!stream && pushed == base_played) {
                base_played = pushed;          // the device ran dry: the clock restarts with this buffer
                base_t = clk::now();
            }
            if (stream && size) platform_audio_put(stream, data, size);
            if (size) wav.write(data, size, d->rate, d->channels);
            pushed += size;
            d->ends.push_back(pushed);
            d->submitted++;
        }
        // release what has played
        uint64_t played;
        if (stream) {
            played = pushed - std::min<uint64_t>(pushed, platform_audio_queued(stream));
        } else if (d->started) {
            double secs = std::chrono::duration<double>(clk::now() - base_t).count();
            played = std::min<uint64_t>(pushed, base_played + (uint64_t)(secs * d->rate) * frame);
        } else {
            played = base_played;
        }
        if (!stream && !d->started) base_played = played;
        bool released = false;
        while (d->submitted > 0 && d->ends.front() <= played) {
            d->released.push_back(d->queued.front());
            d->queued.pop_front();
            d->ends.pop_front();
            d->submitted--;
            released = true;
        }
        if (!stream && d->started && played == pushed) base_played = pushed, base_t = clk::now();
        if (released && d->event_w) kevent_signal(d->event_w);
        d->cv.wait_for(lk, std::chrono::milliseconds(2));
    }
    lk.unlock();
    if (stream) platform_audio_close(stream);
}

static AudioOutDev* dev(uint64_t out) {
    std::lock_guard<std::mutex> lk(g_devs_lock);
    auto it = g_devs.find(out);
    if (it == g_devs.end()) fatal("audout: unknown AudioOut 0x%llx", (unsigned long long)out);
    return it->second;
}

static void open_out(Ctx* c, uint64_t out, uint64_t event, uint64_t param) {
    int32_t rate = *(const int32_t*)(uintptr_t)param;
    uint16_t ch = *(const uint16_t*)(uintptr_t)(param + 4);
    if (rate != 0 && rate != 48000) log("audout: requested %d Hz, the device runs at 48000", rate);
    AudioOutDev* d = new AudioOutDev;
    d->channels = ch == 6 ? 6 : 2;
    uint32_t* o = (uint32_t*)(uintptr_t)out;
    o[0] = d->rate;
    o[1] = d->channels;
    o[2] = d->format;
    if (event) d->event_w = attach_new_system_event(c, event, 1);
    {
        std::lock_guard<std::mutex> lk(g_devs_lock);
        g_devs[out] = d;
    }
    d->th = std::thread(device_thread, d);
    log("audout: opened %u Hz, %u channels, PCM16%s", d->rate, d->channels, event ? ", with buffer event" : "");
    c->x[0] = 0;
}
// OpenDefaultAudioOut(AudioOut*, SystemEvent*, AudioOutParameter const&) / (AudioOut*, AudioOutParameter const&)
static void OpenDefaultAudioOutEvent(Ctx* c) { open_out(c, c->x[0], c->x[1], c->x[2]); }
static void OpenDefaultAudioOut(Ctx* c) { open_out(c, c->x[0], 0, c->x[1]); }
static void set_started(uint64_t out, bool on) {
    AudioOutDev* d = dev(out);
    std::lock_guard<std::mutex> lk(d->m);
    d->started = on;
    d->cv.notify_all();
}
static void StartAudioOut(Ctx* c) { set_started(c->x[0], true); c->x[0] = 0; }
static void StopAudioOut(Ctx* c) { set_started(c->x[0], false); }
static void GetAudioOutState(Ctx* c) {
    AudioOutDev* d = dev(c->x[0]);
    std::lock_guard<std::mutex> lk(d->m);
    c->x[0] = d->started ? kStarted : kStopped;
}
// AppendAudioOutBuffer(AudioOut*, AudioOutBuffer*) -> bool
static void AppendAudioOutBuffer(Ctx* c) {
    AudioOutDev* d = dev(c->x[0]);
    std::lock_guard<std::mutex> lk(d->m);
    d->queued.push_back(c->x[1]);
    d->cv.notify_all();
    c->x[0] = 1;
}
// GetReleasedAudioOutBuffer(AudioOut*) -> AudioOutBuffer* or null
static void GetReleasedAudioOutBuffer(Ctx* c) {
    AudioOutDev* d = dev(c->x[0]);
    std::lock_guard<std::mutex> lk(d->m);
    if (d->released.empty()) { c->x[0] = 0; return; }
    c->x[0] = d->released.front();
    d->released.pop_front();
}
static void CloseAudioOut(Ctx* c) {
    AudioOutDev* d = dev(c->x[0]);
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->quit = true;
        d->cv.notify_all();
    }
    d->th.join();
    std::lock_guard<std::mutex> lk(g_devs_lock);
    g_devs.erase(c->x[0]);
    delete d;
}
// ListAudioDeviceName(AudioDeviceName* (char[256] each), int max) -> count
static void ListAudioDeviceName(Ctx* c) {
    static const char* names[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
    char* out = (char*)(uintptr_t)c->x[0];
    int n = 0;
    for (; n < (int)c->x[1] && n < 3; n++) {
        memset(out + n * 256, 0, 256);
        strcpy(out + n * 256, names[n]);
    }
    c->x[0] = (uint64_t)n;
}
}  // namespace audio

void register_audio_overrides(void (*reg)(const char*, rc_fn)) {
    using namespace audio;
    reg("_ZNK2nn5codec19HardwareOpusDecoder17GetWorkBufferSizeEii", GetWorkBufferSize);
    reg("_ZN2nn5codec19HardwareOpusDecoder10InitializeEiiPvm", Initialize);
    reg("_ZN2nn5codec19HardwareOpusDecoder8FinalizeEv", Finalize);
    reg("_ZNK2nn5codec19HardwareOpusDecoder13IsInitializedEv", IsInitialized);
    reg("_ZNK2nn5codec19HardwareOpusDecoder13GetSampleRateEv", GetSampleRate);
    reg("_ZNK2nn5codec19HardwareOpusDecoder15GetChannelCountEv", GetChannelCount);
    reg("_ZN2nn5codec19HardwareOpusDecoder17DecodeInterleavedEPmPiPsmPKvm", DecodeInterleaved);
    reg("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", OpenDefaultAudioOutEvent);
    reg("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", OpenDefaultAudioOut);
    reg("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", StartAudioOut);
    reg("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", StopAudioOut);
    reg("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", CloseAudioOut);
    reg("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", GetAudioOutState);
    reg("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", AppendAudioOutBuffer);
    reg("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", GetReleasedAudioOutBuffer);
    reg("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioDeviceName);
    reg("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", hle_void);
}

}  // namespace rt
