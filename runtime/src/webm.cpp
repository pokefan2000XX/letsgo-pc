// WebM container parsing (see webm.h). Element IDs are from the Matroska specification.
#include "webm.h"

#include <cstring>

#include "rt.h"

namespace rt::webm {
namespace {

enum : uint32_t {
    kEbml = 0x1A45DFA3, kDocType = 0x4282,
    kSegment = 0x18538067, kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1, kDuration = 0x4489,
    kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
    kCodecPrivate = 0x63A2, kDefaultDuration = 0x23E383, kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA,
    kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F,
    kCluster = 0x1F43B675, kTimecode = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
};

struct Reader {
    const uint8_t* p;
    uint64_t end;
    bool ok = true;

    // a variable-length integer: element IDs keep their length marker, sizes drop it
    // (an all-ones size means "unknown", returned as ~0)
    uint64_t vint(uint64_t& o, bool keep_marker) {
        if (o >= end) {
            ok = false;
            return 0;
        }
        uint8_t first = p[o];
        int n = 1;
        uint8_t mask = 0x80;
        while (n <= 8 && !(first & mask)) mask >>= 1, n++;
        if (n > 8 || o + (uint64_t)n > end) {
            ok = false;
            return 0;
        }
        uint64_t v = keep_marker ? first : (uint64_t)(first & (mask - 1));
        bool all_ones = (first & (mask - 1)) == (uint8_t)(mask - 1);
        for (int i = 1; i < n; i++) {
            v = v << 8 | p[o + (uint64_t)i];
            all_ones &= p[o + (uint64_t)i] == 0xFF;
        }
        o += (uint64_t)n;
        if (!keep_marker && all_ones) return ~0ull;
        return v;
    }
    uint64_t uint(uint64_t o, uint64_t size) {
        uint64_t v = 0;
        for (uint64_t i = 0; i < size && i < 8; i++) v = v << 8 | p[o + i];
        return v;
    }
    double flt(uint64_t o, uint64_t size) {
        if (size == 4) {
            uint32_t b = (uint32_t)uint(o, 4);
            float f;
            memcpy(&f, &b, 4);
            return f;
        }
        if (size == 8) {
            uint64_t b = uint(o, 8);
            double d;
            memcpy(&d, &b, 8);
            return d;
        }
        return 0;
    }
};

// calls fn(id, payload offset, payload size) for each child element of [o, end)
template <class Fn> bool children(Reader& r, uint64_t o, uint64_t end, Fn fn) {
    while (o < end && r.ok) {
        uint64_t id = r.vint(o, true);
        uint64_t size = r.vint(o, false);
        if (!r.ok) return false;
        if (size == ~0ull || o + size > end) size = end - o;    // unknown or overlong: to the parent's end
        fn((uint32_t)id, o, size);
        o += size;
    }
    return r.ok;
}

}  // namespace

std::string mime_of(const std::string& codec) {
    if (codec == "V_VP9") return "video/x-vnd.on2.vp9";
    if (codec == "V_VP8") return "video/x-vnd.on2.vp8";
    if (codec == "A_VORBIS") return "audio/vorbis";
    if (codec == "A_OPUS") return "audio/opus";
    return "";
}

bool parse(File& f, std::string& err) {
    Reader r{f.data.data(), f.data.size()};
    std::string doctype;
    uint64_t scale_ns = 1000000;
    double duration = 0;
    bool segment = false, laced_logged = false;
    children(r, 0, f.data.size(), [&](uint32_t id, uint64_t o, uint64_t size) {
        if (id == kEbml) {
            children(r, o, o + size, [&](uint32_t cid, uint64_t co, uint64_t cs) {
                if (cid == kDocType) doctype.assign((const char*)r.p + co, (size_t)cs);
            });
            return;
        }
        if (id != kSegment) return;
        segment = true;
        children(r, o, o + size, [&](uint32_t sid, uint64_t so, uint64_t ss) {
            if (sid == kInfo) {
                children(r, so, so + ss, [&](uint32_t iid, uint64_t io, uint64_t is) {
                    if (iid == kTimecodeScale) scale_ns = r.uint(io, is);
                    if (iid == kDuration) duration = r.flt(io, is);
                });
            } else if (sid == kTracks) {
                children(r, so, so + ss, [&](uint32_t tid, uint64_t to, uint64_t ts) {
                    if (tid != kTrackEntry) return;
                    Track t;
                    children(r, to, to + ts, [&](uint32_t eid, uint64_t eo, uint64_t es) {
                        switch (eid) {
                        case kTrackNumber: t.number = r.uint(eo, es); break;
                        case kTrackType: t.type = (int)r.uint(eo, es); break;
                        case kCodecId: t.codec.assign((const char*)r.p + eo, (size_t)es); break;
                        case kCodecPrivate: t.codec_private.assign(r.p + eo, r.p + eo + es); break;
                        case kDefaultDuration: t.default_duration_ns = r.uint(eo, es); break;
                        case kVideo:
                            children(r, eo, eo + es, [&](uint32_t vid, uint64_t vo, uint64_t vs) {
                                if (vid == kPixelWidth) t.width = (uint32_t)r.uint(vo, vs);
                                if (vid == kPixelHeight) t.height = (uint32_t)r.uint(vo, vs);
                            });
                            break;
                        case kAudio:
                            children(r, eo, eo + es, [&](uint32_t aid, uint64_t ao, uint64_t as) {
                                if (aid == kSamplingFrequency) t.sample_rate = r.flt(ao, as);
                                if (aid == kChannels) t.channels = (uint32_t)r.uint(ao, as);
                            });
                            break;
                        }
                    });
                    f.tracks.push_back(std::move(t));
                });
            } else if (sid == kCluster) {
                uint64_t cluster_tc = 0;
                auto block = [&](uint64_t bo, uint64_t bs) {
                    uint64_t q = bo;
                    uint64_t tn = r.vint(q, false);
                    if (!r.ok || q + 3 > bo + bs) return;
                    int16_t rel = (int16_t)(r.p[q] << 8 | r.p[q + 1]);
                    uint8_t flags = r.p[q + 2];
                    q += 3;
                    if ((flags & 0x06) && !laced_logged) {
                        log("webm: laced blocks are not split (track %llu)", (unsigned long long)tn);
                        laced_logged = true;
                    }
                    for (size_t i = 0; i < f.tracks.size(); i++)
                        if (f.tracks[i].number == tn) {
                            int64_t tc = (int64_t)cluster_tc + rel;
                            f.samples.push_back(Sample{(uint32_t)i, q, (uint32_t)(bo + bs - q),
                                                       (int64_t)((double)tc * (double)scale_ns / 1000.0), (flags & 0x80) != 0});
                            break;
                        }
                };
                children(r, so, so + ss, [&](uint32_t cid, uint64_t co, uint64_t cs) {
                    if (cid == kTimecode) cluster_tc = r.uint(co, cs);
                    else if (cid == kSimpleBlock) block(co, cs);
                    else if (cid == kBlockGroup)
                        children(r, co, co + cs, [&](uint32_t gid, uint64_t go, uint64_t gs) {
                            if (gid == kBlock) block(go, gs);
                        });
                });
            }
        });
    });
    if (!r.ok) {
        err = "malformed EBML";
        return false;
    }
    if (!segment || (doctype != "webm" && doctype != "matroska")) {
        err = "not a WebM file (doctype '" + doctype + "')";
        return false;
    }
    if (f.tracks.empty()) {
        err = "no tracks";
        return false;
    }
    f.duration_us = (int64_t)(duration * (double)scale_ns / 1000.0);
    return true;
}

}  // namespace rt::webm
