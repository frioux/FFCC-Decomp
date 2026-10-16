# Negative Letter Attachments

## In short

A replied-to letter with an item attached contains both the item ID and the
inventory slot it came from. The GameCube trusts the signed slot number. If the
GBA can produce a negative attachment index, the reply gives a direct
halfword write outside the inventory:

```text
caravan->m_inventoryItems[index] = -1
```

This is stronger than CLES for the same target because it does **not** require
the target's old value to look like food. In particular, index **-72** writes
the low half of the player's `m_romWork` pointer, which is exactly the pointer
edit that produces the enormous player body radius ("big chungus").

There is not yet a controller-only trigger, but the final GBA command is now
proved. A radar stack overflow can execute the legal JP player name `kB3D G`,
which makes the unmodified client send a reply whose attachment value is
`0xFFB80001`. The GameCube interprets that as item 1 from inventory index -72
and performs the pointer write without any food check. What remains unsolved
is producing the complete malicious radar map from retail smith records and
the cross-port name-buffer redirect.

The normal gift cursor's theoretical negative range only overlaps
`eventWork[105..255]`. Radar ROP is not bound by that cursor: it controls the
entire signed high half, giving indices **-32768..32767**. On slot 0 that
reaches all saved event flags, all event-work words, every caravan and monster
record, and almost the entire `Game` object. It still does not directly reach
CFlat's heap-resident permanent `G` variables such as `G59`.

## The write primitive (confirmed)

