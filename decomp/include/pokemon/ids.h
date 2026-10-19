// Species, move, ability and type IDs that code checks for by name (the national numbering, and
// the main series' IDs for the rest)
#pragma once
#include "decomp.h"

namespace pokemon {

enum : u16 {
    kSpeciesKadabra = 64,
    kSpeciesKyogre = 382,
    kSpeciesGroudon = 383,
    kSpeciesArceus = 493,
    kSpeciesKarrablast = 588,
    kSpeciesShelmet = 616,
    kSpeciesKyurem = 646,
    kSpeciesFloette = 670,
    kSpeciesMeowstic = 678,
    kSpeciesSilvally = 773,
    kSpeciesEgg = 810,              // an egg: the name list's entry, what bad eggs read as
};

enum : u16 {
    kMoveScaryFace = 184,
    kMoveGlaciate = 549,
    kMoveFreezeShock = 553,
    kMoveIceBurn = 554,
    kMoveFusionFlare = 558,
    kMoveFusionBolt = 559,
    kMoveDragonAscent = 620,
};

enum : u16 {
    kAbilityMultitype = 121,
    kAbilityRksSystem = 225,
};

enum : u8 {
    kTypeDark = 16,
};

enum : u8 {
    kFormFloetteEternal = 5,
};

}  // namespace pokemon
