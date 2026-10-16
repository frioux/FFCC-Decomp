# JP Character-Name Indexed Writes

## In short

The malformed character-name continuation is not limited to a sequential
overflow. One character-creation record can overwrite the signed packet
counter of the following record. A continuation sent on that following GBA
port then uses the corrupted counter as an index:

```text
destination = &cmakeInfo[channel].m_name[packetCount * 3 - 2]
            = &cmakeInfo[channel] + 5 + 3 * signed(packetCount)
```

For JP channel 3 this is a three-byte write on a three-byte grid covering
about 96 KiB before and after `GbaQue`. The original construction below uses
legal JP names and precisely interrupted transfers. Radar/name execution now
removes that byte restriction entirely: the ASCII name `0F G` calls retail
`Link_Write` with an arbitrary word on JP, EN and PAL. In particular, the
indexed write can replace a JoyBus receive-buffer length and promote itself
to an [arbitrary MEM1 write](gba-gc-arbitrary-write.md). It is still not a
controller-proven speedrun setup because the complete retail radar-map
producer remains unsolved.

Its strongest target found so far is `Joybus.m_fileBaseB`, the pointer used as
both the destination for a newly loaded radar map and the source of the next
GC-to-GBA map download. On the live JP Tipa allocation, two indexed writes
redirect it into the shared letter buffers. That gives a route from ordinary
GC list/letter builders to the GBA radar-map parser, whose row decoder has an
unchecked stack-array expansion.

