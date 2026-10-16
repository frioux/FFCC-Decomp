# Glitch Investigations

Explanations of known FFCC glitches, traced through the decompiled GameCube
(`src/`) and GBA client (`gba/src/cli/`) sources. Each write-up starts with a
plain-language summary and gets more technical as it goes.

| Write-up | Summary |
|---|---|
| [Research Notes](research-notes.md) | Working state: established facts, open questions, where to resume. |
| [GBA → GameCube Trust](gba-gc-trust.md) | **Start here.** Every command the GBA can send, what the GameCube fails to check, and the ranked leads. |
| [Negative Letter Attachments](letter-reply-oob.md) | Radar ROP sends a full signed-16 inventory index and writes `0xFFFF` across almost all of `Game`, including every saved event flag/work word. Index -72 is direct Big Chungus; route-compatible name `えkk3D G` preserves the existing wrong-craft prefix. The retail smith/map producer remains unsolved. |
| [Command List OOB (CLES)](command-list-oob.md) | Command-list slots store unchecked inventory indices. Radar ROP also proves exact control of the packet's unchecked u8 destination and s16 value, covering caravan `+0x204..+0x402`; forged letters can grant items 1-511 in single player. |
| [CLES Targets](cles-targets.md) | What the CLES eat can reach (all of `Game`, not heap), why "food" is a table lookup that heap contents can change, which fixed targets can never be eaten, and how a target could be made edible. |
| [Wrong Equip (GES)](equipment.md) | Equip indices are unchecked: index `5+n` writes a signed-byte item directly into inventory slot `n`. Route-compatible radar ROP proves item -102 into slot 0. |
| [Wrong Craft](wrong-craft.md) | Pausing at the right moment makes the GBA open the blacksmith's "ready to craft" screen with no recipe selected. That screen then sends the GameCube a byte left behind by a letter, which the GameCube uses as an inventory slot. Slot 166 is your gil, so your gil amount picks what gets crafted. |
| [Wrong Craft: Items](wrong-craft-items.md) | What each gil value crafts, including Marr's Pass JP rows for Crystal Mail and a 65,535-power negative weapon. Where the item table lives, unused items in reach, and what lies past either end. |
| [Glitched Weapons](weapon-swing.md) | How a swung item row deals damage, why junk weapons crash, why Marr's item -102 is safe, and how its 65,535-power hit skips Meteor Parasite's phases. |
| [Particle System](particles.md) | How particle sets are loaded into 32 slots, how particles spawn, update, draw and hit, and which slots change per map. |
| [Character Creation Disconnect](character-creation-disconnect.md) | A GBA disconnect wipes that player's creation record to zeros, which would build a blacksmith in slot 1. PAL clears the record before it can be used; EN apparently doesn't (hypothesis). |
| [JP Character-Name Indexed Writes](gba-name-indexed-write.md) | Channel 2 can replace channel 3's signed name-packet counter, turning the next continuation into a three-byte indexed write. A concrete target redirects GC radar-map staging into the letter buffers and exposes an unchecked GBA stack expansion. |
| [GBA-to-GameCube Arbitrary Write](gba-gc-arbitrary-write.md) | Radar/name execution calls `Link_Write` with an unrestricted word on JP, EN and PAL. The cross-port name index can replace a JoyBus receive length, after which orphan `0x55` packets write anywhere in MEM1. The strongest target stages `startmap` directly in `Game`'s pending-script block. |
| [Headless JoyBus Harness](gba-joybus-harness.md) | Boots the retail GBA client under mGBA with a scriptable fake GameCube parent; includes the signed-screen sweep, name-header reset replay, compatibility-overflow command-index proof, and reset-stripped letter-reply replay. |
| [Guest Transfer](guest-transfer.md) | Guests return only their artifacts, matched by character ID. "Restore" just clears the away flag, so orphaned guests can come back later. Powering off between the two saves during a return may sync artifacts while keeping the guest (hypothesis). |
| [Event Script Glitches](scripts/README.md) | Findings from reading every CFlat event script: a Slot B format prompt where B formats the card, Mt. Kilanda's timed barriers, Meteor Parasite phase skips, a held-key re-pop, letters that can never arrive, road events, Mog and the chalice, and unused debug content. |

Conventions:

- **Confirmed** means the behavior is read directly from decompiled code.
  **Hypothesis** means it fits the code and observed behavior but has not been
  traced end to end.
- Code links point at the decomp. Function names come from symbols. Many GBA
  names and all comments are reconstructions.
