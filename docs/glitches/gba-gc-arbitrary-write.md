# GBA-to-GameCube Arbitrary Write

## In short

The character-name indexed write and the unchecked JoyBus receive buffer
compose into a general GameCube memory write:

1. Channel 2 character-name continuations replace channel 3's signed name
   packet counter.
2. Two channel 3 continuations use that counter to replace
   `Joybus.m_recvBuffer[3].m_length` with an arbitrary signed offset.
3. An orphan channel 3 letter continuation (`0x55`) makes
   `JoyBus::SetRecvBuffer` store its other three bytes at that offset.

The result is a controlled three-byte write anywhere in MEM1. Further orphan
continuations write the following three-byte groups. There is no header, CRC,
0x400-byte bound, item-table lookup or 0-63 selector in the final write.

The strongest static target is not an item or a story flag. It is `Game`'s
pending-script block. While the main game is paused, a short sequence can
write:

```text
m_newGameFlag     = 1
m_nextScriptFlags = 0
m_nextScript      = "startmap\0"
```

Unpausing calls `CGame::CheckScriptChange` and loads the named script. EN and
PAL contain `startmap.cft`, whose debug menu can load Meteor Parasite, the
ending maps, or almost any other map script. JP has the same vulnerable GC and
GBA code, but its disc does **not** contain `startmap.cft`; it needs an existing
JP script name instead.

The unrestricted form is currently a **source-complete and
GBA-harness-confirmed exploit chain**, not a speedrun-ready setup. A real
western smith-record stream supplies the complete map header, zero-length LZ
stream, `MapInfo`, row table and `03000001` stack return. A corrected
cleanup-safe name/reply chain can also redirect `Joybus.m_fileBaseB` to that
Smith stream, so the static `LIST_BUF`/`DOWNLOAD_BUF` boundary is no longer
the blocker. The remaining gates are reproducing the multi-port delayed
packet choreography and making the heap-backed Smith record order
deterministic. The complete GC write has not been replayed end to end in
Dolphin.

There is also a substantially narrower **JP-only route which does not need
radar ROP at all**. Legal character names can encode one particular positive
receive length. An ordinary 13-gil letter reply then replaces `G59` with 13.
The normal Moogle return switch sends case 13 to `meteo_1`, whose first two
exits load `meteo_2`, the Meteor Parasite fight.

This restricted route is now reproduced end to end in the integrated
Dolphin/mGBA lab. Retail name-entry packets forge receive slot 0's length,
retail `Link_SendLetterReply` supplies all three reply words, the GameCube
stores 13 in `G59`, and the untouched `mog_0` script loads `meteo_1`. The
remaining speedrun gate is physical timing: the proven replay switches one
powered GBA from port 0 to port 3 for the reply header and back to port 0
before the first continuation. That transaction-boundary cable switch is not
yet reproduced by hand on a console.

EN/PAL's payoff is stronger, but the direct legal-name construction is not a
complete route. An invalid `G59` does load the shipped `startmap`, whose BATTLE
menu enters `METEO 3`. Legal western names can also forge receive slot 3 and
write many higher CFlat globals. The remaining problem is bridging one of
those reachable globals back to `G59`: slot 3's direct `G59` length contains
the unavailable bytes `B4 0A`, while the earlier receive-slot-0 construction
does not survive EN/PAL's active-record cleanup.

There is now a second EN/PAL construction which removes that character-name
cleanup blocker. A wrapped radar row can write the GBA's transmit queue and a
12-halfword Thumb stage in IWRAM. One stage makes queue slot 0 repeat forever;
a sibling stage constructs a high-bit packet which the radar token grammar
cannot store literally. Chaining receive slots 0 -> 1 -> 2 then points slot 2
at `G59+1`. A later ordinary, reset-stripped reply middle sets `G59` to an
invalid value and reaches `startmap`. The retail GBA stages and packet words
are harness-proved. A shorter real smith row now reaches the legal-name ARM
branch, but no real row yet contains either complete wrapped transmit-queue
stage. The pointer chain below can transfer Smith bytes as radar data;
producing the exact wrapped row remains a separate inventory-solver problem.

## Restricted JP route: Moogle nest to Meteor Parasite

This construction uses only legal JP name bytes and the normal letter UI. It
does not require GES item deletion, a wrong-craft table, a malformed radar
file, GBA code execution, or an out-of-range gift cursor.

### Verified retail-code replay

[`jp_mog_0_to_meteo_1_exploit.json`](../../tools/ffcc_lab_trials/jp_mog_0_to_meteo_1_exploit.json)
is the complete replay. Its final traced run made 81,921 GC-to-GBA and 81,922
GBA-to-GC transactions and observed:

```text
port 3 reply:  15 75 CD 00 08
port 0 middle: 55 00 01 00 08
port 0 final:  95 00 00 0D 0A
before reply:  recvBuffer[0].length = 00644B72, G59 = 0
after reply:   receive metadata cleared by the bad CRC, G59 = 13
at the exit:   MapJump.m6 = 50, MapJump.m17 = 1
after A:       mog_0 -> meteo_1, G59 = 9, G60 = 1
```

The reply-producing device is player 1's active GBA. The replay moves that
powered device from port 0 to port 3 immediately before the final A press.
Port 3 consumes the valid reply header. When the GBA presents that header, the
replay moves the same device back to port 0; its middle and final words are
therefore received as continuations through port 0's forged length. Nothing
injects or edits a JoyBus packet.

The lab uses
[`jp_letter_reply_13gil_port0.json`](../../tools/ffcc_lab_fixtures/jp_letter_reply_13gil_port0.json)
as a clearly labelled setup fixture to stand at the ordinary final reply
confirmation. Pressing A still executes retail `Link_SendLetterReply`. It also
uses a setup-only map transition to reach `mog_0`. These fixtures avoid
replaying a boss and menu navigation; they do not write `G59`, the receive
buffer, or the outbound queue. The receive length itself was produced by
legal typed names and cable moves in
[`jp_receive_length_atomic_handoff.json`](../../tools/ffcc_lab_trials/jp_receive_length_atomic_handoff.json).

