// Accessor: the record's fields (pokemon/accessor.h). Each accessor decrypts the record, reads or
// writes its field (a bad egg reads the default blocks and writes to discards) and encrypts it again.
#include <string.h>

#include "pokemon/accessor.h"
#include "pokemon/ids.h"
#include "pokemon/personal.h"

namespace pokemon {

// Party stats: nothing without them

u32 Accessor::GetStatus() { if (!calc_) return 0; Access access(this); return Calc().status; }  // 0x7100729bf0
u8 Accessor::GetPartyLevel() { if (!calc_) return 0; Access access(this); return Calc().level; }  // 0x710072a4f0
u16 Accessor::GetHp() { if (!calc_) return 0; Access access(this); return Calc().hp; }  // 0x7100729d10
u16 Accessor::GetMaxHp() { if (!calc_) return 0; Access access(this); return Calc().maxHp; }  // 0x7100729e30
u16 Accessor::GetAttack() { if (!calc_) return 0; Access access(this); return Calc().attack; }  // 0x7100729f50
u16 Accessor::GetDefense() { if (!calc_) return 0; Access access(this); return Calc().defense; }  // 0x710072a070
u16 Accessor::GetSpAttack() { if (!calc_) return 0; Access access(this); return Calc().spAttack; }  // 0x710072a190
u16 Accessor::GetSpDefense() { if (!calc_) return 0; Access access(this); return Calc().spDefense; }  // 0x710072a2b0
u16 Accessor::GetSpeed() { if (!calc_) return 0; Access access(this); return Calc().speed; }  // 0x710072a3d0
u16 Accessor::GetCp() { if (!calc_) return 0; Access access(this); return Calc().cp; }  // 0x710072a610
bool Accessor::GetUnk100() { if (!calc_) return 0; Access access(this); return Calc().unk100 != 0; }  // 0x710072a870
u8 Accessor::GetUnk101() { if (!calc_) return 0; Access access(this); return Calc().unk101; }  // 0x710072a9a0
u8 Accessor::GetUnkAE() { if (!calc_) return 0; Access access(this); return C().unkAE; }  // 0x710072a730

void Accessor::SetStatus(u32 status) { if (!calc_) return; Access access(this); MutableCalc().status = status; }  // 0x71007308f0
void Accessor::SetPartyLevel(u8 level) { if (!calc_) return; Access access(this); MutableCalc().level = level; }  // 0x7100730a10
void Accessor::SetHp(u16 hp) { if (!calc_) return; Access access(this); MutableCalc().hp = hp; }  // 0x7100730c50
void Accessor::SetMaxHp(u16 maxHp) { if (!calc_) return; Access access(this); MutableCalc().maxHp = maxHp; }  // 0x7100730b30
void Accessor::SetAttack(u16 value) { if (!calc_) return; Access access(this); MutableCalc().attack = value; }  // 0x7100730d70
void Accessor::SetDefense(u16 value) { if (!calc_) return; Access access(this); MutableCalc().defense = value; }  // 0x7100730e90
void Accessor::SetSpAttack(u16 value) { if (!calc_) return; Access access(this); MutableCalc().spAttack = value; }  // 0x7100730fb0
void Accessor::SetSpDefense(u16 value) { if (!calc_) return; Access access(this); MutableCalc().spDefense = value; }  // 0x71007310d0
void Accessor::SetSpeed(u16 value) { if (!calc_) return; Access access(this); MutableCalc().speed = value; }  // 0x71007311f0
void Accessor::SetCp(u16 cp) { if (!calc_) return; Access access(this); MutableCalc().cp = cp; }  // 0x7100731310
void Accessor::SetUnk100(u8 flag) { if (!calc_) return; Access access(this); MutableCalc().unk100 = flag & 1; }  // 0x7100731570
void Accessor::SetUnk101(u8 value) { if (!calc_) return; Access access(this); MutableCalc().unk101 = value; }  // 0x7100731690
void Accessor::SetUnkAE(u8 value) { if (!calc_) return; Access access(this); MutableC().unkAE = value; }  // 0x7100731430
void Accessor::SetUnkED(u8 value) { if (!calc_) return; Access access(this); MutableCalc().unkED = value; }  // 0x7100738530
void Accessor::SetUnkEE(u8 value) { if (!calc_) return; Access access(this); MutableCalc().unkEE = value; }  // 0x7100738650

// Block A
u16 Accessor::GetSpecies() { Access access(this); return A().species; }  // 0x710072aac0
u16 Accessor::GetHeldItem() { Access access(this); return A().heldItem; }  // 0x710072abf0
u32 Accessor::GetId() { Access access(this); return A().id; }  // 0x710072ad20
u32 Accessor::GetExp() { Access access(this); return A().exp; }  // 0x710072ae50
u8 Accessor::GetAbility() { Access access(this); return A().ability; }  // 0x710072b130
bool Accessor::IsSecondAbility() { Access access(this); return (A().abilityFlags >> 1) & 1; }  // 0x710072d730
bool Accessor::IsHiddenAbility() { Access access(this); return (A().abilityFlags >> 2) & 1; }  // 0x710072d860
bool Accessor::IsFavorite() { Access access(this); return (A().abilityFlags >> 3) & 1; }  // 0x710072d990
u16 Accessor::GetMarkings() { Access access(this); return A().markings; }  // 0x710072b260
u32 Accessor::GetPid() { Access access(this); return A().pid; }  // 0x710072dac0
u8 Accessor::GetNature() { Access access(this); return A().nature; }  // 0x710072d600
bool Accessor::IsFatefulEncounter() { Access access(this); return A().genderForm & 1; }  // 0x710072d290
u8 Accessor::GetGender() { Access access(this); return (A().genderForm >> 1) & 3; }  // 0x710072d3c0
u8 Accessor::GetForm() { Access access(this); return A().genderForm >> 3; }  // 0x710072d4e0
u8 Accessor::GetEvHp() { Access access(this); return A().ev[0]; }  // 0x710072b4c0
u8 Accessor::GetEvAttack() { Access access(this); return A().ev[1]; }  // 0x710072b5f0
u8 Accessor::GetEvDefense() { Access access(this); return A().ev[2]; }  // 0x710072b720
u8 Accessor::GetEvSpAttack() { Access access(this); return A().ev[4]; }  // 0x710072b850
u8 Accessor::GetEvSpDefense() { Access access(this); return A().ev[5]; }  // 0x710072b980
u8 Accessor::GetEvSpeed() { Access access(this); return A().ev[3]; }  // 0x710072bab0
u8 Accessor::GetAvHp() { Access access(this); return A().av[0]; }  // 0x710072bbe0
u8 Accessor::GetAvAttack() { Access access(this); return A().av[1]; }  // 0x710072bd10
u8 Accessor::GetAvDefense() { Access access(this); return A().av[2]; }  // 0x710072be40
u8 Accessor::GetAvSpAttack() { Access access(this); return A().av[4]; }  // 0x710072bf70
u8 Accessor::GetAvSpDefense() { Access access(this); return A().av[5]; }  // 0x710072c0a0
u8 Accessor::GetAvSpeed() { Access access(this); return A().av[3]; }  // 0x710072c1d0
u8 Accessor::GetPokerus() { Access access(this); return A().pokerus; }  // 0x710072ebf0
f32 Accessor::GetHeightAbsolute() { Access access(this); return A().heightAbsolute; }  // 0x710072f8f0
u8 Accessor::GetHeightScalar() { Access access(this); return A().heightScalar; }  // 0x710072fb50
u8 Accessor::GetWeightScalar() { Access access(this); return A().weightScalar; }  // 0x710072fc80
u32 Accessor::GetUnk3C() { Access access(this); return A().unk3C; }  // 0x7100738190

// Block B
u8 Accessor::GetIvHp() { Access access(this); return B().ivs & 0x1F; }  // 0x710072c970
u8 Accessor::GetIvAttack() { Access access(this); return (B().ivs >> 5) & 0x1F; }  // 0x710072ca90
u8 Accessor::GetIvDefense() { Access access(this); return (B().ivs >> 10) & 0x1F; }  // 0x710072cbb0
u8 Accessor::GetIvSpAttack() { Access access(this); return (B().ivs >> 20) & 0x1F; }  // 0x710072ccd0
u8 Accessor::GetIvSpDefense() { Access access(this); return (B().ivs >> 25) & 0x1F; }  // 0x710072cdf0
u8 Accessor::GetIvSpeed() { Access access(this); return (B().ivs >> 15) & 0x1F; }  // 0x710072cf10
bool Accessor::IsEgg() { Access access(this); return (B().ivs >> 30) & 1; }  // 0x710072d030
bool Accessor::IsNicknamed() { Access access(this); return B().ivs >> 31; }  // 0x710072d160

// Block C
u32 Accessor::GetUnk94() { Access access(this); return C().unk94; }  // 0x710072f090
u8 Accessor::GetHandlerFriendship() { Access access(this); return C().handlerFriendship; }  // 0x710072f1c0
u8 Accessor::GetUnkA4() { Access access(this); return C().unkA4; }  // 0x710072f2f0
u8 Accessor::GetUnkA5() { Access access(this); return C().unkA5; }  // 0x710072f420
u8 Accessor::GetUnkA6() { Access access(this); return C().unkA6; }  // 0x710072f680
u16 Accessor::GetUnkA8() { Access access(this); return C().unkA8; }  // 0x710072f550
u8 Accessor::GetUnkAC() { Access access(this); return C().unkAC; }  // 0x710072fdb0
u8 Accessor::GetUnkAD() { Access access(this); return C().unkAD; }  // 0x710072fee0

// Block D
u8 Accessor::GetUnkCB() { Access access(this); return D().unkCB[0]; }  // 0x7100730010
u8 Accessor::GetUnkCC() { Access access(this); return D().unkCB[1]; }  // 0x7100730140
u8 Accessor::GetUnkCD() { Access access(this); return D().unkCB[2]; }  // 0x7100730270
u8 Accessor::GetUnkCE() { Access access(this); return D().unkCB[3]; }  // 0x71007303a0
u8 Accessor::GetUnkCF() { Access access(this); return D().unkCB[4]; }  // 0x71007304d0
u8 Accessor::GetUnkD0() { Access access(this); return D().unkCB[5]; }  // 0x7100730600
u8 Accessor::GetEggYear() { Access access(this); return D().eggDate[0]; }  // 0x710072e270
u8 Accessor::GetEggMonth() { Access access(this); return D().eggDate[1]; }  // 0x710072e3a0
u8 Accessor::GetEggDay() { Access access(this); return D().eggDate[2]; }  // 0x710072e4d0
u8 Accessor::GetMetYear() { Access access(this); return D().metDate[0]; }  // 0x710072e600
u8 Accessor::GetMetMonth() { Access access(this); return D().metDate[1]; }  // 0x710072e730
u8 Accessor::GetMetDay() { Access access(this); return D().metDate[2]; }  // 0x710072e860
u8 Accessor::GetUnkD7() { Access access(this); return D().unkD7; }  // 0x710072c300
u16 Accessor::GetEggLocation() { Access access(this); return D().eggLocation; }  // 0x710072e990
u16 Accessor::GetMetLocation() { Access access(this); return D().metLocation; }  // 0x710072eac0
u8 Accessor::GetBall() { Access access(this); return D().ball; }  // 0x710072ed20
u8 Accessor::GetMetLevel() { Access access(this); return D().metLevelOtGender & 0x7F; }  // 0x710072ee50
u8 Accessor::GetOtGender() { Access access(this); return D().metLevelOtGender >> 7; }  // 0x710072ef70
u8 Accessor::GetHyperTraining() { Access access(this); return D().hyperTraining; }  // 0x7100738400
u8 Accessor::GetVersion() { Access access(this); return D().version; }  // 0x710072ded0
u8 Accessor::GetLanguage() { Access access(this); return D().language; }  // 0x710072b390
f32 Accessor::GetWeightAbsolute() { Access access(this); return D().weightAbsolute; }  // 0x710072fa20

// Block A
void Accessor::SetSpecies(u16 species) { Access access(this); MutableA().species = species; }  // 0x71007317b0
void Accessor::SetHeldItem(u16 item) { Access access(this); MutableA().heldItem = item; }  // 0x71007318f0
void Accessor::SetId(u32 id) { Access access(this); MutableA().id = id; }  // 0x7100731a30
void Accessor::SetExp(u32 exp) { Access access(this); MutableA().exp = exp; }  // 0x7100731b70
void Accessor::SetAbility(u8 ability) { Access access(this); MutableA().ability = ability; }  // 0x7100731e70
void Accessor::SetFirstAbility(u8 flag) { Access access(this); u8& abilityFlags = MutableA().abilityFlags; abilityFlags = (abilityFlags & ~1) | (flag & 1); }  // 0x7100734a10
void Accessor::SetSecondAbility(u8 flag) { Access access(this); u8& abilityFlags = MutableA().abilityFlags; abilityFlags = (abilityFlags & ~2) | (flag & 1) << 1; }  // 0x7100734b50
void Accessor::SetHiddenAbility(u8 flag) { Access access(this); u8& abilityFlags = MutableA().abilityFlags; abilityFlags = (abilityFlags & ~4) | (flag & 1) << 2; }  // 0x7100734c90
void Accessor::SetFavorite(u8 flag) { Access access(this); u8& abilityFlags = MutableA().abilityFlags; abilityFlags = (abilityFlags & ~8) | (flag & 1) << 3; }  // 0x7100734dd0
void Accessor::SetMarkings(u16 markings) { Access access(this); MutableA().markings = markings; }  // 0x7100731fb0
void Accessor::SetPid(u32 pid) { Access access(this); MutableA().pid = pid; }  // 0x7100734f10
void Accessor::SetNature(u8 nature) { Access access(this); MutableA().nature = nature; }  // 0x71007348d0
void Accessor::SetFatefulEncounter(u8 flag) { Access access(this); u8& genderForm = MutableA().genderForm; genderForm = (genderForm & ~1) | (flag & 1); }  // 0x7100734520
void Accessor::SetGender(u8 gender) { Access access(this); u8& genderForm = MutableA().genderForm; genderForm = (genderForm & ~6) | (gender & 3) << 1; }  // 0x7100734660
void Accessor::SetForm(u8 form) { Access access(this); u8& genderForm = MutableA().genderForm; genderForm = (genderForm & 7) | form << 3; }  // 0x71007347a0
void Accessor::SetEvHp(u8 value) { Access access(this); MutableA().ev[0] = value; }  // 0x71007325f0
void Accessor::SetEvAttack(u8 value) { Access access(this); MutableA().ev[1] = value; }  // 0x7100732730
void Accessor::SetEvDefense(u8 value) { Access access(this); MutableA().ev[2] = value; }  // 0x7100732870
void Accessor::SetEvSpAttack(u8 value) { Access access(this); MutableA().ev[4] = value; }  // 0x71007329b0
void Accessor::SetEvSpDefense(u8 value) { Access access(this); MutableA().ev[5] = value; }  // 0x7100732af0
void Accessor::SetEvSpeed(u8 value) { Access access(this); MutableA().ev[3] = value; }  // 0x7100732c30
void Accessor::SetAvHp(u8 value) { Access access(this); MutableA().av[0] = value; }  // 0x7100732d70
void Accessor::SetAvAttack(u8 value) { Access access(this); MutableA().av[1] = value; }  // 0x7100732eb0
void Accessor::SetAvDefense(u8 value) { Access access(this); MutableA().av[2] = value; }  // 0x7100732ff0
void Accessor::SetAvSpAttack(u8 value) { Access access(this); MutableA().av[4] = value; }  // 0x7100733130
void Accessor::SetAvSpDefense(u8 value) { Access access(this); MutableA().av[5] = value; }  // 0x7100733270
void Accessor::SetAvSpeed(u8 value) { Access access(this); MutableA().av[3] = value; }  // 0x71007333b0
void Accessor::SetHeightAbsolute(f32 height) { Access access(this); MutableA().heightAbsolute = height; }  // 0x7100736e00
void Accessor::SetHeightScalar(u8 scalar) { Access access(this); MutableA().heightScalar = scalar; }  // 0x7100737080
void Accessor::SetWeightScalar(u8 scalar) { Access access(this); MutableA().weightScalar = scalar; }  // 0x71007371c0
void Accessor::SetUnk3C(u32 value) { Access access(this); MutableA().unk3C = value; }  // 0x7100738050

// Block B
void Accessor::SetIvHp(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~0x1Fu) | (iv & 0x1F); }  // 0x7100733b30
void Accessor::SetIvAttack(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(0x1Fu << 5)) | (iv & 0x1F) << 5; }  // 0x7100733c60
void Accessor::SetIvDefense(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(0x1Fu << 10)) | (iv & 0x1F) << 10; }  // 0x7100733da0
void Accessor::SetIvSpAttack(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(0x1Fu << 20)) | (iv & 0x1F) << 20; }  // 0x7100733ee0
void Accessor::SetIvSpDefense(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(0x1Fu << 25)) | (iv & 0x1F) << 25; }  // 0x7100734020
void Accessor::SetIvSpeed(u8 iv) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(0x1Fu << 15)) | (iv & 0x1F) << 15; }  // 0x7100734160
void Accessor::SetEgg(u8 flag) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(1u << 30)) | (flag & 1u) << 30; }  // 0x71007342a0
void Accessor::SetNicknamed(u8 flag) { Access access(this); u32& ivs = MutableB().ivs; ivs = (ivs & ~(1u << 31)) | (flag & 1u) << 31; }  // 0x71007343e0

