// The battle's events: handler sets, calling an event's handlers, and the event variables (battle/event.h).
#include "battle/event.h"

#include <cstring>

// Whether a move set's work[6] is set (in the move handlers' code, not decompiled yet)
extern "C" bool FUN_7100a3a670(battle::HandlerSet* set, u32* work);

namespace battle {

namespace {

constexpr u32 kOwnedKinds = 1u << kKindMove | 1u << kKindAbility | 1u << kKindItem | 1u << kKind6;

// Takes a set out of the list and gives it back to the free ones (unless none are handed out).
inline void Free(HandlerSet* set) {
    if (g_firstHandlerSet == set) g_firstHandlerSet = set->next;
    if (set->prev) set->prev->next = set->next;
    if (set->next) set->next->prev = set->prev;
    u32 used = g_handlerSetsUsed;
    if (used == 0) return;
    memset(set, 0, sizeof(HandlerSet));
    g_handlerSetsUsed = used - 1;
    g_freeHandlerSets[used - 1] = set;
}

// A variable's slot in the current frame, or -1: the frame ends at the first empty label.
inline int FindSlot(u32 label) {
    for (u32 i = g_vars.frame; i < kMaxVars; i++) {
        if (g_vars.labels[i] == 0) return -1;
        if (g_vars.labels[i] == label) return i;
    }
    return -1;
}

// Where Set puts a variable: its own slot or the first empty one, and slot 0 when the frame is full.
inline int SlotToSet(u32 label) {
    for (u32 i = g_vars.frame; i < kMaxVars; i++)
        if (g_vars.labels[i] == 0 || g_vars.labels[i] == label) return i;
    return 0;
}

inline void Put(u32 label, s32 value, s32 min, s32 max, u8 mode) {
    int i = SlotToSet(label);
    g_vars.labels[i] = label;
    g_vars.values[i] = value;
    g_vars.min[i] = min;
    g_vars.max[i] = max;
    g_vars.modes[i] = mode;
}

// Calls an event's handlers from the first set on: CallHandlers gives the set to go on with, which is
// the one after the current set unless the list changed under it.
inline void Call(Flow* flow, u32 event, bool checkSkips) {
    g_eventDepth++;
    HandlerSet* set = g_firstHandlerSet;
    if (set) {
        HandlerSet* next = CallHandlers(flow, event, kKindAny, checkSkips, set);
        while (next) {
            if ((next->flags & (HandlerSet::kInUse | HandlerSet::kNextToCall)) ==
                (HandlerSet::kInUse | HandlerSet::kNextToCall)) {
                next->flags &= ~HandlerSet::kNextToCall;
                set = next;
            } else {
                set = set->next;
                if (!set) break;
            }
            next = CallHandlers(flow, event, kKindAny, checkSkips, set);
        }
    }
    if (--g_eventDepth == 0)
        for (HandlerSet* s = g_firstHandlerSet; s; s = s->next) s->flags &= ~HandlerSet::kAddedAt;
}

}  // namespace

// 0x7100a66740
void ResetEvents() {
    memset(g_handlerSets, 0, sizeof(g_handlerSets));
    for (u32 i = 0; i < kMaxHandlerSets; i++) g_freeHandlerSets[i] = &g_handlerSets[i];
    g_firstHandlerSet = nullptr;
    g_handlerSetsUsed = 0;
    g_eventDepth = 0;
    memset(&g_vars, 0, offsetof(EventVars, values));
}

// 0x7100a667c0
void WakeAllHandlerSets() {
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next) set->flags &= ~HandlerSet::kAsleep;
}

