; The new-camera hack's script (roadmap phase 7): the cutscene its trigger in the bedroom (t001r0102) starts, which plays
; two of the game's demos with their cameras changed. It runs event POKEFAN_CAMERA_01 (FNV-1a 64 0x13185063d6c081bd),
; which script_id_record.bin gives this script and its message file, script/pokefan_camera.dat. After two lines it plays
; Zapdos's demo (sub_048_thunder), whose camera make_romfs.py edits, then Bill's teleporter demo (ev0330_masaki), whose
; sequence make_romfs.py points at a camera animation of its own; a demo leaves the screen faded, so it fades back in, then
; walks the player out of the trigger. The commands of hacks/new-script-command log each step in letsgo.log: "1 0" at
; the start, "10 0" before Zapdos's demo, "11 0" between the two, "12 0" after Bill's, "2 42" at the end.
;
;   hacks/new-camera/make_romfs.py assembles it, with the other files
.amx file_version=10 amx_version=10 flags=0x1c defsize=12
.stack 0x2000
.nametable 3f000000
.native _Suspend
.native CheckActionCommandRunningState_
.native StartTurnAround_
.native StartForceMove_
.native GetFnvHash64
.native FadeOut_
.native FadeIn_
.native FadeWait_
.native MsgWin_
.native MsgWinClose
.native IsMsgWinEnd_
.native ShowMsgWinCursor_
.native ABKeyWait_
.native PostSoundEventSE
.native PlayDemoScene_
.native printf
.native PokefanLog
.native PokefanAdd
.library 0xfbedaa83 0x0
.library 0xca18aa2e 0x0
.pubvar 0xd7477c97 0x0      ; g_mode: the event the game runs
.pubvar 0x00107770 0x8
.pubvar 0xea20b6c6 0x10
.pubvar 0xea1e9f9b 0x18
.tag 0xca18aa2e 0x40000002

.code
    halt.p 0

Wait:    ; 1 argument: frames
    proc
    _Suspend(arg0)
    zero.pri
    zero.alt
    halt 12
    zero.pri
    retn

WaitAction:    ; 1 argument: an object; waits until its turn or walk has finished
    proc
l_action:
    ifnot CheckActionCommandRunningState_(arg0) goto l_action_done
    Wait(1)
    jump l_action
l_action_done:
    zero.pri
    retn

WaitFade:    ; 0 arguments
    proc
l_fade:
    if FadeWait_() goto l_fade_done
    Wait(1)
    jump l_fade
l_fade_done:
    zero.pri
    retn

Say:    ; 1 argument: the hash of the line's label; shows it, waits for A and plays the button sound
    proc
    MsgWin_(0, 0, arg0, 8, 0, 0, 0)
l_say:
    IsMsgWinEnd_()
    eq.p.c.pri 1
    jnz l_said
    Wait(1)
    jump l_say
l_said:
    ShowMsgWinCursor_(1)
    ABKeyWait_()
    ShowMsgWinCursor_(0)
    PostSoundEventSE(0x62efcd1a)
    zero.pri
    retn

main:
    proc
    load.p.pri 0
    switch t_events
l_cutscene:
    Cutscene()
    jump l_end
l_unknown:
    printf("mode=%d Not Found \n", 0)
    jump l_end
t_events:
    casetbl
        default l_unknown
        case 0x13185063d6c081bd l_cutscene
l_end:
    zero.pri
    retn

Cutscene:    ; 0 arguments
    proc
    stack.p -8
    PokefanLog(1, 0)
    var1 = GetFnvHash64("BLACK")
    StartTurnAround_(0x7a29438e2b197b00, 0, 0)                  ; the player, to face the camera
    WaitAction(0x7a29438e2b197b00)
    Say(0xc21c489235c22493)                                      ; msg_pokefan_camera_01
    Say(0xc21c499235c22646)                                      ; msg_pokefan_camera_02
    MsgWinClose()
    PokefanLog(10, 0)
    PlayDemoScene_("sub_048_thunder", 1, 0, 1, 1)                ; with Bill's options: sub_event_048's (0, 0, 1, 0) leave
                                                                 ; the field to the battle that follows, and the next demo crashed
    PokefanLog(11, 0)
    PlayDemoScene_("ev0330_masaki", 1, 0, 1, 1)                  ; as main_event_0330 plays it, which then fades in
    PokefanLog(12, 0)
    FadeIn_(15, var1)
    WaitFade()
    StartForceMove_(0x7a29438e2b197b00, 1, 0x40b8ec0000000000, 0x4081e80000000000, 0, 0, 0, 1)   ; walk to x 6380.0, z 573.0
    WaitAction(0x7a29438e2b197b00)
    var1 = PokefanAdd(40, 2)
    PokefanLog(2, var1)
    stack.p 8
    zero.pri
    retn

.data
    .cell 0 0 99 99      ; g_mode and the three public variables after it; the strings follow