// Block C
void Accessor::SetHandlerGender(u8 gender) { Access access(this); MutableC().handlerGender = gender; }  // 0x7100736cc0
void Accessor::SetCurrentHandler(u8 handler) { Access access(this); MutableC().currentHandler = handler & 1; }  // 0x7100736190
void Accessor::SetUnk94(u32 value) { Access access(this); MutableC().unk94 = value; }  // 0x71007362d0
void Accessor::SetHandlerFriendship(u8 friendship) { Access access(this); MutableC().handlerFriendship = friendship; }  // 0x7100736550
void Accessor::SetUnkA4(u8 value) { Access access(this); MutableC().unkA4 = value; }  // 0x7100736690
void Accessor::SetUnkA5(u8 value) { Access access(this); MutableC().unkA5 = value; }  // 0x71007367d0
void Accessor::SetUnkA6(u8 value) { Access access(this); MutableC().unkA6 = value; }  // 0x7100736a50
void Accessor::SetUnkA8(u16 value) { Access access(this); MutableC().unkA8 = value; }  // 0x7100736910
void Accessor::SetUnkAC(u8 value) { Access access(this); MutableC().unkAC = value; }  // 0x7100737300
void Accessor::SetUnkAD(u8 value) { Access access(this); MutableC().unkAD = value; }  // 0x7100737440