Leaving the nest has one easy-to-miss human action. `mainMapJump` awards diary
entry 226 and blocks in `waitCubeMes(4)` before calling `MJ_MOGSU`. Walk into
the exit, wait for the new-diary message, and press A. Without that press the
map correctly remains `mog_0` with the exit trigger latched.

### Candidate two-GBA console construction

The original search incorrectly treated JP name entry like the western
single-byte keyboard and walked from `BBF3` to `BC39`. That walk is unnecessary.
The JP character `ぞ` supplies bytes `82 BC`, so the legal name `AAぞ9` has raw
bytes `41 41 82 BC 39`. After its header is removed, its first two
continuations place `BC39` directly in channel 1's packet counter.

The source-complete two-GBA byte schedule is:

1. Let port 0's automatic empty-name transfer finish normally. Its GameCube
   packet count is now 6.
2. On port 0, submit `A`. Use a protocol reset to discard its header, accept
   exactly three of the five zero continuations, then use two more resets to
   discard the remaining two continuations individually. Port 0's count is 9.
3. On port 0, submit `AAぞ9`. Discard its header with one reset, then accept all
   five continuations. The first two write channel 1's count as `BC39`; the
   trailing zeroes only clear temporary character-creation fields.
4. On port 1, submit `A`. Discard its header, accept only its first zero
   continuation, then discard its four remaining continuations individually.
   This writes the high length byte and advances the forged counter to `BC3A`.
5. On port 1, submit `AdKr`. Discard its header, then accept all five
   continuations. The first writes `64 4B 72` to the low three bytes of receive
   slot 0's length. The trailing zeroes clear receive metadata and the first
   payload bytes, which are idle at this point.
6. Finish character creation normally. A later valid name header resets the
   affected creation record but does not restore the unrelated receive length.
7. After a boss, queue the 13-gil letter reply on the powered GBA assigned to
   controller port 0. Carry that queue to the Moogle nest, reset-strip its
   header there, accept the middle and final words, and leave the nest.

This needs ten protocol resets during character creation: three to align port
0, one for `AAぞ9`, five for the one-character port-1 name, and one for
`AdKr`. It uses only the route's existing two GBAs and sends the final reply on
port 0.

The untouched JP GBA client produced the important words in the headless
harness:

```text
AAぞ9:  5C 41 82 BC, 5C 39 00 00, then three zero continuations
AdKr:   5C 64 4B 72, then four zero continuations
```

An optional port-3 variant uses `AAぢB` (`41 41 82 C0 42`) and `Ad?Z` to forge
receive slot 3 in only five creation-time resets, but the final reply must then
come from controller port 3. The two-port slot-0 route is the practical default.

In the observed JP Tipa process:

```text
CFlat.m_permanentVarValues = 8094AEC0
G59                        = 8094AFAC
Joybus.recvBuffer[0].length  = 80306430
Joybus.recvBuffer[0].payload = 80306438
```

`CFlatRuntime::Init` allocates the 0xC00-word value array once; changing map
scripts does not reallocate it. `G59` and `G60` both have the persistent
`0x20` variable flag. Thus the Tipa address remains the Moogle-nest address in
the same boot. The target used below is two bytes before `G59`:

```text
(G59 - 2) - recvBuffer[0].payload = 00644B72
```

Channel 1's name-index grid reaches two bytes before port 0's receive length
at signed packet count -17,351, or `BC39`:

```text
channel 1 indexed-write origin = 80312F83
count BC39 writes              = 8030642E..80306430
count BC3A writes              = 80306431..80306433
```

Both length writes are legal names. A one-character name such as `A` has an
all-zero first continuation, which writes the high length byte. The name
`AdKr` has first continuation `64 4B 72`, which writes the other three bytes.

Channel 0 can install `BC39` in channel 1. After a normally received automatic
empty-name transfer, channel 0's packet count is 6. Discard a new name header
and accept three continuations to advance it to 9. The legal name `AAざ9`
has raw bytes `41 41 82 BC 39`; after discarding its header, its first two
continuations are:

```text
5C 41 82 BC   channel 1 active/result/counter high
5C 39 00 00   channel 1 counter low/CRC
```

The corresponding reset-based experimental schedule is:

1. Put port 0 and port 1 into JP character creation and reach the name screen
   on both. Let the automatic empty-name sends finish normally.
2. On port 0, submit a short name, discard its `0x1C` header with a JoyBus
   protocol reset, accept exactly three `0x5C` words, and discard the other
   two. The GameCube-side port-0 count is now 9.
3. On port 0, submit `AAぞ9`, discard its header, and accept exactly the first
   two continuations above. Channel 1's count is now `BC39`.
4. On port 1, submit `A`, discard its header, and accept only its first
   continuation (`5C 00 00 00`). Discard the other four.
5. On port 1, submit `AdKr`, discard its header, and accept only its first
   continuation (`5C 64 4B 72`). Port 0's receive length is now `00644B72`.
6. Leave character creation without sending a letter reply. Complete a
   dungeon and reach the reply UI during the post-boss mail sequence. Select
   answer 0 and an attachment of exactly 13 gil, but do not make the final
   confirmation yet.
7. Disconnect the link while keeping the GBA powered. Make the final
   confirmation on the disconnected GBA. `Link_SendLetterReply` queues all
   three reply words locally: `0x15`, `0x55`, then `0x95`.
8. Finish the post-boss sequence using a GameCube controller, return to the
   world, enter Tipa, and enter the Moogle nest. Keep the reply-producing GBA
   powered and disconnected throughout. These transitions may replace `G59`;
   entering the nest normally leaves it at 15.
9. Reconnect the still-running GBA inside the nest using a JoyBus protocol
   reset. The reset must discard the presented `0x15` header without rebooting
   the GBA, preserving the queued `0x55` middle and `0x95` final words. Let
   both continuations reach the GameCube, then leave the nest normally.

This does not require an additional saved character. The current route already
creates three: the slot-3 `あ` Tailor first, followed by the slot-1 `い`
Merchant and slot-2 `えkk` Rancher with two GBAs. The four names in steps 2-5
are interrupted submissions, not characters which must be completed. Run the
corruption while creating those latter two characters. After the receive
length is forged, an ordinary accepted name header resets each creation
record's packet count and clears its name field without restoring the
unrelated JoyBus receive length. The two GBAs can then submit their intended
final names and finish creating `い` and `えkk` normally. The one-character
names of the Tailor and Merchant are conveniences rather than byte-critical
parts of the exploit.

