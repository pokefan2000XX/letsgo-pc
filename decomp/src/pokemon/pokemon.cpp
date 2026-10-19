// Pokemon: a Pokémon's methods (pokemon/pokemon.h). Changes to what a stat depends on recalculate
// that stat and the combat power, the way the original does it.
#include "pokemon/pokemon.h"

#include <string.h>

#include "moves/data.h"
#include "pokemon/evolution.h"
#include "pokemon/personal.h"
#include "pokemon/system.h"

namespace pokemon {

// ---- making and destroying ------------------------------------------------------------------------

// 0x7100723770: the Pokémon system's startup calls it after setting up the default records
void Pokemon::Initialize() {}

// 0x7100723780: a zeroed record, taken as decrypted (attaching encrypts it)
Pokemon::Pokemon(gfl::mem::MemoryResource* resource) {
    core_ = CreateCoreData(resource);
    accessor_ = gfl::mem::MakeUnique<Accessor>(resource);
    accessor_->AttachDecoded(core_.Get(), nullptr);
}

// 0x71007239e0
Pokemon::Pokemon(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id) {
    core_ = CreateCoreData(resource, species, level, id);
    accessor_ = gfl::mem::MakeUnique<Accessor>(resource);
    accessor_->AttachEncoded(core_.Get(), nullptr);
    InitNewPokemon();
}

// 0x7100723c30
void Pokemon::InitNewPokemon() {
    accessor_->SetLanguage(GetSystem()->language);
    ResetMoves();
    u16 species = accessor_->GetSpecies();
    u16 speciesName[14];
    GetSpeciesName(speciesName, species, accessor_->GetLanguage());
    accessor_->SetNickname(speciesName);
    accessor_->SetNicknamed(0);
    accessor_->SetHp(accessor_->GetMaxHp());
    accessor_->SetStatus(0);
    RecoverAllPp();
    accessor_->SetUnkED(0);
    accessor_->SetUnkEE(0);
    accessor_->SetCountry(GetSystem()->country);
    accessor_->SetRegion(GetSystem()->region);
    accessor_->SetConsoleRegion(GetSystem()->consoleRegion);
    accessor_->SetUnkAC(100);
    accessor_->SetUnkAD(100);
    UpdateAbsoluteSize();
}

// 0x7100723e00
Pokemon::Pokemon(gfl::mem::MemoryResource* resource, const CreationSpec& spec) {
    core_ = CreateCoreData(resource, spec);
    accessor_ = gfl::mem::MakeUnique<Accessor>(resource);
    accessor_->AttachEncoded(core_.Get(), nullptr);
    InitNewPokemon();
}

// 0x71007243a0: a zeroed record (taken as encrypted), then the other's core data copied in
Pokemon::Pokemon(gfl::mem::MemoryResource* resource, const Pokemon& other) {
    core_ = CreateCoreData(resource);
    accessor_ = gfl::mem::MakeUnique<Accessor>(resource);
    accessor_->AttachEncoded(core_.Get(), nullptr);
    other.accessor_->SerializeCore(&g_scratchCore);
    DeserializeCore(&g_scratchCore);
}

// 0x7100724600, deleting: 0x7100724750
Pokemon::~Pokemon() {
    core_.Reset();
    calc_.Reset();
    accessor_.Reset();
}

// 0x7100728020: a bad egg stays as it is
void Pokemon::MakeEgg(gfl::mem::MemoryResource* resource) {
    accessor_->IsEgg();                   // read and unused
    if (accessor_->IsBadEgg()) {
        return;
    }
    accessor_->SetEgg(1);
    gfl::RefPtr<gfl::str::StrBuf> name(gfl::Create<gfl::str::StrBuf>()(resource, 13));
    GetSpeciesName(name.Get(), kSpeciesEgg, accessor_->GetLanguage());
    accessor_->SetNickname(name->GetPtr());
    accessor_->SetNicknamed(0);
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    accessor_->SetOtFriendship(personal::GetParam(personal::kParamHatchCycles));
}

// ---- the record's fields --------------------------------------------------------------------------

void Pokemon::SetLanguage(u8 language) { accessor_->SetLanguage(language); }  // 0x7100724040
void Pokemon::SetUnkAC(u8 value) { accessor_->SetUnkAC(value); }  // 0x71007242a0
void Pokemon::SetUnkAD(u8 value) { accessor_->SetUnkAD(value); }  // 0x71007242b0
void Pokemon::SerializeCore(CoreData* core) const { accessor_->SerializeCore(core); }  // 0x7100724770
void Pokemon::DeserializeCore(const CoreData* core) { accessor_->DeserializeCore(core); }  // 0x7100724780, virtual
void Pokemon::Clear() { accessor_->Clear(); }  // 0x7100724790
u16 Pokemon::GetSpecies() const { return accessor_->GetSpecies(); }  // 0x71007247c0
void Pokemon::SetHeldItem(u16 item) { accessor_->SetHeldItem(item); }  // 0x71007247f0
u8 Pokemon::GetLanguage() { return accessor_->GetLanguage(); }  // 0x7100724b60
u8 Pokemon::GetVersion() { return accessor_->GetVersion(); }  // 0x7100724b70
void Pokemon::SetVersion(u8 version) { accessor_->SetVersion(version); }  // 0x7100724b80
u8 Pokemon::GetBall() { return accessor_->GetBall(); }  // 0x7100724b90
void Pokemon::SetBall(u8 ball) { accessor_->SetBall(ball); }  // 0x7100724ba0
u8 Pokemon::GetUnkAE() { return accessor_->GetUnkAE(); }  // 0x7100724bb0
u16 Pokemon::GetCp() { return accessor_->GetCp(); }  // 0x7100724e40
u32 Pokemon::GetExp() { return accessor_->GetExp(); }  // 0x7100726780
u8 Pokemon::GetForm() { return accessor_->GetForm(); }  // 0x71007268c0
u32 Pokemon::GetStatus() { return accessor_->GetStatus(); }  // 0x71007269a0
void Pokemon::SetStatus(u32 status) { accessor_->SetStatus(status); }  // 0x71007269b0
void Pokemon::ClearStatus() { accessor_->SetStatus(0); }  // 0x71007269c0
u16 Pokemon::GetMove(u8 slot) { return accessor_->GetMove(slot); }  // 0x7100726cc0
u16 Pokemon::GetRelearnMove(u8 slot) { return accessor_->GetRelearnMove(slot); }  // 0x7100726dd0
u8 Pokemon::GetGender() { return accessor_->GetGender(); }  // 0x7100727ab0
u8 Pokemon::GetNature() { return accessor_->GetNature(); }  // 0x7100727ac0
u8 Pokemon::GetAbility() { return accessor_->GetAbility(); }  // 0x7100727b00
void Pokemon::SetFavorite(u8 flag) { accessor_->SetFavorite(flag & 1); }  // 0x7100727b10
bool Pokemon::IsFavorite() { return accessor_->IsFavorite(); }  // 0x7100727b20
bool Pokemon::IsNicknamed() { return accessor_->IsNicknamed(); }  // 0x7100727ca0
void Pokemon::GetNickname(gfl::str::StrBuf* out) { accessor_->GetNickname(out); }  // 0x7100727cb0
void Pokemon::GetNickname(u16* out, u32 size) { accessor_->GetNickname(out, size); }  // 0x7100727cc0
void Pokemon::GetOtName(gfl::str::StrBuf* out) { accessor_->GetOtName(out); }  // 0x7100727dd0
void Pokemon::GetOtName(u16* out, u32 size) { accessor_->GetOtName(out, size); }  // 0x7100727de0
u8 Pokemon::GetOtGender() { return accessor_->GetOtGender(); }  // 0x7100727e20
void Pokemon::SetOtGender(u8 gender) { accessor_->SetOtGender(gender); }  // 0x7100727e30
u8 Pokemon::GetFriendship() { return accessor_->GetFriendship(); }  // 0x7100727e40
u32 Pokemon::GetUnk94() { return accessor_->GetUnk94(); }  // 0x7100727f90
void Pokemon::SetUnk94(u32 value) { accessor_->SetUnk94(value); }  // 0x7100727fa0
void Pokemon::MarkBadEgg() { accessor_->SetBadEgg(1); }  // 0x7100728120
u32 Pokemon::GetId() { return accessor_->GetId(); }  // 0x7100728130
bool Pokemon::GetUnk100() { return accessor_->GetUnk100(); }  // 0x7100728900
void Pokemon::SetId(u32 id) { accessor_->SetId(id); }  // 0x7100728b40
u32 Pokemon::GetEncryptionKey() { return accessor_->GetEncryptionKey(); }  // 0x7100728b50
bool Pokemon::HasRibbon(u32 ribbon) { return accessor_->HasRibbon(ribbon); }  // 0x7100728ca0
void Pokemon::SetRibbon(u32 ribbon) { accessor_->SetRibbon(ribbon); }  // 0x7100728cb0
bool Pokemon::IsFatefulEncounter() { return accessor_->IsFatefulEncounter(); }  // 0x7100728ce0
void Pokemon::SetFatefulEncounter(u8 flag) { accessor_->SetFatefulEncounter(flag & 1); }  // 0x7100728cf0
f32 Pokemon::GetHeightAbsolute() { return accessor_->GetHeightAbsolute(); }  // 0x7100728de0
f32 Pokemon::GetWeightAbsolute() { return accessor_->GetWeightAbsolute(); }  // 0x7100728df0
u8 Pokemon::GetHeightScalar() { return accessor_->GetHeightScalar(); }  // 0x7100728e00
u8 Pokemon::GetWeightScalar() { return accessor_->GetWeightScalar(); }  // 0x7100728e10
u8 Pokemon::GetUnkAC() { return accessor_->GetUnkAC(); }  // 0x7100728fb0
u8 Pokemon::GetUnkAD() { return accessor_->GetUnkAD(); }  // 0x7100728fc0
u8 Pokemon::GetUnkCB() { return accessor_->GetUnkCB(); }  // 0x7100728fd0
void Pokemon::SetUnkCB(u8 value) { accessor_->SetUnkCB(value); }  // 0x7100728fe0
u8 Pokemon::GetUnkCC() { return accessor_->GetUnkCC(); }  // 0x7100728ff0
void Pokemon::SetUnkCC(u8 value) { accessor_->SetUnkCC(value); }  // 0x7100729000
u8 Pokemon::GetUnkCD() { return accessor_->GetUnkCD(); }  // 0x7100729010
void Pokemon::SetUnkCD(u8 value) { accessor_->SetUnkCD(value); }  // 0x7100729020
u8 Pokemon::GetUnkCE() { return accessor_->GetUnkCE(); }  // 0x7100729030
void Pokemon::SetUnkCE(u8 value) { accessor_->SetUnkCE(value); }  // 0x7100729040
u8 Pokemon::GetUnkCF() { return accessor_->GetUnkCF(); }  // 0x7100729050
void Pokemon::SetUnkCF(u8 value) { accessor_->SetUnkCF(value); }  // 0x7100729060
u8 Pokemon::GetUnkD0() { return accessor_->GetUnkD0(); }  // 0x7100729070
void Pokemon::SetUnkD0(u8 value) { accessor_->SetUnkD0(value); }  // 0x7100729080
void Pokemon::Serialize(void* record) const { accessor_->Serialize(record); }  // 0x71007294c0
void Pokemon::Deserialize(const void* record) { accessor_->Deserialize(record); }  // 0x71007294d0

// 0x71007247d0
u16 Pokemon::GetHeldItem() {
    return accessor_->GetHeldItem();
}

// 0x71007247a0
bool Pokemon::IsEmpty() const {
    return accessor_->GetSpecies() == 0;
}

// ---- markings, other small fields -----------------------------------------------------------------

// 0x7100724800
bool Pokemon::HasMarking(s32 mark) {
    if (mark > 5) {
        return false;
    }
    return GetBitPair(accessor_->GetMarkings(), mark) != 0;
}

// 0x7100724850
u16 Pokemon::GetMarking(s32 mark) {
    return GetBitPair(accessor_->GetMarkings(), mark);
}

// 0x7100724880
void Pokemon::SetMarking(s32 mark, s32 color) {
    if (mark > 5 || color > 2) {
        return;
    }
    accessor_->SetMarkings(SetBitPair(accessor_->GetMarkings(), mark, color));
}

// 0x71007248f0
void Pokemon::ClearMarking(s32 mark) {
    if (mark > 5) {
        return;
    }
    accessor_->SetMarkings(SetBitPair(accessor_->GetMarkings(), mark, 0));
}

// 0x7100724940: marks above 2 are left as they are
void Pokemon::SetAllMarkings(const s32* marks) {
    for (s32 i = 0; i < 6; i++) {
        if (marks[i] <= 2) {
            accessor_->SetMarkings(SetBitPair(accessor_->GetMarkings(), i, marks[i]));
        }
    }
}

// 0x7100724ac0
void Pokemon::GetAllMarkings(s32* marks) {
    for (s32 i = 0; i < 6; i++) {
        marks[i] = GetBitPair(accessor_->GetMarkings(), i);
    }
}

// 0x7100724bc0
void Pokemon::AddUnkAE(u8 amount) {
    u32 value = static_cast<u8>(accessor_->GetUnkAE()) + amount;
    accessor_->SetUnkAE(value > 0xFE ? 0xFF : value);
}

// 0x7100724c00
void Pokemon::SetUnk3CLowByte(u8 value) {
    accessor_->SetUnk3C(SetLowByte(accessor_->GetUnk3C(), value));
}

// 0x7100724c40
void Pokemon::SetUnk3CHighByte(u8 value) {
    accessor_->SetUnk3C(SetHighByte(accessor_->GetUnk3C(), value));
}

// 0x7100724c80: 10-13 are fields Let's Go doesn't keep
u32 Pokemon::GetMetInfo(u32 field) {
    switch (field) {
    case 0: return accessor_->GetEggYear();
    case 1: return accessor_->GetEggMonth();
    case 2: return accessor_->GetEggDay();
    case 3: return accessor_->GetEggLocation();
    case 4: return accessor_->GetMetYear();
    case 5: return accessor_->GetMetMonth();
    case 6: return accessor_->GetMetDay();
    case 7: return accessor_->GetMetLocation();
    case 8: return accessor_->GetBall();
    case 9: return accessor_->GetMetLevel();
    case 14: return accessor_->GetUnkA4();
    case 15: return accessor_->GetUnkA5();
    case 16: return accessor_->GetUnkA8();
    case 17: return accessor_->GetUnkA6();
    }
    return 0;
}

// 0x7100724d70
void Pokemon::SetMetInfo(u32 field, u32 value) {
    switch (field) {
    case 0: accessor_->SetEggYear(value); break;
    case 1: accessor_->SetEggMonth(value); break;
    case 2: accessor_->SetEggDay(value); break;
    case 3: accessor_->SetEggLocation(value); break;
    case 4: accessor_->SetMetYear(value); break;
    case 5: accessor_->SetMetMonth(value); break;
    case 6: accessor_->SetMetDay(value); break;
    case 7: accessor_->SetMetLocation(value); break;
    case 8: accessor_->SetBall(value); break;
    case 9: accessor_->SetMetLevel(value); break;
    case 14: accessor_->SetUnkA4(value); break;
    case 15: accessor_->SetUnkA5(value); break;
    case 16: accessor_->SetUnkA8(value); break;
    case 17: accessor_->SetUnkA6(value); break;
    }
}

// ---- IVs, EVs, AVs --------------------------------------------------------------------------------

// 0x7100724e50
u8 Pokemon::GetIv(u32 stat) {
    switch (stat) {
    case kStatHp: return accessor_->GetIvHp();
    case kStatAttack: return accessor_->GetIvAttack();
    case kStatDefense: return accessor_->GetIvDefense();
    case kStatSpAttack: return accessor_->GetIvSpAttack();
    case kStatSpDefense: return accessor_->GetIvSpDefense();
    case kStatSpeed: return accessor_->GetIvSpeed();
    }
    return 0;
}

// 0x7100724eb0
u8 Pokemon::GetEffectiveIv(s32 stat) {
    if (stat <= 5 && (accessor_->GetHyperTraining() & 1u << (static_cast<u8>(stat) & 31)) != 0) {
        return 31;
    }
    return GetIv(stat);
}

// 0x7100724f10
bool Pokemon::IsHyperTrained(s32 stat) {
    if (stat > 5) {
        return false;
    }
    return (accessor_->GetHyperTraining() & 1u << (static_cast<u8>(stat) & 31)) != 0;
}

// 0x7100724f70: up to 31; the stat and CP follow
void Pokemon::SetIv(u32 stat, u32 value) {
    u32 iv = value < 31 ? value : 31;
    switch (stat) {
    case kStatHp:
        accessor_->SetIvHp(iv);
        UpdateMaxHp(true);
        break;
    case kStatAttack:
        accessor_->SetIvAttack(iv);
        accessor_->SetAttack(CalcAttack());
        break;
    case kStatDefense:
        accessor_->SetIvDefense(iv);
        accessor_->SetDefense(CalcDefense());
        break;
    case kStatSpAttack:
        accessor_->SetIvSpAttack(iv);
        accessor_->SetSpAttack(CalcSpAttack());
        break;
    case kStatSpDefense:
        accessor_->SetIvSpDefense(iv);
        accessor_->SetSpDefense(CalcSpDefense());
        break;
    case kStatSpeed:
        accessor_->SetIvSpeed(iv);
        accessor_->SetSpeed(CalcSpeed());
        break;
    default:
        return;
    }
    UpdateCp();
}

// 0x71007254c0
u8 Pokemon::GetHiddenPowerType() {
    u8 hp = accessor_->GetIvHp();
    u8 attack = accessor_->GetIvAttack();
    u8 defense = accessor_->GetIvDefense();
    u8 speed = accessor_->GetIvSpeed();
    u8 spAttack = accessor_->GetIvSpAttack();
    u8 spDefense = accessor_->GetIvSpDefense();
    return pokemon::GetHiddenPowerType(hp, attack, defense, speed, spAttack, spDefense);
}

// 0x7100725550
u8 Pokemon::GetEv(u32 stat) {
    switch (stat) {
    case kStatHp: return accessor_->GetEvHp();
    case kStatAttack: return accessor_->GetEvAttack();
    case kStatDefense: return accessor_->GetEvDefense();
    case kStatSpAttack: return accessor_->GetEvSpAttack();
    case kStatSpDefense: return accessor_->GetEvSpDefense();
    case kStatSpeed: return accessor_->GetEvSpeed();
    }
    return 0;
}

// 0x71007255b0
u32 Pokemon::GetEvTotal() {
    u32 total = accessor_->GetEvHp();
    total += accessor_->GetEvAttack();
    total += accessor_->GetEvDefense();
    total += accessor_->GetEvSpAttack();
    total += accessor_->GetEvSpDefense();
    total += accessor_->GetEvSpeed();
    return total;
}

// 0x7100725670: the stat and CP follow
void Pokemon::SetEv(u32 stat, u32 value) {
    u32 ev = ClampEv(GetEv(stat), value);
    switch (stat) {
    case kStatHp:
        accessor_->SetEvHp(ev);
        UpdateMaxHp(true);
        break;
    case kStatAttack:
        accessor_->SetEvAttack(ev);
        accessor_->SetAttack(CalcAttack());
        break;
    case kStatDefense:
        accessor_->SetEvDefense(ev);
        accessor_->SetDefense(CalcDefense());
        break;
    case kStatSpAttack:
        accessor_->SetEvSpAttack(ev);
        accessor_->SetSpAttack(CalcSpAttack());
        break;
    case kStatSpDefense:
        accessor_->SetEvSpDefense(ev);
        accessor_->SetSpDefense(CalcSpDefense());
        break;
    case kStatSpeed:
        accessor_->SetEvSpeed(ev);
        accessor_->SetSpeed(CalcSpeed());
        break;
    default:
        return;
    }
    UpdateCp();
}

// 0x7100725620
void Pokemon::AddEv(u32 stat, u32 amount) {
    u32 ev = GetEv(stat) + amount;
    SetEv(stat, ev < 252 ? ev : 252);
}

// 0x71007257a0
void Pokemon::SubtractEv(u32 stat, u32 amount) {
    u32 ev = GetEv(stat);
    SetEv(stat, ev < amount ? 0 : ev - amount);
}

// 0x71007257e0: at most 252, and a raise only as far as the 510 total allows
u32 Pokemon::ClampEv(u32 current, u32 value) {
    value = value < 252 ? value : 252;
    if (value <= current) {
        return value;
    }
    u32 raise = value - current;
    u32 hp = accessor_->GetEvHp();
    u32 attack = accessor_->GetEvAttack();
    u32 defense = accessor_->GetEvDefense();
    u32 spAttack = accessor_->GetEvSpAttack();
    u32 spDefense = accessor_->GetEvSpDefense();
    u32 speed = accessor_->GetEvSpeed();
    u32 total = hp + raise + attack + defense + spAttack + spDefense + speed;
    return value - (total > 510 ? total - 510 : 0);
}

// 0x71007258a0
u8 Pokemon::GetAv(u32 stat) {
    switch (stat) {
    case kStatHp: return accessor_->GetAvHp();
    case kStatAttack: return accessor_->GetAvAttack();
    case kStatDefense: return accessor_->GetAvDefense();
    case kStatSpAttack: return accessor_->GetAvSpAttack();
    case kStatSpDefense: return accessor_->GetAvSpDefense();
    case kStatSpeed: return accessor_->GetAvSpeed();
    }
    return 0;
}

// 0x7100725940: up to 200; the stat and CP follow
void Pokemon::SetAv(u32 stat, u32 value) {
    u32 av = static_cast<u8>(value) < 200 ? value : 200;
    switch (stat) {
    case kStatHp:
        accessor_->SetAvHp(av);
        UpdateMaxHp(true);
        break;
    case kStatAttack:
        accessor_->SetAvAttack(av);
        accessor_->SetAttack(CalcAttack());
        break;
    case kStatDefense:
        accessor_->SetAvDefense(av);
        accessor_->SetDefense(CalcDefense());
        break;
    case kStatSpAttack:
        accessor_->SetAvSpAttack(av);
        accessor_->SetSpAttack(CalcSpAttack());
        break;
    case kStatSpDefense:
        accessor_->SetAvSpDefense(av);
        accessor_->SetSpDefense(CalcSpDefense());
        break;
    case kStatSpeed:
        accessor_->SetAvSpeed(av);
        accessor_->SetSpeed(CalcSpeed());
        break;
    default:
        return;
    }
    UpdateCp();
}

// 0x7100725900: the sum wraps at 256 before SetAv caps it
void Pokemon::AddAv(u32 stat, u8 amount) {
    SetAv(stat, static_cast<u8>(GetAv(stat)) + amount);
}

// 0x7100725a60
void Pokemon::SetUnkD7(u8 value) {
    accessor_->SetUnkD7(value < 100 ? value : 100);
    RecalcStats(true);
}

// ---- stats ----------------------------------------------------------------------------------------

// 0x7100725bd0
u16 Pokemon::GetStat(u32 stat) {
    switch (stat) {
    case kStatHp: return GetMaxHp();
    case kStatAttack: return accessor_->HasCalcData() ? accessor_->GetAttack() : CalcAttack();
    case kStatDefense: return accessor_->HasCalcData() ? accessor_->GetDefense() : CalcDefense();
    case kStatSpAttack: return accessor_->HasCalcData() ? accessor_->GetSpAttack() : CalcSpAttack();
    case kStatSpDefense: return accessor_->HasCalcData() ? accessor_->GetSpDefense() : CalcSpDefense();
    case kStatSpeed: return accessor_->HasCalcData() ? accessor_->GetSpeed() : CalcSpeed();
    }
    return 0;
}

// 0x7100725d10
u16 Pokemon::GetMaxHp() {
    return accessor_->HasCalcData() ? accessor_->GetMaxHp() : CalcMaxHp();
}

// 0x71007263f0
u16 Pokemon::GetHp() {
    return accessor_->HasCalcData() ? accessor_->GetHp() : CalcMaxHp();
}

// 0x7100725d60
u16 Pokemon::CalcMaxHp() {
    u16 species = accessor_->GetSpecies();
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatHp ? 31 : accessor_->GetIvHp();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseHp);
    return CalcHp(species, level, base, iv, accessor_->GetAvHp());
}