// Block D
void Accessor::SetOtFriendship(u8 friendship) { Access access(this); MutableD().otFriendship = friendship; }  // 0x7100736410
void Accessor::SetUnkCB(u8 value) { Access access(this); MutableD().unkCB[0] = value; }  // 0x7100737580
void Accessor::SetUnkCC(u8 value) { Access access(this); MutableD().unkCB[1] = value; }  // 0x71007376c0
void Accessor::SetUnkCD(u8 value) { Access access(this); MutableD().unkCB[2] = value; }  // 0x7100737800
void Accessor::SetUnkCE(u8 value) { Access access(this); MutableD().unkCB[3] = value; }  // 0x7100737940
void Accessor::SetUnkCF(u8 value) { Access access(this); MutableD().unkCB[4] = value; }  // 0x7100737a80
void Accessor::SetUnkD0(u8 value) { Access access(this); MutableD().unkCB[5] = value; }  // 0x7100737bc0
void Accessor::SetEggYear(u8 year) { Access access(this); MutableD().eggDate[0] = year; }  // 0x71007353f0
void Accessor::SetEggMonth(u8 month) { Access access(this); MutableD().eggDate[1] = month; }  // 0x7100735530
void Accessor::SetEggDay(u8 day) { Access access(this); MutableD().eggDate[2] = day; }  // 0x7100735670
void Accessor::SetMetYear(u8 year) { Access access(this); MutableD().metDate[0] = year; }  // 0x71007357b0
void Accessor::SetMetMonth(u8 month) { Access access(this); MutableD().metDate[1] = month; }  // 0x71007358f0
void Accessor::SetMetDay(u8 day) { Access access(this); MutableD().metDate[2] = day; }  // 0x7100735a30
void Accessor::SetUnkD7(u8 value) { Access access(this); MutableD().unkD7 = value; }  // 0x71007334f0
void Accessor::SetEggLocation(u16 location) { Access access(this); MutableD().eggLocation = location; }  // 0x7100735b70
void Accessor::SetMetLocation(u16 location) { Access access(this); MutableD().metLocation = location; }  // 0x7100735cb0
void Accessor::SetBall(u8 ball) { Access access(this); MutableD().ball = ball; }  // 0x7100735df0
void Accessor::SetMetLevel(u8 level) { Access access(this); u8& metLevelOtGender = MutableD().metLevelOtGender; metLevelOtGender = (metLevelOtGender & 0x80) | (level & 0x7F); }  // 0x7100735f30
void Accessor::SetOtGender(u8 gender) { Access access(this); u8& metLevelOtGender = MutableD().metLevelOtGender; metLevelOtGender = (metLevelOtGender & 0x7F) | gender << 7; }  // 0x7100736060
void Accessor::SetHyperTraining(u8 flags) { Access access(this); MutableD().hyperTraining = flags; }  // 0x71007382c0
void Accessor::SetVersion(u8 version) { Access access(this); MutableD().version = version; }  // 0x7100735180
void Accessor::SetCountry(u8 country) { Access access(this); MutableD().country = country; }  // 0x71007320f0
void Accessor::SetRegion(u8 region) { Access access(this); MutableD().region = region; }  // 0x7100732230
void Accessor::SetConsoleRegion(u8 region) { Access access(this); MutableD().consoleRegion = region; }  // 0x7100732370
void Accessor::SetLanguage(u8 language) { Access access(this); MutableD().language = language; }  // 0x71007324b0
void Accessor::SetWeightAbsolute(f32 weight) { Access access(this); MutableD().weightAbsolute = weight; }  // 0x7100736f40