For the later reply, select `えkk` as player 1 on port 0; the forged length is
specifically port 0's receive buffer. The Merchant/mule on the other port can
both supply the port-1 `A`/`AdKr` traffic during setup and absorb the unwanted
unique first letter, leaving the `{name}` letter for `えkk` as in the existing
route.

The `{name}` letter is special to wrong crafting because its rendered body
primes the shared list buffer with the runner's name bytes. It is not special
to the final arbitrary write: that stage only needs a letter with at least one
reply choice and the ordinary gil-attachment UI. The live `{name}` route
letter has two reply lines, so the same letter can serve both purposes as long
as it was read but not previously replied to.

For a 13-gil attachment, those two payload triples are:

```text
00 01 00  00 00 0D
```

They begin at `G59-2`. No JP script directly references `G58`. The middle word
changes the low half of `G58` to 1 and writes zero to the high byte of `G59`;
the final `00 00 0D` bytes replace its low three bytes. Together they set
`G59` to exactly 13. Queuing the entire reply until the nest is necessary
because the ordinary post-boss and world/town transitions replace `G59`.

`SetRecvBuffer` performs the final store before checking the final-word CRC.
The forged CRC necessarily fails and clears the receive buffer afterward, but
the CFlat value remains changed. The CRC walk is about 6.6 MiB and remains
within MEM1.

On leaving the Moogle nest, `MJ_MOGSU` case 13 sets `G59=9`, sets `G60=1`,
and loads `meteo_1`. In `meteo_1`, map exits 0 and 1 both load `meteo_2`.
This is therefore a credible near-credit-warp rather than the broken JP
`startmap` default branch.

The memory corruption and its `meteo_1` payoff are no longer theoretical. The
practical gate is converting the lab's transaction-boundary cable handoffs
into a console setup acceptable to speedrun rules. A hardware port switch can
model the proven port-0 -> port-3 -> port-0 handoff. The older two-GBA proposal
instead uses JoyBus protocol resets to discard selected words while preserving
the GBA CPU and transmit queue; the standalone harness proves that queue
behavior, but that complete reset schedule has not been reproduced on console.

### The GameCube has the required lightweight reset

The retail GameCube program already contains the exact operation needed by
the harness. `JoyBus::ThreadMain` state 3 calls SDK `GBAReset`, waits 15 ms,
and starts the normal `InitialCode` handshake. It calls `JoyBus::ResetQueue`,
which clears only the GameCube command queues; it does **not** clear the
receive-buffer length or `GbaCMakeInfo`. The GBA handles the SDK reset with
`Link_JoyReset`, which replaces the presented word but deliberately preserves
`sTxQueue`.

Retail assembly confirms this layout for every region:

```text
ThreadParam array                         Joybus + 5D10
sizeof(ThreadParam)                       003C
ThreadParam.m_state                       +28
port N state byte                         Joybus + 5D38 + 3C*N
```

The state-0 addresses are `802EF7B8` EN, `802F07F8` PAL and `8030C0E8` JP.
The live lab can request this native path and print all four creation records
before and after it:

```powershell
python tools/gba_gc_warp_lab.py --region EN --live --trigger-joy-reset 0
```

This is an intrusive Dolphin experiment, not part of a runner setup. It is
useful because it tests the real GameCube SDK command, retail GBA interrupt
handler and both reconnect state machines together. In particular, it can
answer whether a state-3 reset is short enough that EN/PAL's character-select
loop never observes the port as disconnected.

A live PAL test answers that question for the ordinary name-entry window. With
the GBA sitting on the character-name keyboard, port 0 began with an active
creation record (`result=28`, `packetCount=6`). Forcing worker state 3 made the
GBA reset and reconnect normally, returning the worker to state 6, but the
creation record became entirely zero. Thus a reset while name entry is active
does not preserve the western counter. This did not crash or reboot the GBA;
the loss is the GameCube character-select loop clearing the record while the
port reports disconnected.

There are only two normal entrances to state 3 in the retail worker:

1. A bulk transfer receives a `LINK_NAK` whose reported transfer type does
   not match the type currently being sent.
2. SI error recovery regains a GBA after timing out from a worker state above
   2.

The first is the promising controller-only target. The GBA emits NAKs for a
malformed small-message sequence, an out-of-order party-position sequence, a
CRC failure, or malformed bulk data. A stale or cross-protocol NAK arriving
during a list/map download therefore makes FFCC reset the link itself. No
ordinary submenu sequence producing that overlap is confirmed yet. The
normal request handlers send `LINK_CANCEL` before starting a replacement
letter/shop/smith transfer, so simply requesting a different list does not
leave the old bulk parser dirty. Likewise, the visible wrong-smith pixel
corruption happens after a correctly framed list has been accepted and does
not by itself prove a transport NAK.

The second entrance explains why disconnect/reconnect is close but not enough
for the repeated EN/PAL name setup. While the multiplayer character-select
screen observes a disconnected port, `CalcCharaSelect` calls
`ClrCmakeInfo` every frame. That erases the counter which the next dropped
header was meant to advance. JP omits this clear. Outside character select,
the same recovery path remains useful for carrying the final letter reply,
because it does not clear the forged JoyBus receive length.

State 3 is not invisible to the menu. `GetGBAConnect` reports false for every
worker state below 5, and state 3 explicitly sleeps for 15 ms before the
multi-step handshake in state 2. At 60 Hz this is close to, and after the
handshake likely longer than, one complete display frame. The PAL live test
confirms that the ordinary active-keyboard window clears the record. A usable
EN/PAL path would therefore have to align the reset
with one of `CalcCharaSelect`'s early-return windows (world-menu transition,
pending menu change, or a menu window state other than 3). Those windows are
worth a live frame-step test, but they do not yet provide the fourteen
repeatable port-1 drops required by the legal western counter walk.

Dolphin exposes two different operations which should not be confused:

- changing the SI device in Controller Settings destroys the integrated mGBA
  core and loses its queue;
- the integrated GBA window's right-click **Connected** toggle only sets
  Dolphin's force-disconnect flag, leaving the core and RAM running.