// 0x7100725e50
u16 Pokemon::CalcAttack() {
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatAttack ? 31 : accessor_->GetIvAttack();
    u8 nature = accessor_->GetNature();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseAttack);
    u8 av = accessor_->GetAvAttack();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    return pokemon::CalcAttack(level, base, iv, nature, av, unkD7, friendship);
}

// 0x7100725f70
u16 Pokemon::CalcDefense() {
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatDefense ? 31 : accessor_->GetIvDefense();
    u8 nature = accessor_->GetNature();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseDefense);
    u8 av = accessor_->GetAvDefense();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    return pokemon::CalcDefense(level, base, iv, nature, av, unkD7, friendship);
}

// 0x7100726090
u16 Pokemon::CalcSpAttack() {
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatSpAttack ? 31 : accessor_->GetIvSpAttack();
    u8 nature = accessor_->GetNature();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseSpAttack);
    u8 av = accessor_->GetAvSpAttack();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    return pokemon::CalcSpAttack(level, base, iv, nature, av, unkD7, friendship);
}

// 0x71007261b0
u16 Pokemon::CalcSpDefense() {
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatSpDefense ? 31 : accessor_->GetIvSpDefense();
    u8 nature = accessor_->GetNature();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseSpDefense);
    u8 av = accessor_->GetAvSpDefense();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    return pokemon::CalcSpDefense(level, base, iv, nature, av, unkD7, friendship);
}