The GBA gift screen constructs an attachment in
[`LetterGift_HandleInput`](../../gba/src/cli/main/letter.c#L2082):

```c
idx = win->cursor + sLetterGiftTop;
sLetterAttachValue = (idx << 16) | gSession.items[idx];
```

When the reply is sent, all 32 bits are transferred. The GameCube reconstructs
the value in [`GbaQueue::ReplyLetter`](../../src/gbaque.cpp#L2411), uses the low
half as the attached item ID, and then deletes the source slot:

```cpp
itemId = value & 0xffff;
...
caravan->DeleteItemIdx(static_cast<int>(value) >> 16, 1);
```

The shift is arithmetic in retail code, so a high half of `0xFFB8` becomes
index -72 rather than 65,464. `DeleteItemIdx` has no range check. JP and EN
always write `0xFFFF` and decrement the inventory count; PAL first checks that
the target is not already `0xFFFF`.

The low-half item is passed to the letter-reply script before the deletion,
but it does not decide whether the deletion happens. This is why the
GameCube-side target does not need to be food, unlike
[CLES](command-list-oob.md).

## Exact radar-ROP packet (confirmed in the retail GBA client)

The JP radar overflow can first return through `GilScreen_Main` at
`02010251`. That epilogue sets `r0=0`, pops `r4-r7`, and returns to the letter
body in `DETAIL_BUF`. At name entry, the stable low bytes already provide
reply answer 1 in `r1` and `isGil=0` in `r2`. The radar row supplies:

```text
r4 = Link_SendLetterReply | 1
r5 = 0x00480001
r6 = 2
```

The six legal ASCII characters in the JP name `kB3D G` are these Thumb
instructions:

```text
kB    neg r3, r5     ; 0xFFB7FFFF
3D    add r3, r6     ; 0xFFB80001
 G    bx  r4
```

The untouched retail binary queues these three words:

```text
0x004D4715  0xFF000155  0x0100B895
```

After packet framing and CRC, their payload is letter 0, answer 1, item mode,
and value `0xFFB80001`. `GbaQueue::ReplyLetter` therefore calls the reply
script with the valid nonzero item ID 1 and then calls `DeleteItemIdx(-72)`.
This removes both previous Big Chungus blockers: the normal 0-63 GBA selector
and CLES's requirement that the target halfword look like edible food.

This proof uses a synthetic radar row. The outstanding retail problem is one
ordered smith list whose bytes simultaneously form the map header, row
metadata, overflow tokens, popped registers, and return pointer.

### The existing `えkk` route name can carry the payload

A second confirmed payload avoids making a separate character named
`kB3D G`. The Any% wrong-craft setup only consumes the first four raw bytes of
the name `えkk`: `82 A6 6B 6B`. Adding four ASCII characters produces the
maximum-length JP name `えkk3D G` without changing those bytes.

If the second return enters at `DETAIL_BUF+2`, the name executes:

```text
kk    ldr r3, [r5, #52]
3D    add r3, r6
 G    bx  r4
```

The smith list remains in `LIST_BUF` after the corrupt map is downloaded.
Radar ROP can therefore make `r5+52` point at any aligned raw Smith-list word
and choose `r6` so their sum is `0xFFB80001`. The harness example stages
`0xFFB7FFFF` and pops `r6=2`; the retail client queues:

```text
0x000DCC15  0xFF000355  0x0100B895
```

This is the same item-1/index--72 reply, with answer byte 3 because the entry
pointer ends in `03`. The GameCube does not range-check that answer before the
deletion. This form is substantially more route-friendly: one character name
can perform the known slot-166 wrong crafts and serve as the final executable
letter payload. The Smith solver still has to locate the load word, its delta,
and the radar stack fields in one ordered list.

## What the normal gift UI permits

Both `sLetterGiftTop` and `Window.cursor` are signed bytes. After integer
promotion their theoretical sum is **-256 to 254**. Before accepting an item,
[`LetterGift_CanGive`](../../gba/src/cli/main/letter.c#L2170) requires the
halfword at `gSession.items[index]` to be greater than `0x124` and not already
referenced by equipment or the command list.

Normal operation never reaches that range:

- `LetterGift_Init` clears the windows and sets `sLetterGiftTop = 0`.
- The up/down handlers stop at the top and bottom of the 64-item list.
- A normal cursor is non-negative.

Therefore a usable setup needs two properties at once: a path into gift logic
that skips initialization, and stale/corrupted signed selection state. The
out-of-bounds word in the GBA also has to pass the `> 0x124` test.

The useful coincidence is that much of `gSession.items[-250..-55]` aliases
`gMapObjs`, the downloaded radar-object list. Its signed coordinates change by
map and can naturally exceed `0x124`. This can satisfy the value gate for some
negative indices, but it does not by itself make the selection negative.

This section describes only the older menu-driven route. The radar/name call
does not use `LetterGift_HandleInput`, `sLetterGiftTop`, or
`LetterGift_CanGive`; its constructed 32-bit value can contain any signed
16-bit index and any nonzero item halfword.

## Slot-0 GameCube targets

On JP, slot 0 is at `Game + 0x13F0` and its inventory begins at
`Game + 0x14A6` (`0x8023BC66`). The offsets are the same in every region. A
full signed index covers byte addresses `0x8022BC66..0x8024BC64`, corresponding
to `Game-0xEB5A` through `Game+0x114A4`. `Game` itself is size `0x11F88`, so
only its final `0xAE4` bytes are outside the positive end.

| Letter index | Slot-0 target | Result of the reply |
|---|---|---|
| -2643 | Start of `Game` | Corrupts the first half of its vtable; expected crash, useful as the lower offset landmark |
| -2635 / -2634 | High / low half of `m_scriptSysVal0` | Forces a large negative or `65535`-like year/system value; survival and routing effect untested |
| -2559 onward | Start of `m_linkTable[8][8][8][8]` | Sets two relationship-table bytes to `FF`; not a world-map unlock table |
| -501 / -500 | High / low half of `m_chaliceElement` | Produces a noncanonical element value, not the exact Holy value 16 |
| **-489..-362** | All 256 bytes of `m_eventFlags` | Each write sets 16 consecutive saved story flags |
| **-361..-106** | `m_eventWork[0..255]` | Any chosen saved script word becomes -1 |
| -105 | `m_bossArtifactStageIndex` | Becomes -1 |
| -104..-92 | Options, pause/state bytes, memory-card state | Usually destructive, no route found |
| -91 | Start of caravan 0 | Corrupts the object's first halfword |
| **-72** | Low half of `m_romWork` | **Huge player radius (big chungus)** |
| -5..-2 | Weapon, armor, tribal and accessory slot | Unequips/corrupts that equipment reference |
| -1 | `m_inventoryItemCount` | Becomes 65,535; future item grants fail |
| 0..63 | Normal inventory | Ordinary gift deletion |
| 1560, 3120, ... | Later caravans' inventory starts | The same relative character targets for save slots 1-8 |
| 22669 onward | `Game.m_scriptFoodBase`, current-map state and later `Game` fields | Pointer/state corruption; no clean route yet |

For a particular saved event flag `f`, index
`-489 + floor(f / 16)` writes `0xFFFF` over its entire 16-flag group. For
example, index -459 sets flags 480-495, including flag 490. For event-work word
`i`, the exact index is `-361 + i`.

For caravan slot 1, all `Game` targets shift down by 1,560 indices. Unlike the
old signed-byte cursor range, the full signed-16 packet can still reach much of
`Game`; the exact upper/lower coverage changes by slot. The relative
`m_romWork` target remains -72 for every caravan.

The negative-letter primitive alone cannot reach the Moogle-hole `G59` target.
`CFlatRuntime::m_permanentVarValues` is a heap allocation, not part of
`Game.m_gameWork`. A separate chain now closes that gap: the cross-port name
index can replace a JoyBus receive length, and unrestricted orphan `0x55`
packets then write anywhere in MEM1. Its cleaner target is the static pending
script block in `Game`, which can request `startmap` directly without writing
heap `G59`. See [GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md).

## Live JP Marr's Pass cross-check (2026-10-09)

The running game was `weapon_0` (Marr's Pass), with controller 0 bound to
caravan slot 0:

- `Game.m_scriptFoodBase[0] = 0x8023BBB0` (slot 0).
- Slot 0 was a Lilty with 10 inventory items and 200 gil.
- GBA `sLetterGiftTop`, cursor, and attachment value were all zero.
- The only non-zero slot-0 words in reachable `eventWork[105..255]` were
  `eventWork[121] = 50` and `[125] = 50`.
- In the corresponding GBA negative range, no word for an event-work target
  passed the attachment test. `gSession.items[-72]` was also zero.
- Naturally attachable residue existed only at indices -50 through -4. Those
  indices land in the first `CGObjWork` fields and equipment, not in story
  state. Notably -5 and -4 would clear the weapon and armor references.

So the ordinary gift-screen route on this save contains no immediate
negative-letter story write or Big Chungus write. The radar-ROP route found
later bypasses both this residue test and negative selection; these live facts
remain relevant only to a menu-driven gift exploit.

## Why the pixel-vomit smith screens matter

[`SmithMode_Update`](../../gba/src/cli/main/mode.c#L341) indexes
`gSmithScreens[gScreen]` without checking that `gScreen < 4`. On JP the memory
after the four real smith screens is:

```text
gSmithScreens -> 14 pointers to Japanese letter strings -> gLetterStates
```

This gives two distinct kinds of OOB behavior.

### Screens 4-8: fixed Japanese text executed as ARM code

The string pointers are even addresses, so the indirect `bx` enters ARM mode
and executes Shift-JIS text bytes as instructions. For example, smith screen 4
phase 0 calls the first Japanese string at `0x0201CC5C`. Its first word decodes
as:

```text
strgt pc, [r2], r2, lsl #1
```

The call-site flags make `GT` true and `r2` still points at `gSmithScreens`.
That instruction overwrites the dispatch table and advances `r2` to
`0x060713B8`, in GBA VRAM. A later conditional multiple-store writes a group
of registers into VRAM. This directly explains the corrupted tiles and later
crash: it is deterministic GBA table/VRAM corruption, not a GameCube script
write.

Other string entries branch through data or perform different register stores.
This is a legitimate exploit surface because the dispatch table modifies
itself, but no controllable store into `sLetterGiftTop`, `sLetterAttachValue`,
or `gSession` has been identified. PAL/EN place keyboard tables after the
smith table instead, so their data-as-code behavior differs.

The word "text" is important here: these particular pointers refer to strings
compiled into the client, not to a received letter or the player's entered
name. The name bytes used by wrong craft do exist in mutable GBA memory: the
letter/buy reuse leaves them in `LIST_BUF` (`0x02032800` JP, `0x02038800` EN,
`0x0203A800` PAL). However, a sweep of every smith-table word addressable by a
signed menu byte (`gScreen = -128..127`), including the 256-KiB EWRAM mirrors,
found no pointer into `LIST_BUF` or `DETAIL_BUF`. The current crash is therefore
fixed-data execution, not yet attacker-supplied code.

There is a concrete second-stage target if a different bug can write a
full-width `gScreen`. Because smith dispatch has no bounds check, these screen
values make one phase fetch its function pointer directly from `LIST_BUF`:

| Region | Exact `LIST_BUF[0..3]` alias | First `LIST_BUF` word reached from init |
|---|---|---|
| JP | screen 4354, init | screen 4354: `LIST_BUF+0` |
| EN | screen 5373, exit | screen 5374: `LIST_BUF+4` |
| PAL | screen 3682, main | screen 3683: `LIST_BUF+8` |

Subsequent phases select the next words. This would turn the recycled buffer
into a function-pointer table, but the only known `gScreen` input is
`JoyArgs.arg`, a signed byte, so none of these indices is currently reachable.
The ordinary `えkk` buy priming is also not by itself a function pointer: the
buy count replaces byte 0 and the three surviving name bytes occupy bytes
1-3. A usable chain would need both a full-width screen write and buffer bytes
that encode a valid Thumb target (or another useful control-flow value).

The known signed command-slot store does not supply that screen write. Its
complete `s8` index range writes halfwords from `0x03002C94` through
`0x03002E92`; `gScreen` is at `0x03003090` and `gSavedScreen` at
`0x030032E8` in all three builds.

JP also contains existing words equal to `DETAIL_BUF`, so overwriting the
dispatch table is not the only theoretical trigger. For example, address
`0x02002344` contains `0x02035800`; smith screen **-12131**, init phase, makes
the unchecked table calculation fetch that word and call `DETAIL_BUF`
directly. This still requires a full-width `gScreen` write. The menu-message
path supplies only an `s8`, and none of the known invalid submenu executions
writes -12131 to `gScreen`.

### JP names are useful instructions, but not callable pointers

A live JP letter beginning with the route name `えkk` put these bytes at the
start of `DETAIL_BUF` (`0x02035800`):

```text
82 A6 6B 6B 0A ...
```

This distinction matters for the full-width screen hypothesis. JP smith screen
5378/init would load that word as the function pointer `0x6B6BA682`; it would
not begin executing at `DETAIL_BUF`. The JP name alphabet alone cannot make an
internal GBA pointer in this position. A pointer to EWRAM, IWRAM or a GamePak
image needs a high byte of `02`, `03` or `08`-`0D`, while a fourth name byte is
ASCII or Shift-JIS. In this letter template, a three-byte name puts the
following newline in the fourth byte and produces a `0x0Axxxxxx` GamePak-ROM
pointer. FFCC is a multiboot client and has no useful cartridge payload there.

The same bytes become much more interesting if a separate corruption first
writes the address `0x02035800` into a dispatch-table slot. JP permits ordinary
ASCII in a name, and the four-character name `n4?z` is the little-endian ARM
instruction:

```text
02035800: 7A3F346E    bvc 0x030029C0
```

The smith call site has `V = 0`, so the branch is taken. `0x030029C0` is the
resident ARM interrupt dispatcher copied into IWRAM; it initializes its I/O
pointer, reads `IE & IF`, then uses `bx` to enter the selected Thumb handler.
This is a compact, legal ARM-to-Thumb bridge and could reach `Link_JoyIntr` if
a Joybus interrupt were pending.

There is also a stronger, exactly seven-character JP name:

```text
name:       ア2xq4?z
raw bytes:  83 41 32 78 71 34 3F 7A
02035800:   78324183    ldmdavc r2!, {r0, r1, r7, r8, lr}
02035804:   7A3F3471    bvc 0x030029D0
```

At the smith call site, `r2` is `gSmithScreens` (`0x02025BE8`). The first
instruction consequently loads `r1 = 0x02005AE9` from the function table just
before it. The branch enters the interrupt dispatcher after its hardware read,
with bit `0x80` set and bit `0x2000` clear in `r1`. That deterministically
selects `IntrTable[0]`, the Thumb function `Link_JoyIntr`, without requiring a
pending interrupt.

The skipped dispatcher prologue leaves `r3` uninitialized before its interrupt
acknowledgement store. On the ordinary main-loop path this is not a practical
fault: `Shake_Update` leaves a small counter in `r3`, and when the status icon
is drawn `Obj_Draw` leaves either its small object ID or a packed OAM word.
The halfword store therefore goes to BIOS/unmapped space and is ignored. If
JoyBus status bit 2 is set, `Link_JoyIntr` calls `Link_Send`, which pops the
oldest queued word into `REG_JOY_TRANS`. An ARM/Thumb emulation using a live JP
RAM snapshot, a pending-send status and a sentinel queued word confirmed the
complete path: `REG_JOY_TRANS` received the sentinel and the transmit count
fell from one to zero before later execution became unstable.

This is still **not yet an exploit**:

- no reachable smith-table entry contains `DETAIL_BUF`;
- screen 5378 treats the first name word as a pointer rather than code;
- the fixed JP text paths overwrite the smith table with fixed PCs/registers,
  not `DETAIL_BUF`, and do not survive to another frame;
- the payload transmits an already queued word; it does not itself construct an
  arbitrary GameCube command.

So JP names do provide substantially better instruction encoding than EN/PAL,
but the missing primitive is still a pointer-to-buffer write. The route name
trick that supplies wrong-craft slot `0xA6` has no direct code-pointer analogue.

The known pixel-vomit submenus were re-stepped with the GBA's EWRAM, IWRAM and
VRAM mirrors modeled. JP smith screens 4-8 execute real ARM stores and can
replace much of `gSmithScreens` with the screen number, zero, `gSession`, the
stack pointer and nearby PCs. They do not read or write `LIST_BUF` or
`DETAIL_BUF`, do not install `0x02035800`, and reach an undefined instruction
or unmapped fetch before returning to `SmithMode_Update`. Consequently their
self-modified table is not dispatched a second time. Misindexed entries that
land on genuine Thumb menu functions can read the shared buffers as ordinary
data; asynchronous list downloads and command-list rebuilding can also change
`LIST_BUF` while the UI is corrupted. Those effects explain a changing buffer
without establishing a jump into it.

### Other nearby decoder overflows do not supply the pointer write

Two unchecked JP GBA decoders looked promising during the same audit:

- `Session_OnCompatibility` trusts a relationship count up to seven while
  `gSession` only has four relationship records. Counts 5-7 overwrite the
  favorites, equipment, memories and early copied inventory inside
  `gSession`. The sender logs counts above four but still transmits them.
  Character creation, however, always installs two parents and exactly two
  optional siblings, so an ordinary save produces count four. The live slot-0
  record was `168, 176, 0, 0, 190, 193, 0` for event words 19-25. Even a
  maximum count-seven overflow ends near `gSession + 0x89`, far short of
  `gScreen` at `gSession + 0x3F0`.
- `Radar_OnMapObj` trusts the downloaded map-object count and would walk past
  its 32-entry array, through several later globals and eventually toward
  `gScreen`. Entry 116's `x1` halfword lands exactly on `gScreen`, so the GBA
  would need a count of at least 117. The GameCube producer scans exactly 32
  map-object records and increments the count at most once per record, so its
  count is capped at 32. Small-message reassembly also checks the CRC and
  advertised length. No stale menu-buffer path controls this header.
- The five `gWindows` records end only eight bytes before `gSavedScreen`.
  Several window helpers trust a row count, but normal screens use indices 0-4
  and at most eleven item records. A contrived letter with 54 reply lines would
  make `LetterAnswer_Init` write item 54 from window 1: its `enabled = 1`
  halfword lands on `gSavedScreen`, while its text pointer simultaneously
  destroys `gSubMode`. The live route letter has two reply lines, names cannot
  insert newlines, and no OOB letter producing the required answer block is
  known. Even if reached, this writes screen 1 rather than a chosen full-width
  value.

These remain useful fault surfaces if a future GameCube-memory corruption can
alter their producers, but neither is currently reachable from the name/list
reuse setup.

### Static execution sweep: no one-shot send gadget

The function call has much more fixed state than the visual corruption first
suggested. At an out-of-range call, `r1` is the signed screen, `r2` is
`gSmithScreens + phase * 4`, `r4` is zero, and the final address addition
leaves `N=Z=C=V=0` for the useful positive screens. In the normal connected
loop the preserved registers also include `r5 = 1`, `r6 = 0`,
`r8 = &sPrevMenuHasInput`, `r9 = &gSession`, and
`r10 = &gSession.outsideMiasma`. If the frame drew the status icon, `r12`
points at the OAM shadow buffer.

All fixed-string entries selectable by a signed menu byte were stepped with
that state for JP, EN, and PAL. The useful negative result is:

- no entry returns to `SmithMode_Update`;
- no reachable string span contains an ARM `bx` instruction, so it cannot
  enter the client's normal Thumb functions;
- no path writes `sTxQueue`, `sTxCount`, `REG_JOY_TRANS`, or `REG_JOYSTAT`;
- no path calls `Link_SendLetterReply` or builds its three packets before the
  terminal undefined instruction/open-bus branch;
- EN/PAL have paths that clear `gSession.appearance` and transfer scratch
  before failing, but none writes the inventory, letter selection, screen, or
  outbound queue;
- JP's useful stores remain confined to the smith table, other fixed EWRAM
  pointer/text data, and VRAM.

That last point was checked with the deliberately permissive success
criterion: the GBA is allowed to crash after transmitting, since it can simply
be rebooted. No string path performs the transmission first.

JP screen 17, exit phase, contains the closest thing to a control-flow gadget.
Its first string word is:

```text
ldm r2, {r2, r3, r7, r11, pc}
```

This loads `pc` with the odd Thumb pointer for `SmithEquipScreen_Init` from the
real smith table. It still does not call that function. The GBA CPU is an
ARM7TDMI (ARMv4T), where loading `pc` with `ldm` stays in ARM state and ignores
the low address bits; interworking for `ldm`/`ldr pc` starts with ARMv5T. The
processor consequently decodes the Thumb function body as ARM data and reaches
an executing coprocessor/undefined encoding without sending anything.

### Negative screens alias whole dispatch tables

`Menu_OnOpen` reads its argument through signed `JoyArgs.arg`, so a directly
received menu byte can set `gScreen` to `-128..127`. The four screen tables are
contiguous in every region. Relative to `gSmithScreens`, the useful negative
indices are therefore:

| Smith screen | Table reached | Notable effect |
|---|---|---|
| -25..-17 | `gFieldScreens[0..8]` | Calls the normal radar through family-menu functions |
| **-16** | `gFieldScreens[9]` | **Calls `LetterScreen_Update` in all three smith phases** |
| -15..-11 | Remaining field entries | Menu/message functions or null pointers |
| -10..-5 | `gCMakeScreens` | Character-creation functions |
| -4..-1 | `gShopScreens` | Shop functions and `Screen_Idle` |

Screen -16 is structurally better than JP screen 11: it runs the complete
letter state machine, including its gift-to-send transition, rather than
calling `LetterGift_Main` under the smith state machine. A raw open-menu event
with argument byte `0xF0` would select it.

There is no retail source for that event yet. `GbaQueue::OpenMenu` casts an
ordinary menu ID to a signed byte, but the retail CFlat xref found only menu 9
and the special value 999. Value 999 is intercepted on the GameCube and only
sets controller mode, so it is never transmitted as screen -25. Pause/unpause
send 11/12, and the existing saved-screen race only preserves ordinary
non-negative field screens. Thus -16 is a good packet-injection or future
screen-write target, not a controller-only setup.

### Screens 8-12: real letter functions, misaligned

The end of the JP string-pointer table overlaps the start of
`gLetterStates`, producing these useful triples:

| Smith screen | Init | Main | Exit |
|---|---|---|---|
| 8 | string 12 | string 13 | `LetterList_Init` |
| 9 | `LetterList_Main` | `LetterList_Exit` | `LetterRead_Init` |
| 10 | `LetterRead_Main` | `LetterRead_Exit` | `LetterGift_Init` |
| **11** | **`LetterGift_Main`** | `LetterGift_Exit` | `LetterSend_Init` |
| 12 | `LetterSend_Main` | `LetterSend_Exit` | `LetterError_Init` |

This alignment is JP-specific. EN/PAL place the 15-pointer
`gNameCharTables` array immediately after `gSmithScreens`. It occupies exactly
five screen triples, so their cleaner alignment is:

| EN/PAL smith screen | Init | Main | Exit |
|---:|---|---|---|
| 9 | `LetterList_Init` | `LetterList_Main` | `LetterList_Exit` |
| 10 | `LetterRead_Init` | `LetterRead_Main` | `LetterRead_Exit` |
| 11 | `LetterGift_Init` | `LetterGift_Main` | `LetterGift_Exit` |
| 12 | `LetterSend_Init` | `LetterSend_Main` | `LetterSend_Exit` |
| 13 | `LetterError_Init` | `LetterError_Main` | `LetterError_Exit` |

This explains the older submenu-glitch observations of a letter list at smith
screen 9 and a specific previously opened letter at smith screen 10. Those are
real letter functions on EN/PAL, not merely text bytes that happened to draw a
similar screen. They still run under `SmithMode_Update`, so completion of an
entire triple resets the screen to smith top instead of advancing from read to
gift or from gift to send as `LetterScreen_Update` would.

Field screen 10 (the normal menu) can be saved before the smith mode race, but
screen 10 only reaches `LetterGift_Init` as its *exit* function; after it
returns, smith mode resets to screen 0. It does not run `LetterGift_Main`.
Screen 11 would skip gift initialization and is the promising entry, but a
normal pause saves the screen that existed *before* screen 11. No reliable way
to preserve 11 is known.

The normal GameCube sender cannot simply issue 11 twice. The joybus thread
caches the pause state and sends exactly one menu 11 on the 0 -> 1 transition,
then one menu 12 on the 1 -> 0 transition. The script-facing `openMenu` call
does accept an unchecked ID, but an xref over all 132 PAL retail CFlat scripts
found only ID 9 (Moogle letters) and 999 (Meteor Parasite's controller-mode
switch). There is no retail script call with ID 11. A deliberately replayed
joybus packet remains a hypothesis, not a demonstrated input setup.

Even reaching smith screen 11 is not sufficient by itself:

1. `LetterGift_Main` needs a negative `sLetterGiftTop + cursor`. Every normal
   window initializer and input handler inspected leaves `gWindows[1].cursor`
   non-negative, while the gift top is bounded to the 64-slot list.
2. `LetterGift_HandleInput` advances `gSubState` after a selection, but
   `SmithMode_Update` advances only from a nonzero function return. Gift main
   returns zero and expects the outer letter state machine to notice the
   changed substate. The smith dispatcher never does, so it remains in gift
   main instead of reaching `LetterSend_Main`.
3. Smith screen 12 does call `LetterSend_Main`, but no normal open-menu or
   saved-screen path can retain screen 12: menu 12 is handled immediately as
   "restore `gSavedScreen`."

The known wrong-craft deletion does not supply a GBA OOB write as a shortcut.
Although the GameCube deletes unchecked slot 166, the GBA item-update handler
uses `idx = cmd->arg & 0x3F`; the echo updates ordinary slot 38.

## Viability

- **Final write proved:** radar ROP plus `kB3D G` sends index -72 with item 1.
  The GC path has no CLES food gate or gift-selection validation.
- **Current route blocker:** the complete corrupt radar map has not yet been
  assembled from an ordered retail smith list. The cross-port pointer redirect
  also needs a controller-timed live confirmation.
- **The older gift-screen route remains blocked:** it still needs (a) a
  repeatable smith-screen-11 entry, (b) a negative gift selection whose
  residue passes `> 0x124`, and (c) a transition into `LetterSend_Main`.
- **If direct screen injection appears:** smith screen -16 runs
  `LetterScreen_Update` and supplies that missing gift-to-send transition, but
  still needs negative selection and menu byte `0xF0`. It is now less direct
  than radar ROP for Big Chungus.
- **Crash-after-send does not rescue the fixed strings:** the full string
  sweep found no write to the transmit queue or Joybus transmit registers
  before failure.
- **Not by itself a credit warp:** `G59` remains out of the negative-letter
  window. The composed receive-length exploit supersedes this limit and can
  write `Game`'s pending script directly.
- **Best next experiment:** solve one ordered smith stream that satisfies the
  corrupt map header, harmless LZ77 prelude, row metadata and radar ROP stack.
  Screen -16 and the older screen-11 pause race remain fallback routes for
  targets other than the proved -72 write.