The latter is suitable for reconnect experiments, subject to the GBA
client's own roughly eleven-frame JoyBus watchdog: a long disconnection calls
the full `Link_Reset`, which *does* clear `sTxQueue`. It still does not solve
the repeated EN/PAL character-name drops unless the reconnect/reset completes
without a character-select frame clearing the indexed-write state.

## EN/PAL debug-warp target and the remaining bridge

EN/PAL is the stronger speedrun target. `MJ_MOGSU` has the same cases as JP,
but its default branch can actually load the shipped `startmap.cft`. Any
`G59` outside the valid return IDs therefore reaches the debug map, whose
BATTLE menu can select `METEO 3` or `LAST 5`. JP has no later valid Moogle ID:
13 is the final useful case and loads only `meteo_1`; all other valid cases
load ordinary towns or dungeons.

The running EN lab gave:

```text
CFlat.m_permanentVarValues = 80925A40
G59                        = 80925B2C
recvBuffer[0].payload      = 802E9B08
recvBuffer[3].payload      = 802EA720
```

There are two initially attractive western constructions:

| Receive slot | Channel-1 count | Counter name | G59 length | Result |
|---:|---:|---|---:|---|
| 0 | `BC39` | walk from legal `BBF3` | `0063C022` | Bytes work, but the active destination record is cleared on every reset |
| 3 | `C041` | `AAAÀA` (`41 41 41 C0 41`) | `0063B40A` | Counter is encodable; direct `G59` length needs unavailable `B4 0A` |

The old slot-0 proposal treated the byte schedule as sufficient. It is not.
`BC` is not on the western keyboard, so the proposal first installs `BBF3`
and advances it by 70 continuations. Installing `BBF3` also writes a nonzero
value to the destination record's `m_active`. Every reset used to discard the
next header consequently makes `CalcCharaSelect` call `ClrCmakeInfo` and erase
the installed counter. The PAL live reset that zeroed the record therefore
blocks this construction, rather than merely making its timing difficult.

Those 70 increments can be made without damaging a live control structure.
Fourteen complete groups of five zero name continuations advance `BBF3` to
`BC39`. The writes cover EN `802E9A2C..802E9AFD`. Thus the walk only replaces
the tail of `m_tempVar__4CMes` and `Joybus.m_pathBuf`; it ends two bytes before
receive buffer 0. PAL has the same relative layout.

The following packet schedule remains useful as a byte-layout proof, but is
**not an executable EN/PAL setup** until the active-record clear is bypassed:

1. Let port 0's automatic empty-name transfer finish normally; its GC packet
   count is now 6. Submit `A`, strip the header, accept exactly three of its
   five zero continuations, and discard the last two. The count is now 9.
2. Keep port 1 at name entry with one-character name `A`, then use port 0 to
   send `AAA»ó`. Strip its header and accept all five continuations. The first
   two set port 1's count to `BBF3`; the trailing three safely zero later bytes.
3. On port 1, submit `A` fourteen times. Strip each header but accept all five
   zero continuations. Dismiss the expected name-error response after each
   submission. This advances the counter by 70 to `BC39`.
4. Submit `A` once more. Strip its header, accept only its first zero
   continuation, and discard the remaining four. This writes the high length
   byte and advances the counter to `BC3A`.
5. Submit `AcÀ"` (`41 63 C0 22`). Strip its header and accept all five
   continuations. The first installs `63 C0 22`; the trailing zeros clear only
   receive metadata and early payload bytes, leaving the forged length
   `0063C022` intact.
6. Carry a reset-stripped 14-gil reply on port 0 to the Moogle nest. Its
   payload is `00 01 00 00 00 0E`, which sets `G59=14`.
7. Leave the nest. The default return branch loads `startmap`; choose
   `METEO 3` from its BATTLE submenu.

`METEO 3` is the safer first live target than `LAST 5`. The start-map handler
sets `G62=G61=63`, clears `G59`, sets `G60=1`, and loads `meteo_3`. That script
sets `eventWork[6]=14`, loads the Meteor Parasite arena, and the native boss
initializer recognizes the `meteo_3` script and starts with `m_meteo3=1` and
`m_index=3`. `LAST 5` exists in the menu, but its ending-state assumptions
still need a live test.

The retail EN and PAL GBA clients emit the same relevant packets. The
headless probe records:

```text
counter C041:  C0 41 41 5C, 00 00 41 5C
walk start BBF3: BB 41 41 5C, 00 00 F3 5C
zero-name group: 00 00 00 5C (five times)
length low:    22 C0 63 5C
14-gil reply:  00 01 00 55, 0E 00 00 95
```

Run the address/alphabet analyzer against a live Dolphin or a supplied CFlat
pointer:

```powershell
python tools/gba_gc_warp_lab.py --region EN --live
python tools/gba_gc_warp_lab.py --region PAL --cflat-values 0xADDRESS
python tools/gba_gc_warp_lab.py --region EN --live --scan-globals
```

The global scan is useful for discovering other legal-name targets. In the EN
snapshot, receive slot 3 first becomes completely name-encodable at `G513`;
there are 980 encodable globals in `G0..G3071`. That does not by itself copy a
value into `G59`, but it exposes a much larger set of progression and object
state candidates than the original G59-only calculation.

The exact retail-client packet probe is:

```powershell
tools/build_gba_joybus_harness.ps1
build/tools/gba_joybus_harness.exe orig/GCCE01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_western_warp_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCP01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_western_warp_probe.txt
```

All bytes in that proposed slot-0 schedule come from normal EN/PAL senders,
but its state lifetime is wrong. The larger radar ROP chain therefore remains
the only source-complete western route to a direct `G59` or `startmap` write.

### A surviving slot-3 counter without an active-record race

There is a cleaner legal-name primitive which survives EN/PAL cleanup.  An
earlier version of this section incorrectly treated the counter installed in
channel 3 as though it used channel 1's indexed-write origin.  In particular,
channel-3 counter `C042` does **not** address receive slot 3 and cannot write
`G1460`.  The correct channel-3 alignment is `C02D`, which addresses bytes 2
and 3 of receive slot 3's length.

Port 0's forward overwrite constructs channel 3's counter while leaving
channel 3's `m_active` byte zero.