// Party stats and the rest of the record (written out)

// 0x710072af80: the friendship towards the current trainer
u8 Accessor::GetFriendship() {
    Access access(this);
    return C().currentHandler != 0 ? C().handlerFriendship : D().otFriendship;
}

// 0x7100731cb0
void Accessor::SetFriendship(u8 friendship) {
    Access access(this);
    if (C().currentHandler != 0) {
        MutableC().handlerFriendship = friendship;
    } else {
        MutableD().otFriendship = friendship;
    }
}

// 0x710072c430
u16 Accessor::GetMove(u8 slot) {
    if (slot > 3) {
        return 0;
    }
    Access access(this);
    return B().moves[slot];
}

// 0x710072c580
u8 Accessor::GetMovePp(u8 slot) {
    if (slot > 3) {
        return 0;
    }
    Access access(this);
    return B().pp[slot];
}

// 0x710072c6d0
u8 Accessor::GetMovePpUps(u8 slot) {
    if (slot > 3) {
        return 0;
    }
    Access access(this);
    return B().ppUps[slot];
}

// 0x710072c820
u16 Accessor::GetRelearnMove(u8 slot) {
    if (slot > 3) {
        return 0;
    }
    Access access(this);
    return B().relearnMoves[slot];
}

// 0x7100733630
void Accessor::SetMove(u8 slot, u16 move) {
    if (slot > 3) {
        return;
    }
    Access access(this);
    MutableB().moves[slot] = move;
}