// 0x7100a667f0
HandlerSet* AddHandlerSet(u32 kind, u16 id, u32 subPriority, u32 priority, u8 pokemon, const Handler* handlers,
                          u32 count) {
    if (g_handlerSetsUsed == kMaxHandlerSets) return nullptr;
    HandlerSet* set = g_freeHandlerSets[g_handlerSetsUsed++];
    if (!set) return nullptr;
    u32 key = (9 - subPriority) << 24 | priority;
    set->prev = nullptr;
    set->next = nullptr;
    set->handlers = handlers;
    set->skipCheck = nullptr;
    set->kind = kind;
    set->priority = key;
    set->id = id;
    set->pokemon = pokemon;
    set->flags = (g_eventDepth & HandlerSet::kAddedAt) | (count & 0xff) << HandlerSet::kCountShift | HandlerSet::kInUse;
    set->owner = kind <= kKind6 && (1u << kind & kOwnedKinds) ? pokemon : kNoPokemon;
    memset(set->work, 0, sizeof(set->work));

    HandlerSet* at = g_firstHandlerSet;
    if (!at) {
        g_firstHandlerSet = set;
        return set;
    }
    if (key > at->priority) {
        at->prev = set;
        set->prev = nullptr;
        set->next = g_firstHandlerSet;
        g_firstHandlerSet = set;
        return set;
    }
    // after the last set whose priority is at least as high
    for (;;) {
        HandlerSet* next = at->next;
        if (!next) {
            at->next = set;
            set->prev = at;
            return set;
        }
        at = next;
        if (key > at->priority) break;
    }
    set->next = at;
    set->prev = at->prev;
    if (at->prev) at->prev->next = set;
    at->prev = set;
    return set;
}

// 0x7100a66910
void FreeRemovedHandlerSets() {
    for (HandlerSet* set = g_firstHandlerSet, *next; set; set = next) {
        next = set->next;
        if (set->kind != kKindRemoved || !(set->flags & HandlerSet::kInUse)) continue;
        if (set->flags & HandlerSet::kRunning)
            set->flags |= HandlerSet::kRemoveWhenDone;
        else
            Free(set);
    }
}

// 0x7100a669f0
void RemoveHandlerSet(HandlerSet* set) {
    if (!set || !(set->flags & HandlerSet::kInUse)) return;
    if (set->flags & HandlerSet::kRunning)
        set->flags |= HandlerSet::kRemoveWhenDone;
    else
        Free(set);
}

// 0x7100a66aa0
void MarkRemoved(HandlerSet* set) {
    set->kind = kKindRemoved;
}

// 0x7100a66ab0
bool IsMarkedRemoved(const HandlerSet* set) {
    return set->kind == kKindRemoved;
}

// 0x7100a66ac0
bool PutToSleep(HandlerSet* set) {
    if (set->flags & HandlerSet::kAsleep) return false;
    set->flags |= HandlerSet::kAsleep;
    return true;
}

// 0x7100a66ae0
bool WakeUp(HandlerSet* set) {
    if (!(set->flags & HandlerSet::kAsleep)) return false;
    set->flags &= ~HandlerSet::kAsleep;
    return true;
}

// 0x7100a66b00
void ClearWork(HandlerSet* set) {
    memset(set->work, 0, sizeof(set->work));
}

// 0x7100a66b20
u16 GetId(const HandlerSet* set) {
    return set->id;
}

// 0x7100a66b30
u8 GetOwner(const HandlerSet* set) {
    return set->owner;
}

// 0x7100a66b40
u32 GetWork(const HandlerSet* set, u8 index) {
    return set->work[index];
}

// 0x7100a66b50
void AnswerEvent8b(HandlerSet* set) {
    set->flags |= HandlerSet::kAnswersEvent8b;
}

// 0x7100a66b60
void AllowReentry(HandlerSet* set) {
    if (set->flags & HandlerSet::kRunning) set->flags |= HandlerSet::kReentrant;
}

// 0x7100a66b80
void SetWork(HandlerSet* set, u8 index, u32 value) {
    set->work[index] = value;
}

// 0x7100a66b90
void CallEvent(Flow* flow, u32 event) {
    Call(flow, event, false);
}

// 0x7100a66c70
void CallEventWithSkipChecks(Flow* flow, u32 event) {
    Call(flow, event, true);
}

// 0x7100a66d50
void SetSkipCheck(HandlerSet* set, SkipCheck check) {
    set->skipCheck = check;
}

// 0x7100a66d60
void ClearSkipCheck(HandlerSet* set) {
    set->skipCheck = nullptr;
}

// 0x7100a66d70
HandlerSet* FirstHandlerSet(u32 kind, u8 pokemon) {
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next)
        if ((set->flags & HandlerSet::kInUse) && set->kind == kind && !(set->flags & HandlerSet::kRemoveWhenDone) &&
            set->pokemon == pokemon)
            return set;
    return nullptr;
}

// 0x7100a66dc0
HandlerSet* NextHandlerSet(HandlerSet* set) {
    if (!set) return nullptr;
    for (HandlerSet* next = set->next; next; next = next->next)
        if (next->kind == set->kind && next->pokemon == set->pokemon && !(next->flags & HandlerSet::kRemoveWhenDone))
            return next;
    return nullptr;
}