A different alignment removes the radar dependency for one high-value target.
Channel 0 can set channel 1's counter to `BC39`; channel 1 can then write the
positive port-0 receive length `00644B72` using only the legal names `A` and
`AdKr`. After a boss, a 13-gil reply can be queued on a disconnected, powered
GBA and carried to Tipa. Reset-stripping its header and delivering both
continuations inside the Moogle nest writes 13 to `G59` and makes its normal
JP return switch load `meteo_1`, which leads directly to the Meteor Parasite
map. See the
[restricted JP Moogle-warp schedule](gba-gc-arbitrary-write.md#restricted-jp-route-moogle-nest-to-meteor-parasite).

## Why channel 3 matters

The four records are consecutive 0x20-byte `GbaCMakeInfo` objects. An
overflow beginning in channel 0 has to cross all four records before it
reaches the useful flags at the end of `GbaQue`. Beginning later is much
shorter:

| Source record | Continuation reaching `m_shopStatusFlags` | Continuation reaching `m_pauseMode` |
|---|---:|---:|
| Channel 0 | 44 | 55 |
| Channel 1 | 33 | 45 |
| Channel 2 | 23 | 34 |
| Channel 3 | 12 | 23 |

The counts include the continuation that contains the target byte. On
channel 3, continuation 12 starts at the three-byte group containing
`m_shopFlags`, `m_shopStatusFlags`, and `m_sellFlg`. Continuation 23 starts at
`m_prevOutOfShoukiFlags`, `m_outOfShoukiFlags`, and `m_pauseMode`.

This alone reduces the synthetic pause/open-menu-11 idea to about five
header-stripped name submissions. More importantly, channel 2 reaches the
beginning of channel 3 even sooner.

## Overwriting the next channel's packet counter

Retail JP addresses are:

```text
GbaQue                         803102AC
cmakeInfo[2]                   80312F9E
cmakeInfo[3]                   80312FBE
cmakeInfo[3].m_packetCount     80312FC0
```

The first channel-2 continuation writes at `cmakeInfo[2] + 5`. Consequently:

| Channel-2 continuation | Destination | Payload use |
|---:|---|---|
| 10 | `80312FBE..80312FC0` | channel-3 active, result, counter high byte |
| 11 | `80312FC1..80312FC3` | counter low byte, CRC bytes |

The high and low counter bytes need not be adjacent bytes of one legal name.
After continuation 10, discard the remaining GBA transmit words; continuation
11 can be the first continuation of a different name. This materially
weakens the JP-text restriction: each counter byte only needs to be obtainable
at its own name position.

For example, these legal JP raw names set channel 3's counter to `DE20`, or
signed -8672:

```text
AAむ    = 41 41 82 DE    first continuation = 41 82 DE
A␠     = 41 20          first continuation = 20 00 00
```

After nine channel-2 continuations have already been accepted:

1. Deliver only the first continuation of `AAむ`. Its third data byte
   writes `DE` to the high counter byte.
2. Clear the remaining GBA transmit words without rebooting or clearing the
   GC record.
3. Strip the next header and deliver only the first continuation of `A␠`.
   Its first data byte writes `20` to the low counter byte.

The nine-word lead-in can be one full five-continuation name plus four
continuations of another. A JoyBus reset can discard the presented fifth
word, but the real UI's automatic empty-name send on reconnect must be
included in an eventual controller script. The headless harness has proved
header loss and queue preservation, not this complete multi-port schedule.

With the counter set, a channel-3 continuation writes at:

```text
80312FC3 + 3 * (s16)packetCount
```

`DE20` therefore writes at `8030CA23`. After the write, the unchecked counter
increments naturally to `DE21`, whose next destination is `8030CA26`.

## Concrete `m_fileBaseB` redirect

The relevant JP `Joybus` fields are:

```text
Joybus.m_fileBaseA       8030CA20
Joybus.m_fileBaseB       8030CA24
Joybus.m_letterBuffer[0] 8030CA28
```

The live Tipa session had:

```text
m_fileBaseA              816F2440
m_fileBaseB              816F55C0
m_fileBaseB_dup          00000D20
m_letterBuffer[0]        816FA600
m_letterBuffer[1]        816FCE40
m_letterBuffer[2]        816FF680
m_letterBuffer[3]        81701EC0
```

The buffers are 0x2840 bytes apart. The following two legal JP names redirect
`m_fileBaseB` to `m_letterBuffer[0] + 0x41`, while leaving the high byte of the
letter-buffer pointer intact:

| Counter | Destination | Name | First continuation | Result |
|---|---|---|---|---|
| `DE20` | `8030CA23` | `AA｛` (`41 41 81 6F`) | `41 81 6F` | changes file A's low byte to `41`; sets file B high half to `816F` |
| `DE21` | `8030CA26` | `えA｛` (`82 A6 41 81 6F`) | `A6 41 81` | sets file B low half to `A641`; preserves letter-buffer high byte `81` |

The result is:

```text
Joybus.m_fileBaseB = 816FA641
```

Only the first continuation of each target name may arrive. In particular,
the second continuation of `えA｛` would overwrite the remaining three bytes
of `m_letterBuffer[0]`. A GBA CPU reboot clears its transmit queue and is safe
after the wanted continuation has reached the GameCube; the corrupted GC
counter and pointer survive it.

The one-byte `m_fileBaseA` change makes later sprite-object downloads unsafe.
It is not needed for the proposed map test, but it is a real side effect and
must not be omitted from a route script.

## Why the redirected map pointer is useful

`JoyBus::LoadMap` does this without validating that `m_fileBaseB` still points
to its allocation:

```c
m_fileBaseB_dup = len;
memset(m_fileBaseB, 0, 0x5000);
memcpy(m_fileBaseB, File.m_readBuffer, m_fileBaseB_dup);
```

At `816FA641`, that 0x5000-byte staging write ends at `816FF641`, still inside
the two contiguous live letter-buffer allocations and 0x3F bytes before
`letterBuffer[2]`. A map transition can therefore place a valid retail radar
map into the shared letter-buffer arena without immediately smashing an
unrelated heap object.

There are then two useful experiments:

1. **Direct malformed download.** Populate `letterBuffer[0]`, redirect the
   pointer, and reboot/reconnect the GBA without changing maps. The GC sends
   0x0D20 bytes beginning at `letterBuffer[0] + 0x41` as map type 1.
2. **Valid-then-corrupt map.** Redirect first, change map once so `LoadMap`
   stages a valid file into the letter buffers, then run a GC list/letter
   builder that overlaps the staged file. Reboot/reconnect without another
   map change so the modified bytes are downloaded and parsed.

The second experiment is preferable for exploitation because the untouched
parts remain a structurally valid map.

## The GBA-side code-execution sink

The bulk receiver itself bounds its cursor to JP EWRAM
`0202E000..0203FFFF`. The consumers do not validate the downloaded map:

- `Radar_InitMap` trusts `BgHeader::mapOffset` and the LZ77 streams and passes
  derived pointers to the BIOS decompressors.
- It derives `sMapRowLen` and `sMapData` from downloaded offsets with no range
  check.
- `Radar_DrawMap` expands each row into `u16 line[256]`. The literal and RLE
  paths both increment `n` without checking it against 256.

A corrupted row length or RLE count can therefore overwrite the GBA stack.
The headless harness now confirms return-address control on the untouched JP
client. `line` begins 0x250 bytes below saved LR, placing LR at halfword index
296. The three-token row `8128 0001 0203` expands 296 filler halfwords and then
writes `0x02030001` over LR. `Radar_DrawMap` reaches its epilogue and branches
to the EWRAM Thumb sentinel; the harness observes `pc=0x02030002` after 3,141
instructions. See [Headless GBA JoyBus Harness](gba-joybus-harness.md#radar-row-expansion-gives-confirmed-gba-pc-control).

There is one payload constraint: a literal halfword with bit 15 set is instead
decoded as an RLE token. Every literal stack value must therefore be below
`0x8000`; an RLE token can only write `0x1000..0x1003`. JP `DETAIL_BUF` is
`02035800`, however, so its Thumb pointer `02035801` is fully encodable.

Initialization has a second, smaller constraint: `Radar_InitMap` invokes the
BIOS LZ77 decoder on `DOWNLOAD_BUF+0x20`. The retail-client harness confirms
that bytes `10 00 00 00` form a harmless zero-output LZ77 stream and return
cleanly. The smith layout therefore does not have to reproduce the original
compressed tiles; it only needs that exact four-byte prelude in addition to
the header, `MapInfo`, row length and token stream.

The harness now proves a two-stage chain. Return address `02010251` lands in a
retail `GilScreen_Main` epilogue which sets `r0=0`, skips eight stack bytes,
loads `r4-r7`, and pops another PC. The radar row therefore controls four
registers and a second return address. It can put a send function in `r4` and
return to a player name at `DETAIL_BUF`.

The legal JP name `1F2F G` decodes as `mov r1,r6; mov r2,r6; bx r4`. With the
stack set to `r4=Link_SendEvent` and `r6=0x42`, the unmodified client queues
packet `14 00 42 42`. Two stronger variants are also confirmed:

- landing at `02010201` leaves `r0=1`; calling `Link_SendItemOp` queues
  `17 01 A6 A6`, bypassing the normal 0-63 selector and using slot 166;
- the name `qB G` is `neg r1,r6; bx r4`. With `r6=72` and
  `r4=Link_SendCmdSlot`, it queues `1F 01 B8 FF`, installing signed command
  reference -72 for the Big Chungus hitbox target.
- the name `kB3D G` is `neg r3,r5; add r3,r6; bx r4`. The `02010251`
  landing already leaves the low bytes of `r0-r2` suitable for a letter
  reply. With `r5=0x00480001`, `r6=2`, and
  `r4=Link_SendLetterReply`, it queues all three reply words for attachment
  value `0xFFB80001`. The GC consequently executes `DeleteItemIdx(-72)`
  directly, with no CLES food-kind check.
- the route-compatible name `えkk3D G` keeps the exact `82 A6 6B 6B`
  prefix used for slot-166 wrong craft. Entering at `DETAIL_BUF+2` executes
  `ldr r3,[r5,#52]; add r3,r6; bx r4`. A raw Smith-list word in persistent
  `LIST_BUF` plus the popped `r6` delta forms `0xFFB80001`. The harness queues
  the complete reply, so a second payload-named character is not inherently
  required.
- a second route-compatible name, `えkk7D8G`, reaches the unchecked **index**
  side of `Link_SendCmdSlot`. Its `add r7,r6; bx r7` constructs the
  non-literal retail address `0200AD4F`, whose existing code sends
  `Link_SendCmdSlot(low8(r6), *(s16 *)(r3+r4+0xF4))`. The harness queues
  `1F EE 34 12`, proving index 238 and exact value `0x1234`. This controls any
  halfword from caravan `+0x204` through `+0x402`; see
  [the full target map](command-list-oob.md#radar-rop-also-supplies-an-arbitrary-indexed-halfword-write).

The `0xFFB80001` value is only the Big Chungus example. The load-plus-add form
can construct any 32-bit attachment with a nonzero low half. Its high half is
an unrestricted signed `DeleteItemIdx` index, so slot 0 reaches all of
`Game.m_gameWork` and most of the full `Game` object, not merely the old
signed-byte gift-selection window.

See [the harness write-up](gba-joybus-harness.md#radar-pc-control-reaches-a-legal-name-payload-and-sends-to-the-gamecube).

### The indexed write now promotes to arbitrary MEM1 write

The legal-name `m_fileBaseB` redirect was the first useful target, not the
ceiling. All regions place `Joybus.m_recvBuffer[3].m_length` at the same
relative count: channel 3 counter `C02C` starts one byte before it. Two raw
`0x5C` continuations replace the complete length and clear `m_cmdFlags`; an
orphan `0x55` continuation then writes three unrestricted bytes at
`payload + signed_length`. Repeated middles continue linearly.

This composes the existing +/-96 KiB name-index window with the unchecked
receive-buffer index into a general GameCube write. The strongest static sink
found so far is `Game + 0xCB28`: while paused, a contiguous stream can set the
pending-script flag, preserve the no-reset flag, and install `startmap\0`.
See [GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md) for regional
addresses and the exact packet planner.

### Smith records are a real but state-dependent producer

`MakeSmithData` emits one 0x38-byte record for every positive inventory ID at
least 401. Each record contains the scaled 32-bit smith price, six raw recipe
halfwords, four raw smith-result IDs, and each result's raw equip flags, value
and attribute plus a zero. Wrong craft can manufacture the record-selecting
inventory IDs.

[`tools/gba_smith_radar_scan.py`](../../tools/gba_smith_radar_scan.py) models
those exact bytes, maps output IDs back to clean wrong-craft gil values, and
searches one tribe or a mixed-tribe/transferred inventory. Live Tipa scans
have found exact two-record rows which expand to saved LR. In one snapshot,
item records 32665 and 11936 reached `02001105`. In a later snapshot, the
mixed-tribe pair 14990/3703 reached the clean `02010251` stack gadget, while
several Lilty pairs reached `02010201`.

These prove that the structured writer contains the needed radar grammar, but
they are not stable recipes: the records are far outside the 1,205-row item
table and changed with the surrounding heap state. The same scan found tens
of thousands of two-record windows with plausible small `mapOffset`, width
and height values. The unsolved constraint is satisfying the entire structure
in one ordered smith list: map header, `MapInfo`, row-length/data pointers,
overflow tokens, popped registers and second PC.

The redirected pointer's low offset is also selectable across printable ASCII
(`letterBuffer[0] + 0x20..0x7E`), rather than being fixed at `+0x41`. That
provides an additional alignment degree of freedom for the complete solver.
For a saved MEM1 dump, the most relevant scanner mode is:

```powershell
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain letter
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain routeletter
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain cmdwrite --cmd-index 245 --cmd-value 81
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain routeequipwrite --equip-index 5 --equip-value -102
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --headers --zero-lz
```

The first command searches three-record streams for the gadget, register
relationship, and `DETAIL_BUF` return required by `kB3D G`. The second targets
the route-compatible `えkk3D G` form and also checks whether its `LIST_BUF`
load word can be placed in a plausible list layout. The `cmdwrite` form checks
the extra-pop gadget, branch arithmetic, staged base word and exact value
halfword; index 245 is letter 0's attachment and value 81 is Crystal Mail. The
`routeequipwrite` form keeps `えkk7D8G` and searches the smaller chain which
queues `1E 05 9A 00`: equip index 5 aliases inventory slot 0 and stores item
-102. Change the index to `5+n` for occupied inventory slot `n`.
The last command finds map-header windows which contain the harmless zero-output
LZ77 prelude. Without
`--mem1`, the same commands read a live JP Dolphin process.

## Status and speedrun value

Confirmed from source and live read-only inspection:

- the cross-record counter overwrite and signed index formula;
- all JP addresses and continuation counts above;
- the legal Shift-JIS byte recipes;
- the live Tipa pointer values and contiguous buffer spacing;
- the unchecked 0x5000-byte `LoadMap` destination;
- the unchecked GBA radar offsets, decompressors, and stack row expansion;
- deterministic GBA PC control from a three-token synthetic radar row;
- a retail-code stack-pop gadget and execution of legal JP name bytes in
  `DETAIL_BUF`;
- successful outbound event, item-use, signed command-slot, and complete
  letter-reply packets;
- exact unrestricted `Link_Write` output from the legal ASCII name `0F G` on
  the JP, EN and PAL retail clients;
- the source-level composition from counter `C02C` through receive-buffer 3's
  signed length to an arbitrary three-byte MEM1 write;
- a route-compatible arbitrary command index/value packet, exposing an exact
  halfword-write window over backup inventory, character metadata and two
  complete letter records;
- a direct negative-letter attachment carrying signed source index -72 and
  nonzero item 1, which reaches the Big Chungus write without a food gate.

Still to prove experimentally:

- a controller-only schedule that delivers exactly 9, 1, 1 channel-2
  continuations while suppressing reconnect-generated name packets;
- moving the same powered GBA between channels, or using two GBAs, without the
  world-menu state clearing the relevant records;
- both pointer writes in the retail GC process;
- one retail GC smith/map layout satisfying the complete header and ROP row.
- one live two-port replay of the receive-length rewrite and pending-script
  payload.

As a current Any% strategy this is not yet competitive: it requires
multi-port character-creation setup and several narrow resets. As an exploit
research direction it is substantially stronger than the sequential
overflows. The GBA-to-GC payoff is no longer hypothetical: the lab chain emits
the full negative-letter reply that makes the GC delete index -72. Producer
plumbing is now the limiting gate; there is no remaining Big Chungus food-kind
condition.