// 0x7100733770
void Accessor::SetMovePp(u8 slot, u8 pp) {
    if (slot > 3) {
        return;
    }
    Access access(this);
    MutableB().pp[slot] = pp;
}

// 0x71007338b0
void Accessor::SetMovePpUps(u8 slot, u8 ppUps) {
    if (slot > 3) {
        return;
    }
    Access access(this);
    MutableB().ppUps[slot] = ppUps;
}

// 0x71007339f0
void Accessor::SetRelearnMove(u8 slot, u16 move) {
    if (slot > 3) {
        return;
    }
    Access access(this);
    MutableB().relearnMoves[slot] = move;
}

// 0x7100737d00: ribbons 0-63
bool Accessor::HasRibbon(u32 ribbon) {
    Access access(this);
    if (ribbon < 32) {
        return (A().ribbons[0] >> ribbon) & 1;
    }
    if (ribbon < 64) {
        return (A().ribbons[1] >> (ribbon - 32)) & 1;
    }
    return false;
}

// 0x7100737eb0
void Accessor::SetRibbon(u32 ribbon) {
    Access access(this);
    if (ribbon < 32) {
        MutableA().ribbons[0] |= 1u << ribbon;
    } else if (ribbon < 64) {
        MutableA().ribbons[1] |= 1u << (ribbon - 32);
    }
}

