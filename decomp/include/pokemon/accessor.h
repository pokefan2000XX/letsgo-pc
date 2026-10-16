// Reads and writes a Pokémon record (record.h) in place.
//
// The record stays encrypted between accesses: each accessor decrypts it, checks the checksum
// (a mismatch marks the record a bad egg), does its work and encrypts it again. Fast mode keeps it
// decrypted across many accesses until EndFastMode.
#pragma once
#include "decomp.h"
#include "gfl/str.h"
#include "pokemon/record.h"

namespace pokemon {

class Accessor {
public:
    Accessor();
    virtual ~Accessor();

    // Takes decrypted data and encrypts it / takes data that is already encrypted
    void AttachDecoded(CoreData* core, CalcData* calc);
    void AttachEncoded(CoreData* core, CalcData* calc);
    bool HasCalcData() const;
    void Clear();
    void ClearCalcData();

    bool IsFastMode() const;
    void StartFastMode();
    void EndFastMode();

    // Copies the encrypted data out (a full record is CoreData followed by CalcData)
    void Serialize(void* record);
    void Serialize(CoreData* core, CalcData* calc);
    void SerializeCore(CoreData* core);
    // Copies encrypted data in and checks it
    void Deserialize(const void* record);
    void Deserialize(const CoreData* core, const CalcData* calc);
    void DeserializeCore(const CoreData* core);

    u32 GetEncryptionKey() const;
    bool IsBadEgg() const;
    // Changes the key and moves the blocks to the order the new key gives
    void SetEncryptionKey(u32 key);
    void SetBadEgg(u8 badEgg);

    // Party stats (0 or nothing without them)
    u32 GetStatus();
    u8 GetPartyLevel();
    u16 GetHp();
    u16 GetMaxHp();
    u16 GetAttack();
    u16 GetDefense();
    u16 GetSpAttack();
    u16 GetSpDefense();
    u16 GetSpeed();
    u16 GetCp();
    bool GetUnk100();
    u8 GetUnk101();
    u8 GetUnkAE();
    void SetStatus(u32 status);
    void SetPartyLevel(u8 level);
    void SetHp(u16 hp);
    void SetMaxHp(u16 maxHp);
    void SetAttack(u16 value);
    void SetDefense(u16 value);
    void SetSpAttack(u16 value);
    void SetSpDefense(u16 value);
    void SetSpeed(u16 value);
    void SetCp(u16 cp);
    void SetUnk100(u8 flag);
    void SetUnk101(u8 value);
    void SetUnkAE(u8 value);
    void SetUnkED(u8 value);
    void SetUnkEE(u8 value);

    // Block A
    u16 GetSpecies();
    u16 GetHeldItem();
    u32 GetId();
    u32 GetExp();
    u8 GetAbility();
    bool IsSecondAbility();
    bool IsHiddenAbility();
    bool IsFavorite();
    u16 GetMarkings();
    u32 GetPid();
    u8 GetNature();
    bool IsFatefulEncounter();
    u8 GetGender();
    u8 GetForm();
    u8 GetEvHp();
    u8 GetEvAttack();
    u8 GetEvDefense();
    u8 GetEvSpAttack();
    u8 GetEvSpDefense();
    u8 GetEvSpeed();
    u8 GetAvHp();
    u8 GetAvAttack();
    u8 GetAvDefense();
    u8 GetAvSpAttack();
    u8 GetAvSpDefense();
    u8 GetAvSpeed();
    u8 GetPokerus();
    f32 GetHeightAbsolute();
    bool HasRibbon(u32 ribbon);
    u8 GetHeightScalar();
    u8 GetWeightScalar();
    u32 GetUnk3C();
    void SetSpecies(u16 species);
    void SetHeldItem(u16 item);
    void SetId(u32 id);
    void SetExp(u32 exp);
    void SetAbility(u8 ability);
    void SetFirstAbility(u8 flag);
    void SetSecondAbility(u8 flag);
    void SetHiddenAbility(u8 flag);
    void SetFavorite(u8 flag);
    void SetMarkings(u16 markings);
    void SetPid(u32 pid);
    void SetNature(u8 nature);
    void SetFatefulEncounter(u8 flag);
    void SetGender(u8 gender);
    void SetForm(u8 form);
    void SetEvHp(u8 value);
    void SetEvAttack(u8 value);
    void SetEvDefense(u8 value);
    void SetEvSpAttack(u8 value);
    void SetEvSpDefense(u8 value);
    void SetEvSpeed(u8 value);
    void SetAvHp(u8 value);
    void SetAvAttack(u8 value);
    void SetAvDefense(u8 value);
    void SetAvSpAttack(u8 value);
    void SetAvSpDefense(u8 value);
    void SetAvSpeed(u8 value);
    void SetHeightAbsolute(f32 height);
    void SetRibbon(u32 ribbon);
    void SetHeightScalar(u8 scalar);
    void SetWeightScalar(u8 scalar);
    void SetUnk3C(u32 value);

