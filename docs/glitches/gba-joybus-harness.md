# Headless GBA JoyBus Harness

## Purpose

`tools/gba_joybus_harness.cpp` boots an unmodified FFCC GBA multiboot image
under Dolphin's bundled mGBA core without running the GameCube game. It acts as
a scriptable GameCube parent and can issue the four JoyBus operations directly:
reset, poll, receive a GBA word, and send a GameCube word.

This makes destructive wrong-menu experiments cheap and deterministic. A GBA
crash is restored from an mGBA snapshot rather than requiring Dolphin and the
GameCube to repeat the setup. It also separates two questions which are easy
to conflate during live testing:

- did corrupt code merely damage graphics or stop executing;
- did it actually queue or present a 32-bit word to the GameCube first?

The harness is an experiment driver, not proof that the retail GameCube can
produce every tested parent packet. Synthetic parent inputs must still be
connected to a real in-game sender before they become a speedrun setup.

## Building

The Windows helper expects a Dolphin checkout with its Release mGBA and zlib
libraries already built. Its default root is `C:\Projects\dolphin`; pass a
different root when needed.

```powershell
tools/build_gba_joybus_harness.ps1
```

The output is `build/tools/gba_joybus_harness.exe`. A smoke test which boots
the JP client and completes FFCC's six-step handshake is:

```powershell
build/tools/gba_joybus_harness.exe orig/GCCJGC/files/dvd/gba/ffcc_cli.bin
```

## Parent transcript language

Pass a text file as the second argument. Supported commands are:

```text
handshake
reset
poll
read
write WORD
send-message HEXBYTES
send-bulk-file TYPE PATH
wait FRAMES
keys MASK
drain
state
regs
dump ADDRESS SIZE
poke8 ADDRESS VALUE
poke16 ADDRESS VALUE
poke32 ADDRESS VALUE
call-thumb ADDRESS R0 [R1]
call-thumb-until ADDRESS R0 R1 STOP [LIMIT]
step-trace COUNT
quiesce
sweep-smith
probe-queue-reset
probe-letter-reply-reset
probe-western-warp-packets
probe-smith-letter-index TARGET
probe-western-smith-letter-index EN|PAL TARGET
probe-radar-stack
probe-radar-complete-western
probe-radar-western-stack-pivot
probe-radar-rop
probe-radar-item-rop
probe-radar-cmdslot-rop
probe-radar-letter-rop
probe-radar-route-letter-rop
probe-radar-cmdwrite-rop
probe-radar-equipwrite-rop
probe-radar-route-equipwrite-rop
probe-radar-raw-write-rop
probe-radar-raw-write-rop-en
probe-radar-raw-write-rop-pal
```

`write` accepts an arbitrary little-endian 32-bit GameCube-to-GBA word. This
is enough to replay real `JoyBus::Send*` traffic or deliberately malformed
traffic. `send-message` adds the normal small-message framing and CRC, while
`send-bulk-file` reproduces the retail `LINK_DATA` header, 0x2FD-byte blocks,
per-block CRCs, padding, and ACK/NAK exchange for an arbitrary binary file.
Type 1 downloads to `DOWNLOAD_BUF`; types 3, 6, 7, 8, and 9 download to
`LIST_BUF`. `call-thumb` makes isolated retail functions callable with
controlled `r0` and `r1`. `poke*` and the probes currently use JP retail
addresses.

The smith-letter-index probes use the complete retail item-534 (Firaga +2)
smith record with its inventory index set to 1. At target 178, the unmodified
JP, EN and PAL clients all reach the same selection and queue
`0x00B2020C`, the normal `REQ_LETTER(178)` packet. Replays are in:

- `tools/gba_joybus_jp_smith_letter_index_178_probe.txt`
- `tools/gba_joybus_en_smith_letter_index_178_probe.txt`
- `tools/gba_joybus_pal_smith_letter_index_178_probe.txt`

## Confirmed experiments

