# Wrong Equip (GES)

## In short

When you equip something on the GBA, it sends the GameCube "equip slot X uses
inventory slot Y". The GameCube stores that as-is. It doesn't check that the
item is a weapon or armor, or even that the slot is in range. If the GBA picks
from a stale list (GES), any item can go in any equip slot.

What happens next comes from one detail: **the game has a single table where
each row is both an item's data and its attack definition.** When you swing,
the game uses your equipped item's row as the attack. Equip a spell and every
swing uses the spell's row: its attack type, power, particles and timing. That
gives the spell effect on top of the contact hit. Fields that a real weapon row
would fill (reach, lunge, timing) get whatever the non-weapon row has there,
which can be extreme values.

## GameCube side (confirmed)

**Packet** (`0x1E`, built by [link.c `Link_SendEquipSlot`](../../gba/src/cli/main/link.c#L936)):

| Byte | Meaning |
|---|---|
| 0 | `0x1E` |
| 1 | equip index (s8): 0 weapon, 1 armor, 2 tribal, 3 accessory |
| 2 | inventory slot (s8, −1 = none) |
| 3 | 0 |

**Store.** [gbaque.cpp `ChgEquipPosData`](../../src/gbaque.cpp#L612) →
[gobjwork.cpp `ChgEquipPos`](../../src/gobjwork.cpp#L524):

```cpp
m_equipment[idx] = equip;   // no range, ownership or category check
```

The GameCube builds the equip list for the GBA correctly. `GetEquipData`
only offers slots 0–63 holding item IDs ≤ `0x9E`. But it never re-checks the
answer. The list (message 6) goes into the GBA's `DETAIL_BUF`, and the picker
reads candidate slots straight from there
([equip.c](../../gba/src/cli/main/equip.c)). If `DETAIL_BUF` holds something
else, such as a letter body or command-list item info, the candidates are
whatever bytes are there. The letter pause gives control of the GBA menus
while the GameCube isn't refreshing that data. **Hypothesis:** this is the
GES mechanism.

### Equip index 5 is a direct inventory write

The unchecked first byte is stronger than a wrong category. `m_equipment`
starts at caravan offset `+0xAC`, while `m_inventoryItems` starts at `+0xB6`:

```text
Link_SendEquipSlot(5 + inventorySlot, itemByte)
    -> m_inventoryItems[inventorySlot] = sign_extend(itemByte)
```

Indices 5-68 cover all 64 ordinary inventory slots. The item byte covers IDs
0-127 and -128 through -1. Inventory count is unchanged, so overwriting an
occupied slot is the clean use.

The JP radar ROP proves a route-compatible producer. Name `えkk7D8G` retains
the wrong-craft bytes `82 A6 6B 6B`; retail gadget `0200570D` supplies `r0=5`
and the name branches into `Smith_EquipResult`'s send tail. The untouched
client queues `1E 05 9A 00`. GC sign-extends `9A` to item -102 and writes it
over inventory slot 0. If that is the equipped weapon slot, the next swing
uses the already-vetted 65,535-power fire row. Use index `5+n` for another
known occupied slot. Values 60 and 81 similarly inject Dreamcatcher and
Crystal Mail; Firaga +2 (534) is outside the signed-byte payload.

The packet and GC alias are proven, but the full retail route still needs a
real ordered Smith/map stream for the radar overflow. Item -102 also retains
its inventory-sort hazard, so inject it after the last relevant sort.

## One table, two meanings (confirmed)

`Game.unkCFlatData0[2]` is a table of 0x48-byte rows indexed by item ID. The
code reads it through two structs:

| Offset | As item (`SItemFlatRow`) | As attack (`SCharaItemRow`) |
|---|---|---|
| `0x00` | `m_kind` (1 weapon, `0x45` armor, `0x7F` accessory, …) | `m_effect` |
| `0x06` | `m_value` (strength or defense bonus) | `m_basePower` |
| `0x08` | `m_attribute` | `m_staType` (damage branch) |
| `0x0E` | — | `m_actionType` |
| `0x14` | `m_particles[3]` | `m_particleEntries` |
| `0x20`–`0x22` | `m_price` | `m_attackStartFrame` / `m_attackEndFrame` |
| `0x26` | `m_smithMaterials[0]` | `m_speed` |
| `0x2A` | `m_smithMaterials[2]` | `m_distance` |
| `0x2E` | `m_smithMaterialCounts[1]` | `m_power` |

### How the equipped item is used

- **Model.** `changeWeapon` → `LoadWeapon(row.m_model & 0xFFF, row.m_model >> 12)`
  ([partyobj.cpp](../../src/partyobj.cpp#L179)). Any item's model can be
  held.
- **Stats.** `CalcStatus` ([gobjwork.cpp](../../src/gobjwork.cpp#L1753))
  adds `m_value` as strength only for kind 1, as defense plus an effect for
  `0x45`, and as an effect only for `0x7F`. A spell (another kind) in the
  weapon slot adds nothing.
- **Attack.** A normal attack does
  `GetCurrentWeaponItem(…, weaponRef); m_itemId = weaponRef; changeStat(1…)`
  ([partyobj.cpp](../../src/partyobj.cpp#L1504)). From then on, the swing is
  driven by `m_itemId`:
  - **Hit frame.** `putParticleFromItem(m_itemId, …)` spawns the row's
    particles ([charaobj.cpp](../../src/charaobj.cpp#L2968)). For a spell row,
    that's the spell effect, and spell particles can carry their own hit
    collision (`SetParticleWorkCol`).
  - **Contact.** The target calls `onDamage(this, m_itemId, …)`
    ([charaobj.cpp](../../src/charaobj.cpp#L2832)). Damage branches on the
    row's `m_staType` and `m_basePower`. A spell row takes the spell's damage
    or status branch.

  **Hypothesis:** the "double cast" is these two hits combined. The spell's
  particle hits separately from the contact damage, and both use the spell
  row.

### Movement range (open)

Party animation rows are chosen by tribe and gender
([partyobj.cpp](../../src/partyobj.cpp#L351)), not by weapon. So the
per-weapon lunge most likely comes from the attack row. For a non-weapon,
`m_distance`/`m_speed` are actually its **smith material** fields, which are
`0xFFFF` for most non-recipe items. If one of them is read as signed during
the swing, it becomes `-1`. The exact field the swing uses hasn't been found
yet. The candidates are the CFlat swing script and `CGCharaObj` state handling
of `m_speed` / `m_distance` / `m_attackStartFrame`.

## Command-list equip swap

A command-list entry whose item row has kind 1 is also equipped as the pending
weapon ([partyobj.cpp](../../src/partyobj.cpp#L1456)). Combined with
out-of-range command-list slots, any 16-bit value whose row has kind 1 can
become your weapon. See [command-list-oob.md](command-list-oob.md).
