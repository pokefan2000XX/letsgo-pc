// Accessor: attaching records, fast mode and copying records in and out (pokemon/accessor.h).
#include "pokemon/accessor.h"

#include <string.h>

namespace pokemon {

// 0x7100729540
Accessor::Accessor() : calc_(nullptr), core_(nullptr), encrypted_(false), fastMode_(false) {}

// 0x7100729560, deleting: 0x7100729570
Accessor::~Accessor() {}

// 0x7100729580
void Accessor::AttachDecoded(CoreData* core, CalcData* calc) {
    calc_ = calc;
    core_ = core;
    encrypted_ = false;
    fastMode_ = false;
    Encode();
}

// 0x7100729600
void Accessor::AttachEncoded(CoreData* core, CalcData* calc) {
    calc_ = calc;
    core_ = core;
    encrypted_ = true;
    fastMode_ = false;
}

// 0x7100729610
bool Accessor::HasCalcData() const {
    return calc_ != nullptr;
}

// 0x7100729620
void Accessor::Clear() {
    if (core_ != nullptr) {
        memset(core_, 0, sizeof(CoreData));
    }
    if (calc_ != nullptr) {
        *calc_ = CalcData();
    }
    encrypted_ = false;
    fastMode_ = false;
    Encode();
}

// 0x71007296d0
void Accessor::ClearCalcData() {
    if (calc_ == nullptr) {
        return;
    }
    if (fastMode_) {
        *calc_ = CalcData();
        return;
    }
    StartFastModeInline();
    *calc_ = CalcData();
    EndFastModeInline();
}

// 0x71007297f0
bool Accessor::IsFastMode() const {
    return fastMode_;
}

// 0x7100729800
void Accessor::StartFastMode() {
    StartFastModeInline();
}

// 0x7100729890
void Accessor::EndFastMode() {
    EndFastModeInline();
}

// 0x7100729920
void Accessor::Serialize(void* record) {
    Serialize(static_cast<CoreData*>(record), reinterpret_cast<CalcData*>(static_cast<u8*>(record) + sizeof(CoreData)));
}

// 0x7100729930: the copy is always encrypted, in fast mode too
void Accessor::Serialize(CoreData* core, CalcData* calc) {
    bool wasFastMode = fastMode_;
    if (wasFastMode) {
        EndFastModeInline();
    }
    if (core != nullptr) {
        memcpy(core, core_, sizeof(CoreData));
    }
    if (calc != nullptr) {
        *calc = *calc_;
    }
    if (wasFastMode) {
        StartFastModeInline();
    }
}

// 0x7100729a70
void Accessor::SerializeCore(CoreData* core) {
    Serialize(core, nullptr);
}

// 0x7100729a80
void Accessor::Deserialize(const void* record) {
    Deserialize(static_cast<const CoreData*>(record),
                reinterpret_cast<const CalcData*>(static_cast<const u8*>(record) + sizeof(CoreData)));
}

// 0x7100729a90: decrypts once to check the checksum
void Accessor::Deserialize(const CoreData* core, const CalcData* calc) {
    if (core != nullptr) {
        memcpy(core_, core, sizeof(CoreData));
    }
    if (calc != nullptr) {
        *calc_ = *calc;
    }
    encrypted_ = true;
    fastMode_ = false;
    DecodeForAccess();
    EncodeAfterAccess();
}

// 0x7100729bc0
void Accessor::DeserializeCore(const CoreData* core) {
    Deserialize(core, nullptr);
}

// 0x7100729bd0
u32 Accessor::GetEncryptionKey() const {
    return core_->encryptionKey;
}

// 0x7100729be0
bool Accessor::IsBadEgg() const {
    return BadEgg();
}

}  // namespace pokemon