// 0x7100a66e10
void PutMovesToSleep(u16 owner) {
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next)
        if (set->kind == kKindMove && set->owner == owner && FUN_7100a3a670(set, set->work))
            set->flags |= HandlerSet::kAsleep;
}

// 0x7100a66e80
void WakeMoves(u16 owner) {
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next)
        if (set->kind == kKindMove && set->owner == owner) set->flags &= ~HandlerSet::kAsleep;
}

// 0x7100a66ec0
void Suppress(u16 owner, u32 kind) {
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next)
        if (set->owner == owner && set->kind == kind) set->flags |= HandlerSet::kSuppressed;
}

// 0x7100a66f10
bool Unsuppress(u16 owner, u32 kind) {
    bool found = false;
    for (HandlerSet* set = g_firstHandlerSet; set; set = set->next) {
        if (set->owner == owner && set->kind == kind) {
            set->flags &= ~HandlerSet::kSuppressed;
            found = true;
        }
    }
    return found;
}

// 0x7100a66f70
void ResetVars() {
    if (g_vars.frame != 0) memset(&g_vars, 0, offsetof(EventVars, values));
}

// 0x7100a66fa0
void PushVars() {
    u32 i = g_vars.frame;
    if (i >= kMaxVars) return;
    while (g_vars.labels[i] != 0) {
        g_vars.frame = ++i;
        if (i >= kMaxVars) return;
    }
    if (i >= kMaxVars - 1) return;
    g_vars.frame = i + 1;
    g_vars.labels[i] = kVarFrameMark;
}

// 0x7100a67000
void PopVars() {
    u32 frame = g_vars.frame;
    if (frame == 0) return;
    // clear the frame (its index wraps at 16 bits)
    for (u16 i = frame; i < kMaxVars && g_vars.labels[i] != 0; i++) g_vars.labels[i] = 0;
    // and its mark, then go back to the frame below's start: just after the mark before it
    frame--;
    g_vars.frame = frame;
    g_vars.labels[frame] = 0;
    if (frame == 0) return;
    for (u32 i = frame; i != 0; i--) {
        if (g_vars.labels[i - 1] == kVarFrameMark) {
            g_vars.frame = i;
            return;
        }
    }
    g_vars.frame = 0;
}

// 0x7100a67090
void SetVar(u32 label, s32 value) {
    Put(label, value, 0, 0, kVarModeRewritable);
}

// 0x7100a67100
void SetConstVar(u32 label, s32 value) {
    Put(label, value, 0, 0, kVarModeConst);
}

// 0x7100a67170
void SetVarRewritableOnce(u32 label, s32 value) {
    Put(label, value, 0, 0, kVarModeRewritableOnce);
}

// 0x7100a671e0
void MakeVarRewritableOnce(u32 label) {
    int i = FindSlot(label);
    if (i >= 0) g_vars.modes[i] = kVarModeRewritableOnce;
}

// 0x7100a67230
void SetRatioVar(u32 label, s32 value, s32 min, s32 max) {
    Put(label, value, min, max, kVarModeRatio);
}

// 0x7100a672a0
bool RewriteVar(u32 label, s32 value) {
    int i = FindSlot(label);
    if (i < 0 || g_vars.modes[i] > kVarModeRewritableOnce) return false;
    g_vars.values[i] = value;
    if (g_vars.modes[i] == kVarModeRewritableOnce) g_vars.modes[i] = kVarModeRewritten;
    return true;
}

// 0x7100a67340
void MultiplyVar(u32 label, s32 ratio) {
    int i = FindSlot(label);
    if (i < 0 || g_vars.modes[i] != kVarModeRatio) return;
    s32 max = g_vars.max[i];
    s32 min = g_vars.min[i];
    s32 value = (s32)((u64)((s64)g_vars.values[i] * ratio + 0x800) >> 12);
    if ((min | max) != 0) {
        if (value < min) value = min;
        if (value > max) value = max;
    }
    g_vars.values[i] = value;
}

// 0x7100a673e0
s32 GetVar(u32 label) {
    for (u32 i = g_vars.frame; i < kMaxVars; i++) {
        if (g_vars.labels[i] == label) return g_vars.values[i];
        if (g_vars.labels[i] == 0) break;
    }
    return 0;
}