### Signed smith-screen sweep

`tools/gba_joybus_sweep.txt` snapshots a clean client and runs every signed
8-bit smith screen, all three screen phases and eight key states: 6,144 cases
in about ten seconds on the development machine.

Several negative indices send real protocol words before stopping. These are
mostly legitimate function-table aliases rather than useful data-as-code:

| Smith index | Aliased screen | Observed output |
|---:|---|---|
| -10, init | CMake Name | one `0x1C` header and five `0x5C` continuations |
| -8, init | CMake Look | `EVT_CMAKE_LOOK` with stale look `FF` |
| -6, exit | CMake Confirm | `EVT_CMAKE_END` (`0x00000514`) |
| -16 | Field Letters | letter-list request/state traffic |

This confirms that a crash after a send is harmless to the already presented
word. It does not make the negative indices reachable: the normal submenu race
saves field screens 0-10. A raw `EVT_OPEN_MENU` argument is signed, so a
synthetic parent can select these indices, but no retail producer of a negative
menu byte has been found.

The positive JP smith screens which execute fixed Japanese text remain much
less useful. The sweep did not find a controlled transfer-buffer word from
those reachable pixel-vomit entries.

A separate stateful replay in
`tools/gba_joybus_pixel_vomit_probe.txt` left screen 4's self-modified table in
place for later frames. Its first executed ARM store changes smith screen 0's
init pointer from `0x0201736D` to the fixed text address `0x0201CC68`; execution
then reaches the undefined-instruction vector and stops. Later frames neither
call the rewritten entry nor send a useful word. Thus the visible persistence
is real, but it does not provide a recovery-and-dispatch stage before the
client dies.

### Compatibility overflow to command-list write

`tools/gba_joybus_compat_probe.txt` sends one correctly framed compatibility
message with five relationship names. The fifth name's tenth byte is `E8`, so
the retail decoder stores signed `cmdSlotCount = -24`. Calling the exact sink
reached after the Command List's Up-wrap/removal presents:

```text
send-message bytes=28 words=11 crc=0xC95E
REG_JOY_TRANS: 1F E7 FF FF
drain: 0xFFFFE71F
```

The send occurs before the unsafe row redraw. This proves the entire GBA half
of the exploit: a GC variable-length payload corrupts persistent GBA menu
state, and an ordinary GBA action turns it into a different valid GC command.
The retail blocker is producing a fifth relationship on the GameCube; normal
character creation produces four.

### Name header discarded by JoyBus reset

`tools/gba_joybus_queue_reset_probe.txt` calls the retail JP
`Link_SendCMakeName` with the legal name bytes `41 81 F4` (`A♪`). The client
places the header in `REG_JOY_TRANS` and five continuations in `sTxQueue`:

```text
before-reset presented=0x414B911C
queued=5 0x00F4815C 0x0000005C 0x0000005C 0x0000005C 0x0000005C
```

The fake parent then issues a real JoyBus reset and performs the complete FFCC
handshake again. The result is:

```text
after-reset presented=0x00570056 queued=5
reconnect ... words=0x12345678,0x00F4815C,0x0000005C,...
```

Thus `Link_JoyReset` discards the unconsumed header while preserving all five
continuations. If the GameCube's `GbaCMakeInfo::m_packetCount` is zero, the
first continuation writes `F4` to `m_playerSlot`, producing signed slot -12.
This is the first emulator-confirmed producer for the parser underflow; the
remaining work is forcing the reset window with the retail GameCube parent and
testing how far negative-slot character creation survives.

