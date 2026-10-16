// WebM (Matroska subset) container parsing for the movie:: replacement: tracks and the
// sample table, from a file held in memory. The game's two movies are VP9 + Vorbis with
// unlaced SimpleBlocks; laced blocks are reported as one sample each and logged.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt::webm {

struct Track {
    uint64_t number = 0;          // TrackNumber (block references use it)
    int type = 0;                 // 1 video, 2 audio
    std::string codec;            // CodecID: V_VP9, A_VORBIS, ...
    std::vector<uint8_t> codec_private;
    uint32_t width = 0, height = 0;
    double sample_rate = 0;
    uint32_t channels = 0;
    uint64_t default_duration_ns = 0;
};

struct Sample {
    uint32_t track;               // index into File::tracks
    uint64_t offset;              // payload position in File::data
    uint32_t size;
    int64_t time_us;
    bool key;
};

struct File {
    std::vector<uint8_t> data;
    std::vector<Track> tracks;
    std::vector<Sample> samples;  // in file order
    int64_t duration_us = 0;
};

// false (with `err`) if `f.data` is not a WebM file this parser understands
bool parse(File& f, std::string& err);

// MIME type the movie:: API reports for a CodecID ("" if unknown)
std::string mime_of(const std::string& codec);

}  // namespace rt::webm
