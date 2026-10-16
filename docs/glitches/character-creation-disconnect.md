# Character Creation Disconnect

## In short

In multiplayer, you create characters on the GBA. Each choice you make is sent
to the GameCube, which keeps a small record per player. When you confirm on the
GBA, the GameCube builds the character from that record.

If a GBA drops off during creation, the GameCube wipes that player's record to
all zeros. A zeroed record, if it were ever built into a character, would be a
**blacksmith in slot 1**: every field takes its first value, including the
target slot.

Observed behavior (reported by players, not reproduced here):

- **EN:** turning the GBA off on the last frame of creation creates a
  blacksmith in slot 1, apparently from a partly copied record. If another slot
  is already a blacksmith, Tipa ends up with two blacksmith families.
- **JP/PAL:** the same input doesn't touch slot 1. With one GBA, it cancels
  the forced character selection instead.

The PAL code explains both PAL's safety and why a blacksmith in slot 1 is
exactly what a wipe would produce. EN has no decomp yet, so what EN changes
is a **hypothesis**.

## What a wiped record becomes (confirmed)

The GameCube's record is a `GbaCMakeInfo` per GBA port. The GBA sends the
name, look, job, birthday and favorite foods as you finish each step
([gbaque.cpp:2755](../../src/gbaque.cpp#L2755) and the functions after it).
The target slot is not sent by the GBA. The GameCube stores it when you pick
an empty slot (`InitCmakeInfo`, [gbaque.cpp:2714](../../src/gbaque.cpp#L2714)).

A disconnect wipes the whole record to zeros (`ClrCmakeInfo`,
[gbaque.cpp:2737](../../src/gbaque.cpp#L2737)). Built by `SetMakeChara`
([wm_menu.cpp:7128](../../src/wm_menu.cpp#L7128)), a zeroed record gives:

| Field | Zeroed result |
|---|---|
| Slot | 0, shown in game as slot 1 |
| Job | 0 (blacksmith, matching what's observed) |
| Tribe, gender, look | first option of each |
| Name | empty |
| Birthday | 0/0 |
| Favorite foods | all eight get the same value |

Any field the GBA sends after the wipe overwrites its zero, so a "partial"
copy is possible. That part hasn't been traced.

## Malformed name continuation: a GameCube OOB write

There is a separate, stronger bug in the character-name packet assembler.
`GbaCMakeInfo` places the fields in this order:

```text
active, result, packetCount, crc, playerSlot, name[17], charaType,
birthday[2], favorites[4], jobType
```

The GameCube initializes `packetCount` to zero. A normal GBA name transfer
sends a `0x1C` header, which changes it to one, followed by five `0x5C`
continuations. However, `ChkCMakeName` accepts a continuation first and uses
`packetCount * 3 - 2` as an unchecked index into `name`. With a zero count, the
three payload bytes land at `name[-2]`, `name[-1]`, and `name[0]`.
`name[-1]` is the GameCube-owned `playerSlot` byte.

Thus a raw packet whose bytes are `5C xx SS cc` changes the eventual character
destination to `SS`. A subsequent `14 05 00 00` (`EVT_CMAKE_END`) completes
the record without checking that name, appearance, birthday, favorites, or job
were filled in. The second operation is accepted regardless of the GBA's
current menu mode.

There is no upper bound in the other direction either. Once six name words
produce a bad CRC, the handler leaves `packetCount` at six. Further `0x5C`
words write three bytes at a time beyond `name`; keeping `name[16]` zero makes
the handler's repeated `strlen` safe. This can overwrite every later
`GbaCMakeInfo`, the flags at the end of `GbaQue`, and eventually the following
BSS object. About 52 additional continuation words reach the end of `GbaQue`
from channel 0. It is a sequential forward overwrite and cannot skip the live
state in between.

`SetMakeChara` converts `playerSlot` through `signed char` and does not check
0-7 before indexing either the world-map model array or
`Game.m_caravanWorkArr`. Slot `FF` is therefore slot -1. Since each
`CCaravanWork` is 0xC30 bytes and the array starts at `Game + 0x13F0`, the fake
slot begins at `Game + 0x7C0`, inside `CGameWork`. Calling `LoadInit()` on that
fake object zeros all 256 saved event flags and the first 200 event-work
halfwords, along with part of the link table and the chalice/event header.

This is a broad destructive zero, not arbitrary data placement, and it does
not reach CFlat permanent globals such as `G59`. It may also crash during the
remaining character initialization. Still, it is a genuine negative
GameCube-array index controlled by one GBA payload byte.

### JoyBus reset can discard the header and preserve the continuations

The normal UI cannot enqueue a continuation before its header, but the
transport can expose the packets in that order. `Link_SendCMakeName` writes
the header directly to `REG_JOY_TRANS` when the transmit queue is empty and
puts the five continuation words in `sTxQueue`. The GBA's `Link_JoyReset`
then does two important things:

1. it replaces `REG_JOY_TRANS` with the idle word;
2. it **does not clear `sTxQueue` or `sTxCount`**.

After the ordinary six-step reconnect handshake, the preserved continuation
words are therefore the first character-name packets available to the
GameCube. This was reproduced against the retail JP multiboot image with the
[headless JoyBus harness](gba-joybus-harness.md). It uses Dolphin's bundled
mGBA core, so the reset, interrupt and register behavior are emulated by the
same GBA implementation used for normal FFCC play.

One concrete legal JP name is `A♪`, encoded as `41 81 F4`. The real
`Link_SendCMakeName` emits:

```text
presented header: 414B911C
queued word 0:    00F4815C
queued words 1-4: 0000005C
```

Issuing JoyBus reset before reading the header changes the presented word to
the idle word but leaves all five queued words. Reconnecting produced this
sequence in the harness:

```text
12345678, 00F4815C, 0000005C, 0000005C, 0000005C, 0000005C
```

The host ID is the handshake's final stale transmit value. The next word is
the malformed first CMake continuation. Its `F4` byte becomes
`m_playerSlot = -12`, and the five continuations leave `m_packetCount = 5`.
A later normal header resets the packet count but does not restore the slot.

This is a **confirmed packet-order primitive**, not yet a confirmed retail
setup. Its practical gates are:

- It needs a JoyBus protocol reset while the GBA remains powered. Rebooting
  the GBA CPU runs `Link_Init`/`Link_Reset` and clears the queue.
- The reset must land after the name routine presents the header and before
  the GameCube link thread consumes it. The emulator can impose that ordering;
  cable timing or a crash whose interrupt handler remains alive has not been
  tested on hardware.
- JP's character-selection loop does not call `ClrCmakeInfo` merely because
  the GBA is disconnected, so the corrupted record survives this reset path.
  PAL/EN have an explicit disconnected-port clear in the corresponding code.
- Before final creation, `ChkCMakeCharaType` calls `ChgModel(playerSlot)`.
  Slot -12 addresses handle 20 (still inside the 40-entry handle array) but
  writes before the heap `m_charaModelData` array.

The primitive is not limited to the initial underflow. Repeating a name
submission and discarding each new header delivers another five continuation
words. The GameCube reconnect path does not clear
`GbaCMakeInfo::m_packetCount`, so each cycle advances the unchecked
destination by 15 bytes. Relative to channel 0's `GbaCMakeInfo`, the first
cycle writes `+5..+19`, the second writes `+20..+34`, and so on. Nine cycles
reach the flags after all four creation records; twelve cycles reach the
following global `Wind` object.

This is a real forward-overwrite producer using only the ordinary name sender
and repeated protocol resets. It is not currently a route win: the bytes are
restricted to positions 1-15 of legal JP names, the overwrite destroys all
intermediate creation records, and the nearby post-array fields are JoyBus
refresh/mode flags rather than story state. Each retry also needs the same
narrow reset timing. It is nevertheless materially stronger than treating
the malformed continuation as a one-shot `m_playerSlot` edit.

The sequential interpretation is not the strongest use. Starting from
channel 3 reaches the post-array flags in 12-23 continuations, and channel 2
can overwrite channel 3's signed packet counter in continuations 10 and 11.
That converts the next channel-3 continuation into a three-byte indexed write
covering roughly +/-96 KiB around `GbaQue`. A concrete JP recipe reaches the
GC-to-GBA map source pointer and redirects it into the shared letter buffers;
see [JP character-name indexed writes](gba-name-indexed-write.md).

Directly writing byte `F4` to JP port 1's `m_playerSlot` at `0x80312F64`
after completing the character fields, then selecting Create, reaches the
finalizer but hard-freezes Dolphin while repeatedly reporting an ISI at
`0x00000000`. This rules out slot -12 as a usable final character-creation
result in that state.

A direct `FF` (-1) test at the same point also hard-freezes. Retail JP
disassembly identifies the immediate structural gate. At `0x800EDD94`, the
inlined finalizer loads the fake object's first word as a vtable; at
`0x800EDDA4` it loads virtual slot `+0x0C`; and `0x800EDDB4` branches through
that value to call `CCaravanWork::Init`. A negative array alias has not been
constructed as a `CCaravanWork`, so its first word is not the required vtable
pointer. A zero entry produces exactly the observed ISI at address zero.

For a diagnostic-only bypass, slot -1's fake base is `0x8023AF80` and the JP
`CCaravanWork` vtable is `0x8022CCBC`. Seeding the former with the latter
before selecting Create lets the virtual dispatch target the real `Init` at
`0x800A0748`. This does not make the route legitimate: an exploit would still
need a prior write of that vtable pointer, and the subsequent initialization
corrupts `CGameWork` before passing slot -1 to the script system call.

That diagnostic was tested. With `0x8023AF80 = 0x8022CCBC`, Create survives
the former ISI, leaves the world map, and begins loading the village. Dolphin
reads `GC23.fnt`, `m0012_00.mrg`, `village.cfd`, and the `c000_root` model and
texture, then stops progressing. This proves the vtable is the immediate
first crash gate and that the fake initialization runs far enough to request
a map transition, but not that the resulting state is playable.

There are two separable later hazards. Before fake-object initialization,
JP `0x800EDBFC` stores the new model number at
`m_charaModelData[-1].m_modelNo`, which is `0x2C` bytes before the allocated
array and may corrupt its heap metadata. After initialization, JP
`0x800EDDF0` invokes the script system call with -1 as its slot argument.
Temporarily replacing the former instruction with a NOP, then separately
substituting zero for the latter argument, distinguishes those failures from
the bulk `CGameWork` corruption.

The failure is consistent with the alias target. `SetMakeChara` treats
`0x80232970` as a complete `CCaravanWork` and runs `LoadInit`, `Init`, and
`LoadFinished` across it. That `0xC30`-byte range overlaps JP SDK data,
including interrupt, reset, SI, PAD, DVD, and audio tables. The operation is
therefore much larger than a single controlled write and destroys live
runtime state before creation can return. This does not establish the result
for slots -1 through -11, whose alias bases differ.

The JP keyboard cannot place `FF` in name byte 2, so this route does not yet
produce the especially valuable slot -1. Its closest legal negative value is
`F4` (-12), supplied by the music-note character. Other legal two-byte
characters provide many negative values from -128 through -12. On JP, fake
slot -12 begins at `0x80232970`, before `Game`, rather than inside
`CGameWork`; its structured initialization has now been observed to hard-freeze
the game after corrupting static runtime data rather than directly changing
story flags.

### Why two blacksmiths are possible

Duplicate jobs are rejected only when the GBA sends a job choice
(`ChkCMakeJob`, [gbaque.cpp:2930](../../src/gbaque.cpp#L2930)). A zero left
behind by the wipe never goes through that check.

This gives at most **two** blacksmiths. The zeroed slot is always slot 1, so
repeating the trick overwrites slot 1 each time. Any normal creation afterward
has blacksmith rejected because slot 1 now holds one.

## Why PAL is safe (confirmed)

`CMenuPcs::CalcCharaSelect` ([wm_menu.cpp:7191](../../src/wm_menu.cpp#L7191))
runs these steps in order every frame:

1. Read each port's connection state.
2. For each disconnected port, wipe the record **and** clear the "finished"
   flag ([wm_menu.cpp:7283-7285](../../src/wm_menu.cpp#L7283-L7285)).
3. Build a character for each port whose "finished" flag is set
   ([wm_menu.cpp:7356-7360](../../src/wm_menu.cpp#L7356-L7360)).

The "finished" flag is set by the GBA's final message, but only if the
GameCube can still send to that GBA (`CMakeEnd`,
[gbaque.cpp:693](../../src/gbaque.cpp#L693)). If the GBA is gone, creation is
cancelled instead. So in PAL a wiped record never reaches step 3.

### What PAL does instead

On the last frame, the "finished" message arrives while the GBA still counts
as connected, so PAL builds the character from the real record. Once the GBA
is off, each later frame clears that player's confirmed, in-progress and
finished flags ([wm_menu.cpp:7365-7372](../../src/wm_menu.cpp#L7365-L7372)).
The screen only moves on once every connected (or recently disconnected)
player has confirmed ([wm_menu.cpp:7551](../../src/wm_menu.cpp#L7551)). With
one GBA, its only selection was just cleared, so the screen never moves on.
That matches the cancelled selection. Exactly what makes the selection
"forced" hasn't been traced.

### One theoretical PAL gap

`CMakeEnd` runs on the link thread. It first confirms the send to the GBA,
then sets the "finished" flag. If the main loop ran steps 1 and 2 between
those two actions, step 3 of the same frame would build a zeroed record. The
window is microseconds, and the thread priorities haven't been checked.

## EN (hypothesis)

EN builds this code differently. `SetMakeChara` is a separate function in EN
(`0x80116C58`, 576 bytes) but is inlined into `CalcCharaSelect` in PAL. That
suggests the code was changed between versions. The observed EN behavior means
a wiped or half-written record reaches the build step. That would happen if
EN's step 2 runs after step 3, or if EN's step 2 doesn't clear the "finished"
flag. Confirming this needs the EN decomp.