// 0x71007262d0
u16 Pokemon::CalcSpeed() {
    u8 level = GetLevel();
    u8 iv = accessor_->GetHyperTraining() & 1 << kStatSpeed ? 31 : accessor_->GetIvSpeed();
    u8 nature = accessor_->GetNature();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 base = personal::GetParam(personal::kParamBaseSpeed);
    u8 av = accessor_->GetAvSpeed();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    return pokemon::CalcSpeed(level, base, iv, nature, av, unkD7, friendship);
}

// 0x7100725080: HP rises with max HP; when max HP drops, HP is capped
void Pokemon::UpdateMaxHp(bool keepFainted) {
    u16 oldMax = GetMaxHp();
    u16 hp = GetHp();
    accessor_->SetMaxHp(CalcMaxHp());
    if (hp == 0 && keepFainted) {
        return;
    }
    u16 max = GetMaxHp();
    accessor_->SetHp(max < oldMax ? (max < hp ? max : hp) : hp + (max - oldMax));
}

// 0x7100725170
void Pokemon::UpdateCp() {
    u8 level = GetLevel();
    u8 nature = accessor_->GetNature();
    u8 unkD7 = accessor_->GetUnkD7();
    u8 friendship = accessor_->GetFriendship();
    u8 ivs[6];
    ivs[kStatHp] = accessor_->GetHyperTraining() & 1 << kStatHp ? 31 : accessor_->GetIvHp();
    ivs[kStatAttack] = accessor_->GetHyperTraining() & 1 << kStatAttack ? 31 : accessor_->GetIvAttack();
    ivs[kStatDefense] = accessor_->GetHyperTraining() & 1 << kStatDefense ? 31 : accessor_->GetIvDefense();
    ivs[kStatSpAttack] = accessor_->GetHyperTraining() & 1 << kStatSpAttack ? 31 : accessor_->GetIvSpAttack();
    ivs[kStatSpDefense] = accessor_->GetHyperTraining() & 1 << kStatSpDefense ? 31 : accessor_->GetIvSpDefense();
    ivs[kStatSpeed] = accessor_->GetHyperTraining() & 1 << kStatSpeed ? 31 : accessor_->GetIvSpeed();
    u8 avHp = accessor_->GetAvHp();
    u8 avAttack = accessor_->GetAvAttack();
    u8 avDefense = accessor_->GetAvDefense();
    u8 avSpAttack = accessor_->GetAvSpAttack();
    u8 avSpDefense = accessor_->GetAvSpDefense();
    u8 avSpeed = accessor_->GetAvSpeed();
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    u32 baseHp = personal::GetParam(personal::kParamBaseHp);
    u32 baseAttack = personal::GetParam(personal::kParamBaseAttack);
    u32 baseDefense = personal::GetParam(personal::kParamBaseDefense);
    u32 baseSpAttack = personal::GetParam(personal::kParamBaseSpAttack);
    u32 baseSpDefense = personal::GetParam(personal::kParamBaseSpDefense);
    u32 baseSpeed = personal::GetParam(personal::kParamBaseSpeed);
    accessor_->SetCp(CalcCp(level, nature, unkD7, friendship, baseHp, baseAttack, baseDefense, baseSpAttack,
                            baseSpDefense, baseSpeed, ivs[kStatHp], ivs[kStatAttack], ivs[kStatDefense],
                            ivs[kStatSpAttack], ivs[kStatSpDefense], ivs[kStatSpeed], avHp, avAttack, avDefense,
                            avSpAttack, avSpDefense, avSpeed));
}