`GbaCMakeInfo` records are 0x20 bytes apart. For port 0, continuation count 30
starts one byte before port 3's record; a zero continuation therefore clears
the byte before the record plus port 3's `m_active` and `m_resultCode` without
touching its counter. Count 31 starts at port 3's counter. A first continuation
of `AÀ-` is `C0 2D 00`, so it installs counter `C02D` while preserving
`m_active=0`.

Port 0 can reach the required count without an untypeable byte:

1. Reset port 0 once while its GBA is idle on name entry. EN/PAL clears its
   active creation record.
2. Submit a name which duplicates an existing saved character. The duplicate
   rejection leaves the now-inactive record at packet count 6.
3. Submit five short names with each header discarded and all five zero-padded
   continuations accepted. The counts advance 6, 11, 16, 21, 26, then 31. The
   last continuation at count 30 clears port 3's active/result bytes.
4. Submit `AÀ-`, discard its header, and accept all continuations. Its first
   continuation installs `C02D`; the later zero continuations touch only later
   fields in port 3's record.

Because port 3 remains inactive, its counter survives a later protocol reset.
`C02D` writes only the low 16 bits of that receive length.  This is enough to
turn an ordinary letter reply into a second-stage counter loader:

1. Ensure receive slot 3 is clear, so its length high half is zero.
2. On channel 3, submit raw name `A¿,` (`41 BF 2C`), discard its header and
   allow only its first continuation.  It stores length `0000BF2C`.
3. Much later, queue a **56,886-gil** reply on channel 3.  Suppress its `0x15`
   header and deliver both continuation words.  The six payload bytes begin
   four bytes before channel 1's CMake counter and end in `DE 36`, while also
   leaving channel 1 inactive:

   ```text
   00 01 00 00 DE 36
   |  preceding  |count|
   ```

The relative addresses are identical in all three regions:

```text
recv3 payload -> channel-1 counter write start = 0000BF2C
channel-1 count DE36 destination              = Joybus.m_fileBaseB + 1
```

Thus one further channel-1 name continuation replaces the low three bytes of
`m_fileBaseB` while preserving its normal `0x81` high byte.  This is the
missing western Smith-to-Radar bridge.  Point it at the exact map header
embedded in the already-built Smith buffer, then ask the GBA for its radar
map.  `SendDataFile` labels the transfer type 1 and reads from the redirected
pointer, so the bytes arrive at `DOWNLOAD_BUF`, not `LIST_BUF`.

For the observed Tipa allocation, channel 1's buffer is `816FCE40`.  A
20-record Smith list has a 24-byte header; the complete later-row radar stream
begins at record 0 byte 31, hence at `letterBuffer[1] + 0x37 = 816FCE77`.
Raw western name `AoÎw` (`41 6F CE 77`) emits `6F CE 77` in its first
continuation and redirects `m_fileBaseB` to that exact byte.  The existing
20-record layout then supplies records 0, 1, 5 and 17-19 to the proven radar
overflow.

The heap address must be checked in the actual run; `AoÎw` is correct for
the observed allocation, not an address-independent magic name.  Usually the
record count and choice of one of the four 0x2840-spaced letter buffers provide
enough low-byte freedom to find another keyboard-legal pointer.  Do the
redirect only after the current map has loaded: a later `JoyBus::LoadMap`
zeros and refills 0x5000 bytes through the corrupted pointer.

This closes the static GC `LIST_BUF`/GBA `DOWNLOAD_BUF` mismatch.  The
remaining runner work is physical choreography--preserving the prepared
channel-1 continuation until the 56,886-gil reply installs `DE36`--and making
the documented Smith inventory order deterministic on EN/PAL.  Direct
slot-3 access to `G59` remains impossible; the route instead obtains GBA code
execution, uses the already-proven packet sender, and writes the western
`startmap` request.

## A real western smith stream reaches the name/ARM stage

The radar sink no longer depends on a fabricated row. In the saved PAL Tipa
snapshot (`itemTable=80955BC0`), a normal smith list can contain a complete
hardware-aligned radar file beginning at record 0 byte 31:

| Record | Item ID | Purpose |
|---:|---:|---|
| 0 | 4000 | map header and zero-output LZ77 prelude |
| 1 | 14624 | continuation of the header/prelude |
| 5 | 405 | `MapInfo`, row lengths and data offset |
| 10 | 10284 | first part of overflowing row 5 |
| 11 | 17225 | middle of row 5 |
| 12 | 416 | end of row 5 and saved PC |

Other records may use a harmless clean filler such as item 407. The exact
41-token row expands to 298 halfwords and replaces the saved PC with
`03000001`. On the untouched PAL client it falls through the live `LinkWork`
scalars to the transmit queue. A legal seven-byte name provides:

```text
5C 43 78 47   Thumb: arithmetic; BX PC
5C E1 C0 7A   ARM: BVC 0203860C
```

`0203860C` is PAL `DOWNLOAD_BUF+60C`. EN has the same construction with name
byte `D1`, targeting `0203460C`. The harness has executed the real
`Radar_InitMap`/`Radar_DrawMap` path, reached the queued name, switched from
Thumb to ARM and arrived at both regional targets.

A later-row layout leaves the earlier inventory records free. With the same
records 0, 1 and 5, records 17-19 equal to items 1369, 13568 and 416 make row
21 expand to the same `03000001` return. Using base position `(81,752)` selects
that row and produces a particularly clean ARM-entry state (`Z=1`, valid
stack, `r4=r5=0`, `r6=00010000`). This is useful for searching a second-stage
gadget without sacrificing most of the inventory payload.

All three adjacent PAL name bytes are present in the shipped western name-entry
table, and the untouched client reaches each predicted page with the same ARM
register state:

| PAL name byte | ARM target | Smith source byte | First inventory record |
|---:|---:|---:|---:|
| `E0` | `0203820C` | record 9 byte 51 | record 9 is fixed filler; record 10 is the first free record |
| `E1` | `0203860C` | record 28 byte 11 | record 28 |
| `E2` | `02038A0C` | record 46 byte 27 | record 46 |

The source-byte mapping includes the 31-byte Smith prefix skipped by the map
pointer. Changing the legal name byte by one moves the ARM target by `0x400`,
but shifts the inventory-relative landing by 16 bytes because a Smith record is
56 bytes. This gives three genuinely different instruction grammars. `E2` is
the largest unconstrained suffix: records 46-63 can all be chosen without
changing the row-21 map producer.