// 0x710072dbf0: a bad egg is called "Egg" in its language
void Accessor::GetNickname(gfl::str::StrBuf* out) {
    if (BadEgg()) {
        GetSpeciesName(out, kSpeciesEgg, GetLanguage());
        return;
    }
    Access access(this);
    out->SetStr(B().nickname);
}

// 0x710072dd50: at most size - 1 characters (and no more than 12)
void Accessor::GetNickname(u16* out, u32 size) {
    u32 length = size < 13 ? size : 13;
    if (BadEgg()) {
        u16 name[13];
        GetSpeciesName(name, kSpeciesEgg, GetLanguage());
        gfl::str::StrNCopy(out, name, length);
        return;
    }
    Access access(this);
    gfl::str::StrNCopy(out, B().nickname, length);
}

// 0x7100735050
void Accessor::SetNickname(const u16* name) {
    Access access(this);
    gfl::str::StrNCopy(MutableB().nickname, name, 13);
}

// 0x7100736b90
void Accessor::SetHandlerName(const u16* name) {
    Access access(this);
    gfl::str::StrNCopy(MutableC().handlerName, name, 13);
}

// 0x710072e000
void Accessor::GetOtName(gfl::str::StrBuf* out) {
    Access access(this);
    out->SetStr(D().otName);
}

