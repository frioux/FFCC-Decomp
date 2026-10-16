# What the GameCube Trusts from the GBA

## In short

In multiplayer, the GBA runs the menus and sends the GameCube small 4-byte
commands such as "equip slot X", "drop item Y" or "buy row Z". The GameCube
assumes the GBA only sends sensible values. Nearly every handler uses those
bytes directly as array indices or amounts, without range, ownership or
affordability checks.

The GBA's own menus normally keep the values sensible. The glitches come from
making the GBA work from **stale data**:

- a menu list left over from an earlier download (`LIST_BUF`, `DETAIL_BUF`);
- a GBA inventory or gil that no longer matches the GameCube's;
- a GameCube paused in the middle of an exchange.

Any of these makes the GBA send a value the GameCube would never produce
itself.

## Input table

Packets reach `GbaQueue::ExecutQueue`
([gbaque.cpp](../../src/gbaque.cpp#L738)). The only check common to all of them
is that the player's `CCaravanWork` exists. "u8 slot" means the byte is used
directly as `m_inventoryItems[slot]`. Slots 0–163 are valid, slot 166 is gil's
low half, and see [command-list-oob.md](command-list-oob.md#what-a-given-slot-reaches)
for the rest.

| Command | Handler | Unchecked input | Effect of a bad value |
|---|---|---|---|
| `0x14`/10 Craft | `SetSmithData` | u8 slot; item row; materials; price | Crafts from any value, including gil. Missing materials are free. See [wrong-craft.md](wrong-craft.md). |
| `0x1F` Command List | `ChgCmdLst` | u8 list index (array is `[8]`); s16 slot | OOB slot: on use, reads any ±64 KB value as an item and writes `-1` back. OOB index: chosen s16 written to `+0x204…+0x402`. See [command-list-oob.md](command-list-oob.md). |
| `0x1E` Equip | `ChgEquipPos` | s8 equip index; s8 slot; item category | Equips any slot's contents as weapon, armor and so on. See [equipment.md](equipment.md). |
| `0x17`/1 Use | `FGUseItem` | u8 slot | `useItem(m_inventoryItems[slot])`, then writes `-1` there. |
| `0x17`/2 Drop | `FGPutItem` | u8 slot; item ID | Spawns a pickup of **any** item ID. `putItem` doesn't validate it. |
| `0x17`/3 Discard | `DeleteItemIdx` | u8 slot | Writes `-1` at the slot (slot 165/166 overwrites a gil half). |
| `0x14`/8 Sell | `SetSellData` | u8 slot; whether the slot holds anything | Pays a quarter of `itemTable[m_inventoryItems[slot]].m_price`. An empty slot reads row −1. Repeating it while paused at a shop is **multi-sell** (known). |
| `0x14`/9 Buy | `SetBuyData` | u8 row into `m_shopList[16]`; u8 quantity; affordability | Items are always granted. Gil is clamped to 0, so missing gil isn't charged. |
| `0x1A` Drop gil | `ChgMoneyData` → `FGPutGil` | 32-bit amount vs. balance | Spawns a pickup of the full amount, then clamps gil at 0. This is the GameCube side of the GBA gil desync. |
| `0x14`/0,1 Take attachment | `MoveLetterItem` | u8 letter index (`m_letters[100]`); claimed flag | Grants the attachment again on every request (known as **multi-take**). Index ≥ 100 reads later fields as a "letter" and sets a claimed bit inside them. |
| `0x0C`/2 Open letter | `MakeLetterData` / `FGLetterOpen` | u8 letter index | Out-of-bounds letter "open" (**known**). |
| `0x15` Reply to letter | `ReplyLetter` | signed attachment inventory index | Writes `-1` at any selected index before or after the inventory. Index -72 directly corrupts the player-data pointer used for body radius. See [negative letter attachments](letter-reply-oob.md). |
| `0x14`/6 Birthday | `CMakeBarthday` | 2 bytes | Stored as-is. Character creation only. |
| `0x5C` Character-name continuation | `ChkCMakeName` | packet ordering; unchecked assembly index | If sent before the `0x1C` header, writes two bytes before `m_name`. The second byte replaces the signed character destination slot. See [character creation](character-creation-disconnect.md#malformed-name-continuation-a-gamecube-oob-write). |

The `0x5C` bug also composes across ports. A channel-2 overflow can replace
channel 3's signed packet counter; the following channel-3 continuation is a
three-byte indexed write over a +/-96 KiB window. The first concrete target is
`Joybus.m_fileBaseB`, which changes both where the GC loads a radar map and
what it later downloads to the GBA. See
[JP character-name indexed writes](gba-name-indexed-write.md).

That 96-KiB window is no longer the ceiling. Radar/name execution can call
retail `Link_Write` with an unrestricted word on JP, EN and PAL. Counter
`C02C` makes the indexed write replace receive buffer 3's signed length; an
orphan `0x55` packet then stores three unrestricted bytes at
`payload + length`. This is a general MEM1 write, and it can fill `Game`'s
pending-script block with `startmap` while paused. See
[GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md).

The GBA's own menus usually keep these in range. For example, item operations
use `% 64` ([item.c](../../gba/src/cli/main/item.c#L307)), sell is limited to
the 64-slot list, and buy is limited to the downloaded count. But the GBA's
checks run against **its own copy** of inventory and gil (`gSession`), which
can go out of sync.

## Why stale GBA state is easy to get

1. **A paused GameCube doesn't answer.** `ExecutQueue` doesn't run while
   paused, so requests queue up (up to 64) and run together at unpause. Any
   check the GBA would have made after a reply never happens. Multi-take works
   this way: the GBA only marks an attachment as taken after a reply
   ([letter.c](../../gba/src/cli/main/letter.c#L1005)), so each A press while
   paused queues another grant, and `MoveLetterItem` never checks
   `m_attachmentClaimed`.
2. **Shared buffers.** `LIST_BUF` holds every downloaded list and also the
   Command List candidates. `DETAIL_BUF` holds the letter body, equip
   candidates and command-list item info. A screen reached without its own
   download step reads whatever is left there.
3. **The inventory count can wrap.** `m_inventoryItemCount` is an unsigned
   short. Every delete path decrements it whenever it removes a non-`-1` value,
   including out-of-range "items" such as gil halves. It never checks for 0.
   Once it wraps to 65535, `AddItem`'s `>= 0x40` check fails forever, so you
   can't receive any more items. This is the "bad things at count 0" after
   multi-sell.
4. **Unchecked GBA-side stores.** `Session_OnCmdSlot` stores raw values, and
   `Mode_OnSet` / `Menu_OnOpen` restore screens without checking the mode.

## Character-name continuation underflows the destination slot

Character-name transfer has a cleaner GameCube-side parser bug than the
wrong-screen crashes. `ChkCMakeName` distinguishes a header from a continuation
only through the command byte's top two bits. A header (`0x1C`) sets
`m_packetCount = 1`. A continuation (`0x5C`) instead calculates:

```cpp
writeIndex = m_packetCount * 3 - 2;
m_name[writeIndex++] = byte1;
m_name[writeIndex++] = byte2;
m_name[writeIndex]   = byte3;
```

Immediately after `InitCmakeInfo`, `m_packetCount` is zero. A continuation sent
before its header therefore writes `m_name[-2]`, `m_name[-1]`, and `m_name[0]`.
In the 0x20-byte `GbaCMakeInfo`, `m_name[-1]` is exactly `m_playerSlot`.
Consequently the raw word with bytes:

```text
5C xx SS cc
```

sets the destination slot to `SS`. No CRC is checked yet because the packet
count has only advanced to one. `CMakeEnd` also does not validate the assembled
record, so a following event word `14 05 00 00` marks creation complete.

`SetMakeChara` sign-extends the slot byte and uses it without a range check for
both `m_wm.m_charaModelData[slot]` and `Game.m_caravanWorkArr[slot]`. Values
`0x80`-`0xFF` therefore index before both arrays. This is source-confirmed. A
JoyBus reset now supplies a concrete way to expose the normal name-entry
continuation words without their header: the reset replaces
`REG_JOY_TRANS` but preserves `sTxQueue`. The retail JP GBA image was made to
emit the legal name `A♪`, reset and reconnect in the
[headless harness](gba-joybus-harness.md); the first post-handshake name word
had wire bytes `5C 81 F4 00`, setting the slot byte to `F4` (-12). See
[Character Creation Disconnect](character-creation-disconnect.md#joybus-reset-can-discard-the-header-and-preserve-the-continuations)
for the exact transcript and remaining retail timing gates.

The same parser also supplies an unbounded forward overwrite. On a CRC error,
`ChkCMakeName` sends an error reply but does not reset `m_packetCount`. Keep
`m_name[16]` zero so `strlen` remains bounded, then send more `0x5C` words:
each word advances the count and writes the next three chosen bytes. Starting
from channel 0, this walks through the remaining three creation records and
the tail of `GbaQue` (`m_shopFlags`, single-player/controller/pause state, and
so on), then into the following BSS object. Reaching the end of `GbaQue` takes
only about 52 continuation words after the first failed record. This is a
contiguous forward overwrite, not an arbitrary-address primitive, and
destroying the intervening live state may still crash.

There is a much stronger cross-record variant. Channel 2 continuation 10
overwrites the high byte of channel 3's signed `m_packetCount`, and
continuation 11 overwrites its low byte. Because those two continuations can
come from separately interrupted legal JP names, the bytes can be chosen
independently. A following channel-3 continuation then writes three chosen
name bytes at:

```text
&cmakeInfo[3] + 5 + 3 * (s16)m_packetCount
```

This covers roughly 96 KiB on either side of `GbaQue` on a three-byte grid.
The concrete count `DE20` targets `8030CA23`, one byte before
`Joybus.m_fileBaseB`; two legal-name writes can redirect that map pointer into
the shared letter buffers in the observed JP Tipa heap. See
[JP Character-Name Indexed Writes](gba-name-indexed-write.md) for the exact
names, interruption schedule, pointer value, and GBA radar-parser sink.

Slot `0xFF` (`-1`) is especially concrete. A `CCaravanWork` is 0xC30 bytes and
the real array begins at `Game + 0x13F0`, so the fake object begins at
`Game + 0x7C0`, inside `CGameWork`. Its `LoadInit()` call then clears ranges
which overlap:

- the tail of `m_linkTable`, town name, chalice element and event header;
- all 256 saved `m_eventFlags` bytes;
- the first 200 saved `eventWork` halfwords.

That is a broad zeroing primitive, not a chosen-value write. It does not reach
CFlat's heap-allocated permanent variables such as `G59`, so it is not by
itself the Moogle-hole credit warp. It is nevertheless the first direct route
found from one malformed GBA word to a negative GameCube character-array
index. The final character build performs many additional writes and a script
system call, so survival and the resulting world-map state need retail tests.

## Compatibility overflow produces an out-of-range command index

`Session_OnCompatibility` trusts the relationship count in the GameCube's
variable-length message. Its destination has only four name slots. With a
fifth entry, the fifth 18-byte `strcpy` begins at `gSession.favorites`; byte 9
of that NPC name becomes the signed `gSession.cmdSlotCount`.

This reaches an outbound command, not just damaged local state. The Command
List uses that signed count as its row count, Up wraps its s8 cursor to
`count - 1`, and `Link_SendCmdSlot` transmits the wrapped cursor as a u8. A JP
lab packet with name byte 9 equal to `E8` produced the complete outbound word
`1F E7 FF FF`, then remained valid even though subsequent drawing was unsafe.
The GameCube consequently writes `0xFFFF` at
`CCaravanWork + 0x204 + 2*0xE7 = +0x3D2`.

The stock GC sender is the blocker. `GetCompatibility` is itself willing to
serialize five to seven relationships, but a newly created character has the
two parents and only two selected siblings. Static script xrefs found no
ordinary later writer that fills a fifth relationship word. Also, valid JP
NPC strings only yield command indices 64-147, 168, 184, 214, and 231; they do
not yield index 243, which would write the low half of `m_letterCount`.
See [Command List Out-of-Bounds](command-list-oob.md#a-stronger-index-write-proven-in-the-gba-lab-producer-gated-in-retail)
and the [harness transcript](../../tools/gba_joybus_compat_probe.txt).

## Wrong-menu 9/10: a strided GameCube bit write

The submenu race gives a concrete producer for the previously known
out-of-bounds open-letter request. This is more controlled than the crashing
data-as-code smith screens because the GBA eventually sends an ordinary
`REQ_LETTER` packet and the GameCube handles it normally.

### Screen 10 gives index 255

`LetterList_Init` initializes the signed 16-bit `sLetterSel` to `-1`. If the
player enters Letters but does not select a letter, returns to field screen 10
(the main menu), and performs the smith submenu race, the wrong smith screen
eventually runs `LetterRead_Main`:

- EN/PAL smith screen 10 is the complete Letter Read function triple.
- JP smith screen 10 calls `LetterRead_Main` as its init function because the
  letter table is shifted by the fourteen JP text pointers.

With no letter body downloaded, `LetterRead_Main` retries
`Link_SendRequest(REQ_LETTER, sLetterSel)` every 60 GBA frames. The request
argument is `u8`, so `-1` becomes **255**. The GameCube consequently runs both
`FGLetterOpen(255)` and `MakeLetterData(..., 255)` with no range check.

This source-level chain is complete, although the exact controller sequence
has not yet been confirmed in retail play. A minimal test is: leave and freshly
re-enter the letter list so `DATA_LETTER` is clear, do not open a letter, back
out to the main menu, perform the ordinary ID-10 smith pause race, and leave
the resulting screen alive for at least 60 GBA frames. If an old letter body
is still marked valid, `LetterRead_Main` consumes that stale body instead of
sending index 255.

### What opening OOB letter N writes

Each GameCube `CLetterWork` is 12 bytes and the real array starts at
`CCaravanWork + 0x3EC`. The final operation in `FGLetterOpen(N)` sets its
`opened` bit. On big-endian GameCube memory this ORs `0x80` into the byte at:

```text
CCaravanWork + 0x3EC + 12*N
```

Equivalently, when the target is a halfword or word aligned at that address,
it ORs its high bit with `0x8000` or `0x80000000`. Useful landmarks are:

| Letter index | GameCube target | Result |
|---:|---|---|
| 100 | `m_evtState0` | OR `0x80000000` |
| 101-121 | `m_evtFlags` | sets caravan event flags `39 + 96*k` |
| 122-164 | `m_evtWordArr[0, 6, ... 252]` | OR `0x8000` |
| 165 | `m_baseCmdListSlots` | makes the base count negative |
| 170 | `m_shopListCount` | makes the current shop count negative |
| 171 / 172 | `m_shopList[5]` / `[11]` | turns an ordinary shop item into a negative item ID |
| 173 / 174 | `m_shopArgs[0]` / `[3]` | OR `0x80000000` |
| 175 / 176 | `m_shopRandSeed` / `m_shopData2` | OR `0x80000000` |
| 177 | next caravan's `m_saveSlot` | begins writing into the next character block |
| **192-202** | next caravan's inventory slots **3, 9, ... 63** | **turns the existing item into a persistent negative item** |
| 219 | next caravan's gil | OR `0x80000000` |
| 220 | next caravan's command-list slot 4 | OR `0x8000` into its inventory reference |
| 254 | next caravan's `m_shopState` | OR `0x80000000` |
| 255 | next caravan's `m_equipEffectFlags` | OR `0x80000000` |

For an active save-slot-8 character, index 255 instead reaches
`CMonWork[3] + 0x80`, a monster status timer. None of these fixed index-255
targets is currently a speedrun payoff. The message type used by
`MakeLetterData` is still masked to nine bits, so this path does not turn the
letter text into code execution.

The next-character inventory targets are the best structural result. For
example, index 192 changes item `x` in the following character's inventory
slot 3 into `x | 0x8000` without going through `AddItem`, and the change lives
in saveable caravan data. It could manufacture an already-equipped negative
weapon or an OOB food row if a useful low half can first be placed in one of
the reachable slots. In the current JP Marr's Pass snapshot, using caravan
array slot 0 as the source would change slot 1's item 385 to `-32383`; that row
has an unknown damage type and a junk particle bank, so it is not useful.
Scanning ordinary low item IDs 0-1204 found no food-kind row and no safe
high-power magic row after the high bit is added. The currently active caravan
is array slot 1, whose next slot is empty, so indices 192-202 have no immediate
effect in that live setup.

Those same indices also make `MakeLetterData` a constrained GC-to-GBA packet
compiler. For index `N = 192 + k`, it interprets six consecutive halfwords of
the next character's inventory as:

```text
slot 3 + 6*k: header / message type
slot 4 + 6*k: attachment
slots 5..8 + 6*k: four message substitution variables
```

`FGLetterOpen` sets bit 15 of the header first, but that bit is outside the
nine-bit message-type field. Therefore the low item ID in the header chooses
one of the stock letter templates, and the following item IDs feed its JP
`{name}`, `{item}`, monster, NPC, place, subject, and number substitutions.
For example, a header item 4-7 selects letter type 1, whose body contains a
`{name 0}` tag; the inventory item two slots later is then treated as a
caravan-array index and that character's saved name is copied into the letter
sent to `DETAIL_BUF`.

This is a genuine GC → GBA payload path, but not yet GBA code execution. With
all substitution indices valid, an exhaustive JP `c_system.cfd` expansion
gave a longest rendered line of only 42 bytes. `LetterRead_PrintNextLine`'s
unchecked local buffer is 256 bytes, so stock names and table strings cannot
overflow it. An out-of-range substitution variable makes the GameCube read a
string pointer or fake caravan name out of bounds; that could produce a long
line, but it is layout-dependent and is more likely to crash the GameCube's
own unchecked 0x400-byte render buffer first. No stable retail tuple is known.

The shop targets are less useful than they initially appear. Marr's Pass shop
row 5 is item 307, so index 171 would make it `0x8133` (`-32461`), and even the
subsequent bogus price read lands in mapped memory. But screen 9/10 letter
dispatch is reached under smith mode. Opening the merchant afterward runs
`ShopRequest(0)`, clearing all sixteen shop rows before Buy can use the
corruption.

### Screen 9 could choose any u8 index, but has a timing gate

Smith screen 9 runs letter-list code (a clean triple on EN/PAL and a shifted
`LetterList_Main` entry on JP). A forged positive list count therefore lets
the cursor set `sLetterSel` to any value through 255 and immediately send that
value in `REQ_LETTER`.

There is a clean way to forge the count without name bytes. After a real
letter-list download has set `DATA_LETTER_LIST`, opening the Command menu runs
`CmdListScreen_BuildCandidates`, which writes signed 16-bit inventory slots at
the start of `LIST_BUF`. With candidate slots `a` and `b`, the letter code now
reads the count as approximately `a + (b << 16)`. The high term also moves the
letter-name pointer by `b << 19`; that is a multiple of the GBA EWRAM mirror
period, so it folds back to the same physical buffer instead of immediately
leaving RAM. A Buy download is not required. Importantly, Command-list writes
do not clear the stale `DATA_LETTER_LIST` bit.

The remaining gate is entering smith screen 9 before normal field screen 9
runs `LetterScreen_Setup`. That setup calls `Xfer_ClearLetterData`, clears the
stale-valid bit, and requests a genuine list which overwrites the forged
buffer. The likely experiment is to prepare the forged buffer, select Letters
from field screen 10 while the smith's initial dialogue is waiting, and land
the pause packet after `gScreen` becomes 9 but before its first update. This
needs frame-advance testing; the ordinary submenu race by itself does not
prove that this narrower window exists.

## Reset-stripped letter replies append without a bound or CRC

The three-word letter-reply protocol has a second packet-order flaw. A normal
reply consists of a `0x15` header, a `0x55` middle word and a `0x95` final word.
`Link_JoyReset` preserves the queued words, so two resets can isolate only the
middle word:

1. Reset while the header is presented. The middle and final words survive.
2. Reconnect and let the GameCube consume the middle word.
3. Reset again while the final word is presented. The CRC check never runs.

The retail JP GBA image produces exactly this ordering in
[`tools/gba_joybus_letter_reply_reset_probe.txt`](../../tools/gba_joybus_letter_reply_reset_probe.txt).
The observed middle word was presented after reconnect and the second reset
replaced the pending final word with the idle word.

On the GameCube, `JoyBus::SetRecvBuffer` has a 0x400-byte payload and no length
check. With an empty buffer, an orphaned middle word is treated as a header:
its first two payload bytes become the expected CRC and its third byte becomes
payload byte 0. Every later orphaned middle word appends three more bytes.
`InitialCode` resets the command queues during reconnect but does **not** call
`ClrRecvBuffer`, so the length and payload survive both resets.

The length progression is therefore `1, 4, 7, ...`. After 342 isolated middle
words it is exactly 1024; the 343rd writes beyond the payload. For ports 0-2,
the first overwritten field is the next port's receive-buffer length. For
port 3, it is `Joybus.m_gbaBootImage`, followed by the image size, disk-ID
pointer and the four `OSThread` records. Continued writes are a sequential
forward GameCube `.bss` overwrite.

The normal, non-isolated length hop is much less promising than that sounds.
In the live JP Tipa process, the three possible first corrupt lengths point
near `0x803164xx`, `0x812F64xx`, or `0x813164xx`: respectively zeroed Wind
padding, a character-fur allocation, and character-animation blocks. None is
a story-global or control target. Continuing from port 3 toward the saved
thread contexts also fails as a clean PC pivot: the middle-word cadence
restricts the high byte of `SRR0` to 0 or 1, while isolating enough final words
to make an `0x80xxxxxx` address first destroys the saved GPRs and stack
pointer. These observations do not remove the unchecked write, but they rule
out the obvious short Tipa target and thread-resume routes.

This is a genuine GBA-to-GC memory-corruption chain, but not a speedrun setup
yet:

- a controller-only attempt needs hundreds of successful reply submissions
  and two protocol resets per three appended bytes;
- ordinary reply middle bytes are restricted to answer, `isGil`, and the high
  byte of the attachment value;
- the nearest port-3 target is the GBA boot-image pointer, whose `0x80...`
  address cannot be preserved with those ordinary byte positions;
- the radar/name ROP primitive can call `Link_SendLetterReply`, making this
  overflow an important post-execution bridge, but the retail smith/map
  producer for that code path is not yet assembled.

The hundreds-of-resets conclusion applies only to the original sequential
form. With radar PC control, ASCII name `0F G` calls `Link_Write` directly.
The cross-port name index can first replace `m_length`, so a single orphan
middle targets any address rather than walking forward 1,024 bytes. The
source-level composition and regional packet plan are documented in
[GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md).

The bulk downloader does not offer the same splice. Although `Link_JoyReset`
also preserves its GBA parser state, the GC reconnect path sends
`LINK_CHECK_CRC`, which clears `gXferActive`, before any new bulk body. Every
actual retransmission is also preceded by `LINK_CANCEL`.

## Leads, ranked

1. **Arbitrary MEM1 write and pending-script warp.** The cross-port name
   counter can replace receive buffer 3's signed length. Radar/name execution
   then emits unrestricted orphan `0x55` packets, turning `SetRecvBuffer` into
   a three-byte arbitrary write. While paused, one contiguous stream can set
   `m_newGameFlag=1`, keep `m_nextScriptFlags=0`, and write `startmap\0`.
   EN/PAL therefore have a direct path to the retail debug map; JP can name an
   existing script but lacks `startmap.cft`. The raw sender is harness-proved
   on all three retail clients. The remaining gates are a stable retail radar
   row, repeated/batched sends, and a live two-port replay. See
   [GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md).
2. **Cross-port character-name indexed write.** Channel 2 can overwrite
   channel 3's signed packet counter one byte at a time; channel 3 then writes
   legal-name triples on a 96-KiB, three-byte grid. The concrete `DE20` setup
   redirects `Joybus.m_fileBaseB` into the live letter-buffer arena. A map
   load can then stage a valid radar file there, a GC builder can mutate it,
   and the next download reaches a GBA row decoder with an unchecked
   `u16 line[256]` expansion. Counter math, target addresses and legal name
   bytes are source-complete. The GBA sink and payoff are now proved in the
   harness: radar PC control reaches a stack-pop gadget and executes legal JP
   name bytes in `DETAIL_BUF`. A complete letter reply with value
   `0xFFB80001` makes the GC perform `DeleteItemIdx(-72)` directly. The
   route-compatible name `えkk3D G` preserves the existing wrong-craft prefix
   and constructs that value from a raw `LIST_BUF` word plus one popped delta.
   This bypasses both the ordinary selector and the CLES food-kind gate. It can
   also choose the full signed high half, exposing `DeleteItemIdx` indices
   -32768..32767: on JP slot 0, that covers all event flags/work and nearly the
   entire `Game` object. A sibling route-compatible payload, `えkk7D8G`, now
   proves exact control of both fields in command `0x1F`: all 256 u8 indices
   and all 65,536 halfword values. Its caravan `+0x204..+0x402` window includes
   backup inventory, persistent tribe/gender/look, letter count, and both
   complete first letter records. Forging an unclaimed letter gives the
   single-player letter menu any item ID 1-511, including Ultima weapons,
   Dreamcatcher and Crystal Mail. More directly, the same `えkk7D8G` bytes can
   use gadget `0200570D` and `Smith_EquipResult`'s tail to queue
   `1E 05 9A 00`. Equip index 5 aliases inventory slot 0, so GC stores signed
   item -102 over an occupied weapon slot: the known safe 65,535-power Meteor
   phase-skip row, without crafting it. The remaining gates are the multi-port/reset
   controller schedule and one structured smith layout satisfying the map
   header plus complete ROP row.
3. **Reset-stripped letter-reply middles.** Two resets preserve one `0x55`
   word while removing its header and final CRC word. Repetition gives an
   unchecked sequential overwrite of the GameCube's 0x400-byte receive
   buffer. The transport ordering and static sink are proved; the gates are
   the 343-cycle cost, restricted bytes, and a retail protocol-reset trigger.
4. **Malformed character-name continuation.** A lone `0x5C` continuation
   writes its second payload byte into `m_playerSlot`; `0xFF` makes the final
   character build treat `Game + 0x7C0` as slot -1 and wipe every saved event
   flag plus the first 200 event-work values. The confirmed JoyBus-reset
   ordering can produce legal-name values such as `F4` (-12), but the JP
   keyboard cannot produce `FF`. Sequential header stripping also reaches the
   channel-3 shop flags in 12 continuations and `m_pauseMode` in 23. The
   cross-record counter pivot above is the useful extension of this primitive.
5. **Negative letter attachment.** The reply packet preserves the signed high
   half of `(inventory index << 16) | item`. On slot 0 it can write `-1` to
   late `eventWork`, and index -72 directly creates the huge-radius pointer
   corruption without CLES's food check. Radar/name ROP constructs this packet
   directly; only the older menu-driven form still needs a negative gift
   selector. See [negative letter attachments](letter-reply-oob.md).
6. **Drop an arbitrary item (`0x17`/2).** `putItem` creates a pickup for any
   ID. A drop request with slot 166 would drop "item number = gil" (which is
   easy to control) for anyone to pick up. The GBA sends `idx % 64`, so this
   needs a GBA path that produces a slot ≥ 64. None found yet. Command-list
   refs don't go through it.
7. **Compatibility → command-list index ≥ 8 (`0x1F`).** The GBA half is now
   proven: a fifth relationship name corrupts the signed command count, cursor
   wrap sends the resulting u8 index, and the GC performs the halfword write.
   The missing retail step is making `GetCompatibility` send five entries;
   ordinary creation always makes four. Valid JP NPC names further restrict
   the useful indices, with NPC 19's name selecting 231/name bytes 8-9.
8. **Sell an emptied slot.** If the GBA still shows an item that the GameCube
   has already removed (for example, a drop queued during a pause and then a
   sell), the sale reads `itemTable[-1].m_price`. That's the 0x48 bytes before
   the item table. Its value decides whether this pays out, and it hasn't been
   checked.
9. **Buy beyond your gil.** If the GBA's gil is higher than the GameCube's,
   `SetBuyData` grants every item and only clamps gil to 0. With the gil desync,
   this means free items.
10. **Arbitrary open-letter index through smith screen 9.** This gives the
   exact high-bit write mapped above, including persistent negative items in
   the following save-slot character. The forged count is easy; preserving it
   through entry to field screen 9 is the remaining timing gate.
11. **Letter index ≥ 100 attachment take.** `MoveLetterItem` reads
   `m_evtState`, the event flags and later fields as letters. A non-zero
   "attachment" there is granted (gil up to 51100, or items outside
   1–0x9E) and sets a claimed bit inside event data. The open-letter primitive
   above supplies an index, but a forged entry also has to expose a takeable
   attachment before the GBA will send this second command.