    // Block B
    void GetNickname(gfl::str::StrBuf* out);
    void GetNickname(u16* out, u32 size);
    u16 GetMove(u8 slot);
    u8 GetMovePp(u8 slot);
    u8 GetMovePpUps(u8 slot);
    u16 GetRelearnMove(u8 slot);
    u8 GetIvHp();
    u8 GetIvAttack();
    u8 GetIvDefense();
    u8 GetIvSpAttack();
    u8 GetIvSpDefense();
    u8 GetIvSpeed();
    bool IsEgg();
    bool IsNicknamed();
    void SetNickname(const u16* name);
    void SetMove(u8 slot, u16 move);
    void SetMovePp(u8 slot, u8 pp);
    void SetMovePpUps(u8 slot, u8 ppUps);
    void SetRelearnMove(u8 slot, u16 move);
    void SetIvHp(u8 iv);
    void SetIvAttack(u8 iv);
    void SetIvDefense(u8 iv);
    void SetIvSpAttack(u8 iv);
    void SetIvSpDefense(u8 iv);
    void SetIvSpeed(u8 iv);
    void SetEgg(u8 flag);
    void SetNicknamed(u8 flag);

    // Block C
    u8 GetFriendship();   // the current trainer's
    u32 GetUnk94();
    u8 GetHandlerFriendship();
    u8 GetUnkA4();
    u8 GetUnkA5();
    u8 GetUnkA6();
    u16 GetUnkA8();
    u8 GetUnkAC();
    u8 GetUnkAD();
    void SetFriendship(u8 friendship);
    void SetHandlerName(const u16* name);
    void SetHandlerGender(u8 gender);
    void SetCurrentHandler(u8 handler);
    void SetUnk94(u32 value);
    void SetHandlerFriendship(u8 friendship);
    void SetUnkA4(u8 value);
    void SetUnkA5(u8 value);
    void SetUnkA6(u8 value);
    void SetUnkA8(u16 value);
    void SetUnkAC(u8 value);
    void SetUnkAD(u8 value);

    // Block D
    void GetOtName(gfl::str::StrBuf* out);
    void GetOtName(u16* out, u32 size);
    bool IsOtName(const u16* name);
    u8 GetUnkCB();
    u8 GetUnkCC();
    u8 GetUnkCD();
    u8 GetUnkCE();
    u8 GetUnkCF();
    u8 GetUnkD0();
    u8 GetEggYear();
    u8 GetEggMonth();
    u8 GetEggDay();
    u8 GetMetYear();
    u8 GetMetMonth();
    u8 GetMetDay();
    u8 GetUnkD7();
    u16 GetEggLocation();
    u16 GetMetLocation();
    u8 GetBall();
    u8 GetMetLevel();
    u8 GetOtGender();
    u8 GetHyperTraining();
    u8 GetVersion();
    u8 GetLanguage();
    f32 GetWeightAbsolute();
    void SetOtName(const u16* name);
    void SetOtFriendship(u8 friendship);
    void SetUnkCB(u8 value);
    void SetUnkCC(u8 value);
    void SetUnkCD(u8 value);
    void SetUnkCE(u8 value);
    void SetUnkCF(u8 value);
    void SetUnkD0(u8 value);
    void SetEggYear(u8 year);
    void SetEggMonth(u8 month);
    void SetEggDay(u8 day);
    void SetMetYear(u8 year);
    void SetMetMonth(u8 month);
    void SetMetDay(u8 day);
    void SetUnkD7(u8 value);
    void SetEggLocation(u16 location);
    void SetMetLocation(u16 location);
    void SetBall(u8 ball);
    void SetMetLevel(u8 level);
    void SetOtGender(u8 gender);
    void SetHyperTraining(u8 flags);
    void SetVersion(u8 version);
    void SetCountry(u8 country);
    void SetRegion(u8 region);
    void SetConsoleRegion(u8 region);
    void SetLanguage(u8 language);
    void SetWeightAbsolute(f32 weight);

private:
    // Decrypts the record for one access and encrypts it again afterwards (unless in fast mode)
    class Access {
    public:
        explicit Access(Accessor* accessor) : accessor_(accessor) { accessor_->DecodeForAccess(); }
        ~Access() { accessor_->EncodeAfterAccess(); }
        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;