// 0x710072e130
void Accessor::GetOtName(u16* out, u32 size) {
    Access access(this);
    gfl::str::StrNCopy(out, D().otName, size < 13 ? size : 13);
}

// 0x710072f7b0
bool Accessor::IsOtName(const u16* name) {
    Access access(this);
    return gfl::str::StrEqual(D().otName, name);
}

// 0x71007352c0
void Accessor::SetOtName(const u16* name) {
    Access access(this);
    gfl::str::StrNCopy(MutableD().otName, name, 13);
}

// 0x7100730730
void Accessor::SetEncryptionKey(u32 key) {
    Access access(this);
    const u8* from = kBlockOrder[ShuffleValue(core_->encryptionKey)];
    const u8* to = kBlockOrder[ShuffleValue(key)];
    u8 blocks[kBlockCount][kBlockSize];
    for (u32 i = 0; i < kBlockCount; i++) {
        memcpy(blocks[to[i]], core_->blocks[from[i]], kBlockSize);
    }
    memcpy(core_->blocks, blocks, sizeof(blocks));
    core_->encryptionKey = key;
}

// 0x71007308d0: only the flag; the record stays as it is
void Accessor::SetBadEgg(u8 badEgg) {
    core_->flags = (core_->flags & ~kFlagBadEgg) | (badEgg & 1) << 2;
}

}  // namespace pokemon