// 0x7100725aa0: in fast mode, so the record is decrypted once
void Pokemon::RecalcStats(bool keepFainted) {
    bool started = false;
    if (!accessor_->IsFastMode()) {
        accessor_->StartFastMode();
        started = true;
    }
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    u32 exp = accessor_->GetExp();
    accessor_->SetPartyLevel(CalcLevelFromExp(species, form, exp));
    UpdateMaxHp(keepFainted);
    accessor_->SetAttack(CalcAttack());
    accessor_->SetDefense(CalcDefense());
    accessor_->SetSpAttack(CalcSpAttack());
    accessor_->SetSpDefense(CalcSpDefense());
    accessor_->SetSpeed(CalcSpeed());
    UpdateCp();
    bool fast = accessor_->IsFastMode();
    if (started && fast) {
        accessor_->EndFastMode();
    }
}

// 0x7100726440
void Pokemon::SetHp(u32 hp) {
    u32 max = accessor_->GetMaxHp();
    accessor_->SetHp(max < hp ? max : hp);
}

// 0x7100726480
void Pokemon::RecoverHp(u32 amount) {
    u32 max = accessor_->GetMaxHp();
    u32 hp = accessor_->GetHp() + amount;
    accessor_->SetHp(max < hp ? max : hp);
}

// 0x71007264d0: a boxed Pokémon is always at full HP
bool Pokemon::IsFullHp() {
    if (!accessor_->HasCalcData()) {
        return true;
    }
    u16 max = accessor_->GetMaxHp();
    return max == accessor_->GetHp();
}

