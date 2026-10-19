// gflib3's file system, as the game's data loaders use it: a path, a request that reads a file
// into memory, and the file it read. gflib3 stays recompiled: the functions declared here link to
// the originals (decomp/symbols.txt).
#pragma once
#include "decomp.h"
#include "gfl/object.h"

namespace gfl::fs {

class FileRequest;

// A path: a counted string with a view into it
struct Path {
    RefPtr<RefObject> text_;
    const void* unk08_;
    const void* unk10_;
    u64 unk18_;
    RefPtr<RefObject> unk20_;
    u32 unk28_;
};
DECOMP_SIZE(Path, 0x30);

// A file read into memory (shared by everything that reads it)
class File : public RefObject {
public:
    // Stops telling a request about the file (deferred while the file is calling requests back)
    void RemoveRequest(FileRequest* request);

    u8 unk58_[0x108];
    size_t size_;                         // 0 when there's no such file
    u8 unk168_[8];
    const void* data_;
};
DECOMP_OFFSET(File, size_, 0x160);
DECOMP_OFFSET(File, data_, 0x170);

// A function a request calls back: `invoke` runs it, `destroy` frees what it holds
struct Callback {
    ~Callback() {
        if (destroy != nullptr) {
            void (*destroyStorage)(void*) = destroy;
            unk10 = 0;
            unk18 = 0;
            invoke = nullptr;
            destroy = nullptr;
            destroyStorage(storage);
        }
    }

    void (*invoke)(void* storage);
    void (*destroy)(void* storage);
    u64 unk10;
    u64 unk18;
    u8 storage[0x60];
};
DECOMP_SIZE(Callback, 0x80);

// A request on a file: the file once it's read, and functions to call when it is
class FileRequest {
public:
    virtual void OnRead();                // calls `read`
    virtual void OnUnk();                 // calls `unk`
    ~FileRequest() {
        if (file_.Get() != nullptr) {
            RefPtr<File> file(file_.Get());
            file->RemoveRequest(this);
            file_.Reset();
        }
    }

    RefPtr<File> file_;
    Callback read_;
    Callback unk_;
};
DECOMP_SIZE(FileRequest, 0x110);

// What the data loaders read files with (it adds nothing)
class ReadRequest : public FileRequest {};

// Reads a file into memory (now, without callbacks) and puts it in the request; false when it
// can't be read
bool ReadFile(FileRequest* request, const Path& path, u32 unk, void* device, u64 unk2);

}  // namespace gfl::fs