The retail GameCube parent does have a matching primitive. JoyBus worker state
3 calls SDK `GBAReset`, then performs the ordinary reconnect handshake. Its
state byte is `Joybus + 0x5D38 + 0x3C*port`; this is independently visible in
the retail assembly as the `0x5D10` `ThreadParam` base, `0x3C` stride and
`0x28` member offset. `tools/gba_gc_warp_lab.py --live
--trigger-joy-reset PORT` requests that path and snapshots the GC-side CMake
records around it. A live PAL test during the ordinary character-name keyboard
window reset and reconnected the GBA successfully, but changed its active
creation record (`result=28`, `packetCount=6`) to all zeroes. The straightforward
active-keyboard timing is therefore unusable on PAL. The remaining problem is
a natural trigger aligned with a character-select early-return or transition
window, not the existence of a suitable reset on either endpoint.

### Letter-reply middle word survives while both guards are discarded

`tools/gba_joybus_letter_reply_reset_probe.txt` calls the retail JP
`Link_SendLetterReply` with letter 7, answer 2 and value `0x12345678`. The
normal three-word transmission is:

```text
reply-before-reset presented=0x07192815
queued=2 0x12000255 0x78563495
```

The first reset discards the `0x15` header and preserves the `0x55` middle
word plus `0x95` final word. After reconnect, the fake parent reads only the
middle word and resets once more before reading the final word:

```text
middle=0x12000255 final-presented=0x78563495 queued=0
reply-after-second-reset presented=0x00570056 queued=0
```