// 0x7100726530
bool Pokemon::IsFainted() const {
    if (!accessor_->HasCalcData()) {
        return false;
    }
    return accessor_->GetHp() == 0;
}

// 0x7100726980
bool Pokemon::HasStatus() {
    return accessor_->GetStatus() != 0;
}

// ---- moves ----------------------------------------------------------------------------------------

// 0x71007273c0
void MoveList::Clear() {
    memset(this, 0, sizeof(*this));
}

// 0x71007273d0: every entry, whatever the count
bool MoveList::Contains(u32 move) {
    for (u32 i = 0; i < 55; i++) {
        if (moves[i] == move) {
            return true;
        }
    }
    return false;
}

// 0x71007270f0
void Pokemon::SetMove(u8 slot, u32 move) {
    if (slot > 3) {
        return;
    }
    u8 pp = moves::GetMaxPp(move, 0);
    accessor_->SetMove(slot, move);
    accessor_->SetMovePpUps(slot, 0);
    accessor_->SetMovePp(slot, pp);
}

// 0x7100726e00
void Pokemon::ClearMove(u8 slot) {
    if (slot > 3) {
        return;
    }
    accessor_->SetMove(slot, 0);
    accessor_->SetMovePpUps(slot, 0);
    accessor_->SetMovePp(slot, 0);
}

// 0x7100726d50
bool Pokemon::HasMove(u32 move) {
    for (u8 slot = 0; slot < 4; slot++) {
        if (accessor_->GetMove(slot) == move) {
            return true;
        }
    }
    return false;
}

// 0x7100726cd0
u8 Pokemon::GetMoveCount() {
    u8 count = 0;
    for (u8 slot = 0; slot < 4; slot++) {
        if (accessor_->GetMove(slot) != 0) {
            count++;
        }
    }
    return count;
}

// 0x7100727180
void Pokemon::SwapMoves(u8 slot1, u8 slot2) {
    if (slot1 == slot2 || slot1 > 3 || slot2 > 3) {
        return;
    }
    u16 move = accessor_->GetMove(slot1);
    u8 pp = accessor_->GetMovePp(slot1);
    u8 ppUps = accessor_->GetMovePpUps(slot1);
    accessor_->SetMove(slot1, accessor_->GetMove(slot2));
    accessor_->SetMovePp(slot1, accessor_->GetMovePp(slot2));
    accessor_->SetMovePpUps(slot1, accessor_->GetMovePpUps(slot2));
    accessor_->SetMove(slot2, move);
    accessor_->SetMovePpUps(slot2, ppUps);
    accessor_->SetMovePp(slot2, pp);
}

// 0x71007272b0
void Pokemon::CompactMoves() {
    if (accessor_->GetMove(0) == 0) {
        SwapMoves(0, 1);
        SwapMoves(1, 2);
        SwapMoves(2, 3);
    }
    if (accessor_->GetMove(1) == 0) {
        SwapMoves(1, 2);
        SwapMoves(2, 3);
    }
    if (accessor_->GetMove(2) == 0) {
        SwapMoves(2, 3);
    }
}

// 0x7100727850
LearnResult Pokemon::LearnMove(u32 move) {
    if (HasMove(move)) {
        return kLearnAlreadyKnown;
    }
    u8 slot = 0;
    while (accessor_->GetMove(slot) != 0) {
        if (++slot == 4) {
            return kLearnMovesFull;
        }
    }
    u8 pp = moves::GetMaxPp(move, 0);
    accessor_->SetMove(slot, move);
    accessor_->SetMovePpUps(slot, 0);
    accessor_->SetMovePp(slot, pp);
    return kLearnLearned;
}

// 0x7100726e70
void Pokemon::PushMove(u32 move) {
    if (HasMove(move)) {
        return;
    }
    u8 slot = 0;
    while (accessor_->GetMove(slot) != 0) {
        if (++slot == 4) {
            break;
        }
    }
    if (slot == 4) {
        for (u8 from = 1; from < 4; from++) {
            u16 shifted = accessor_->GetMove(from);
            u8 pp = accessor_->GetMovePp(from);
            u8 ppUps = accessor_->GetMovePpUps(from);
            accessor_->SetMove(from - 1, shifted);
            accessor_->SetMovePp(from - 1, pp);
            accessor_->SetMovePpUps(from - 1, ppUps);
        }
        slot = 3;
    }
    u8 pp = moves::GetMaxPp(move, 0);
    accessor_->SetMove(slot, move);
    accessor_->SetMovePpUps(slot, 0);
    accessor_->SetMovePp(slot, pp);
}

// 0x7100724050: the level comes from the EXP, even with party stats
void Pokemon::ResetMoves() {
    for (u8 slot = 0; slot < 4; slot++) {
        accessor_->SetMove(slot, 0);
        accessor_->SetMovePpUps(slot, 0);
        accessor_->SetMovePp(slot, 0);
    }
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    u16 levelSpecies = accessor_->GetSpecies();
    u8 levelForm = accessor_->GetForm();
    u32 exp = accessor_->GetExp();
    u8 level = CalcLevelFromExp(levelSpecies, levelForm, exp);
    personal::LoadLevelUpMoves(species, form);
    for (u32 i = 0; i < personal::GetLevelUpMoveCount(); i++) {
        if (personal::GetLevelUpMoveLevel(i) > level) {
            break;
        }
        PushMove(personal::GetLevelUpMove(i));
    }
}