    private:
        Accessor* accessor_;
    };

    // Decrypts the record if it's encrypted; a failed checksum marks it a bad egg
    void DecodeForAccess() {
        if (!encrypted_) {
            return;
        }
        DecodeData(core_->blocks, sizeof(core_->blocks), core_->encryptionKey);
        if (calc_ != nullptr) {
            DecodeData(calc_, sizeof(CalcData), core_->encryptionKey);
        }
        encrypted_ = false;
        if (core_->checksum != CalcChecksum(core_->blocks, sizeof(core_->blocks))) {
            core_->flags |= kFlagBadEgg;
        }
    }
    // Updates the checksum and encrypts
    void Encode() {
        if (core_ != nullptr) {
            core_->checksum = CalcChecksum(core_->blocks, sizeof(core_->blocks));
            EncodeData(core_->blocks, sizeof(core_->blocks), core_->encryptionKey);
        }
        if (calc_ != nullptr && core_ != nullptr) {
            EncodeData(calc_, sizeof(CalcData), core_->encryptionKey);
        }
        encrypted_ = true;
    }
    // Encrypts again after an access, unless fast mode keeps the record decrypted
    void EncodeAfterAccess() {
        if (!encrypted_ && !fastMode_) {
            Encode();
        }
    }
    void StartFastModeInline() {
        DecodeForAccess();
        fastMode_ = true;
    }
    void EndFastModeInline() {
        fastMode_ = false;
        if (!encrypted_) {
            Encode();
        }
    }

    // The blocks of a decrypted record; a bad egg reads the default blocks and writes to discards
    u8* Block(u32 which) const {
        return core_->blocks[kBlockOrder[ShuffleValue(core_->encryptionKey)][which]];
    }
    bool BadEgg() const { return (core_->flags & kFlagBadEgg) != 0; }
    const BlockA& A() const { return BadEgg() ? g_badEggBlockA : *reinterpret_cast<const BlockA*>(Block(0)); }
    const BlockB& B() const { return BadEgg() ? g_badEggBlockB : *reinterpret_cast<const BlockB*>(Block(1)); }
    const BlockC& C() const { return BadEgg() ? g_badEggBlockC : *reinterpret_cast<const BlockC*>(Block(2)); }
    const BlockD& D() const { return BadEgg() ? g_badEggBlockD : *reinterpret_cast<const BlockD*>(Block(3)); }
    const CalcData& Calc() const { return BadEgg() ? g_badEggCalc : *calc_; }
    BlockA& MutableA() { return BadEgg() ? g_discardBlockA : *reinterpret_cast<BlockA*>(Block(0)); }
    BlockB& MutableB() { return BadEgg() ? g_discardBlockB : *reinterpret_cast<BlockB*>(Block(1)); }
    BlockC& MutableC() { return BadEgg() ? g_discardBlockC : *reinterpret_cast<BlockC*>(Block(2)); }
    BlockD& MutableD() { return BadEgg() ? g_discardBlockD : *reinterpret_cast<BlockD*>(Block(3)); }
    CalcData& MutableCalc() { return BadEgg() ? g_discardCalc : *calc_; }

public:
    // The layout (public so it can be checked)
    CalcData* calc_;
    CoreData* core_;
    bool encrypted_;
    bool fastMode_;
};
DECOMP_SIZE(Accessor, 0x20);
DECOMP_OFFSET(Accessor, calc_, 0x08);
DECOMP_OFFSET(Accessor, core_, 0x10);
DECOMP_OFFSET(Accessor, encrypted_, 0x18);
DECOMP_OFFSET(Accessor, fastMode_, 0x19);

}  // namespace pokemon
