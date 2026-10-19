"""hacks/phase1-test/code/file_log.cpp against the original (decomp/tools/difftest.py --hack phase1-test):
the hook opens files exactly as the original does, since it only logs and then calls the original, for
files in RomFS (found and missing) and in an archive (found and not)."""
OPEN = 0x71000238E0
ARCHIVE_LOOKUP = 0x7100028500                      # what the original asks an archive for a file
OPEN_FILE = "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
GET_FILE_SIZE = "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"
FOUND_KEY, ENTRY = 0x55, 0xA000


def setup(emu):
    def open_file(e):                              # OpenFile(FileHandle* out, char const* path, int mode)
        if e.read_cstr(e.reg(1)).endswith("missing.bin"):
            return 0x202                           # path not found
        e.write(e.reg(0), (0x1234).to_bytes(8, "little"))
        return 0

    def file_size(e):                              # GetFileSize(long* out, FileHandle)
        e.write(e.reg(0), (0x4321).to_bytes(8, "little"))
        return 0
    emu.import_handlers[OPEN_FILE] = open_file
    emu.import_handlers[GET_FILE_SIZE] = file_size
    emu.stub_function(ARCHIVE_LOOKUP, "archive lookup", lambda e: ENTRY if e.reg(1) == FOUND_KEY else 0)


def file_object(e, archive, key, path):
    f = e.alloc(0x180)
    if path is not None:
        name = e.alloc(0x80)
        e.write(name + 0x60, e.cstr(path).to_bytes(8, "little"))
        e.write(f + 0x60, name.to_bytes(8, "little"))
    e.write(f + 0x148, archive.to_bytes(8, "little"))
    e.write(f + 0x68, key.to_bytes(8, "little"))
    return f


def test_file_log_opens_like_the_original(t):
    e = t.emu
    for archive, key, path in ((0, 0, "rom:/bin/test.bin"), (0, 0, "rom:/bin/missing.bin"),
                               (0xB000, FOUND_KEY, None), (0xB000, 0x66, None)):
        f = file_object(e, archive, key, path)
        t.check(t.call(OPEN, f))
        t.check(e.read(f, 0x180))