// 0x7100727670
LearnResult Pokemon::LearnLevelUpMove(u32* index, u32* move, MoveList* seen) {
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    u8 level = GetLevel();
    personal::LoadLevelUpMoves(species, form);
    u8 count = personal::GetLevelUpMoveCount();
    u32 atLevel = 0;
    for (u32 i = 0; i < count; i++) {
        u16 moveLevel = personal::GetLevelUpMoveLevel(i);
        u16 candidate = personal::GetLevelUpMove(i);
        bool offered = seen != nullptr && seen->Contains(candidate);
        if (level < moveLevel) {
            break;
        }
        if (level != moveLevel) {
            continue;
        }
        if (!offered && atLevel == *index) {
            if (seen != nullptr) {
                seen->AddIfNew(candidate);
            }
            *move = candidate;
            (*index)++;
            return LearnMove(candidate);
        }
        atLevel++;
    }
    return kLearnNoMove;
}

// 0x7100727980
LearnResult Pokemon::LearnEvolutionMove(u32* index, u32* move, MoveList* seen) {
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    personal::LoadLevelUpMoves(species, form);
    u8 count = personal::GetLevelUpMoveCount();
    for (u32 i = *index; i < count; i++) {
        u16 candidate = personal::GetLevelUpMove(i);
        if (personal::GetLevelUpMoveKind(i) != 0) {
            continue;
        }
        if (seen != nullptr) {
            if (seen->Contains(candidate)) {
                continue;
            }
            seen->AddIfNew(candidate);
        }
        *move = candidate;
        *index = i + 1;
        return LearnMove(candidate);
    }
    return kLearnNoMove;
}

// 0x7100728910: Kyurem's forms (Kyurem, White, Black) each have their own version of two moves.
// A move from another form becomes this form's; its PP is kept, down to the new max PP.
void Pokemon::UpdateKyuremMoves(u8 form) {
    for (u8 slot = 0; slot < 4; slot++) {
        u16 move = accessor_->GetMove(slot);
        if ((form != 0 && move == kMoveScaryFace) || (form != 1 && move == kMoveFusionFlare) ||
            (form != 2 && move == kMoveFusionBolt)) {
            u8 pp = accessor_->GetMovePp(slot);
            accessor_->SetMove(slot, kKyuremMoves[0][form]);
            u8 max = GetMaxPp(slot);
            if (max < pp) {
                accessor_->SetMovePp(slot, max);
            }
        }
        if ((form != 0 && move == kMoveGlaciate) || (form != 1 && move == kMoveIceBurn) ||
            (form != 2 && move == kMoveFreezeShock)) {
            u8 pp = accessor_->GetMovePp(slot);
            accessor_->SetMove(slot, kKyuremMoves[1][form]);
            u8 max = GetMaxPp(slot);
            if (max < pp) {
                accessor_->SetMovePp(slot, max);
            }
        }
    }
}

// 0x7100727370
bool Pokemon::CanLearnMachine(u32 machine) {
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    return personal::CanLearnMachine(machine);
}

// 0x7100726de0
void Pokemon::SetRelearnMove(u8 slot, u16 move) {
    if (slot > 3) {
        return;
    }
    accessor_->SetRelearnMove(slot, move);
}

// 0x71007269d0
u8 Pokemon::GetMovePp(u8 slot) {
    return accessor_->GetMovePp(slot);
}

// 0x71007269f0
void Pokemon::SetMovePp(u8 slot, u8 pp) {
    u8 max = GetMaxPp(slot);
    accessor_->SetMovePp(slot, max < pp ? max : pp);
}

// 0x7100726a60
u8 Pokemon::GetMaxPp(u8 slot) {
    u16 move = accessor_->GetMove(slot);
    return moves::GetMaxPp(move, accessor_->GetMovePpUps(slot));
}

// 0x7100726bb0: nothing for an empty slot
void Pokemon::RecoverPp(u8 slot, u8 amount) {
    if (accessor_->GetMove(slot) == 0) {
        return;
    }
    u8 max = GetMaxPp(slot);
    u32 pp = accessor_->GetMovePp(slot) + amount;
    SetMovePp(slot, pp > max ? max : pp);
}

// 0x7100726ab0
void Pokemon::RecoverAllPp() {
    for (u8 slot = 0; slot < 4; slot++) {
        RecoverPp(slot, GetMaxPp(slot));
    }
}

// 0x7100726c80
u8 Pokemon::GetMovePpUps(u8 slot) {
    return accessor_->GetMovePpUps(slot);
}

// 0x7100726ca0
void Pokemon::SetMovePpUps(u8 slot, u8 ppUps) {
    accessor_->SetMovePpUps(slot, ppUps < 4 ? ppUps : 3);
}

// ---- names, trainers, friendship ------------------------------------------------------------------

// 0x7100727cd0
void Pokemon::SetNickname(const gfl::str::StrBuf* name) {
    const u16* text = name->GetPtr();
    accessor_->SetNickname(text);
    accessor_->SetNicknamed(1);
    u16 species = accessor_->GetSpecies();
    u16 speciesName[14];
    GetSpeciesName(speciesName, species, accessor_->GetLanguage());
    if (gfl::str::StrEqual(text, speciesName)) {
        accessor_->SetNicknamed(0);
    }
}

// 0x71007241e0
void Pokemon::ClearNickname() {
    u16 species = accessor_->GetSpecies();
    u16 speciesName[14];
    GetSpeciesName(speciesName, species, accessor_->GetLanguage());
    accessor_->SetNickname(speciesName);
    accessor_->SetNicknamed(0);
}

// 0x7100727d60
bool Pokemon::IsNicknameSpeciesName() {
    u16 species = accessor_->GetSpecies();
    u16 speciesName[14];
    GetSpeciesName(speciesName, species, GetSystem()->language);
    u16 nickname[13];
    accessor_->GetNickname(nickname, 13);
    return gfl::str::StrEqual(nickname, speciesName);
}

// 0x7100727df0
void Pokemon::SetOtName(const gfl::str::StrBuf* name) {
    accessor_->SetOtName(name->GetPtr());
}

// 0x7100727b30
bool Pokemon::IsOriginalTrainer(const TrainerInfo* trainer) {
    return accessor_->GetOtGender() == trainer->gender && accessor_->GetId() == trainer->id &&
           accessor_->IsOtName(trainer->name);
}

// 0x7100727ba0
bool Pokemon::SetCurrentTrainer(const TrainerInfo* trainer) {
    if (IsOriginalTrainer(trainer)) {
        accessor_->SetCurrentHandler(0);
        return true;
    }
    accessor_->SetCurrentHandler(1);
    accessor_->SetHandlerName(trainer->name);
    accessor_->SetHandlerGender(trainer->gender);
    accessor_->SetUnkA4(0);
    accessor_->SetUnkA5(0);
    accessor_->SetUnkA8(0);
    accessor_->SetUnkA6(0);
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    accessor_->SetHandlerFriendship(personal::GetParam(personal::kParamBaseFriendship));
    return false;
}

