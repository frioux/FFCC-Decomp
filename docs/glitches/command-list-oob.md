# Command List Out-of-Bounds (CLES)

## In short

Each command-list slot stores an **inventory slot number**. The GBA sends the
number and the GameCube stores it without checking it. When you use that
command, the GameCube reads "inventory slot N" even if N is far outside the
inventory. Whatever 16-bit value is there is treated as an item ID:

- If the ID's item-table row is a **weapon**, it gets equipped (equip swap).
- If it's **food**, it gets eaten, and the GameCube then deletes the "item" by
  writing `-1` (`0xFFFF`) to that memory. This is the "eat boss HP and it
  becomes −1" effect.

The slot number is a signed 16-bit value, so it can reach about ±64 KB from the
player's data.

## How a bad slot number gets sent (hypothesis)

The GBA builds the Command List's candidate table (`sCmdCandidates`) directly in
`LIST_BUF`, the same buffer that receives letter, shop, smith and artifact lists
([cmdlist.c](../../gba/src/cli/main/cmdlist.c#L417)). The candidate count is
saved separately, but the values are read back from `LIST_BUF` each time you
assign one:

```c
gSession.cmdSlots[slot] = sCmdCandidates[idx];   // = ((s16 *)LIST_BUF)[idx]
Link_SendCmdSlot(slot, (s16)gSession.cmdSlots[slot]);
```

([cmdlist.c `CmdList_SetSlot`](../../gba/src/cli/main/cmdlist.c#L527))

If a list download or a scouter update (`Scouter_OnInfo`, 0x200 bytes) lands in
`LIST_BUF` while the Command List is open, the candidate rows now contain
arbitrary s16 values from that data. Assigning a row sends the value as-is. The
easiest way to make that happen is to queue list requests while the GameCube is
paused (see the letter pause in [wrong-craft.md](wrong-craft.md#6-letter-pause-and-the-gamecube-request-queue)).
The GameCube serves them at unpause, and the Artifact screen re-requests its
list every 60 frames.

## GameCube side (confirmed)

**Packet** (`0x1F`, built by [link.c `Link_SendCmdSlot`](../../gba/src/cli/main/link.c#L949)):

| Byte | Meaning |
|---|---|
| 0 | `0x1F` |
| 1 | command-list index (u8) |
| 2–3 | inventory slot (s16) |

**Store.** [gobjwork.cpp `ChgCmdLst`](../../src/gobjwork.cpp#L510) does
`m_commandListInventorySlotRef[index] = slot`. Neither value is checked.

**Use.** [gobjwork.cpp `GetCmdListItem`](../../src/gobjwork.cpp#L2065) returns
`m_inventoryItems[ref]` for the selected slot. Then
[partyobj.cpp](../../src/partyobj.cpp#L1432) reads that item's row,
`unkCFlatData0[2] + itemId * 0x48`. Neither the slot nor the item ID is
range-checked. What happens depends on the row's kind field:

| Kind | Effect |
|---|---|
| `1` | Weapon. Queued as the pending weapon (equip swap). |
| `0x17D`, `0x186` | `useItem` (food heal). On success, `DelCmdListAndItem` runs. |
| `0x100`, `0x1F5`, `0xDF`, `0x125` | Cast or ability via `changeStat(2…)`, using the row's `m_fieldA` for some kinds. |

**Write-back.** [gobjwork.cpp `DelCmdListAndItem`](../../src/gobjwork.cpp#L2123):

```cpp
short inventorySlot = m_commandListInventorySlotRef[cmdListIdx];
if (m_inventoryItems[inventorySlot] != -1)
    m_inventoryItems[inventorySlot] = 0xFFFF;   // -1 written at CCaravanWork + 0xB6 + 2*slot
```

### What a given slot reaches

Addresses are relative to the player's `CCaravanWork`, with
`m_inventoryItems` at `+0xB6`:

| Slot | Offset | Field |
|---|---|---|
| 0–163 | `0x0B6`–`0x1FC` | Inventory and artifacts (valid) |
| 165 / 166 | `0x200` / `0x202` | `m_gil` high / low half |
| 167–174 | `0x204` | `m_commandListInventorySlotRef[8]` |
| 183 / 184 | `0x224` / `0x226` | `m_currentCmdListIndex` / `m_weaponIdx` |
| 185–… | `0x228`… | Backup equipment, backup inventory, then letters (`0x3EC`) |
| negative / >~1500 | outside the object | The rest of the static `Game` object and the `.bss` around it (see below) |

The caravan works are not heap objects: they are `Game.m_caravanWorkArr[9]`
(PAL `0x802202B0`, stride `0xC30` = 1,560 slots). The slot is a signed 16-bit
index, so each caravan reaches ±64 KB around its inventory. For caravan 0 that
is `0x80210366`–`0x80230362`: about 60 KB of `.bss` before `Game`, and all of
`Game` up to `+0x114A2`. Higher caravans shift the window up by `0xC30` each,
reaching the start of `Graphic` (`0x80230E48`) from caravan 1 on. Script
globals (`CFlat.m_permanentVarValues`) are on the heap and out of reach.

In reach (PAL, slot numbers for caravan 0; subtract 1,560 per caravan index):

| Target | Address | Slot |
|---|---|---|
| Year (low half) | `Game+0x12` | −2,634 |
| Chalice element (low half) | `Game+0x10BE` | −500 |
| Story flags / story counters | `Game+0x10D4` / `Game+0x11D4` | −489 / −361 |
| Current stage index | `Game+0x13D4` | −105 |
| Other caravans (inventory, gil, HP) | `Game+0x13F0 + 0xC30·k` | 1,560·k − 91 … |
| Monster HP, monster `i` | `Game+0x81BC + 0x110·i` | 13,963 + 68·i |
| Party object pointers | `Game+0xC5B0` | 22,661 … |

Only a halfword that currently holds a food ID (`0x17D`–`0x187`, 381–391)
is eaten, and it always becomes `0xFFFF`. That is why the reach is wide but
the useful targets are few: monster HP that happens to be 381–391 (the known
boss-HP effect), the low half of gil when you set gil to `k·65536 + 381…391`
(it jumps to `k·65536 + 65535`; untested), and food in another caravan's
inventory. Pointers whose low half is `0x0180` or `0x0184` would be corrupted
and most likely crash.

[cles-targets.md](cles-targets.md) goes further: "food" is decided by a table
lookup that heap contents can change, so some fixed targets can be made
edible, and it includes a scanner for live memory.

## A stronger index write: proven in the GBA lab, producer-gated in retail

`ChgCmdLst` doesn't bounds-check the **index** either. That array is
`short[8]`, but the wire index is a byte. A packet with index at least 8 writes
the supplied s16 to `CCaravanWork + 0x204 + 2*index`, covering `+0x204` through
`+0x402`. This includes the backup inventory, transient equipment-effect
flags, most of the saved name, tribe/gender/look, progression, and the first
part of the letter array.

There is now a source-confirmed and emulator-confirmed GBA producer. The
compatibility decoder trusts the GameCube's relationship count, but
`gSession.relationNames` only has four 18-byte entries. A fifth relationship
name begins at `gSession.favorites`; its tenth byte overwrites the signed
`gSession.cmdSlotCount`. The Command List copies that count into
`gWindows[2].rows`. Pressing Up from its initial row 2 wraps the signed cursor
to `cmdSlotCount - 1`, and `CmdList_SetSlot` sends the cursor after converting
it to the protocol's u8 index.

The headless JP client was given a fifth name whose byte 9 was `E8`. It stored
`cmdSlotCount = -24`; the wrap selected row `-25`, and removing that row
presented this word to the GameCube before the damaged redraw could fail:

```text
REG_JOY_TRANS = 1F E7 FF FF
```

That is command `0x1F`, index `0xE7` (231), value `0xFFFF`. On the GameCube it
writes `CCaravanWork + 0x3D2`, bytes 8-9 of the saved player name. The exact
reproduction is in
[`tools/gba_joybus_compat_probe.txt`](../../tools/gba_joybus_compat_probe.txt).
A non-remove candidate can supply a different halfword; ordinary candidates
are inventory-slot numbers, while a recycled `LIST_BUF` can supply the raw
halfwords already used by CLES.

The remaining gate is on the GameCube producer, not this sink. Normal
character creation always installs the two parents and exactly two of the
four siblings, so `GbaQueue::GetCompatibility` sends four relationships. It
can send up to seven and merely prints a warning above four, but no ordinary
script path that leaves a fifth relationship nonzero has been found.

The valid JP NPC-name table also limits what a retail fifth name could select.
Its observed byte-9 values produce indices 64-147, 168, 184, 214, or 231.
Thus the interesting fixed targets are late backup inventory, the high half
of `m_equipEffectFlags`, and name bytes 8-9. It does **not** naturally reach
the low half of `m_letterCount` (index 243) or the tribe/gender fields. An
arbitrary synthetic parent can reach every byte index, but that is a lab
capability, not a speedrun setup.

### Radar ROP supplies a signed command reference directly

The JP radar-map stack overflow provides a second, stronger GBA producer.
Harnessed retail code returns through `02010201`, loads
`r4=Link_SendCmdSlot` and `r6=72`, then executes the legal player-name bytes
`qB G` from `DETAIL_BUF`:

```text
qB    neg r1, r6       // r1 = -72
 G    bx  r4
```

The client queues `1F 01 B8 FF`: command-list index 1, signed inventory
reference -72. This bypasses both the ordinary 0-63 candidate list and the
compatibility overflow's limited u8 index/value choices. It proves the final
GBA-to-GC packet and the exact Big Chungus index on the untouched JP client.

This is not yet a retail route. The radar row and its stack words are still
synthetic; the redirected GameCube map buffer must express the same header and
ROP stream through structured smith records. Once that producer is solved,
the command-reference setup itself is no longer a blocker.

### Radar ROP also supplies an arbitrary indexed halfword write

The fixed `1F 01 B8 FF` packet above understates this command. The wire format
contains an unchecked u8 **destination index** as well as an arbitrary s16
value. `GbaQueue::ChgCmdLstData` passes both directly to:

```cpp
m_commandListInventorySlotRef[index] = value;
```

The destination is therefore `CCaravanWork + 0x204 + 2*index`, for every
index from 0 through 255. A second synthetic retail-client chain now proves
control of both fields while preserving the existing Any% name prefix.

The first return uses the retail epilogue at `02002555`. It skips four stack
bytes, pops `r3`, then pops `r4-r7` and a second PC. The maximum-length JP name
`えkk7D8G`, entered at its first `k`, is:

```text
kk    ldr r3, [r5, #52]
7D    add r7, r6
8G    bx  r7
```

The `えkk` bytes are still exactly `82 A6 6B 6B`. A staged Smith-list word
supplies a convenient EWRAM base through the `ldr`. For command index `EE`,
the demonstrated popped values `r6=000040EE` and `r7=02006C61` add to
`0200AD4F`, the argument-building tail of the retail `CmdList_SetSlot`.
That tail executes:

```text
Link_SendCmdSlot(low8(r6), *(s16 *)(r3 + r4 + 0xF4))
```

Changing the low byte of `r6` selects any index 0-255; adjusting `r7` preserves
the same branch target. `r3`, `r4`, and a raw Smith-list halfword select all
65,536 values. The harness example selects index 238 and value `0x1234`:

```text
probe-radar-cmdwrite-rop queued ... packets=0x1234EE1F count=1
```

Those little-endian bytes are exactly `1F EE 34 12`. This is a confirmed
retail GBA-to-GC packet, not an inferred calling convention. As with the other
radar probes, the remaining route gate is finding one real Smith/map stream
with the required header and expanded stack words.

The complete destination window is:

| Index | `CCaravanWork` target | Consequence |
|---:|---|---|
| 0-7 | `+0x204` command references | Ordinary command-slot edit |
| 8-15 | `+0x214` command grouping | Glitched command grouping |
| 16-17 | current command / weapon indices | Usually normalized by the post-store weapon check |
| 18-21 | backup equipment | Exact saved tutorial equipment |
| 22 | backup inventory count | Exact count |
| 23-186 | all 164 backup item/artifact slots | Any 16-bit item ID, but restoration is producer-gated |
| 188-189 | backup gil high/low halves | Exact 32-bit backup gil in two writes |
| 208-217 | shop state, job, equip flags, JoyBus ID | Exact persistent character/shop metadata |
| 227-235 | all 18 name bytes | Persistent post-creation rename/data bytes |
| 237-240 | progress, tribe, gender, appearance | Persistent character metadata |
| 242-243 | letter count high/low halves | Exact count, including unsafe values |
| 244-249 | letter 0 | Complete forged letter record |
| 250-255 | letter 1 | Complete forged letter record |

The write is followed by `CheckAndResetCurrentWeaponIdx`. For an out-of-range
index this does not undo the target store, but it may reset the current weapon
and command selection to zero. If a negative value is sent to index 0-7 and
that index is current, `ChgCmdLst` also searches for the next command. Those
are side effects to route around, not bounds checks.

#### Most concrete payoff: forge item-bearing letters

Two complete `CLetterWork` records lie inside the window. Their attachment is
a 9-bit value, so a forged unclaimed letter can carry any item ID 1-511, or up
to 51,100 gil. After returning to single-player mode, the ordinary GC letter
menu calls `AddItem` directly when the attachment is taken; it does not have
the GBA letter screen's odd exclusion of item IDs 1-158.

This includes all four Ultima weapons (IDs 15, 31, 47 and 64), Dreamcatcher
(60), Crystal Mail (81), and Force Ring (158). A minimal clean synthetic
record uses a valid message/sender zero, the chosen attachment halfword, zero
temporary variables, and a positive letter count. In practice an existing
valid unclaimed letter may need only its attachment halfword changed.

This is the clearest speedrun-facing use of the new primitive: once the retail
Smith/map producer is solved, Crystal Mail and Dreamcatcher no longer require
their own safe wrong-craft rows, and the same character can receive an Ultima
weapon. It does **not** directly grant Firaga +2 (ID 534), because the letter
attachment field is only nine bits. The known 65,535-power Marr's Pass weapon
also remains a separate wrong craft.

The backup inventory is a wider item-injection surface, but no retail script
in the JP `cft` directory calls `sysControl(19, ...)`, the only native path to
`BackupTutorialItem`. Without such a restore trigger it is storage, not an
inventory grant. Out-of-range tribe/gender/appearance values save, but most
consumers index model and combat tables without validation; valid 0-3 changes
are plausible, while glitched values are crash candidates rather than a
demonstrated movement exploit.