// 0x7100a67450
bool FindVar(u32 label, s32* value) {
    for (u32 i = g_vars.frame; i < kMaxVars; i++) {
        if (g_vars.labels[i] == label) {
            *value = g_vars.values[i];
            return true;
        }
        if (g_vars.labels[i] == 0) break;
    }
    return false;
}

// 0x7100a674c0
void SetPointerVars47(void* pointer) {
    SetVar(0x47, (s32)((u64)pointer >> 32));
    SetVar(0x48, (s32)(u64)pointer);
}

// 0x7100a67590
void* GetPointerVars47() {
    return (void*)((u64)(u32)GetVar(0x47) << 32 | (u32)GetVar(0x48));
}

// 0x7100a67630
void SetPointerVars23(void* pointer) {
    SetVar(0x23, (s32)((u64)pointer >> 32));
    SetVar(0x24, (s32)(u64)pointer);
}

// 0x7100a67700
void* GetPointerVars23() {
    return (void*)((u64)(u32)GetVar(0x23) << 32 | (u32)GetVar(0x24));
}

// 0x7100a677a0
HandlerSet* CallHandlers(Flow* flow, u32 event, u32 kind, bool checkSkips, HandlerSet* set) {
    HandlerSet* next = set->next;
    if (next) next->flags |= HandlerSet::kNextToCall;
    u32 flags = set->flags;
    if ((flags & (HandlerSet::kRunning | HandlerSet::kReentrant)) == HandlerSet::kRunning) return next;
    if (flags & (HandlerSet::kAsleep | HandlerSet::kSuppressed)) return next;
    if (kind != kKindAny && set->kind != kind) return next;
    if (!(flags & HandlerSet::kInUse)) return next;
    u32 addedAt = flags & HandlerSet::kAddedAt;
    if (addedAt != 0 && addedAt >= g_eventDepth) return next;
    if (event == kEvent8b && !(flags & HandlerSet::kAnswersEvent8b)) return next;
    u32 count = flags >> HandlerSet::kCountShift & 0xff;
    for (u32 i = 0; i < count; i++) {
        if (set->handlers[i].event != event) continue;
        if (checkSkips) {
            if (ShouldSkip(flow, set, event)) return next;
            flags = set->flags;
        }
        set->flags = flags | HandlerSet::kRunning;
        set->handlers[i].function(set, flow, set->pokemon, set->work);
        // a set that ran again inside its own handler is still running
        u32 after = set->flags;
        flags = after & ~(after & HandlerSet::kReentrant ? HandlerSet::kReentrant : HandlerSet::kRunning);
        set->flags = flags;
        if (!(after & HandlerSet::kRemoveWhenDone)) return next;
        // removed while it ran: the event goes on with the set after it, which may have changed
        if (next) {
            u32 nextFlags = next->flags;
            if (!(nextFlags & HandlerSet::kInUse)) {
                next = set->next;
                if (next) {
                    nextFlags = next->flags;
                    while (!(nextFlags & HandlerSet::kInUse)) asm volatile("");  // as the original: never ends
                }
            }
            if (next) {
                next->flags = nextFlags | HandlerSet::kNextToCall;
                flags = set->flags;
            }
        }
        if (!(flags & HandlerSet::kInUse)) return next;
        if (flags & HandlerSet::kRunning)
            set->flags = flags | HandlerSet::kRemoveWhenDone;
        else
            Free(set);
        return next;
    }
    return next;
}

// 0x7100a67990
bool ShouldSkip(Flow* flow, HandlerSet* set, u32 event) {
    BattlePokemon* owner = nullptr;
    if (set->owner != kNoPokemon) {
        owner = flow->GetPokemon(set->owner);
        if (owner && set->kind == kKindAbility && owner->HasCondition(0x10)) return true;
    }
    if (event != kEventCb && set->kind == kKindItem) {
        if (flow->HasFieldEffect(7)) return true;
        if (owner && (owner->HasCondition(0x13) || owner->HasFlag(9))) return true;
    }
    for (HandlerSet* s = g_firstHandlerSet; s; s = s->next) {
        if (!s->skipCheck) continue;
        if (s->kind == kKindAbility) {
            BattlePokemon* pokemon = flow->GetPokemon(s->owner);
            if (pokemon && pokemon->HasCondition(0x10)) continue;
        }
        if (s->flags & HandlerSet::kSuppressed) continue;
        if (s->skipCheck(s, flow, set->kind, event, set->id, set->pokemon)) return true;
    }
    return false;
}

}  // namespace battle