// 0x7100727e50
void Pokemon::SetFriendship(u32 friendship) {
    accessor_->SetFriendship(friendship < 256 ? friendship : 255);
    RecalcStats(true);
}

// 0x7100727e90
void Pokemon::AddFriendship(u32 amount) {
    SetFriendship(accessor_->GetFriendship() + amount);
}

// 0x7100727ee0
void Pokemon::SubtractFriendship(u32 amount) {
    u32 friendship = accessor_->GetFriendship();
    SetFriendship(friendship < amount ? 0 : friendship - amount);
}

// 0x7100727f30
void Pokemon::SetOtFriendship(u32 friendship) {
    accessor_->SetOtFriendship(friendship < 256 ? friendship : 255);
}

// 0x7100727f50
u8 Pokemon::GetHandlerFriendship() {
    return accessor_->GetHandlerFriendship();
}

// 0x7100727f70
void Pokemon::SetHandlerFriendship(u32 friendship) {
    accessor_->SetHandlerFriendship(friendship < 256 ? friendship : 255);
}

// ---- other state ----------------------------------------------------------------------------------

// 0x7100725470
void Pokemon::SetHyperTrained(s32 stat) {
    if (stat > 5) {
        return;
    }
    u32 bit = 1u << (static_cast<u8>(stat) & 31);
    accessor_->SetHyperTraining(bit | accessor_->GetHyperTraining());
}

// 0x7100729090
s8 Pokemon::GetNatureModifier(u16 nature, u32 stat) {
    return pokemon::GetNatureModifier(nature, stat);
}

// 0x7100727ad0
u32 Pokemon::GetNatureFlavor(u32 flavor) {
    return pokemon::GetNatureFlavor(accessor_->GetNature(), flavor);
}

// 0x7100724240
void Pokemon::Heal() {
    accessor_->SetHp(accessor_->GetMaxHp());
    accessor_->SetStatus(0);
    RecoverAllPp();
    accessor_->SetUnkED(0);
    accessor_->SetUnkEE(0);
}

// 0x7100727fb0
bool Pokemon::IsEgg(EggCheck check) const {
    bool egg = accessor_->IsEgg();
    bool bad = accessor_->IsBadEgg();
    switch (check) {
    case kEggValid: return egg && !bad;
    case kEggBad: return bad;
    case kEggAny: return egg || bad;
    }
    return false;
}

// 0x7100728b00
bool Pokemon::IsShiny() {
    u32 id = accessor_->GetId();
    return pokemon::IsShiny(id, accessor_->GetPid());
}

// 0x7100728cc0
bool Pokemon::HasPokerus() {
    return accessor_->GetPokerus() != 0;
}

// 0x7100728d00
bool Pokemon::StartFastMode() {
    if (accessor_->IsFastMode()) {
        return false;
    }
    accessor_->StartFastMode();
    return true;
}

// 0x7100728d50
bool Pokemon::EndFastMode(bool started) {
    bool fast = accessor_->IsFastMode();
    if (!fast || !started) {
        return false;
    }
    accessor_->EndFastMode();
    return true;
}

// ---- evolution and forms --------------------------------------------------------------------------

// 0x7100728140
bool Pokemon::CanEvolve() {
    return GetSystem()->evolution->HasEvolutions(this);
}

// 0x7100728170
bool Pokemon::GetLevelUpEvolution(const EvolutionContext* context, const Party* party, u32* species, u32* index) {
    u16 current = accessor_->GetSpecies();
    *species = GetSystem()->evolution->GetLevelUpEvolution(this, party, context, index);
    return current != *species;
}

// 0x71007281e0
bool Pokemon::GetItemEvolution(const EvolutionContext* context, u32 item, u32* species, u32* index) {
    u16 current = accessor_->GetSpecies();
    *species = GetSystem()->evolution->GetItemEvolution(this, context, item, index);
    return current != *species;
}

// 0x7100728250
bool Pokemon::GetTradeEvolution(Pokemon* partner, u32* species, u32* index) {
    u16 current = accessor_->GetSpecies();
    *species = GetSystem()->evolution->GetTradeEvolution(this, partner, index);
    return current != *species;
}

// 0x71007282c0: the evolution's held item is used up, and Silvally takes its memory's form
void Pokemon::Evolve(u32 species, u8 index) {
    u16 oldSpecies = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    personal::LoadEvolutions(oldSpecies, form);
    u16 method = personal::GetEvolutionMethod(index);
    if (personal::HasEvolutionForm(index)) {
        form = personal::GetEvolutionForm(index);
    }
    ChangeSpecies(species, method == kEvolveLevelFemaleForm1 ? form + 1 : form);
    if (method == kEvolveTradeHeldItem || method == kEvolveHeldItemDay || method == kEvolveHeldItemNight) {
        accessor_->SetHeldItem(0);
    }
    u8 ability = accessor_->GetAbility();
    if (species == kSpeciesSilvally && ability == kAbilityRksSystem) {
        ChangeForm(GetMemoryType(accessor_->GetHeldItem()));
    }
}

// 0x71007283c0: keeps the ability slot, and the nickname if it has one
void Pokemon::ChangeSpecies(u32 species, u32 form) {
    if (accessor_->GetSpecies() == species) {
        return;
    }
    accessor_->SetSpecies(species);
    accessor_->SetForm(form);
    bool second = accessor_->IsSecondAbility();
    bool hidden = accessor_->IsHiddenAbility();
    u8 slot = hidden ? 2 : second;
    u16 ability = GetAbilityId(species, form, slot);
    accessor_->SetFirstAbility(slot == 0);
    accessor_->SetSecondAbility(slot == 1);
    accessor_->SetHiddenAbility(hidden);
    accessor_->SetAbility(ability);
    accessor_->SetGender(GetValidGender(species, form, accessor_->GetGender()));
    if (!accessor_->IsNicknamed()) {
        ClearNickname();
    }
    RecalcStats(true);
    UpdateAbsoluteSize();
}

// 0x71007285d0: Kyurem's fusion moves follow
void Pokemon::ChangeForm(u32 form) {
    u16 species = accessor_->GetSpecies();
    if (accessor_->GetForm() == static_cast<u8>(form)) {
        return;
    }
    if (species == kSpeciesKyurem) {
        UpdateKyuremMoves(form);
    }
    accessor_->SetForm(form);
    bool second = accessor_->IsSecondAbility();
    bool hidden = accessor_->IsHiddenAbility();
    accessor_->SetAbility(GetAbilityId(species, form, hidden ? 2 : second));
    accessor_->SetGender(GetValidGender(species, form, accessor_->GetGender()));
    RecalcStats(true);
}

// 0x71007286b0: Meowstic's form 0 is male and form 1 female; true when the gender changed
bool Pokemon::FixGenderForForm() {
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    if (species != kSpeciesMeowstic) {
        return false;
    }
    u8 gender = accessor_->GetGender();
    if (form == 0 && gender != 0) {
        accessor_->SetGender(0);
        return true;
    }
    if (form == 1 && gender != 1) {
        accessor_->SetGender(1);
        return true;
    }
    return false;
}

