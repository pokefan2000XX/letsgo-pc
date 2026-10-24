# Script commands

The scripting API of the game's Pawn scripts (phase 6): what the 389 commands the game registers take, return and do. [SCRIPTS.md](SCRIPTS.md) covers the format, the disassembler and how scripts start; this file covers the commands. Everything here was read from the game's own handlers (decompiled into `orig\analysis\cmd_dec`) and from how the 705 field scripts call them. Command names, addresses and argument meanings are given; no game text and no whole scripts.

`python -P tools/amx.py commands --uses` lists every command with its handler address, the argument counts the scripts use and how often. Its output (also in `build\script_commands.txt`) is the index; this file is the explanation.

## How to read the tables

- **Name (calls)**: the command's name as `tools/amx.py` prints it, and how many call sites the game's scripts have for it. Most scripts carry the same copy of a few include files, so a count like 383 means "every script that shows a message", not 383 different uses.
- **Arguments**: in call order, `name:type`. Types:
  - `int`: a signed integer (the handler reads 32 bits unless noted). `bool`: 0 or non-zero.
  - `hash`: a 64-bit FNV-1a hash (an object, flag, work, event, label or trainer ID). Scripts write them as 64-bit constants, e.g. `FlagGet(0xd2ce7a764469c673)`.
  - `hash32`: a 32-bit ID, used by the sound system (`0x62efcd1a`).
  - `float`: an IEEE-754 **double** in the 64-bit cell (Pawn's float library in this game is 64-bit; `0x4076800000000000` is 360.0). The handlers narrow it to a 32-bit float. Make one with `float(n)` and do arithmetic with `floatadd` and friends (the Math section). Angles are **degrees**.
  - `string`: the data address of a string stored one character per cell (a literal such as `"BLACK"` in the source). The handler reads it through the host's address translation (0x7100672cd0) and either hashes it (FNV-1a 64) or uses it as a name.
  - `ref`: the data address of cells the handler writes (by reference), e.g. `&var3`.
  - `obj`: a field object, named by the 64-bit hash of its name, or the player's hash `0x7a29438e2b197b00` (the player, P1). `GetTargetFieldObjectID()` gives the object the event was started on; `GetPartnerObjectHash(0)` gives the partner Pokémon.
- **Returns**: what comes back in PRI. "bool" is 0 or 1. A command that returns nothing returns 0 (or an unused value); the table says "-".
- **Async**: how the command finishes. "starts, poll X" means it returns at once and the script keeps calling X (usually with `_Suspend(1)` and `halt 12` between polls) until X says it is done. "sleeps" means the handler itself sets the script's sleep state (the `FUN_7100672cc0(amx, 12)` call, or a helper that does): the script yields right after the call and continues next frame without a polling loop of its own. Everything else is immediate.
- **(from call sites)** marks a meaning inferred from how scripts call a command and not from reading everything the handler does; **(unsure)** marks a guess. Anything without a mark was read from the handler.

## Calling convention

A command handler is `s64 fn(void* amx, const s64* params)`. `params[0]` is the number of argument *bytes* (8 per argument), `params[1..]` are the argument cells. The return value goes to PRI. The script's own bookkeeping sits behind the amx pointer (`amx+0x84` is the script holder: the event's four u32 arguments and the target object are in its TempWork array at +0x2c0, see [Flags, works](#flags-works-and-temp-works)); `FUN_7100672cc0(amx, 12)` stores the "sleep" result code that makes the interpreter return to the game after the current command.

**Strings.** An argument typed `string` is the data address of a string with one character per cell. `GetFnvHash64`, `ExtraMsgLoad`, `SetAnimationState*Parameter_` and many more resolve it, copy up to 64/128 characters and hash it with FNV-1a 64 (offset 0xcbf29ce484222645, prime 0x100000001b3). So **a name given as a string and its precomputed hash are interchangeable wherever the command takes a `hash`**; the game's scripts mix both: `GetFnvHash64("BringPoke")` and a constant `0xb7448a2f1cfc56ec`.

**By reference.** `printf`, `GetTrainerBattleResult_`, `GetWildBattleResult_` take the address of cells (`&var3`) and write results into them.

**Asynchronous commands.** The game never blocks inside a command. A script that must wait uses its include file's wait loop:

```
wait one frame:      _Suspend(1); halt 12                (f_0008 in every script)
wait n frames:       for i in 0..n: wait one frame        (the Wait(n) function)
wait for a command:  while not Is...End(): wait one frame
```

The polling commands and their polarity (read from the scripts' loops):

| Start | Poll | Done when |
|---|---|---|
| `MsgWin_` | `IsMsgWinEnd_()` | returns 1 |
| `StartTurnAround_`, `StartTurnAroundToTargetObject_`, `StartForceMove_`, `StartHoppingCommand_`, `StartSpinAction_`, ... | `CheckActionCommandRunningState_(obj)` | returns 0 (1 = still running) |
| `PlayPokemonMotion`, `StartPlayMotionDisableLookAt_` | `IsEndPokemonMotion_(obj)` / `IsWaitStateCharaMotion_(obj)` | returns 1 |
| `EvCameraMove` | `EvCameraMoveWait_()` | returns 1 |
| `FadeIn_`, `FadeOut_` | `FadeWait_()` | returns 1 |
| `JinglePlay` | `JingleIsPlaying()` | returns 0 |
| `PostSoundEventSE` (a waited one) | `IsActiveEventSystemSE_(id)` | returns 0 |
| `PlayParticleVfx_` | `IsEndParticleVfx_(handle)` | returns 1 |
| `ExtraMsgLoad` | `ExtraMsgIsLoaded_()` | returns 1 |
| `AddFieldObject_` | `WaitForAddingFieldObject_()` | returns 1 |
| `PokemonVoicePlay` | `PokemonVoiceIsPlaying(id, obj)` | returns 0 |
| `FeedBackEffectStart` | `FeedBackEffectIsEnd_()` | returns 1 |

The include files also give names to a few idioms the scripts repeat: `Wait(n)` above; `StartTurnAround(obj, deg, frames)` (a wrapper of `StartTurnAround_`); `MsgWin` (a six-argument function that sets the name registers, calls `MsgWin_` and waits for `IsMsgWinEnd_` unless bit 2 of its flags is set); `ABKeyWait`, which waits for the message to end, shows the cursor, waits for A/B and plays `PostSoundEventSE(0x62efcd1a)`; `YesNoWin`, which calls `YesNoWin_` and returns `!TempWorkGet(12)`.

## Messages and text

Text lives in per-script message files (`script/<name>.dat`); a message is addressed by the 64-bit hash of its label. `ExtraMsgLoad` loads a second file at run time.

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `MsgWin_` (383) | `label0:hash` (0 in every call), `int` (0), `label:hash` the message, `type:int` the window kind (5 or 8 in the calls), `bool`, `bool`, `bool` | - | Opens the message window with the message `label`. The three bools are bits 1, 4 and 8 of the flags the `MsgWin` wrapper takes; they select a window variant. (from call sites; the bools are unsure) | sleeps one frame, then poll `IsMsgWinEnd_()` |
| `IsMsgWinEnd_` (383) | - | bool | 1 when the message window has finished or there is none. | poll |
| `MsgWinClose` (405) | - | - | Closes the message window. | immediate |
| `ShowMsgWinCursor_` (766) | `show:bool` | 1 | Shows or hides the "press A" cursor of the current message window. | immediate |
| `ABKeyWait_` (383) | - | - | Waits for A or B on the message window. The scripts call it after `IsMsgWinEnd_` with the cursor shown. It creates the wait task and sleeps, and the script is not resumed before the key. | sleeps until the key |
| `MsgWinEmpty_` (35) | - | - | Opens a message window with no text, for list menus to sit in. | immediate; poll `MsgWinLoadWait_()` |
| `MsgWinLoadWait_` (35) | - | bool | 1 when the (empty) window's assets have loaded. | poll |
| `YesNoWin_` (58) | `bool`, `int` (0), `label1:hash`, `label2:hash`, `int`, `int`, `bool` | - | Shows a Yes/No choice with two option texts; the choice goes into **TempWork 12** (0 = the first option). The `YesNoWin` wrapper returns `!TempWorkGet(12)`. (from call sites) | sleeps; read TempWork 12 afterwards |
| `ListMenuAdd_` (35) | `slot:int` (0), `index:int`, `label:hash`, `bool` | - | Adds one entry to the open list menu (a text label, or a number when the first argument of the call is 1). | immediate |
| `ListMenuStart_Seq` (35) | `int`, `int` (0), `x:int`, `y:int`, `default:int` (-1 or a row), `cancelable:bool` | - | Shows the list menu built with `ListMenuAdd_`; the chosen row ends up in **TempWork 12** (the wrapper returns `TempWorkGet(12)`). (from call sites) | script yields; result in TempWork 12 |
| `ScrollListCreate` (7) | `rows:int` | - | Creates a scrolling list with room for that many rows. | sleeps |
| `ScrollListAdd` (38) | `index:int`, `label:hash`, `int` (0) | - | Adds an entry (a message label) to the scroll list. (from call sites) | immediate |
| `ScrollListStart_` (7) | `hash`, `hash`, `int`, `int` | - | Starts the scroll list (shop-like menus). (unsure) | sleeps |
| `WordSetNumber` (57) | `register:int`, `value:int`, `digits:int` (0 = as many as needed), `int` (0) | - | Puts a number into message register `register`, to show as a `{n}` tag in the next message. | immediate |
| `PG_WordSetRegister` (668) | `kind:int`, `register:int`, `value1`, `value2` | - | Puts a word into a message register by kind. Kind 1 = the player's name (every message goes through `PG_WordSetRegister(1, r, 0, 0)` first); other kinds read `value1` (u32 or u16) and for 11 also `value2`; the game uses 2, 3, 4-9, 10, 11, 12, 13, 15, 16, 17, 21-23. 2 = the rival's name (seen headless 2026-10-24); 3 = a species' name (`value1` the species; the handler calls `pokemon::GetSpeciesName`), 11 looks like species and form; 17 = a party Pokémon's nickname, `value1` its place in the party (the handler takes the party's member as `PokePartyCallInputNickname` does; seen headless 2026-10-24: `[VAR 0102(0000)]` after `PG_WordSetRegister(17, 0, 0, 0)` showed the first Pokémon's name); 10 an item (from call sites, unsure); the rest (unsure). Kinds 14 and 18-20 do nothing. | immediate |
| `ExtraMsgLoad` (72) | `file:string` (e.g. `"script/fld_item.dat"`) | - | Loads an extra message file, by the hash of its path, into the message system, unless that file is already the top of the script's stack of two. Pair with `ExtraMsgUnload`. | starts, poll `ExtraMsgIsLoaded_()` |
| `ExtraMsgIsLoaded_` (62) | - | bool | 1 when the file asked for is loaded. | poll |
| `ExtraMsgUnload` (72) | - | - | Pops and unloads the script's most recent extra message file. | immediate |
| `OpenWalletWindow_` (11) / `UpdateWalletWindow_` (11) / `CloseWalletWindow_` (11) | - | bool: 1 if the shop UI exists | Show, refresh and close the money window in the corner. | immediate |
| `DisplayPlaceName_` (1) | `bool` | - | Shows the place-name banner. | immediate |

## Characters and field objects

"Field object" is anything placed on the map: the player, NPCs, trainers, Pokémon. All are addressed by a 64-bit name hash (`obj`). Positions are in world units (the camera positions in the scripts are in the low thousands); angles are degrees, 0..360 (`GetFieldObjectAngle_` normalises into that range). The player is `0x7a29438e2b197b00`. Event scripts normally start with `var = GetTargetFieldObjectID()` to learn who was talked to.

### Looking things up

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `GetTargetFieldObjectID` (571) | - | hash | The object the event runs on (the NPC talked to, the trigger stepped on). Same value as `TempWorkGet(14)`. | immediate |
| `GetFieldObjectAngle_` (616) | `obj` | float | The object's facing angle in degrees, 0..360. | immediate |
| `GetFieldObjectPositionX_` (109) / `GetFieldObjectPositionY_` (151) / `GetFieldObjectPositionZ_` (99) | `obj` | float | One coordinate of the object's position. | immediate |
| `GetFieldObjectBetweenAngle_` (260) | `from:obj`, `to:obj` | float | Angle in degrees from `from` to `to` (what you turn to face it). | immediate |
| `GetAnglePositionToFieldObject_` (22) | `x:float`, `z:float`, `obj` | int | Angle from the point (x, z) to the object; the wrapper feeds it to `float()`. (from call sites) | immediate |
| `IsFieldObjectExists_` (99) | `obj` | bool | 1 if an object with that hash is in the scene. | immediate |
| `IsPokemonObject` (368) | `obj` | bool | 1 if the object is a Pokémon (not a human). | immediate |
| `GetPokemonObjectMonsNo` (177) / `GetPokemonObjectFormNo` (177) | `obj` | int | Species number / form of a Pokémon object (species 25 if it isn't found). | immediate |
| `GetCharaNameHashFromUniqueHash` (259) | `unique:hash` | hash | Maps an object's unique hash to its character-name hash (the model name, e.g. the hash of `"childm"`). Scripts compare it with `GetFnvHash64("...")`. | immediate |
| `GetCharaUniqueHashFromNameHash` (1) | `name:hash` | hash | The reverse. | immediate |
| `GetBgEventLookAtPosX` (111) / `GetBgEventLookAtPosZ` (111) | `obj` | float | X / Z of the look-at point of a background event (a sign, a PC) the player is facing; used to aim the camera and turn the player. | immediate |
| `GetDefaultEyeState_` (33) / `GetDefaultMouthState_` (32) | `obj` | int | The object's default eye / mouth expression index (what to go back to after a scripted expression). | immediate |
| `IsFieldObjectSetupTiming_` (4) | `obj` | bool | 1 when the object is ready to be set up (right after being added). (unsure) | poll |
| `GetNowReactionData` (5) | `int` | - | Reads the partner's current reaction (survey, shiver, look back) for the field-move scripts. (unsure) | immediate |

### Creating, deleting, showing

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `AddFieldObject_` (42) | `obj` | - | Creates the object that the map's placement data names (`obj`); it is added over the next frames. | starts, poll `WaitForAddingFieldObject_()` |
| `WaitForAddingFieldObject_` (43) | - | bool | 1 when nothing is left to add. | poll |
| `DeleteFieldObject_` (96) | `obj` | - | Removes the object from the scene. | immediate |
| `SetFieldObjectVisibility_` (45) | `obj`, `visible:bool` | - | Shows or hides the object. A hidden object still blocks the way (its collision stays): use `DeleteFieldObject_` for someone who leaves a scene. | immediate |
| `GetFieldObjectVisibility_` (0) | `obj` | bool | Reads it. | immediate |
| `SetFieldObjectActiveDynamicCollision_` (2) / `SetFieldObjectActiveStaticCollision_` (1) | `obj`, `bool` | bool | Turns the object's collision against moving / static things on or off. | immediate |
| `SetFieldObjectTerrainHieghtAdjustFlag_` (10) | `obj`, `bool` | bool | Whether the object follows the terrain height. (The misspelling is the game's.) People are set on the ground when the zone loads; 0 in the zone's `init_` event 1 while `IsFieldObjectSetupTiming_(obj)`, then `SetFieldObjectPositionXYZ_`, keeps one up (a box on a table, seen in the game; Mt. Moon's scenes do the same). | immediate |
| `FieldAreaZoneReset` (8) | - | - | Reloads the field area's zone (resets what the event moved or hid). (from the name) | sleeps |
| `ResetFieldAreaZone_` (0) | - | - | A second variant of the same. (unsure) | sleeps |
| `DisappearEncountSymbols` (15) | - | - | Hides the overworld Pokémon on the map (the symbols you can walk into). | immediate |
| `EncountPokeSetVisible` (1) | `bool` | - | Shows or hides the overworld Pokémon symbols. | immediate |
| `EjectEncountAnd2POnObject` (1) | `obj` | - | Pushes overworld Pokémon and player 2 off the object's spot. (unsure) | sleeps |
| `SetCharaShadowEnabled` (0) / `IsCharaShadowEnabled` (0) / `SetAreaShadowEnabled` (0) / `IsAreaShadowEnabled` (0) | - | - | Shadow switches (unused). | - |

### Position and angle

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `SetFieldObjectPositionXZAndAngle_` (95) | `obj`, `x:float`, `z:float`, `angle:float` | - | Puts the object at (x, z) facing `angle`, height from the terrain. | immediate |
| `SetFieldObjectPositionXZ_` (45) | `obj`, `x`, `z` | - | Same, keeping the angle. | immediate |
| `SetFieldObjectPositionXYZ_` (21) | `obj`, `x`, `y`, `z` | - | Sets all three coordinates. | immediate |
| `SetFieldObjectAngle_` (48) | `obj`, `angle:float` | - | Sets the facing angle at once. | immediate |
| `SetFieldObjectAngleToTargetObject_` (11) | `obj`, `target:obj` | bool | Snaps `obj` to face `target`. | immediate |
| `SetFieldObjectAngleToTargetPosition_` (0) / `SetFieldObjectAngleXYZ_` (0) | `obj`, floats | - | Snap to face a point / set three angles (unused). | immediate |

### Moving and turning (action commands)

The "action commands" run on the object over several frames. Start one, then poll `CheckActionCommandRunningState_(obj)` until it returns 0 (the include file wraps the loop).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `CheckActionCommandRunningState_` (330) | `obj` | bool | 1 while the object is still running an action command. | poll |
| `StartTurnAround_` (315) | `obj`, `angle:float` (degrees), `frames:int` | bool: started | Turns the object to the absolute angle over `frames` frames (the handler converts to radians). At 0 the player faces the camera; 0 frames, the scripts' usual value, still turns it over several frames (both seen in the game, hacks/new-cutscene). | starts, poll `CheckActionCommandRunningState_` |
| `StartTurnAroundToTargetObject_` (322) | `obj`, `target:obj`, `frames:int` | bool | Turns the object to face another object. | starts, same poll |
| `StartTurnAroundToTargetPosition_` (3) | `obj`, `x`, `z`, `frames` | bool | Turns to face a point. | starts, same poll |
| `StartForceMove_` (112) | `obj`, `kind:int`, `x:float`, `z:float`, `hasEndAngle:bool`, `angle:float`, `float`, `bool` | bool | Walks (kind 0) or runs (kind 1) the object straight to (x, z) under script control (no pathing; 0x7100185d50 -> 0x7100243370). With `hasEndAngle` it turns to `angle` (degrees) at the end. The game's wrapper computes that angle with `GetAnglePositionToFieldObject_`. The command (update 0x71002dbec0, state at +0x60: 1 start, 2 turn on the spot, 3 move, 5 end turn) turns on the spot first, 16 frames, only when the point is more than 15 degrees off the facing (0x71002dd040); under that it faces the new way at once and moves (0x71002dc8d0), so legs that each bend 15 degrees or less make one run without stops. A target within 1 unit ends it at once. A second `StartForceMove_` while one runs does not cancel it: the old one waits, then ends by putting the object at its own target (0x71002dcd90), a snap back if the new one went on (hacks/smooth-moves ends the older command when a newer one starts; the game's own scripts always wait for the end). `StartForceMove_(player, 1, x, z, 0, 0, 0, 1)`, main_event_0010's arguments for an NPC, turns the player toward the point and walks there (seen in the game, hacks/new-cutscene). | starts, same poll |
| `StartForceMoveToPlayer_` (3) | `obj`, `int`, `float`, `bool` | bool | Moves the object next to the player. (from call sites) | starts, same poll |
| `IsRunningActionCommandForceMove_` (4) | `obj` | bool | 1 while a forced move is running. | poll |
| `StartHoppingCommand_` (20) | `obj`, `int`, `int`, `height:float` | - | Makes the object hop in place, `height` high. (from call sites) | starts, same poll |
| `StartSpinAction_` (6) | `obj`, `hash`, `int`, `float` | bool | Spins the object. The hash is one of three phase names the scripts give as strings: `START_TO_SPIN`, `SPIN_TO_END`, `START_TO_END`. | starts, same poll |
| `StartSpinningCommand_` (1) | `obj`, `int`, `int`, `int` | - | Keeps spinning. (unsure) | starts, same poll |
| `StartSurveyCommand_` (6) | `obj`, `float`, `int`, `int` | - | The partner's "look around" reaction. (from call sites) | starts, same poll |
| `StartShiveringCommand_` (4) | `obj`, `float`, `int`, `int` | - | The partner shivers. (from call sites) | starts, same poll |
| `StartLookingBackCommand_` (0) | `obj`, 3x`int` | - | The partner looks back (unused). | starts |
| `Player2Eject` (249) | `mode:int` (0 or 1), `flag:bool` | bool: 1 if player 2 exists | Takes player 2 (the second player of the two-player mode) out of the event. Mode 0 plays the animated removal and sleeps; mode 1 removes them at once. The flag is stored in the holder (+0x39a), and `CallTrainerBattleCore` / `CallWildBattle` read it when they pick a one- or two-player battle. Main events open with it (an include wrapper picks mode 0 or 1 from its own argument). | mode 0 sleeps |

### Looking (head and eye tracking)

An object's head can be told to follow a point, an angle or another object. The blend length is a frame count (0 is treated as 1 on enabling; 0 or less on disabling means "use the default, 8 frames"). The include files follow every enabling with `Wait(1)`.

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `EnableFieldObjectLookAtAngle_` (544) | `obj`, `yaw:float`, `pitch:float`, `frames:int` | - | Makes the head turn to relative angles (degrees; the include file picks the signs with a mode 0-7). When the object is the player and the partner is being ridden it applies to the partner. (from call sites) | immediate (blends) |
| `EnableFieldObjectLookAtFieldObject_` (292) | `obj`, `target:obj`, `frames` | - | The head follows another object. | immediate (blends) |
| `EnableFieldObjectLookAtPos_` (113) | `obj`, `x`, `y`, `z`, `frames` | - | The head turns to a world point. | immediate (blends) |
| `DisableFieldObjectLookAt_` (337) | `obj`, `frames:int` | - | Stops the head tracking, blending back over `frames` (<1: default 8). | immediate (blends) |
| `IsFinishFieldObjectLookAt_` (5) | `obj` | bool | 1 when the head has reached its target. | poll |
| `EnableFieldObjectLookAtAngleSpeed_` (0) / `...FieldObjectSpeed_` (0) / `...PosSpeed_` (0) | as the above plus a speed | - | Variants with a speed instead of a frame count (unused). | immediate |
| `SetLightDirectionCharaAngle` (49) | `angle:float` | - | Rotates the light that falls on characters (for cutscene lighting). (from call sites) | immediate |
| `SetLightDirectionChara` (3) | `x`, `y`, `z:float` | - | Sets that light direction. | immediate |
| `SetLightDirectionCharaDefault` (27) | - | - | Back to the default light. | immediate |

### Animations and motions

Characters run an animation state machine; scripts drive it through named parameters. The name is a `string` (hashed at run time).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `SetAnimationStateBoolParameter_` (335) | `obj`, `name:string`, `value:bool` | 1 | Sets a bool parameter of the object's animator, e.g. `"chairwait01"`. | immediate |
| `SetAnimationStateIntParameter_` (351) | `obj`, `name:string`, `value:int` | 1 | Sets an int parameter. Most calls are `"EyeType"` and `"MouthType"` (the expression), also `"field_status"`. | immediate |
| `SetAnimationStateFloatParameter_` (12) | `obj`, `name:string`, `value:float` | - | Sets a float parameter. | immediate |
| `SetAnimationStateTrigger_` (93) | `obj`, `name:string` | - | Fires a trigger (starts a motion state such as `"anime01"`). | immediate |
| `GetAnimationStateBoolParameter_` (259) | `obj`, `name:string` | bool | Reads a bool parameter (e.g. `"chairwait01"`). The common talk function reads one of the person's; for a person built on Red's or Green's model (`tr0022_00`, `tr0023_00` templates) that call never returns and the game hangs (seen headless 2026-10-23), so a script for such a person turns them, shows `MsgWin_` and waits without it. | immediate |
| `GetAnimationStateIntParameter_` / `...FloatParameter_` / `...Trigger_` (0 each) | `obj`, `name:string` | value | Readers for the other types (unused). | immediate |
| `ResetAnimationState_` (19) | `obj`, `bool` | bool | Resets the animator to its start state. | immediate |
| `IsAnimationStateName_` (5) | `obj`, `name:hash` | bool | 1 if the animator is in the state with that name hash. | immediate |
| `IsWaitStateCharaMotion_` (280) | `obj` | bool | 1 when the character is back in its waiting state (so a triggered motion has finished) or no longer exists. | poll |
| `StartPlayMotionDisableLookAt_` (66) | `obj`, `hash`, `int`, `bool`, `bool` | - | Plays a motion and turns the head tracking off for it (the wrapper chooses between this and `SetAnimationStateTrigger_` by a flag, using the motion name from a table). (from call sites) | starts, poll `IsWaitStateCharaMotion_` |
| `PlayPokemonMotion` (123) | `obj`, `kind:int` (2), `motion:string` (e.g. `"to_kw32_happyB01"`), `bool` (1) | - | Plays a named motion on a Pokémon object. | starts, poll `IsEndPokemonMotion_(obj)` |
| `PlayPokemonMotionSerial` (0) | `obj`, `int`, `string` | - | Same, queued after the last motion (unused). | sleeps |
| `IsEndPokemonMotion_` (45) | `obj` | bool | 1 when the motion has ended. | poll |
| `IsEndPokemonMotionSerial_` (9) | `obj` | bool | Same for a queued motion. | poll |
| `SetTrainerSpeechBalloonVisibility` (4) | `obj`, `visible:int` | bool | Shows or hides the "!" balloon over a trainer. (from the name) | immediate |

## The partner Pokémon and riding

Every event begins by dealing with the Pokémon that follows the player ("follow partner") or that the player rides ("ride partner"). The first argument of these commands is the **player index**: 0 for the player, 1 for player 2 (the include files call each of them three times: with 0, with 1, and with a variable). "Buddy" in the command names is the party's partner Pokémon (Pikachu or Eevee; species 25 is returned when there is none); "Partner" is the follower object. Scripts also branch on the game version: `RomGetVersion()` returns **42** for Let's Go, Pikachu and **43** for Let's Go, Eevee (`PokemonVoicePlay(25...)` against `133`).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `GetPartnerObjectHash` (283) | `player:int` | hash | The object hash of that player's follower Pokémon (so scripts can pass it to the object commands above). | immediate |
| `IsPlayerRiding_` (278) | `player:int` | bool | 1 if the player is riding their partner. | immediate |
| `DeleteFollowPartner_` (363) | `player:int` | bool | Removes the following partner at once. | immediate |
| `DeleteRidePartner_` (360) | `player:int`, `bool` | bool | Removes the ride partner at once; the bool is passed on to the removal (the scripts pass their own argument). (from call sites) | immediate |
| `StartFollowPartnerIntoTheBall_` (360) | `player:int` | bool | Calls the following partner back into its Poké Ball (animated). | starts (no poll; the scripts `Wait`) |
| `StartRidePartnerIntoTheBall_` (360) | `player:int`, `bool` | bool | Same for the ridden partner. | starts |
| `StartFollowPartnerAppearFromTheBall_` (3) / `StartRidePartnerAppearFromTheBall_` (3) | `player:int` | bool | The reverse: the partner comes out of the ball. | starts |
| `GetRideType_` (1) | `player:int` | int | Which kind of ride the player is on. (unsure) | immediate |
| `StartCreatePartner_` (0) / `WaitCreatePartner_` (0) | `player:int` | bool | Create the partner object / wait for it (unused). | start / poll |
| `PartnerPokeSetEnable` (4) | `bool` | - | Enables or disables the partner Pokémon's field behaviour. | immediate |
| `PartnerPokeIsEnable` (0) / `SetPartnerPokemonValidState_` (0) / `GetPartnerPokemonValidState_` (0) | - | - | State flags of the partner (unused). | - |
| `BuddyPokeSetEnable` (8) | `bool` | - | Enables or disables the buddy Pokémon (the one in the party that follows). | immediate |
| `BuddyPokeIsEnable` (0) / `BuddyPokeSetVisible` (0) / `SetBuddyPokemonVisibility_` (0) / `SetBuddyPokemonValidState_` (0) / `GetBuddyPokemonValidState_` (0) | - | - | Visibility/validity switches (unused). | - |
| `BuddyPokeDetach` (29) | `bool` (0 or 1) | - | Detaches the buddy Pokémon from the player (it keeps standing where it is) so the event can move it. (from the name and call sites) | immediate |
| `BuddyPokeAttach` (26) | - | - | Attaches it again. | immediate |
| `BuddyPokeReflectModelSex` (0) | - | - | Re-applies the model's sex (unused). | - |
| `PlayPartnerPokeMotion` (1) | `motion:int` | bool | Plays one of the partner's field reactions. | starts, poll `IsEndPartnerPokeMotion_` |
| `IsEndPartnerPokeMotion_` (1) | `motion:int` | bool | 1 when it is over. | poll |
| `StartSeaRide_` (2) | `hash`, `hash`, `hash` | - | Starts surfing; arguments come from the field-move scripts. (unsure) | sleeps |
| `SetupGadget_` (2) / `UnsetupGadget_` (2) | `name:string` (`"scope"`, `"flute"`) / - | - | Shows / removes a handheld tool model on the player. (from call sites) | immediate |
| `KairikiRaycast` (1) | `int`, `x:float`, `y:float`, `z:float` | bool | Strength: a ray cast to check whether a boulder can be pushed. (from call sites) | immediate |
| `KairikiHitEncountAnd2P` (1) | `x`, `y`, `z:float` | - | Strength: knocks overworld Pokémon and player 2 out of the boulder's way. (from call sites) | sleeps |
| `ExecuteHidenFlash_` (1) | `int` (75) | bool | Flash: runs the lighting effect. (unsure) | immediate |
| `PushActionLog_Kesagiri` (2) / `PushActionLog_Oshidashi` (1) | - | - | Records a field-move use (cutting a tree, pushing a boulder) in the player's action log. (from the names) | immediate |
| `DebugPartnerReaction*_` (0), `DebugFindIaigiriTreeCloseTo_` (0) | - | - | Debug helpers. | - |

## Camera

The event camera is a scripted camera that replaces the game's. The pattern is `EvCameraStart()`, any number of `EvCameraMove(...)` each followed by `Wait` or `EvCameraMoveWait_`, then `EvCameraEnd(...)`. Positions are world units, angles are degrees.

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `EvCameraMove` (945) | `frames:int`, `px,py,pz:float` (camera position), `tx,ty,tz:float` (look-at target), `ease:int`, `fov:float` (degrees), `roll:float` (degrees) | - | Moves the event camera to the new position and target over `frames` frames (the handler uses 1/frames as the rate; 0 jumps), with interpolation type `ease` (0, 3 and 4 are used). `fov` of 35-40 is typical; a value of 0 or less keeps the current one. Roll is converted to radians. The order of position and target is (from call sites). | starts, poll `EvCameraMoveWait_()` |
| `EvCameraMoveWait_` (95) | - | bool | 1 when no camera move is running. | poll |
| `EvCameraStart` (187) | - | - | Switches to the event camera. | immediate |
| `EvCameraEnd` (153) | `frames:int`, `ease:int` | - | Returns to the game camera, blending over `frames`; also clears the camera objects. | immediate (blends) |
| `EvCameraMoveOffsetChr` (8) | `obj`, `obj`, `float`, `int`, `obj` | - | Camera relative to a character (an offset in the character's frame). (from call sites; unsure) | starts, poll `EvCameraMoveWait_` |
| `EvCameraMoveOffsetLookAt` (7) | `frames:int`, `ox,oy,oz:float`, `ease:int`, `int` | - | Moves the camera by an offset from its current look-at point. (from call sites) | starts, same poll |
| `EvCameraMoveOffset` (2) | `frames`, `ox,oy,oz`, `ease`, `int` | - | Moves the camera by an offset from where it is. | starts, same poll |
| `EvCameraGetPosX` / `EvCameraGetPosY` / `EvCameraGetPosZ` (3 each) | - | float | The event camera's current position. | immediate |
| `EvCameraGetLookAtX` / `...Y` / `...Z` (0) | - | float | Its look-at point (unused). | immediate |
| `EvCameraShake_` (1) | 12 args (six floats, some ints, a bool) | - | Shakes the camera. (unsure) | immediate |
| `EvCameraShakeEnd` (0) | `int` | - | Stops it (unused). | immediate |
| `EvCameraMoveDist`, `EvCameraMoveDistChr`, `EvCameraMoveDistChrEasy`, `EvCameraMoveDistChrEasyPair`, `EvCameraMoveDistRateChrEasy`, `EvCameraMoveDistRateChrEasyPair` (0) | | | Move relative to characters by distance (unused). | |
| `RadialBlurStart` (23) | `frames:int`, `cx:float`, `cy:float`, `...:float`, `strength:float` (1.0), `int` (-1) | - | Starts a radial blur. (from call sites; unsure) | starts |
| `RadialBlurReset` (2) | `frames:int` | - | Fades the blur out. | immediate |
| `RadialBlurIsEnd_` (0) | - | bool | 1 when it has finished (unused). | poll |
| `FeedBackEffectStart` (8) | `frames:int`, seven `float`s (colour and scale, around 1.0) | - | A feedback (echo/glow) screen effect. (unsure) | starts, poll `FeedBackEffectIsEnd_` |
| `FeedBackEffectReset` (7) | `frames:int` | - | Fades it out. | immediate |
| `FeedBackEffectIsEnd_` (2) | - | bool | 1 when the effect is done. | poll |
| `DepthOfFieldStart` / `DepthOfFieldSet_` / `DepthOfFieldSetChara` / `DepthOfFieldEnd` (0) | | | Depth-of-field switches for cutscenes (unused). | |

## Fades and screen effects

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `FadeOut_` (139) | `frames:int`, `type:hash` (FNV of a name), `bool`, `bool` | - | Fades the screen out over `frames` frames. The type is a string hash: `"BLACK"` (the default for anything unknown), `"WHITE"`, `"CROSS"`, `"WIPE"`, and seven more types the handler knows by hash (it maps 11 types in all). The include file wraps it as `FadeOut(frames, "BLACK", a, b)` followed by `Wait(1)`. The two bools are unsure. | starts, poll `FadeWait_()` |
| `FadeIn_` (151) | `frames:int`, `type:hash` | - | Fades back in with that type. | starts, poll `FadeWait_()` |
| `FadeWait_` (161) | - | bool | 1 when the fade has finished. | poll |
| `FadeCheck_` (1) | - | bool | 1 while a fade is running (the opposite of `FadeWait_`). | immediate |
| `MapChangeCore_` (16) | `zone:hash`, `entry:hash`, `x,y,z:float`, `bool`, `bool`, `bool`, `bool` | - | Warps the player to another zone (map) at a position, with the screen change. The arguments after the position are flags (fade, keep the partner, 2P); the include file's `MapChange` wrapper builds them. (from call sites; the flags are unsure) | sleeps until the map has changed |
| `SetRespawnZone` (2) | `zone:hash` | bool | Sets where the player is taken after a whiteout (a Poké Center). | immediate |
| `PlayerGetZoneID` (11) | - | hash | The hash of the zone the player is in. | immediate |

## Sound and music

Sound goes through the audio middleware's event system: a 32-bit ID names an event, probably a Wwise-style hash of its name. Scripts use constants (e.g. `0x62efcd1a`, the text-advance sound).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `PostSoundEventSE` (3161) | `event:hash32` | - | Plays a sound effect event (doors, elevators, menus, cutscene effects). | immediate; optionally poll `IsActiveEventSystemSE_(event)` |
| `IsActiveEventSystemSE_` (27) | `event:hash32` | bool | 1 while that effect is still playing; the include file waits with a 300-frame limit and prints a warning. | poll |
| `PostSoundEventBGM` (100) | `event:hash32` | - | Starts, changes or stops the field BGM by posting its event. | immediate |
| `SoundSetState` (19) | `group:hash32`, `state:hash32` | - | Sets a sound state (for example, the BGM mood) in the middleware. | immediate |
| `JinglePlay` (123) | `id:hash32` | - | Plays a jingle (fanfare), ducking the BGM. | starts, poll `JingleIsPlaying()` |
| `JingleIsPlaying` (93) | - | bool | 1 while a jingle plays. The include file waits up to 390 frames (then prints a warning) and then calls `JingleResumeBGM`. | poll |
| `JingleResumeBGM` (264) | - | - | Resumes the BGM after a jingle. | immediate |
| `JingleResumeNewBGM` (0) | `event` | - | Resumes with a different BGM (unused). | immediate |
| `PlaySEParallel` (2) | `id:hash`, `mode:int` | - | Plays a looping effect in parallel with the event system (1 start, 2 another variant). (from call sites) | immediate |
| `StopSEParallel` (2) | `id:hash` | - | Stops it. | immediate |
| `PokemonVoicePlay` (209) | `monsNo:int`, `form:int`, `kind:int`, `int`, `delay:int` (frames; 0 or less plays now), `obj` (0 for none) | - | Plays a Pokémon's cry. The include file's wrapper plays the partner's cry with `RomGetVersion()` choosing 25 (Pikachu) or 133 (Eevee). With a delay it is queued. (from call sites; the meaning of `kind` and the 4th value is unsure) | starts, poll `PokemonVoiceIsPlaying` |
| `PokemonVoicePlaySpecial` (28) | `voice:int` (from a per-version table), `int`, `int`, `obj` | - | Plays one of the partner's special voices (index into a table with a Pikachu and an Eevee column). | starts |
| `PokemonVoiceIsPlaying` (20) | `voice:int`, `obj` | bool | 1 while that cry plays (with an object, also while the object is not ready). | poll |

## Flags, works and temp works

Save-game state is addressed by 64-bit hashes of names that no longer exist in the game, so scripts only ever show constants. Three kinds: **flags** (bits), **works** (32-bit integers in the save) and **temp works** (the script's own scratch array, not saved).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `FlagSet` (570) | `flag:hash` | - | Sets an event flag in the save. | immediate |
| `FlagReset` (204) | `flag:hash` | - | Clears it. | immediate |
| `FlagGet` (386) | `flag:hash` | bool | Reads it. | immediate |
| `WorkSet` (251) | `work:hash`, `value:int` | - | Writes a save work (the scene/progress counters and most script state). | immediate |
| `WorkGet` (140) | `work:hash` | int | Reads one. | immediate |
| `TempWorkSet` (356) | `index:int`, `value` | - | Writes slot `index` of the script's temp work array (u64 cells at the script holder +0x2c0). | immediate |
| `TempWorkGet` (460) | `index:int` | value | Reads it. Known slots: **0-3** the event's four u32 arguments (what `ReserveScript` and a trainer's talk pass); **12** the result of the last menu/choice (`YesNoWin_`, `ListMenuStart_Seq`, `CallPokeSelect_`, with -1 for "cancelled"); **13** a value set when the event starts that scripts convert with `float()` (unsure); **14** the target object (`GetTargetFieldObjectID`). (0-3 and 14 from reading the event runner 0x710017a750; 12 and 13 from call sites) | immediate |
| `VanishFlagSet` / `VanishFlagReset` / `VanishFlagGet` (0) | `flag:hash` | bool | Flags that hide placed things (unused by field scripts). | immediate |
| `SetAllEventFlag` / `SetAllVanishFlag` / `SetAllSceneWork` (0) | `bool` / `int` | - | Debug: set every flag/work. | immediate |
| `GlobalTempWorkSet` / `GlobalTempWorkGet` (0) | `index`, `value` | value | A temp work array shared between scripts (unused). | immediate |
| `AddRecord_` (53) | `record:int`, `amount:int` (1) | int | Adds to a player record counter (169 is used just before trainer battles). | immediate |
| `GetRecord_` (4) | `record:int` | int | Reads a record counter. | immediate |
| `SetRecord_` (0) | `record`, `value` | int | Writes one (unused). | immediate |
| `SaveActionLogScenario` (27) / `DeleteActionLogScenario` (11) | `name:string` (e.g. `"sinario_rocket"`) | 1 | Adds / removes a story-progress entry in the player's action log (by the hash of the name). | immediate |
| `SaveActionLogPlayer_PickupItem` (1) | `item:int`, `count:int` | - | Logs an item pickup. | immediate |
| `GetNowTitle` (3) / `ChangeTitleMonsNo` (4) / `ChangeTitleSpecial` (4) / `IsGetTitleMonsNo` (12) / `IsGetTitleSpecial` (10) / `AddTitleMonsNo` (1) / `AddTitleSpecial` (1) | `title:int` | int / bool | The player's trainer titles (species-based ones, e.g. 808/809, and special ones): read, set, test and unlock. (from call sites) | immediate |
| `GetPocketMoney_` (10) / `AddPocketMoney_` (2) / `ConsumePocketMoney_` (11) | - / `amount:int` | int | The player's money. `ConsumePocketMoney_` spends it and returns the result. | immediate |
| `PlayerGetSex` (28) | - | int | The player's sex. | immediate |
| `PlayerGetID` (0) | - | int | The player's trainer ID (unused). | immediate |
| `RomGetVersion` (234) | - | int | 42 = Let's Go, Pikachu; 43 = Let's Go, Eevee. | immediate |
| `RomGetLanguageID` (0) | - | int | The game's language code (unused). | immediate |
| `GetPublicRand` (2) | `n:int` | int | A random number below `n` from the shared generator (0 if `n` is 0). | immediate |
| `GetCaptureResult` (6) | - | int | The outcome of the last capture (Go Park). | immediate |
| `SetDisplayPlaceNameFlag` / `GetDisplayPlaceNameFlag` (0) | `bool` | bool | Whether entering a zone shows the place-name banner (unused). | immediate |

## Items, party and Pokémon

### Items

Items are numbered by the game's item list (the scripts pass numbers, e.g. 4 for a Poké Ball).

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `ItemAdd` (94) | `item:int`, `count:int` | bool | Adds to the bag; 0 when it doesn't fit. | immediate |
| `ItemAddCheck` (19) | `item`, `count` | bool | 1 if that many would fit. | immediate |
| `ItemSub` (12) | `item`, `count` | bool | Removes from the bag. | immediate |
| `ItemGetNum` (45) | `item:int` | int | How many the bag holds. | immediate |
| `GetPocketNumberFromItemNumber_` (53) | `item:int` | int | Which bag pocket the item belongs to. | immediate |
| `ItemIsEventItem` (51) / `ItemIsWazaMachine` (50) | `item:int` | bool | Is it a key item / a TM. | immediate |
| `GetWazaNoFromItemNo_` (2) | `item:int` | int | The move a TM teaches. | immediate |
| `ItemIsMegaStone` (0), `ItemGetCategory` (0), `ItemGetNutsID` (0) | `item` | | Item queries (unused). | immediate |
| `PlayerAddDressupItem` (40) / `PlayerIsGetDressupItem` (0) / `PlayerGetDressupItemPrice` (0) | `category:int`, `item:hash` | - / bool / int | The player's outfit pieces (dress-up shop). Add, test, price. | immediate |
| `BuddyPokeAddDressupItem` (39) / `BuddyPokeIsGetDressupItem` (16) / `BuddyPokeGetDressupItemPrice` (2) | `category:int`, `item:string or hash` (`"naked"` / `"none"` clear a slot) | - / bool / int | The same for the partner Pokémon's costume. | immediate |
| `UseEncountAuxItem_` (3) / `GetEncountAuxState_` (1) / `GetRemainingEncountAuxStepCount_` (1) | `item:int` / - / - | bool / int / int | The lure items (their use, their state, their remaining steps). (from the names) | immediate |

### Party and Pokédex

Party commands take the index of a party slot; -1 means "any" (`PokePartyIsRecover(-1)`), and the partner slot index 6 means "none".

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `PokePartyGetCount` (74) | `mode:int` (0, 1 or 2) | int | Number of Pokémon in the party; the scripts mostly ask with mode 1. (mode meaning unsure) | immediate |
| `BuddyPokeGetMonsNo` (31) | - | int | Species number of the buddy Pokémon (25 if none). | immediate |
| `PokePartyGetParam` (29) | `slot:int`, `param:int`, `index:int` | int | Reads a parameter of a party Pokémon. `param` ids seen: 1, 2, 6, 7, 8, 10, 14, 60. (which is which: unsure) | immediate |
| `PokePartySetParam` (12) | `slot`, `param:int` (1020-1025), `value` | - | Writes a parameter. (unsure) | immediate |
| `PokePartyAddParam` (2) | `slot`, `param`, `value` | - | Adds to a parameter. (from the name) | immediate |
| `PokePartyRecoverAll` (20) / `AllPokeRecover` (2) | - | - | Fully heals the party / every Pokémon the player has. | immediate |
| `PokePartyRecover` (0) / `AllPokeIsRecover` (0) | `slot` | | Single-slot heal / test (unused). | |
| `PokePartyIsRecover` (4) | `slot:int` (-1 = all) | bool | 1 if the Pokémon is at full HP and PP. (from call sites) | immediate |
| `PokePartyAddMember` (12) | `pokemon:hash` | - | Gives the player a Pokémon described by the event's table (gift Pokémon). | sleeps |
| `PokePartyRemoveMember` (0), `PokePartyLevelUp` (0), `PokePartySetDefaultName` (0), `PokePartyCallInputNickname` (0) | `slot` (`PokePartyCallInputNickname`: `slot`, `int` 0) | | Party edits (unused by the game). `PokePartyCallInputNickname(slot, 0)` opens the nickname keyboard for that party Pokémon and the script waits for it (the PC runtime's text dialog: "What would you like to nickname your Pikachu?", seen headless 2026-10-24); it does nothing for an egg or an empty place. | waits |
| `PokePartyGetWazaCount` (3) / `PokePartyGetWazaID` (2) / `PokePartySetWaza` (2) / `PokePartyHaveWaza` (0) | `slot` [, `index`, `move`] | int | Move slots of a party Pokémon. | immediate |
| `HasRemainedSkill_` (1) | `slot` | bool | 1 if the Pokémon has a learnable move left to remember. (from the name) | immediate |
| `PokePartyIsPartner` (2) / `PokePartyIsBuddy` (2) / `PokePartyIsOwner` (0) | `slot` [, `which`] | bool | Is this slot the partner / the buddy Pokémon / owned by the player. | immediate |
| `GetPartnerIndex_` (19) / `SetPartnerIndex_` (5) | `player:int` / `slot:int` (6 = none) | int / bool | Which party slot is the partner Pokémon. | immediate |
| `GokigenCalc` (7) | `delta1:int`, `delta2:int` | 1 | Changes the buddy's mood ("gokigen"); the values (e.g. -50 and 50) are the change for the two mood meters. (unsure) | immediate |
| `AllPokeIsFull` (9) | - | bool | 1 if the storage is full. | immediate |
| `AllPokeGetCount` (1) | - | int | Number of Pokémon the player owns. | immediate |
| `AllPokeMonsNoExists` (0) | `species`, ... | | (unused) | |
| `ZukanGetCountCaptureLocal` (5) / `ZukanGetCountSeeLocal` (1) / `ZukanIsCompleteCaptureLocal` (3) / `ZukanIsCompleteSeeLocal` (0) | - | int / bool | Pokédex counts and completeness (caught / seen). | immediate |
| `FieldPokeTrade` (1) / `FieldPokeTradeCheckSendPoke` (1) / `FieldPokeTradeTradeDemo` (1) | `trade:hash`, `int` / `int` | | The in-game trade: perform it, check the Pokémon offered, play the animation. (from the names) | |
| `SodateyaGetPokeNum` (1), `SodateyaGetLeftPokemonMonsNo` (2), `SodateyaGetLeftPokemonFormNo` (1), `SodateyaLeavePokemon` (1), `SodateyaTakeBackPokemon` (1), `SodateyaGiveItem` (1), `SodateyaCheckLevelUp` (1), `SodateyaWordSetLeftPokeNickName` (2), `SodateyaWordSetItem` (1), `SodateyaCreatePokemonObject_` (4), `SodateyaDeletePokemonObject` (1) | | | The Day Care: leave, take back, level check, message words and the on-screen Pokémon. (from the names and call sites) | |
| `BuddyPokeAddWaza` (1) | `move` | - | Teaches the buddy a move. (from the name) | immediate |
| `PairPokeGetMonsNo` (0) | - | int | (unused) | |

## Battles

A battle starts, the script yields, the game runs the battle and resumes the script.

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `CallTrainerBattleCore` (53) | `0` (mode), `trainer:int` (index, from `ConvertTrainerIDHashToIndex`), `0`, `flags:int` (512 in the calls; bit 0x80 is tested), then four zeros | - | Starts a trainer battle. A one-vs-one or two-player battle is picked from player 2's presence (see `Player2Eject`). The include file's wrapper adds `AddRecord_(169)` first. (from call sites; flags unsure) `CallTrainerBattleCore(0, 61, 0, 16, 0, 0, 0, 0)` from a field event fights trainer 61 and comes back to a black screen (the script fades in); the result is then 1 won, 0 lost (seen in the game; the studio's `battle_result()`). | sleeps; then `GetTrainerBattleResult_` |
| `GetTrainerBattleResult_` (52) | `ref` (3 cells) | - | Writes the result into the three cells: [0] result code (the scripts compare it with 2 and call `CallBattleLose_` on a loss), [1] and [2] extra data from the battle. The include file's `GetTrainerBattleResult()` returns cell 0. (from call sites) | immediate |
| `CallBattleLose_` (56) | - | - | The blackout flow after a lost battle. (from call sites) | sleeps |
| `CallWildBattle` (11) | `encounter:hash` (or a variable), `flags:int` (0 or 10) | - | Starts a wild battle with a table-defined Pokémon; bits of `flags` choose options. | sleeps; then `GetWildBattleResult_` |
| `GetWildBattleResult_` (6) | `ref` (3 cells) | - | The same result layout for a wild battle. | immediate |
| `ConvertTrainerIDHashToIndex` (5) / `ConvertTrainerIDIndexToHash` (17) | `hash` / `index:int` | int / hash | Converts between a trainer's ID hash and its index in the trainer table. | immediate |
| `GetTrainerUniqueHash_` (2) | `trainer:hash` | hash | The trainer's unique object hash. (unsure) | immediate |
| `GetTrainerInfo` (10) | `trainer:int`, `field:int` (0-3) | int | One field of the trainer's data (0 is the trainer ID; the others come from the trainer table). (unsure) | immediate |
| `GetTrainerMsgID` (2) | `trainer:int`, `kind:int` | - | Message ID for a trainer's lines. (unsure) | immediate |
| `GetMaxLevelInTrainerPartyOfLastBattle` (1) | - | int | Highest level in the trainer party just fought. | immediate |
| `SaveEventEncountPokemonParam` (1) / `ClearEventEncountPokemonParam` (0) | `hash` / - | - | Stores / clears the parameters of the event Pokémon about to be fought. (unsure) | sleeps / immediate |
| `AI_CMD` (0) | (battle AI) | - | The battle AI scripts' only command, not callable from field scripts. | |

## Demos, cutscenes and special screens

All of these start a game-side event or screen; the script sleeps while it runs.

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `PlayDemoScene_` (9) | `scene:hash`, four `bool`s | - | Plays a demo scene with options. (from call sites) | sleeps |
| `PlayCutin_` (2) | `name:string` (e.g. `"sd9080_kairiki"`) | - | Plays a cut-in (the overlay used for field moves). | sleeps |
| `CallPokeSelect_` (7) | `mode:int` (12 in every call) | 1 | Opens the Pokémon-selection screen. The scripts read the chosen party slot (-1 if cancelled) with `TempWorkGet(12)` afterwards. (from call sites) | sleeps, then `TempWorkGet(12)` |
| `CallShojoEvent` (2) | - | - | Runs a special event the game starts itself. (unsure) | sleeps |
| `CallGoParkInformationEvent` (3) | `int` | - | The Go Park information screen. | sleeps |
| `CallGoPokeStatusEvent` (1) | - | - | Go Park Pokémon status screen. | sleeps |
| `CallGameClearEvent_` (1) | - | - | The ending flow. (from the name) | sleeps |
| `CallBuddyMoodEvent_` (1) | - | - | The buddy mood screen. (from the name) | sleeps |
| `CallEventPokecenRecovery` (2) | `x,y:float`, `hash`, `hash`, `float`, `hash` | - | The Poké Center healing sequence at a position. (from call sites) | unsure |
| `ExecuteBuyShopEvent_` (1) / `ExecuteSellShopEvent_` (1) | `shop:hash` / - | - | Opens the buy / sell shop UI. | unsure |
| `ExecuteShowTownMapEvent_` (1) | - | - | Opens the Town Map. | |
| `ExecuteWazaOmoidashiEvent_` (1) | `slot:int` | - | The move reminder screen. | sleeps |
| `ExecuteBuddyWazaWasureEvent_` (1) | `hash` | - | The buddy's move-deleter screen. | sleeps |
| `CaptureCall` (1) | `hash` | - | Starts the capture call (Go Park). (unsure) | sleeps |
| `ReportStart_` (1) / `ReportWait_` (1) / `IsReportRunning_` (1) | - | - | The save (report) screen: start it, wait for it, ask whether it runs. | start / poll |
| `StickWait_` (0) | - | - | Waits for the stick (unused). | |

## Particles and visual effects

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `PlayParticleVfx_` (213) | `effect:int` (an index into the game's table of particle files), `obj`, `locator:string` (e.g. `"loc_eff_head"` or `"EffOverHead01"`, the attachment point), `ox:float`, `oy:float`, `oz:float`, `scale:float`, `follow:bool`, `handle:int` | handle | Plays a particle effect attached to a locator on the object, with an offset and scale. The script picks the `handle` it will later pass to `IsEndParticleVfx_`. The bare `PlayParticleVfx(...)` wrappers fill in the locator and zero offsets. (from call sites) | starts, poll `IsEndParticleVfx_(handle)` |
| `IsEndParticleVfx_` (59) | `handle` | bool | 1 when that effect has finished. | poll |
| `IsLoadedParticleVfx_` (1) | `handle` | bool | 1 when its resources have loaded. | poll |
| `PlayParticleVfxOnBuddy_` (11) | `effect:int`, `handle`, `ox,oy,oz,scale:float`, `bool`, `bool` | - | The same on the buddy Pokémon (no object argument). | starts |
| `PlayParticleVfxOnCamera_` (2) | `effect:int`, seven `float`s, `handle` | - | The same fixed to the camera. | starts |

## Math (float)

Pawn's float library, with all values doubles. Scripts do all arithmetic on angles and positions here; an integer used as a float first goes through `float()`.

| Command (calls) | Arguments | Returns | What it does |
|---|---|---|---|
| `float` (1721) | `n:int` | float | Converts an integer to a float. |
| `floatadd` (682) / `floatsub` (689) / `floatmul` (36) / `floatdiv` (31) | `a:float`, `b:float` | float | a+b, a-b, a*b, a/b. |
| `floatcmp` (536) | `a`, `b` | 1 if a > b, -1 if a < b, 0 if equal | Compares (the include files build `<` and `>=` out of it). |
| `floatround` (347) | `x:float`, `mode:int` | int | To an integer. Mode 0 rounds half up (adds 0.5, then truncates); modes 1, 2 and 3 truncate toward zero (the handler has no separate floor or ceil). |
| `floatsqroot` (2) | `x` | float | Square root. |
| `floatabs`, `floatsin`, `floatcos`, `floattan`, `floatpower`, `floatlog`, `floatfract`, `strfloat` (0) | | float | The rest of Pawn's float library (unused). |

## Script control

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `_Suspend` (386) | `frames:int` | - | Stores `frames - 1` in the holder (+0x278) as the sleep counter; the script then runs `halt 12` so it is resumed after that many frames. Scripts call it as `_Suspend(1)` and then `halt 12`. | the script sleeps |
| `printf` (1430) | `format:string`, up to three `ref`s | - | Debug print into the game's log (`%d`, `%f`, `%s`). Every script's dispatcher ends with `printf("... Not Found")` for unknown events. | immediate |
| `GetFnvHash64` (846) | `s:string` | hash | FNV-1a 64 of a string (up to 127 characters). | immediate |
| `GlobalCall` (93) | `event:hash`, four `int`s (0 in every call) | - | Runs another event (a script entry, named by its event hash) from this one, passing up to four arguments. (from call sites; unsure) | runs nested (unsure) |
| `ReserveScript` (4) | `event:hash` | - | Queues an event to run after this one; its four u32 arguments are zero (the handler stores the hash and four zeros). | immediate |
| `WeakReserveScript` (0) | `event:hash`, four `int`s | - | A reservation that a later strong one replaces. | immediate |
| `IsReserveScriptSetting` (0) | - | bool | Whether a reservation is pending (unused). | immediate |
| `GlobalCallDebugOnetime` (0) | | | Debug (unused). | |
| `CommandNOP` (0) | - | - | Does nothing. | |
| `DebugPrintInt4` (12) | five args | 1 | A debug print that does nothing in the release build. | immediate |
| `DebugPrintFloat4` (0), `DebugSetSceneEditMode` / `DebugGetSceneEditMode` (0), `DebugEventJumpSaveReset` (0) | | | Debug (unused). | |

## Everything else

| Command (calls) | Arguments | Returns | What it does | Async |
|---|---|---|---|---|
| `FieldItemUnregister` (0) | `item:hash` | - | Removes a placed field item from the map's item list (unused). (unsure) | immediate |
| `PokefanLog`, `PokefanAdd` | | | The two example commands of `hacks\new-script-command`; not the game's. | |

The core library (`toupper`, `tolower`, `swapchars`, `setarg`, `getarg`, `numargs`, `random`, `min`, `max`, `clamp`, `funcidx`, `heapspace`) is registered, but no field script calls it. The battle AI scripts register the single command `AI_CMD`, which is only available to them.

## The 112 commands the game's scripts never call

Registered by the game but called by none of its scripts (112 game commands; with the two hack commands `PokefanLog` and `PokefanAdd`, which the tool lists too, that is the 114 with 0 calls). They have real handlers, so a mod can call them.

- **Camera:** `DepthOfFieldStart`, `DepthOfFieldSet_`, `DepthOfFieldSetChara`, `DepthOfFieldEnd` (depth of field); `EvCameraGetLookAtX`, `EvCameraGetLookAtY`, `EvCameraGetLookAtZ` (read the look-at point); `EvCameraMoveDist`, `EvCameraMoveDistChr`, `EvCameraMoveDistChrEasy`, `EvCameraMoveDistChrEasyPair`, `EvCameraMoveDistRateChrEasy`, `EvCameraMoveDistRateChrEasyPair` (camera moves by a distance from characters); `EvCameraShakeEnd` (stop a shake); `RadialBlurIsEnd_` (poll the blur).
- **Characters:** `ChrRot_`, `ChrRotTarget_` (turn an object, older variants); `SetFieldObjectAngleXYZ_`, `SetFieldObjectAngleToTargetPosition_` (set angles); `GetFieldObjectVisibility_`; `EnableFieldObjectLookAtAngleSpeed_`, `EnableFieldObjectLookAtFieldObjectSpeed_`, `EnableFieldObjectLookAtPosSpeed_` (look-at with a speed instead of frames); `GetAnimationStateTrigger_`, `GetAnimationStateIntParameter_`, `GetAnimationStateFloatParameter_` (animator readers); `PlayPokemonMotionSerial` (queued motion); `StartLookingBackCommand_`; `StartCreatePartner_`, `WaitCreatePartner_` (make the partner object); `SetPartnerPokemonValidState_`, `GetPartnerPokemonValidState_`, `SetBuddyPokemonValidState_`, `GetBuddyPokemonValidState_`, `PartnerPokeIsEnable`, `BuddyPokeIsEnable`, `BuddyPokeSetVisible`, `SetBuddyPokemonVisibility_`, `BuddyPokeReflectModelSex` (partner/buddy switches); `SetCharaShadowEnabled`, `IsCharaShadowEnabled`, `SetAreaShadowEnabled`, `IsAreaShadowEnabled` (shadows); `ResetFieldAreaZone_`; `DebugFindIaigiriTreeCloseTo_`, `DebugPartnerReactionLookingBack_`, `DebugPartnerReactionShivering_`, `DebugPartnerReactionSurvey_` (debug).
- **Field:** `CommandNOP`; `DebugEventJumpSaveReset`, `DebugGetSceneEditMode`, `DebugSetSceneEditMode`, `DebugPrintFloat4` (debug); `FieldItemUnregister`; `GetDisplayPlaceNameFlag`, `SetDisplayPlaceNameFlag`; `GlobalCallDebugOnetime`; `GlobalTempWorkGet`, `GlobalTempWorkSet`; `IsReserveScriptSetting`; `WeakReserveScript` (reserve a script that a later one replaces); `JingleResumeNewBGM`; `PlayerGetID`; `RomGetLanguageID`; `SetAllEventFlag`, `SetAllSceneWork`, `SetAllVanishFlag` (debug set-all); `SetRecord_`; `StickWait_`; `VanishFlagGet`, `VanishFlagSet`, `VanishFlagReset`; `WordSetMoney` (put an amount of money into a message register).
- **Items:** `ItemGetCategory`, `ItemGetNutsID`, `ItemIsMegaStone`, `PlayerGetDressupItemPrice`, `PlayerIsGetDressupItem`.
- **Party and Pokédex:** `AllPokeIsRecover`, `AllPokeMonsNoExists`, `PairPokeGetMonsNo`, `PokePartyCallInputNickname` (the nickname screen), `PokePartyGetCountEx`, `PokePartyHaveWaza`, `PokePartyIsOwner`, `PokePartyLevelUp`, `PokePartyRecover`, `PokePartyRemoveMember`, `PokePartySetDefaultName`, `ZukanIsCompleteSeeLocal`.
- **Battle:** `ClearEventEncountPokemonParam`; **battle AI:** `AI_CMD` (for the AI scripts only).
- **Float:** `floatabs`, `floatcos`, `floatfract`, `floatlog`, `floatpower`, `floatsin`, `floattan`, `strfloat`.
- **Core:** `clamp`, `funcidx`, `getarg`, `heapspace`, `max`, `min`, `numargs`, `random`, `setarg`, `swapchars`, `tolower`, `toupper`.