The fixed map layout is also cheaper than its out-of-range IDs initially make
it look. In the captured PAL Tipa table, every required record has a
zero-material wrong-craft source. The important examples are item 4000 from a
Clavat at 65,131 gil, item 14624 from a Lilty at 30,590, item 405 from a Lilty
at 64,954, filler 407 from a Lilty at 5,415, item 1369 from a Lilty at 15,230,
item 13568 from a Yuke at 16,936 and item 416 from a Lilty at 5,425. These are
snapshot-derived recipes and must be rechecked against the route's live item
table, but they require no repeated material deletion.

The ARM suffix search now uses complete records. An earlier first-PC scan
truncated a row exactly at its overwritten return address and appeared to
reach `Link_Write(0000000E)`. With the record's real suffix present, the suffix
normally replaces `sTxCount` with `10031003`; the apparent sender disappears.
`0000000E` was the ordinary `Link_SendScreenId(14)` argument, not arbitrary
packet construction. One full-row sender does exist with items 4437 and 5659,
but its cheapest known sources require thousands of material deletions, so it
is not a speedrun setup.

The zero-material PAL search has so far ruled out a direct sender through eight
records at the `E2` landing and through the seven free pre-row records at the
`E0` landing. It can make controlled EWRAM constants and populate all three
DMA0 words. Item 1276 contributes the useful unconditional instruction
`STM r0,{r11,r12,pc}`; with `r0=040000B0`, it installs source, destination and
control in one operation. The best current chain still has the wrong values
(`DAD=03007EFF`, no enable bit) and never touches `REG_JOY_TRANS`. Preparing
`r11=&packet`, `r12=04000154` and a later enabled control write is now the
concrete DMA subproblem. Two records that look like ARM-to-Thumb
`BLX` candidates land on invalidly aligned Thumb halfwords and fault. These are
search results, not proof that longer record chains are impossible.

The old `probe-radar-bg-affine` experiment is not evidence of a BIOS-call
payload. No payload bytes were installed at `DOWNLOAD_BUF+60C`; the client
merely executed cleared zero words until the harness considered the probe
returned. It must not be cited as a self-written-code success.

`MakeSmithData` normally sends these bytes to western `LIST_BUF` (`02038800`
EN, `0203A800` PAL), while `Radar_InitMap` uses `DOWNLOAD_BUF`.  The
`C02D -> BF2C -> DE36` pointer chain above is the static western bridge across
that boundary.  What remains unproved is its full controller choreography and
a deterministic EN/PAL inventory producer for the exact record order.

Legal names can also branch directly into the intact smith list. EN byte
`E2` reaches `LIST_BUF+20C`; PAL byte `EA` does the same. The list header size
changes the record-relative landing:

| Total records | Landing within a record |
|---:|---:|
| 20-23 | byte 52 |
| 24-27 | byte 48 |
| 28-31 | byte 44 |
| 32-35 | byte 40 |
| 36-39 | byte 36 |
| 40-43 | byte 32 |
| 44-47 | byte 28 |
| 48-51 | byte 24 |
| 52-55 | byte 20 |
| 56-59 | byte 16 |
| 60-63 | byte 12 |
| 64 | byte 8 |

This is real ARM data-as-code control, but not yet a sender. Sweeping every
clean record and alignment found no direct `Link_Write`/queue write. Two
records do contain the valid ARMv4 instruction
`SUBEQ pc,sp,r1,LSR #31` (item 2579 at byte 8 and item 2308 at byte 12), but
the tested three-halfword post-PC tail only runs into the ordinary caller
stack and crashes without sending.

One hardware-sensitive candidate is explicitly ruled out. Item 20838 can
execute `LDREQ pc,[r2],#FFF` while `r2=040000B0`, the DMA3 source register.
The CPU sees open-bus value `6E756E78`, not the debugger-visible DMA address,
so it branches to garbage.

## Wrapped IWRAM can become a repeating packet source

The radar overflow is not limited to its saved return address. On the JP
retail client used by the harness, `Radar_DrawMap`'s unchecked row begins at
`03007CAC`. Its important output halfwords are:

| Output line | Mirrored IWRAM target | Use |
|---:|---:|---|
| 296-297 | saved return PC | Return to the self-written stage |
| 426 | `03000000` | First Thumb instruction after IWRAM wraps |
| 438 | `sLinkWork.connected` | Keep JoyBus in its connected path |
| 454-581 | `sTxQueue[0..63]` | Outbound GameCube packet words |
| 582-583 | `sTxCount` | Queue count |

This corrects the earlier provisional `03007CA0` base: the real row is six
halfwords later. Saved-PC line 296 was already correct, but the old
self-write/queue line numbers were not.

Every literal radar output halfword must be below `0x8000`; a high-bit input
token is decoded as an RLE command. The following complete stage obeys that
restriction:

```text
03000000  ldr  r0, [pc, #12]   ; &sTxCount
03000002  movs r1, #0
03000004  mvns r1, r1          ; r1 = -1
03000006  str  r1, [r0]
03000008  ldr  r2, [pc, #8]    ; 03000001
0300000A  bx   r2               ; spin with IRQs enabled
0300000C  0000 0000
03000010  03000138              ; JP sTxCount
03000014  03000001              ; stage | Thumb bit
```

The row initially leaves `sTxCount=0`, installs queue word `00040055`, and
returns to `03000001`. The stage changes the signed count to -1. The retail
`Link_TxPop` routine then behaves unusually:

```c
if (sTxCount != 0) {
    REG_JOY_TRANS = sTxQueue[0];
    for (i = 1; i < sTxCount; i++)
        sTxQueue[i - 1] = sTxQueue[i];
    sTxCount--;
}
```

For a negative count, the shift loop never runs. Every GameCube poll presents
the same queue word again and decrements the still-negative count. The GBA's
main thread remains in the two-instruction loop, so its normal watchdog never
clears the queue. The harness observes the same packet on consecutive calls:

```text
probe-radar-queue-repeat armed ... packet=0x00040055 count=0xFFFFFFFF
pop=1 presented=0x00040055 ... count=0xFFFFFFFE
pop=2 presented=0x00040055 ... count=0xFFFFFFFD
```