// 0x7100728750: into the Mega form it can take, remembering its form
void Pokemon::MegaEvolve() {
    u8 form = accessor_->GetForm();
    if (form == GetSystem()->evolution->GetMegaForm(this)) {
        return;
    }
    u8 megaForm = GetSystem()->evolution->GetMegaForm(this);
    u8 oldForm = accessor_->GetForm();
    ChangeForm(megaForm);
    accessor_->SetUnk100(1);
    accessor_->SetUnk101(oldForm);
}

// 0x71007287f0
bool Pokemon::CanMegaEvolve() {
    u8 form = accessor_->GetForm();
    return form != GetSystem()->evolution->GetMegaForm(this);
}

// 0x7100728830
void Pokemon::MegaEvolveTo(u8 form) {
    if (!GetSystem()->evolution->CanMegaEvolveTo(this, form)) {
        return;
    }
    u8 oldForm = accessor_->GetForm();
    ChangeForm(form);
    accessor_->SetUnk100(1);
    accessor_->SetUnk101(oldForm);
}

// 0x71007288b0
void Pokemon::RevertMegaEvolution() {
    if (!accessor_->GetUnk100()) {
        return;
    }
    ChangeForm(accessor_->GetUnk101());
    accessor_->SetUnk100(0);
}

// 0x7100728da0: forgets a Mega Evolution without changing the form back
void Pokemon::ClearMegaEvolution() {
    RecalcStats(true);
    accessor_->SetUnk100(0);
    accessor_->SetUnk101(0);
}

// 0x7100728b60: Arceus's from its plate, Silvally's from its memory
u8 Pokemon::GetType1() const {
    u16 species = accessor_->GetSpecies();
    u8 ability = accessor_->GetAbility();
    if (species == kSpeciesArceus && ability == kAbilityMultitype) {
        return GetPlateType(accessor_->GetHeldItem());
    }
    if (species == kSpeciesSilvally && ability == kAbilityRksSystem) {
        return GetMemoryType(accessor_->GetHeldItem());
    }
    personal::Load(species, accessor_->GetForm());
    return personal::GetParam(personal::kParamType1);
}

// 0x7100728c00
u8 Pokemon::GetType2() const {
    u16 species = accessor_->GetSpecies();
    u8 ability = accessor_->GetAbility();
    if (species == kSpeciesArceus && ability == kAbilityMultitype) {
        return GetPlateType(accessor_->GetHeldItem());
    }
    if (species == kSpeciesSilvally && ability == kAbilityRksSystem) {
        return GetMemoryType(accessor_->GetHeldItem());
    }
    personal::Load(species, accessor_->GetForm());
    return personal::GetParam(personal::kParamType2);
}

// ---- size -----------------------------------------------------------------------------------------

// The scalars scale the species' height by 0.6-1.4 and its weight by 0.8-1.2 (times the height's
// scale). Two of the factors are a float step off 0.8 and 0.4.
static constexpr f32 kHeightRange = 0x1.999998p-1f;
static constexpr f32 kWeightRange = 0x1.99999cp-2f;

// 0x7100728e20
u32 Pokemon::GetHeightClass() {
    u8 scalar = accessor_->GetHeightScalar();
    if (scalar < 0x10) {
        return 0;
    }
    if (scalar < 0x30) {
        return 1;
    }
    if (scalar < 0xD0) {
        return 2;
    }
    return scalar < 0xF0 ? 3 : 4;
}

// 0x71007242c0
void Pokemon::UpdateAbsoluteSize() {
    u32 height = personal::GetParam(personal::kParamHeight);
    u32 weight = personal::GetParam(personal::kParamWeight);
    f32 heightScale = accessor_->GetHeightScalar() / 255.0f * kHeightRange + 0.6f;
    f32 absoluteHeight = heightScale * height;
    f32 weightScale = accessor_->GetWeightScalar() / 255.0f * kWeightRange + 0.8f;
    f32 absoluteWeight = heightScale * (weightScale * weight);
    accessor_->SetHeightAbsolute(absoluteHeight);
    accessor_->SetWeightAbsolute(absoluteWeight);
}

static u8 ClampScalar(f32 value) {
    s32 scalar = static_cast<s32>(value);
    scalar = scalar < 0 ? 0 : scalar;
    return scalar < 255 ? scalar : 255;
}

// 0x7100728e90: the scalars back from the absolute size. The weight's uses the height's scale
// times the weight where the forward formula divides, so it doesn't round-trip.
void Pokemon::SetAbsoluteSize(f32 height, f32 weight) {
    u16 species = accessor_->GetSpecies();
    personal::Load(species, accessor_->GetForm());
    f32 baseHeight = personal::GetParam(personal::kParamHeight);
    f32 baseWeight = personal::GetParam(personal::kParamWeight);
    u8 heightScalar = ClampScalar((height + baseHeight * -0.6f) / (baseHeight * kHeightRange) * 255.0f);
    f32 weightScale = height / baseHeight * weight;
    u8 weightScalar = ClampScalar((weightScale + baseWeight * -0.8f) / (baseWeight * kWeightRange) * 255.0f);
    accessor_->SetHeightAbsolute(height);
    accessor_->SetHeightScalar(heightScalar);
    accessor_->SetWeightAbsolute(weight);
    accessor_->SetWeightScalar(weightScalar);
}

// ---- level and EXP --------------------------------------------------------------------------------

// 0x7100726580
u8 Pokemon::GetLevel() {
    if (accessor_->HasCalcData()) {
        return accessor_->GetPartyLevel();
    }
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    u32 exp = accessor_->GetExp();
    return CalcLevelFromExp(species, form, exp);
}

// 0x7100726600
void Pokemon::AddLevels(u8 levels) {
    u8 level = GetLevel();
    u32 target = level + levels < 100 ? level + levels : 100;
    if (target == level) {
        return;
    }
    personal::LoadGrowthTable(accessor_->GetSpecies(), accessor_->GetForm());
    u32 exp = personal::GetMinExp(target);
    personal::LoadGrowthTable(accessor_->GetSpecies(), accessor_->GetForm());
    u32 max = personal::GetMinExp(100);
    accessor_->SetExp(max < exp ? max : exp);
    RecalcStats(false);
}

// 0x7100726710
void Pokemon::SetExp(u32 exp) {
    personal::LoadGrowthTable(accessor_->GetSpecies(), accessor_->GetForm());
    u32 max = personal::GetMinExp(100);
    accessor_->SetExp(max < exp ? max : exp);
    RecalcStats(false);
}

// 0x7100726790
void Pokemon::AddExp(u32 exp) {
    u32 total = accessor_->GetExp() + exp;
    personal::LoadGrowthTable(accessor_->GetSpecies(), accessor_->GetForm());
    u32 max = personal::GetMinExp(100);
    accessor_->SetExp(max < total ? max : total);
    RecalcStats(false);
}

// 0x7100726810
u32 Pokemon::GetLevelExp() {
    u16 species = accessor_->GetSpecies();
    u8 form = accessor_->GetForm();
    u8 level = GetLevel();
    personal::LoadGrowthTable(species, form);
    return personal::GetMinExp(level);
}

// 0x71007268d0
u32 Pokemon::GetNextLevelExp() {
    u8 level = GetLevel();
    u32 next = level + 1 >= 100 ? 100 : level + 1;
    personal::LoadGrowthTable(accessor_->GetSpecies(), accessor_->GetForm());
    return personal::GetMinExp(next);
}

}  // namespace pokemon
