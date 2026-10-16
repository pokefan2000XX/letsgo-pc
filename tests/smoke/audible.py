"""PASS if a 16-bit PCM WAV has at least N one-second stretches with sound in them.

    python -P tests/smoke/audible.py build/smoke/audio.wav 2

A second counts when any channel's RMS reaches 300 (about -40 dBFS). The
runtime writes these files with LETSGO_AUDIO_WAV.
"""
import array
import math
import struct
import sys


def main():
    path, need = sys.argv[1], int(sys.argv[2])
    with open(path, "rb") as f:
        header = f.read(44)
        data = array.array("h", f.read())
    if header[:4] != b"RIFF" or header[8:12] != b"WAVE":
        print("FAIL: %s is not a WAV file" % path)
        return 1
    channels = struct.unpack("<H", header[22:24])[0]
    rate = struct.unpack("<I", header[24:28])[0]
    per = rate * channels
    loud = 0
    for s in range(len(data) // per):
        seg = data[s * per:(s + 1) * per]
        if any(math.sqrt(sum(v * v for v in seg[c::channels]) / rate) >= 300 for c in range(channels)):
            loud += 1
    verdict = "PASS" if loud >= need else "FAIL"
    print("%s: %s has sound in %d of %d seconds (needs %d)" % (verdict, path, loud, len(data) // per, need))
    return 0 if loud >= need else 1


if __name__ == "__main__":
    sys.exit(main())