### Receive-slot chain to EN/PAL `G59`

`00040055` is bytes `55 00 04 00`: an orphan letter-reply middle whose three
payload bytes are `00 04 00`. Starting from a clear receive buffer, word 342
leaves length `0x400`. Words 343 and 344 cross the end of port 0's payload and
write the next port's big-endian length as `00000400`; they also clear its
command flag. The repeated word does not touch port 2's length until word 687,
so disconnecting port 0 any time after read 344 and before read 687 gives a
large stopping window rather than a one-frame cutoff.

Port 1's new length `0x400` points exactly at port 2's receive length. Two
port-1 middles can now install the offset from port 2's payload to `G59+1`.
For the observed EN process:

```text
port 2 payload = 802EA318
G59 + 1        = 80925B2D
length         = 0063B815

55 00 63 B8    writes length bytes 00 63 B8
55 15 00 00    writes final byte 15 and clears following flags
```

The first word contains the otherwise unrepresentable halfword `B863`. A
second 12-halfword-safe self-written stage constructs `B8630055` in a register,
stores it over queue slot 0, and spins. Queue slot 1 and count 2 are installed
by the row, so the ordinary transmit interrupt emits both words in order. The
harness result is:

```text
probe-radar-queue-patch armed ... packet=0xB8630055 count=2
pop=1 presented=0xB8630055 count=1
pop=2 presented=0x00001555 count=0
```

Port 2 can now remain ordinary. After a boss, queue any reply with answer 1
and no attachment, keep that GBA powered, and reset-strip its `0x15` header
when reconnecting inside the Moogle nest. Its middle word is `55 01 00 00`.
Because slot 2's forged length points at `G59+1`, those bytes replace the low
three bytes of `G59` with `01 00 00`. Provided its normal high byte is zero,
`G59` becomes `0x00010000`, misses every valid Moogle return case, and EN/PAL
load `startmap`. The final CRC may fail and clear the receive buffer afterward;
the write to the CFlat global has already happened.

This is a three-port construction. It eliminates the unavailable western
keyboard bytes, repeated character-creation resets, active-record lifetime,
and player-name code payload. It is **not yet a runner setup** because no real
smith inventory order has been found for either exact wrapped row. The rows
are small--23 and 25 radar tokens in the synthetic proof--but their exact
halfwords still have to come from craftable 0x38-byte smith records.

The exact dynamic packet values can be generated with:

```powershell
python tools/gba_gc_write_plan.py --region EN --cflat-pointer 0x80925A40 --queue-pivot
```

Use the live region's `m_permanentVarValues` pointer; the sample address is
from the observed EN Tipa process. Smith-record searches use:

```powershell
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain txrepeat --tx-word 0x00040055
python tools/gba_smith_radar_scan.py --mem1 <dump> --tribe mixed --rop-chain txpatch --tx-word 0xB8630055 --tx-count 2 --tx-next-word 0x00001555
```

The 354 stable disc-authored smith rows contain neither stage; the search must
include the live heap-backed out-of-range rows, just like the earlier radar
ROP work.

## The unrestricted GBA sender

The radar row overflow controls `r4-r7` and a second PC. The four ASCII bytes
`0F G` are also two legal Thumb instructions:

```text
30 46    mov r0, r6
20 47    bx  r4
```

Putting the retail `Link_Write|1` address in `r4` and any 32-bit word in `r6`
therefore presents that exact word to the GameCube. This is stronger than
calling `Link_SendEvent` or `Link_SendLetterReply`: no byte is restricted by a
menu field or packet constructor.

The headless harness presents `0x78563455` unchanged on every retail client:

| Region | `Radar_DrawMap` | Pop/return gadget | `Link_Write|1` | Result |
|---|---:|---:|---:|---|
| JP | `02003F94` | `02010251` | `020014C1` | `0x78563455` |
| EN | `0200197C` | `020150C5` | `02002699` | `0x78563455` |
| PAL | `0200197C` | `02015281` | `02002699` | `0x78563455` |

The probes are:

```powershell
build/tools/gba_joybus_harness.exe orig/GCCJGC/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCE01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_en_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCP01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_pal_probe.txt
```

A crash after `Link_Write` is harmless. Once the GC has read the word, the GBA
may be rebooted and the GC-side parser state remains.

## Turning the name index into a receive-length write

`GbaQueue::ChkCMakeName` handles a continuation as:

```cpp
writeIndex = cmakeInfo[channel].m_packetCount * 3 - 2;
cmakeInfo[channel].m_packetCount++;
cmakeInfo[channel].m_name[writeIndex + 0] = byte1;
cmakeInfo[channel].m_name[writeIndex + 1] = byte2;
cmakeInfo[channel].m_name[writeIndex + 2] = byte3;
```

Channel 2 continuation 10 writes channel 3's counter high byte, and
continuation 11 writes its low byte. Raw `Link_Write` removes the former JP
name-alphabet restriction. Set the counter to `C02C`, signed -16340. On all
three regions the next channel 3 continuation begins exactly one byte before
receive buffer 3's length:

| Region | Channel 3 indexed-write base | Receive 3 length | Receive 3 payload |
|---|---:|---:|---:|
| PAL | `802F76D3` | `802EB758` | `802EB760` |
| EN | `802F6693` | `802EA718` | `802EA720` |
| JP | `80312FC3` | `80307048` | `80307050` |

The relative layout is identical, hence the same `C02C` counter in every
region. Two continuation triples can write the complete big-endian length and
clear `m_cmdFlags`:

```text
count C02C:  [disposable preceding byte, length byte 0, length byte 1]
count C02D:  [length byte 2, length byte 3, 00]
```

The disposable byte is the last payload byte of receive buffer 2. It should
not hold a live message during the setup.

## Why one `0x55` word is an arbitrary write

For a nonzero receive length, `JoyBus::SetRecvBuffer` does this without a
range check:

```cpp
buf->m_payload[buf->m_length++] = byte1;
buf->m_payload[buf->m_length++] = byte2;
buf->m_payload[buf->m_length++] = byte3;
```

To write at address `T`, store this signed 32-bit value in the length:

```text
length = T - &Joybus.m_recvBuffer[3].m_payload[0]
```

Then present wire bytes `55 AA BB CC` on channel 3. `RecvGBA` first appends
the word to its secondary queue, `GBARecvSend` removes that same entry, and
`SetRecvBuffer` writes `AA BB CC` at `T`. A missing `0x15` header does not
underflow the secondary queue. Because the word is a middle continuation, no
CRC is checked and no `ReplyLetter` handler runs.

Additional `0x55` words write `T+3`, `T+6`, and so on. A normal-looking raw
header `15 00 00 00` afterward calls `ClrRecvBuffer` and is a useful cleanup.

## Direct script warp

The relevant `CGame` fields are contiguous:

```text
Game + CB28  int m_newGameFlag
Game + CB2C  u32 m_nextScriptFlags
Game + CB30  char m_nextScript[256]
```

`CGame::SetNextScript` copies the name and sets `m_newGameFlag = 1`.
`CheckScriptChange` clears the flag, loads the named CFlat script, and only
performs a new-game reset when `m_nextScriptFlags` is nonzero.

Static addresses are:

| Region | Pending flag | Reset flags | Script name |
|---|---:|---:|---:|
| PAL | `8022B9E8` | `8022B9EC` | `8022B9F0` |
| EN | `8022A9A8` | `8022A9AC` | `8022A9B0` |
| JP | `802472E8` | `802472EC` | `802472F0` |

For EN `startmap`, point receive buffer 3 at `8022A9A8` and stream these six
payload triples while the main game is paused:

```text
00 00 00   01 00 00   00 00 73   74 61 72   74 6D 61   70 00 00
| m_newGameFlag | | reset flags | |       "startmap\0"        |
```

The second triple makes the flag nonzero before the name is complete. Pausing
is therefore required: JoyBus reception continues, while the main loop cannot
observe the flag until the full block and optional cleanup have arrived.

This is better than the original `G59` plan:

- it uses only static addresses;
- it works from any script rather than requiring a Moogle-hole exit;
- it can name any script present on the disc;
- one forged length streams the flag, reset flags and name together.

For EN/PAL, `startmap`'s BATTLE submenu exposes `METEO 0`, `METEO 2`,
`METEO 3`, `LAST 5`, `LAST 3`, and `LAST 4`. `map_jump_from_startmap` maps
those last three choices directly to `last_5`, `last_3`, and `last_4`.
`last_5` is the post-Meteor ending map. Which choice is accepted as a timed
run completion, and whether a direct `last_5` load starts in a usable state,
still need live testing.

JP can directly name an existing script such as `last_5` or `meteo_2`, but
cannot use `startmap`: the JP `mog_0` script contains the same default
`loadCFlat('startmap')` branch, yet the JP disc omits both `startmap.cft` and
its debug file.

## Exact packet planner

[`tools/gba_gc_write_plan.py`](../../tools/gba_gc_write_plan.py) prints the
raw words, byte order, target addresses and forged length. Examples:

```powershell
python tools/gba_gc_write_plan.py --region EN --script startmap
python tools/gba_gc_write_plan.py --region PAL --script startmap
python tools/gba_gc_write_plan.py --region JP --script last_5
python tools/gba_gc_write_plan.py --region EN --target 0x8021DE91 --data 000008
```

The emitted schedule uses only ports 2 and 3:

1. On port 2, send a name header and nine zero continuations.
2. Send continuation 10 as `5C 01 00 C0`.
3. Send continuation 11 as `5C 2C 00 00`.
4. On port 3, send the two calculated `0x5C` length writes.
5. Send the listed `0x55` data words, then optional cleanup.

Allow `GbaQueue::ExecutQueue` to process each `0x1C`/`0x5C` word before the
next one. Rebooting a GBA after its successful send is safe. A physical setup
can use two GBAs or move one powered/rebooted GBA from channel 2 to channel 3,
provided reconnect does not clear the already-corrupted GC records.

## Other targets

Once the transport is usable, exact item writes are no longer the interesting
ceiling:

- `Game.m_gameWork` permits exact year, event-flag and event-work values;
- the low three bytes of a normal small CFlat global can be replaced while
  preserving its zero high byte;
- pointers, pending map state and script names are addressable;
- Big Chungus can be written directly, although the negative-letter
  `DeleteItemIdx(-72)` route is shorter.

Writing only `m_scriptSysVal0` to a high year is not a complete progression
skip. The normal `changeYear()` script also derives region state—for example,
year 5 sets `eventWork[1]=5` and `[2]=5`, while year 8 or later sets them to 6
and 1. A raw year write bypasses that derivation. Direct script loading is a
cleaner high-value target.

## Remaining gates

- The restricted JP Moogle warp is complete in the integrated retail-code lab:
  `G59=13` and `mog_0 -> meteo_1` are both observed. It still needs a practical
  console equivalent for the transaction-boundary cable handoffs. A GBA CPU
  reboot is not a substitute because it clears the transmit queue.
- A real PAL smith/list sequence builds the complete map header and
  overflowing row, and the `C02D -> BF2C -> DE36` chain redirects the type-1
  map source to it. The chain still needs a controller-level replay, a live
  pointer check for the chosen western allocation, and route-stable wrong-craft
  recipes for the snapshot-dependent record IDs.
- The compact real row reaches three legal-name ARM landings. Complete-record
  searches have not yet turned their inventory bytes into a sender. The `E2`
  landing has the most free suffix and can write the complete DMA0 register
  triple, but no known chain supplies the JoyBus destination plus an enabled
  control word; the tested stack pivots also close without a packet.
- The current `0F G` payload emits one arbitrary word per execution. The
  packet plan therefore needs repeated executions/reboots unless a batching
  payload or callable retail multiword sender is found.
- A live four-GBA/cable-handoff replay produces receive length `00644B72` from
  legal names. The smaller proposed two-GBA reset schedule remains untested on
  console.
- The direct-script block must be streamed while paused and then tested for
  scene/map initialization on each target script.

The underlying receive-buffer write and synthetic transmit stages are
harness-proven, but the retail path which produces their packets is still the
central gate. The work above escapes the 0-63 item check and the food-row
lottery in the model; it does not yet constitute a runner-performable EN/PAL
debug warp.