This is the ordering needed for the GameCube receive-buffer overflow described
in [What the GameCube Trusts from the GBA](gba-gc-trust.md#reset-stripped-letter-replies-append-without-a-bound-or-crc).
The harness proves the GBA queue behavior; the unchecked GameCube append and
reconnect lifetime are established from the GameCube source.

The integrated Dolphin/mGBA lab now proves the GameCube half as well. In
[`jp_mog_0_to_meteo_1_exploit.json`](../../tools/ffcc_lab_trials/jp_mog_0_to_meteo_1_exploit.json),
the active retail client executes `Link_SendLetterReply` for a 13-gil
attachment. A powered cable handoff lets port 3 consume the header and returns
the same GBA to port 0 for the two continuations. Retail
`JoyBus::SetRecvBuffer` writes `G59=13`; leaving the Moogle nest subsequently
loads `meteo_1`. This is distinct from the standalone fake-parent probe: both
processors and the map script are running together, and the trace contains no
injected packet.

### EN/PAL restricted-warp packet lab

`tools/gba_joybus_western_warp_probe.txt` runs identically against the EN and
PAL retail clients. It calls their real `Link_SendCMakeName` and
`Link_SendLetterReply` functions and captures the exact words for both
candidate receive slots:

```powershell
build/tools/gba_joybus_harness.exe orig/GCCE01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_western_warp_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCP01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_western_warp_probe.txt
```

The legal name `AAAÀA` emits continuations containing `C0` and `41`, enough
to install the receive-slot-3 counter `C041`, although that slot's length is
not keyboard-encodable. Receive slot 0 needs counter `BC39`; the probe now
demonstrates the fully legal workaround. Name `AAA»ó` installs `BBF3`;
fourteen five-word zero name groups advance it the remaining 70 steps. The
writes remain inside the
GC message-temporary/path-buffer interval immediately before receive slot 0.
The legal length name `AcÀ"` then emits `63 C0 22`.

The companion `tools/gba_gc_warp_lab.py` reads the live CFlat pointer from
Dolphin, checks all counter and length bytes against the EN/PAL keyboard,
prints the safe counter walk and exact overwritten ranges, and can enumerate
every directly encodable CFlat global.

### Radar row expansion gives confirmed GBA PC control

`tools/gba_joybus_radar_stack_probe.txt` installs a synthetic downloaded map
row and calls the retail JP `Radar_DrawMap`. In that function, `line[256]`
begins 0x250 bytes below the saved LR, so halfword index 296 is the return
address. The compressed row has only three tokens:

```text
8128 0001 0203
```

`8128` is an RLE token with count 296. It fills through the intervening stack
locals, and the two literal tokens replace LR with the Thumb pointer
`0x02030001`. The retail function exits its row loop and branches there:

```text
probe-radar-stack hijacked pc=0x02030002 steps=3141
```

The target contains a two-byte `b .` sentinel, so this is deterministic PC
control rather than merely an exception after stack damage. Both address
halfwords must have bit 15 clear; otherwise the decoder treats that halfword
as another RLE token. This probe establishes the GBA sink under controlled map
bytes. It does not establish that a retail GameCube builder can yet emit the
three-token row after the map-pointer redirect.

`Radar_InitMap` also passes `DOWNLOAD_BUF+0x20` to the BIOS LZ77 decoder before
installing the corrupt row pointers. This does not require a full valid image:
`tools/gba_joybus_radar_init_zero_lz_probe.txt` confirms that the four-byte
stream header `10 00 00 00` (type `0x10`, decompressed size zero) returns
cleanly in the retail client. A retail smith-stream solver therefore needs one
exact zero-length LZ77 word at map offset `0x20`, not an entire compressed tile
map.

### A complete smith-record map reaches the western ARM stage

`probe-radar-complete-western` replaces the earlier fabricated three-token
row with the exact bytes made by a PAL smith list in the saved Tipa snapshot.
The map source begins at record 0 byte 31:

```text
record 0  item 4000
record 1  item 14624
record 5  item 405
record 10 item 10284
record 11 item 17225
record 12 item 416
```

The first pair supplies the header and zero-length LZ stream, item 405 supplies
`MapInfo` and its row lengths, and the final three records supply the 41 tokens
of row 5. `Radar_DrawMap` expands that real row to exactly 298 halfwords and
returns to `03000001`. The existing LinkWork bytes fall through to the queued
western name, switch to ARM, and branch to PAL `DOWNLOAD_BUF+60C`:

```text
probe-radar-stream entry target=0x03000001
probe-radar-arm-branch target=0x0203860C
```

The EN image uses the same construction with legal name byte `D1`, targeting
`0203460C`; PAL uses `E1`. A later-row form places items 1369, 13568 and 416
at records 17-19 and selects row 21 with base position `(81,752)`. It reaches
the same PC with a clean `Z=1` ARM-entry state while leaving records 6-16 free.

The later-row probe also accepts the adjacent legal PAL name bytes. The retail
client confirms these three ARM entries without patching its code:

```text
probe-radar-complete-western-late-slot 0xE0 -> 0203820C
probe-radar-complete-western-late-slot 0xE1 -> 0203860C
probe-radar-complete-western-late-slot 0xE2 -> 02038A0C
```

Each arrives with `r0=00040000`, `r2=r12=040000B0`, `r6=00010000`,
`sp=03007F00`, ARM mode and `Z=1`. In the real Smith byte stream the landings
correspond to record 9 byte 51, record 28 byte 11 and record 46 byte 27. The
adjacent name character is therefore an alignment control, not merely an
equivalent branch target.

`gba_smith_arm_search.py` uses Unicorn only as a fast candidate filter over
complete zero-material Smith records. Its hits require replay in this mGBA
harness. Current bounded results contain no sender: `E2` has no hit through
eight selected records, while `E0` has no hit through its seven free records
before the fixed row-21 records. Item 1276 can execute
`STM r0,{r11,r12,pc}` with `r0=040000B0`, populating DMA0's source,
destination and control words in one record. The observed destination is
`03007EFF` and the PC-derived control word does not enable DMA. No searched
state yet supplies `r12=04000154` plus a later enabled control write, so this
is DMA register control rather than a JoyBus packet.

Do not treat `probe-radar-bg-affine` as proof of a BIOS gadget. That legacy
probe did not place code at its alleged ARM target and only traversed cleared
memory. Likewise, first-PC probes must include the entire final Smith record;
truncating at the saved PC suppresses suffix writes and creates false sender
results.

This proves a real GBA-side producer, not the final GameCube setup. Normal
smith data arrives in `LIST_BUF`, whereas the tested map is consumed from
`DOWNLOAD_BUF`. Finding a retail wrong-menu or pointer-lifetime operation that
crosses those buffers remains the decisive western staging problem.

A legal name can instead branch directly to the intact list (EN byte `E2`,
PAL byte `EA`). Item-count alignment exposes every four-byte record offset.
The clean-record sweep found no direct sender. The most promising ARMv4 stack
pivot, `SUBEQ pc,sp,r1,LSR #31`, does execute in mGBA but the currently
available three-halfword tail runs into caller-stack data and crashes without
presenting a word. `probe-radar-western-stack-pivot` preserves this negative
result as a reproducible test.

### Radar PC control reaches a legal-name payload and sends to the GameCube

The first controlled return need not be the final payload. JP address
`02010251` is a useful epilogue inside `GilScreen_Main`:

```text
movs r0, 0
add sp, 8
pop {r4-r7}
pop {r1}
bx r1
```

At entry, the restored stack is `Radar_DrawMap`'s original stack. The gadget
loads `r4-r7` from expanded row halfwords 302-309 and its next PC from
310-311. JP `DETAIL_BUF` is `02035800`, so `02035801` is an encodable return
pointer. A letter body downloaded there survives later smith-list and map
downloads.

`tools/gba_joybus_radar_rop_probe.txt` puts `Link_SendEvent` in `r4`, `0x42`
in `r6`, and the ASCII name `1F2F G` at `DETAIL_BUF`. Those six legal JP name
characters are three Thumb instructions:

```text
1F    mov r1, r6
2F    mov r2, r6
 G    bx  r4
```

The untouched retail client queues a real outbound word:

```text
probe-radar-rop queued ... packet=0x42420014 count=1
```

Little-endian packet bytes are `14 00 42 42`, or
`Link_SendEvent(0, 0x42, 0x42)`. This proves the complete synthetic
GC-parent-to-GBA-to-GC loop, not merely execution of a sentinel.

Two variants show immediate exploit value:

- `probe-radar-item-rop` uses the sibling `02010201` landing, which reaches
  the same epilogue with `r0=1`, then calls `Link_SendItemOp`. It queues
  `17 01 A6 A6`, an unchecked use request for slot 166 (gil).
- `probe-radar-cmdslot-rop` loads `r6=72` and executes the legal
  four-character name `qB G`: `neg r1,r6; bx r4`. With
  `r4=Link_SendCmdSlot`, it queues `1F 01 B8 FF`, installing signed inventory
  reference **-72** into command slot 1. This is the Big Chungus hitbox-pointer
  index.
- `probe-radar-letter-rop` takes the stronger route. The six-character name
  `kB3D G` executes `neg r3,r5; add r3,r6; bx r4`. With
  `r5=0x00480001`, `r6=2`, and `r4=Link_SendLetterReply`, it constructs
  attachment value `0xFFB80001` and queues the complete three-word reply:

  ```text
  0x004D4715  0xFF000155  0x0100B895
  ```

  The GameCube reads item 1 and signed source index -72, then calls
  `DeleteItemIdx(-72)`. This produces Big Chungus directly and bypasses the
  CLES food-kind check.
- `probe-radar-route-letter-rop` preserves the established Any% name prefix.
  The seven-character JP name `えkk3D G` enters at its first `k` and executes
  `ldr r3,[r5,#52]; add r3,r6; bx r4`. It loads a staged raw word from
  `LIST_BUF`, adds a popped delta, and queues the same `0xFFB80001` reply.
  Its first four bytes remain `82 A6 6B 6B`, so the ordinary `えkk`
  slot-166 wrong-craft setup is unchanged. Changing the staged word/delta can
  select any signed-16 `DeleteItemIdx` target, not only -72.
- `probe-radar-cmdwrite-rop` preserves the same prefix with the name
  `えkk7D8G`. A different retail epilogue at `02002555` supplies one extra
  popped register. The name loads a staged Smith-list base, adds two popped
  registers to synthesize the otherwise non-literal address `0200AD4F`, and
  enters the argument-building tail of `CmdList_SetSlot`. The unmodified
  client queues:

  ```text
  probe-radar-cmdwrite-rop queued ... packets=0x1234EE1F count=1
  ```

  This is packet `1F EE 34 12`: command index 238 and exact value `0x1234`.
  Varying the popped values and staged halfword controls all 256 destinations
  and all 65,536 values. On the GC this writes anywhere from caravan `+0x204`
  through `+0x402`, including backup inventory, persistent character metadata,
  letter count, and both complete first letter records.

- `probe-radar-route-equipwrite-rop` keeps `えkk7D8G`, restores `r4=5`
  through retail gadget `0200570D`, and enters `Smith_EquipResult`'s send
  tail. The untouched client proves the inventory-alias packet:

  ```text
  probe-radar-route-equipwrite-rop queued ... packets=0x009A051E count=1
  ```

  Those bytes are `1E 05 9A 00`: on GC, equip index 5 aliases inventory slot
  0 and signed byte `9A` is item -102. `probe-radar-equipwrite-rop` proves the
  same packet with the simpler non-route name `(F1F G`.

- The three `probe-radar-raw-write-rop*` variants put retail `Link_Write|1`
  in `r4`, `0x78563455` in `r6`, and execute ASCII name `0F G`:

  ```text
  30 46    mov r0, r6
  20 47    bx  r4
  ```

  JP, EN and PAL each present the exact unrestricted word `0x78563455`. EN
  uses the corresponding `GilScreen_Main` epilogue at `020150C5`; PAL uses
  `02015281`. This proves that the post-radar sender is not JP-only and is not
  limited to a canned packet helper. It is the outbound half of the
  [arbitrary GameCube write](gba-gc-arbitrary-write.md).

These payload-specific probes synthesize their radar stack words. They prove
that a GBA crash is unnecessary and that legal player-name bytes are a viable
second stage in all regions. A separate real smith stream supplies a complete
western header and PC-control row, but it does not yet supply these larger
register payloads. The corrected GC-side `C02D -> BF2C -> DE36` pointer chain
now supplies the static `LIST_BUF` -> `DOWNLOAD_BUF` bridge; its multi-port
controller choreography is not harnessed end to end. Of the demonstrated payoffs, the letter-reply variant
reaches the widest address range. The command-write variant is narrower but
writes an exact value rather than fixed `0xFFFF`; its most practical target is
a forged item-bearing letter which can be claimed later in single-player mode.

## Smith screen 9 selects fake letter 178

The smith-letter-index probes model a normal completed letter list followed by
the ordinary one-recipe smith header `01 01 00 00` and the complete item-534
(Firaga +2) smith record. They then enter smith screen 9 and drive the retail
letter-list input code with Down and A. JP, EN and PAL all produce:

```text
top=171 cursor=7 selected=0xB2
queued: 0x00B2020C
```

Little-endian packet bytes are `0C 02 B2 00`, the normal GC request to open
letter 178. On the GC, that index's opened-bit store lands on the next
`CCaravanWork::m_id`. A male Yuke then derives the desired 655.35-radius data
pointer after reload, but the same corrupt ID is also an invalid player model
number. The required post-load ID sanitizer is described in
[GBA/GC trust](gba-gc-trust.md#smith-screen-9-can-select-index-178-with-an-ordinary-one-recipe-packet).
The replay inputs are
[`tools/gba_joybus_jp_smith_letter_index_178_probe.txt`](../../tools/gba_joybus_jp_smith_letter_index_178_probe.txt),
[`tools/gba_joybus_en_smith_letter_index_178_probe.txt`](../../tools/gba_joybus_en_smith_letter_index_178_probe.txt),
and
[`tools/gba_joybus_pal_smith_letter_index_178_probe.txt`](../../tools/gba_joybus_pal_smith_letter_index_178_probe.txt).
