#!/usr/bin/env python3
"""Search FFCC MakeSmithData records for Radar_DrawMap return-address control.

The GameCube smith-list builder accepts positive inventory IDs through 32767
and copies unusually useful halfwords from their (usually out-of-bounds) item
rows.  This tool models the exact 0x38-byte record sent to the GBA, finds
records that a wrong craft can create for one tribe, and searches record
streams for initializable map headers or a radar row that expands through
saved LR and a retail ROP/name payload.

No emulator memory is modified.  By default the tool reads a running JP
Dolphin for compatibility with the original workflow.  A raw 24-MiB MEM1 dump
and its region's ``--item-table`` address can be supplied for JP, EN or PAL.
The item table is loaded from param.cfd at runtime, so a DOL alone is not a
valid search image.
"""

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path


MEM1_BASE = 0x80000000
MEM1_SIZE = 0x01800000
JP_ITEM_TABLE = 0x80979FC0
ROW_SIZE = 0x48
SAVED_LR_HALFWORD = 296
TRIBES = ("clavat", "lilty", "yuke", "selkie")


class Mem1:
    def __init__(self, data):
        if len(data) != MEM1_SIZE:
            raise ValueError(f"expected {MEM1_SIZE:#x} bytes of MEM1, got {len(data):#x}")
        self.data = data

    def _offset(self, address, size):
        offset = address - MEM1_BASE
        if offset < 0 or offset + size > len(self.data):
            return None
        return offset

    def u16(self, address):
        offset = self._offset(address, 2)
        if offset is None:
            return None
        return struct.unpack_from(">H", self.data, offset)[0]


class ParamMem:
    """Read-only item-table view backed by a retail ``param.cfd``.

    This deliberately exposes only the 1,205 disc-authored rows.  It is useful
    for finding heap-independent smith streams; out-of-range wrong-craft rows
    still require a live MEM1 snapshot.
    """

    def __init__(self, path, item_table):
        data = path.read_bytes()
        # The third DATA chunk in retail param.cfd is the 1,205 x 0x48 item
        # table.  Its payload begins at file offset 0x175F0 in every region.
        start = 0x175F0
        size = 1205 * ROW_SIZE
        if len(data) < start + size or data[start - 0x10:start - 0x0C] != b"DATA":
            raise ValueError(f"{path} does not have the expected retail item DATA chunk")
        self.data = data[start:start + size]
        self.item_table = item_table

    def u16(self, address):
        offset = address - self.item_table
        if offset < 0 or offset + 2 > len(self.data):
            return None
        return struct.unpack_from(">H", self.data, offset)[0]


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def ror32(value, shift):
    shift &= 31
    if shift == 0:
        return value & 0xFFFFFFFF
    return ((value >> shift) | (value << (32 - shift))) & 0xFFFFFFFF


def gba_load16(data, offset):
    """Model ARM7TDMI/GBA unaligned LDRH, returning the full register value."""
    aligned = offset & ~1
    if aligned < 0 or aligned + 2 > len(data):
        return None
    value = struct.unpack_from("<H", data, aligned)[0]
    return ror32(value, (offset & 1) * 8)


def gba_load_s16(data, offset):
    """Model the Thumb LDRSH behavior used for MapInfo x/y."""
    if offset < 0 or offset >= len(data):
        return None
    if offset & 1:
        value = data[offset]
        return value - 0x100 if value & 0x80 else value
    value = gba_load16(data, offset)
    return signed16(value)


def gba_load32(data, offset):
    """Model ARM7TDMI/GBA unaligned LDR word rotation."""
    aligned = offset & ~3
    if aligned < 0 or aligned + 4 > len(data):
        return None
    value = struct.unpack_from("<I", data, aligned)[0]
    return ror32(value, (offset & 3) * 8)


def row_address(item_table, item_id):
    return item_table + signed16(item_id & 0xFFFF) * ROW_SIZE


def row_halfword(mem, item_table, item_id, offset):
    return mem.u16(row_address(item_table, item_id) + offset)


def wrong_craft_sources(mem, item_table, tribe, max_material_deletes=0):
    """Map result item -> bounded-cost gil values for one result column.

    With the default limit of zero this retains the original clean-source
    behavior.  A positive limit also admits rows whose three material loops
    perform at most that many DeleteItemIdx calls in total.  Reads may start
    on either side of the real item table because the base item is loaded
    signed.
    """
    sources = defaultdict(list)
    result_offset = 0x38 + tribe * 2
    for gil in range(0x10000):
        materials = [row_halfword(mem, item_table, gil, 0x26 + i * 2)
                     for i in range(3)]
        counts = [row_halfword(mem, item_table, gil, 0x2C + i * 2)
                  for i in range(3)]
        result = row_halfword(mem, item_table, gil, result_offset)
        price = row_halfword(mem, item_table, gil, 0x24)
        if any(value is None for value in (*materials, *counts, result, price)):
            continue
        deletes = 0
        for material, count in zip(materials, counts):
            if material == 0 or count == 0:
                break
            deletes += count
        if result >= 401 and deletes <= max_material_deletes:
            sources[result].append((gil, price, materials[0], counts[0]))
    return sources


def smith_record(mem, item_table, item_id, rate):
    """Return the 28 GBA-native halfwords made for one smith-list record."""
    smith_price = row_halfword(mem, item_table, item_id, 0x24)
    if smith_price is None:
        return None
    price = int(smith_price * rate / 100.0) & 0xFFFFFFFF
    words = [price & 0xFFFF, price >> 16]

    for offset in range(0x26, 0x32, 2):
        value = row_halfword(mem, item_table, item_id, offset)
        if value is None:
            return None
        words.append(value)

    results = []
    for offset in range(0x38, 0x40, 2):
        value = row_halfword(mem, item_table, item_id, offset)
        if value is None:
            return None
        results.append(value)
    words.extend(results)

    for result in results:
        if result == 0:
            words.extend((0, 0, 0, 0))
            continue
        fields = [row_halfword(mem, item_table, result, offset) for offset in (4, 6, 8)]
        if any(value is None for value in fields):
            return None
        words.extend((*fields, 0))
    assert len(words) == 28
    return words


def pc_hits(words, first_record_words=28):
    """Yield token starts that overwrite saved LR with a GBA EWRAM address."""
    for start in range(len(words) - 2):
        expanded = 0
        cursor = start
        while cursor < len(words) - 2 and expanded < SAVED_LR_HALFWORD:
            token = words[cursor]
            cursor += 1
            expanded += (token & 0x3FF) if token & 0x8000 else 1
        if expanded != SAVED_LR_HALFWORD or cursor + 1 >= len(words):
            continue
        low, high = words[cursor:cursor + 2]
        if low & 0x8000 or high & 0x8000 or not (low & 1):
            continue
        target = low | (high << 16)
        if (0x02000001 <= target <= 0x0203FFFF or
                0x03000001 <= target <= 0x03007FFF):
            yield start, cursor, target, first_record_words


def expand_row(words, start, limit):
    """Expand radar tokens, stopping after limit output halfwords."""
    output = []
    cursor = start
    while cursor < len(words) and len(output) < limit:
        token = words[cursor]
        cursor += 1
        if token & 0x8000:
            count = token & 0x3FF
            output.extend([0x1000 | ((token >> 10) & 3)] * min(count, limit - len(output)))
        else:
            output.append(token)
    return output, cursor


def build_smith_buffer(records, slots=None, shop_args=(0, 0, 0, 0)):
    """Build the byte layout MakeSmithData writes for an ordered recipe list."""
    if slots is None:
        slots = list(range(len(records)))
    count = len(records)
    record_start = (count + 1 + 3) & ~3
    output = bytearray(record_start + count * 0x38 + 16)
    output[0] = count
    output[1:1 + count] = bytes(slots)
    cursor = record_start
    for record in records:
        struct.pack_into("<28H", output, cursor, *record)
        cursor += 0x38
    for value in shop_args:
        struct.pack_into("<I", output, cursor, value)
        cursor += 4
    return output


def inspect_map_overlay(map_data, smith_data, pointer_lo=0x20, pointer_hi=0x7E):
    """Describe pointer alignments that leave map structures in the download."""
    results = []
    for pointer in range(pointer_lo, pointer_hi + 1):
        arena = bytearray(pointer + len(map_data))
        arena[pointer:pointer + len(map_data)] = map_data
        arena[:len(smith_data)] = smith_data
        download = arena[pointer:pointer + min(0xD20, len(map_data))]
        if len(download) < 0x40:
            continue
        map_offset = gba_load32(download, 0x0C)
        width = gba_load16(download, 0x14)
        height = gba_load16(download, 0x16)
        if map_offset > len(download) - 9:
            continue
        x = gba_load_s16(download, map_offset)
        y = gba_load_s16(download, map_offset + 2)
        data_offset = gba_load32(download, map_offset + 4)
        if None in (x, y, data_offset):
            continue
        data = map_offset + data_offset
        if data < 0 or data > len(download) - 4:
            continue
        row_count = min(height >> 3, len(download) - (map_offset + 8), 512)
        if row_count <= 0:
            continue
        row_cursor = data
        overflow_rows = []
        for row in range(row_count):
            token_count = download[map_offset + 8 + row]
            end = row_cursor + token_count * 2
            if end > len(download):
                break
            words = []
            for token in range(token_count):
                value = gba_load16(download, row_cursor + token * 2)
                if value is None:
                    break
                words.append(value & 0xFFFF)
            if len(words) != token_count:
                break
            row_hits = list(pc_hits(words, 0))
            if row_hits:
                overflow_rows.extend((row, token_count, hit[2]) for hit in row_hits)
            row_cursor = end
        results.append((pointer, map_offset, width, height, x, y, data_offset, data, overflow_rows))
    return results


def fmt_source(source):
    prefix = ""
    if len(source) == 5:
        tribe, gil, price, material, count = source
        prefix = f"tribe={tribe} "
    else:
        gil, price, material, count = source
    signed = signed16(gil)
    return f"{prefix}gil={gil} ({signed:+d}) price={price} material0={material} count0={count}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mem1", type=Path,
                        help="raw 24-MiB MEM1 dump instead of live Dolphin")
    parser.add_argument("--state", type=Path,
                        help="Dolphin savestate to scan instead of a live process")
    parser.add_argument("--param", type=Path,
                        help="use only the stable item rows from retail param.cfd")
    parser.add_argument("--item-table", type=lambda x: int(x, 0), default=JP_ITEM_TABLE)
    parser.add_argument("--tribe", choices=TRIBES + ("mixed",), default="lilty",
                        help="result column, or mixed if items may be transferred between tribes")
    parser.add_argument("--rate", type=int, default=100, help="smith price percentage (default: 100)")
    parser.add_argument("--max-item", type=lambda x: int(x, 0), default=0x7FFF,
                        help="ignore record IDs above this value (default: 0x7FFF)")
    parser.add_argument("--all-stable-records", action="store_true",
                        help="with --param, scan every stable positive ID >= 401 even if no clean wrong-craft source is known")
    parser.add_argument("--max-material-deletes", type=int, default=0,
                        help="admit wrong crafts with at most this many material deletions (default: 0/clean only)")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--find-halfword", type=lambda x: int(x, 0),
                        help="list craftable smith records containing this raw u16")
    parser.add_argument("--find-halfwords", nargs="+", type=lambda x: int(x, 0),
                        help="list craftable smith records containing any of these raw u16 values")
    parser.add_argument("--max-halfword-index", type=int, default=27,
                        help="with --find-halfword, only report record word indices up to this value")
    parser.add_argument("--pair", nargs=2, type=lambda x: int(x, 0), metavar=("A", "B"),
                        help="inspect one ordered pair instead of searching all pairs")
    parser.add_argument("--headers", action="store_true",
                        help="list craftable record windows that form a plausible map header")
    parser.add_argument("--zero-lz", action="store_true",
                        help="with --headers, also require 10 00 00 00 at map offset 0x20")
    parser.add_argument("--rop-chain", choices=("event", "item", "cmdslot", "letter", "routeletter", "selfletter", "txqueue", "txrepeat", "txrepeat-pop", "txrepeat-gadget", "txpatch", "cmdwrite", "equipwrite", "routeequipwrite", "raw"),
                        help="find three-record chains that execute a legal JP name payload")
    parser.add_argument("--raw-region", choices=("JP", "EN", "PAL"), default="JP",
                        help="retail GBA addresses for raw/tx* chains (default: JP)")
    parser.add_argument("--raw-word", type=lambda x: int(x, 0),
                        help="require the exact Link_Write word for --rop-chain raw")
    parser.add_argument("--gadget", type=lambda x: int(x, 0),
                        help="override the first Thumb return target for a multi-record ROP search")
    parser.add_argument("--name-entry", type=lambda x: int(x, 0),
                        help="override the Thumb address of the executable name payload")
    parser.add_argument("--computed-call", action="store_true",
                        help="for raw ROP, let the name add r6 to r4/r7 to form Link_Write")
    parser.add_argument("--capture-regs", action="store_true",
                        help="with --computed-call, list gadget/name returns without requiring a callable sum")
    parser.add_argument("--infowin-chain", action="store_true",
                        help="for raw ROP, use InfoWin_Open's second pop before Link_Write")
    parser.add_argument("--infowin-name", action="store_true",
                        help="for raw ROP, make InfoWin_Open's second pop return to a computed-call name")
    parser.add_argument("--direct-infowin", action="store_true",
                        help="for raw ROP, return directly to InfoWin_Open's pop/send gadget")
    parser.add_argument("--packet-pop", action="store_true",
                        help="for raw ROP, use an in-flight player-stat POP {r0,r6,pc} payload")
    parser.add_argument("--compact-pop", action="store_true",
                        help="for EN raw ROP, use the 020102D3 POP {r4-r6}; POP {r1}; BX r1 epilogue")
    parser.add_argument("--compact-pop-chain", action="store_true",
                        help="for EN raw ROP, chain 020102D3 into the western GilScreen pop gadget")
    parser.add_argument("--packet-pop-mask", type=lambda x: int(x, 0), default=0x41,
                        help="favorite byte used as POP register mask (default: 0x41)")
    parser.add_argument("--max-records", type=int, default=10,
                        help="maximum records in a multi-record raw chain (default: 10)")
    parser.add_argument("--first-pcs", action="store_true",
                        help="enumerate GBA return PCs reachable with multiple smith records")
    parser.add_argument("--show-words", action="store_true",
                        help="with --first-pcs, print row tokens and expanded stack words")
    parser.add_argument("--start-word", type=int,
                        help="with --first-pcs, require this 0..27 word offset in the first record")
    parser.add_argument("--gba-image", type=Path,
                        help="retail ffcc_cli.bin used to show code at --first-pcs targets")
    parser.add_argument("--probe-script", type=Path,
                        help="with --first-pcs, write harness probes for every listed row")
    parser.add_argument("--western-arm-probe-script", action="store_true",
                        help="print PAL exact-entry ARM probes for each craftable record at Smith slot 28")
    parser.add_argument("--western-arm-pair-first", nargs="+", type=lambda x: int(x, 0),
                        help="print exact-entry probes pairing each listed slot-28 item with every craftable slot-29 item")
    parser.add_argument("--western-arm-pair-second", nargs="+", type=lambda x: int(x, 0),
                        help="with --western-arm-pair-first, restrict slot-29 probes to these items")
    parser.add_argument("--western-arm-branch-range", nargs=2, type=lambda x: int(x, 0),
                        metavar=("START", "END"),
                        help="list direct ARM branches from slot 29 into [START, END)")
    parser.add_argument("--letter-index", type=lambda x: int(x, 0), default=-72,
                        help="signed DeleteItemIdx target for letter chains (default: -72)")
    parser.add_argument("--letter-item", type=lambda x: int(x, 0), default=1,
                        help="nonzero low-half item value for letter chains (default: 1)")
    parser.add_argument("--cmd-index", type=lambda x: int(x, 0), default=238,
                        help="u8 ChgCmdLst destination for cmdwrite chains (default: 238/tribe)")
    parser.add_argument("--cmd-value", type=lambda x: int(x, 0), default=0x1234,
                        help="signed or raw 16-bit value for cmdwrite chains (default: 0x1234)")
    parser.add_argument("--equip-index", type=lambda x: int(x, 0), default=5,
                        help="signed or raw equip index for equipwrite chains (default: 5/inventory slot 0)")
    parser.add_argument("--equip-value", type=lambda x: int(x, 0), default=-102,
                        help="signed or raw byte written by equipwrite chains (default: -102)")
    parser.add_argument("--tx-word", type=lambda x: int(x, 0), default=0x00000055,
                        help="word installed by --rop-chain txqueue (default: orphan 0x55)")
    parser.add_argument("--any-tx55", action="store_true",
                        help="for txrepeat, accept any low-bit packet whose command byte is 0x55")
    parser.add_argument("--tx-count", type=int, default=1,
                        help="copies installed by --rop-chain txqueue (default: 1, max: 64)")
    parser.add_argument("--tx-next-word", type=lambda x: int(x, 0), default=0x00001555,
                        help="second word installed by --rop-chain txpatch (default: EN G59 pivot tail)")
    parser.add_argument("--map", type=Path,
                        default=Path("orig/GCCJGC/files/dvd/gba/m01_1.mcd"),
                        help="valid map staged before the smith overwrite")
    args = parser.parse_args()
    if not -0x8000 <= args.letter_index <= 0x7FFF:
        parser.error("--letter-index must be a signed 16-bit value")
    if not 1 <= args.letter_item <= 0xFFFF:
        parser.error("--letter-item must be 1..65535")
    if not 0 <= args.cmd_index <= 0xFF:
        parser.error("--cmd-index must be 0..255")
    if not -0x8000 <= args.cmd_value <= 0xFFFF:
        parser.error("--cmd-value must be a signed or raw 16-bit value")
    if not -0x80 <= args.equip_index <= 0xFF:
        parser.error("--equip-index must be a signed or raw 8-bit value")
    if not -0x80 <= args.equip_value <= 0xFF:
        parser.error("--equip-value must be a signed or raw 8-bit value")
    if not 1 <= args.tx_count <= 64:
        parser.error("--tx-count must be 1..64")
    if args.zero_lz and not args.headers:
        parser.error("--zero-lz requires --headers")
    if args.max_records < 1 or args.max_records > 64:
        parser.error("--max-records must be 1..64")
    if args.max_material_deletes < 0:
        parser.error("--max-material-deletes must be nonnegative")
    if args.find_halfword is not None and not 0 <= args.find_halfword <= 0xFFFF:
        parser.error("--find-halfword must be a raw 16-bit value")
    if args.find_halfwords is not None and any(
            not 0 <= value <= 0xFFFF for value in args.find_halfwords):
        parser.error("--find-halfwords values must be raw 16-bit values")
    if args.find_halfword is not None and args.find_halfwords is not None:
        parser.error("use either --find-halfword or --find-halfwords")
    if not 0 <= args.max_halfword_index < 28:
        parser.error("--max-halfword-index must be 0..27")
    if args.start_word is not None and not 0 <= args.start_word < 28:
        parser.error("--start-word must be 0..27")
    if args.probe_script and not args.first_pcs:
        parser.error("--probe-script requires --first-pcs")
    if args.western_arm_pair_second and not args.western_arm_pair_first:
        parser.error("--western-arm-pair-second requires --western-arm-pair-first")

    if sum(value is not None for value in (args.mem1, args.state, args.param)) > 1:
        parser.error("--mem1, --state and --param are mutually exclusive")
    if args.param:
        mem = ParamMem(args.param, args.item_table)
    elif args.mem1:
        data = args.mem1.read_bytes()
        mem = Mem1(data)
    elif args.state:
        from dolphin_state_mem1 import extract_mem1
        param = Path("orig/GCCJGC/files/dvd/cft/param.cfd").read_bytes()
        anchor = param[0x175F0:0x175F0 + 0x200]
        data = extract_mem1(args.state, anchor, args.item_table)
        mem = Mem1(data)
    else:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from cles_scan import read_dolphin_mem1
        data = read_dolphin_mem1(b"GCCJGC")
        mem = Mem1(data)
    if args.tribe == "mixed":
        sources = defaultdict(list)
        for tribe_index, tribe_name in enumerate(TRIBES):
            for item_id, tribe_sources in wrong_craft_sources(
                    mem, args.item_table, tribe_index, args.max_material_deletes).items():
                sources[item_id].extend((tribe_name, *source) for source in tribe_sources)
    else:
        tribe = TRIBES.index(args.tribe)
        sources = wrong_craft_sources(
            mem, args.item_table, tribe, args.max_material_deletes)
    if args.all_stable_records:
        if not args.param:
            parser.error("--all-stable-records requires --param")
        for item_id in range(401, 1205):
            sources.setdefault(item_id, [(item_id, 0, 0, 0)])
    records = {}
    for item_id in sorted(sources):
        if item_id > min(32767, args.max_item):  # The signed inventory ID must remain positive.
            continue
        record = smith_record(mem, args.item_table, item_id, args.rate)
        if record is not None:
            records[item_id] = record

    if args.western_arm_probe_script:
        for item_id, words in records.items():
            # The proven later-row PAL layout branches to DOWNLOAD_BUF+0x60c,
            # which is byte 11 of Smith inventory record 28.
            payload = struct.pack("<28H", *words)[11:]
            print("core-reset")
            print(f"probe-western-arm-payload {item_id} {payload.hex()} 512")
        return
    if args.western_arm_pair_first is not None:
        layout = ([4000, 14624] + [407] * 3 + [405] + [407] * 11 +
                  [1369, 13568, 416] + [407] * 8)
        missing = [item_id for item_id in layout if item_id not in records]
        if missing:
            parser.error(f"western ARM backing records are not craftable under these filters: {missing}")
        backing_prefix = b"".join(struct.pack("<28H", *records[item_id])
                                  for item_id in layout)
        for first_id in args.western_arm_pair_first:
            first = records.get(first_id)
            if first is None:
                parser.error(f"slot-28 item {first_id} is not a craftable Smith record under these filters")
            first_raw = struct.pack("<28H", *first)
            prefix = first_raw[11:]
            # DOWNLOAD_BUF begins at byte 31 of Smith record 0.  Preserve the
            # exact earlier records so a payload branch back into the same
            # downloaded map executes retail-representable inventory bytes.
            backing = (backing_prefix + first_raw)[31:]
            print(f"western-arm-backing {backing.hex()}")
            second_ids = args.western_arm_pair_second or records.keys()
            for item_id in second_ids:
                words = records.get(item_id)
                if words is None:
                    parser.error(f"slot-29 item {item_id} is not a craftable Smith record under these filters")
                payload = prefix + struct.pack("<28H", *words)
                print("core-reset")
                print(f"probe-western-arm-payload {first_id}+{item_id} "
                      f"{payload.hex()} 1024")
        return
    if args.western_arm_branch_range is not None:
        range_start, range_end = args.western_arm_branch_range
        if range_start >= range_end:
            parser.error("--western-arm-branch-range START must be below END")

        def branch_target(word, address):
            if (word & 0x0E000000) != 0x0A000000 or word >> 28 == 0xF:
                return None
            displacement = (word & 0xFFFFFF) << 2
            if displacement & 0x02000000:
                displacement -= 0x04000000
            return (address + 8 + displacement) & 0xFFFFFFFF

        payload_base = 0x0203860C
        packed = {item_id: struct.pack("<28H", *words)
                  for item_id, words in records.items()}
        # The first slot-29 instruction straddles the record boundary.  Group
        # slot-28 candidates by the one byte they contribute to that word.
        first_by_tail = defaultdict(list)
        for item_id, raw in packed.items():
            first_by_tail[raw[55]].append(item_id)
        for second_id, raw in packed.items():
            for tail, first_ids in first_by_tail.items():
                word = int.from_bytes(bytes((tail,)) + raw[:3], "little")
                destination = branch_target(word, payload_base + 44)
                if destination is not None and range_start <= destination < range_end:
                    print(f"boundary first={','.join(map(str, first_ids))} second={second_id} "
                          f"word={word:08X} target={destination:08X} cond={word >> 28:X}")
            for byte_index in range(3, 55, 4):
                word = int.from_bytes(raw[byte_index:byte_index + 4], "little")
                address = payload_base + 45 + byte_index
                destination = branch_target(word, address)
                if destination is not None and range_start <= destination < range_end:
                    print(f"second={second_id} byte={byte_index} word={word:08X} "
                          f"target={destination:08X} cond={word >> 28:X}")
        return

    print(f"tribe={args.tribe} output IDs={len(sources)} smith records={len(records)} "
          f"rate={args.rate} max-material-deletes={args.max_material_deletes}")
    find_halfwords = ({args.find_halfword} if args.find_halfword is not None else
                      set(args.find_halfwords or ()))
    if find_halfwords:
        hits = []
        for item_id, words in records.items():
            for index, word in enumerate(words[:args.max_halfword_index + 1]):
                if word in find_halfwords:
                    hits.append((word, index, item_id))
        hits.sort()
        print(f"halfwords={','.join(f'{value:04X}' for value in sorted(find_halfwords))} "
              f"hits={len(hits)} "
              f"max-index={args.max_halfword_index}")
        for word, index, item_id in hits[:args.limit]:
            print(f"  halfword={word:04X} record={item_id} word={index} byte={index * 2:#04x} "
                  + fmt_source(sources[item_id][0]))
        return
    if args.first_pcs:
        def run_record(words, start, position, low_word, captured, token_budget):
            captured = list(captured)
            consumed = 0
            for token in words[start:]:
                if consumed >= token_budget:
                    break
                consumed += 1
                if token & 0x8000:
                    count = token & 0x3FF
                    value = 0x1000 | ((token >> 10) & 3)
                else:
                    count = 1
                    value = token
                capture_stop = min(position + count, 296)
                for output_index in range(max(position, 288), capture_stop):
                    captured[output_index - 288] = value
                if position <= 296 < position + count:
                    low_word = value
                if position <= 297 < position + count:
                    if low_word is None:
                        return None
                    return 298, low_word, value, tuple(captured), consumed
                position += count
            return position, low_word, None, tuple(captured), consumed

        record_items = sorted(records)
        # State is expanded position plus a possible line[296] value when a
        # record boundary falls between the two return-address halfwords.
        # For equal state, fewer row tokens dominates because the row-length
        # field is only one byte.
        best = {}
        targets = {}
        for item in record_items:
            starts = range(28) if args.start_word is None else (args.start_word,)
            for start in starts:
                result = run_record(records[item], start, 0, None, (None,) * 8, 255)
                if result is None:
                    continue
                position, low, high, captured, consumed = result
                path = (item,)
                if high is not None:
                    target = low | (high << 16)
                    if (low & 1 and
                            (0x02000001 <= target <= 0x0203FFFF or
                             0x03000001 <= target <= 0x03007FFF)):
                        key = (target, captured) if args.capture_regs else target
                        targets.setdefault(key, (consumed, start, path, captured))
                    continue
                if position < 298:
                    state_capture = captured if args.capture_regs else ()
                    key = (position, low, state_capture, 1)
                    old = best.get(key)
                    if old is None or consumed < old[0]:
                        best[key] = (consumed, start, path, captured)

        for depth in range(1, args.max_records):
            frontier = [(position, low, captured, data)
                        for (position, low, captured, used), data in best.items()
                        if used == depth]
            if not frontier:
                break
            next_best = {}
            for position, low, state_capture, (used_tokens, start, path, captured) in frontier:
                for item in record_items:
                    remaining = 255 - used_tokens
                    if remaining <= 0:
                        continue
                    result = run_record(records[item], 0, position, low, captured, remaining)
                    if result is None:
                        continue
                    new_position, new_low, high, new_captured, consumed = result
                    new_tokens = used_tokens + consumed
                    new_path = path + (item,)
                    if high is not None:
                        target = new_low | (high << 16)
                        if (new_low & 1 and
                                (0x02000001 <= target <= 0x0203FFFF or
                                 0x03000001 <= target <= 0x03007FFF)):
                            key = ((target, new_captured) if args.capture_regs else target)
                            previous = targets.get(key)
                            candidate = (new_tokens, start, new_path, new_captured)
                            if previous is None or (len(new_path), new_tokens) < (len(previous[2]), previous[0]):
                                targets[key] = candidate
                        continue
                    if new_position < 298:
                        next_state_capture = new_captured if args.capture_regs else ()
                        key = (new_position, new_low, next_state_capture, depth + 1)
                        old = next_best.get(key)
                        if old is None or new_tokens < old[0]:
                            next_best[key] = (new_tokens, start, new_path, new_captured)
            best.update(next_best)

        image = args.gba_image.read_bytes() if args.gba_image else None
        rows = []
        for key, (tokens, start, path, captured) in targets.items():
            target = key[0] if args.capture_regs else key
            code = b""
            if image is not None:
                offset = (target & ~1) - 0x02000000
                if 0 <= offset < len(image):
                    code = image[offset:offset + 16]
            # Rank direct multi-register returns first.  BDF0 is
            # pop {r4-r7,pc}; BCF0 begins the two-pop epilogue used by the
            # already-confirmed GilScreen gadget.
            first_half = struct.unpack_from("<H", code)[0] if len(code) >= 2 else -1
            rank = 0 if first_half == 0xBDF0 else 1 if first_half == 0xBCF0 else 2
            rows.append((rank, len(path), tokens, target, start, path, code, captured))
        rows.sort()
        print(f"multi-record reachable first PCs={len(rows)}")
        probes = []
        for rank, _, tokens, target, start, path, code, captured in rows[:args.limit]:
            code_text = code.hex(" ").upper() if code else "outside supplied image"
            print(f"  pc={target:08X} records={path} start=A+{start} tokens={tokens} code={code_text}")
            if args.capture_regs:
                regs = [captured[index] | (captured[index + 1] << 16)
                        for index in range(0, 8, 2)]
                print("    " + " ".join(
                    f"r{4 + index}={value:08X}" for index, value in enumerate(regs)))
            if args.show_words:
                stream = records[path[0]][start:]
                for item in path[1:]:
                    stream += records[item]
                expanded, _ = expand_row(stream, 0, 340)
                print("    tokens=" + " ".join(f"{word:04X}" for word in stream))
                print("    line288=" + " ".join(f"{word:04X}" for word in expanded[288:340]))
                print("    transcript=probe-radar-stream " + args.raw_region + " " +
                      " ".join(f"0x{word:04X}" for word in stream[:tokens]))
            if args.probe_script:
                stream = records[path[0]][start:]
                for item in path[1:]:
                    stream += records[item]
                # The first-PC search stops as soon as line[296..297] is
                # known, but the hardware consumes the complete Smith row.
                # Preserve the remaining record words: they land above the
                # saved return address and can affect the hijacked path's
                # caller stack.  Truncating here produced useful control-flow
                # proofs, but not retail-shaped execution.
                probes.append(
                    f"# pc={target:08X} records={path} start=A+{start} "
                    f"pc_tokens={tokens} row_tokens={min(len(stream), 255)}\n"
                    "core-reset\nprobe-radar-stream " + args.raw_region + " " +
                    " ".join(f"0x{word:04X}" for word in stream[:255]))
        if args.probe_script:
            args.probe_script.parent.mkdir(parents=True, exist_ok=True)
            args.probe_script.write_text("\n".join(probes) + "\n", encoding="ascii")
            print(f"wrote harness probes={len(probes)} path={args.probe_script}")
        return
    if args.pair:
        missing = [item for item in args.pair if item not in records]
        if missing:
            raise SystemExit(f"pair item(s) not clean-craftable/safe for {args.tribe}: {missing}")
        first, second = args.pair
        smith_data = build_smith_buffer([records[first], records[second]])
        print(f"smith bytes={len(smith_data)} records=({first},{second})")
        print("A source: " + fmt_source(sources[first][0]))
        print("B source: " + fmt_source(sources[second][0]))
        plausible = inspect_map_overlay(args.map.read_bytes(), smith_data)
        print(f"pointer alignments with in-download map/info/data={len(plausible)}")
        for pointer, map_offset, width, height, x, y, data_offset, data, overflows in plausible:
            suffix = ""
            if overflows:
                suffix = " OVERFLOW " + ",".join(
                    f"row={row}/tokens={count}/pc={target:08X}" for row, count, target in overflows)
            print(f"  +{pointer:02X}: map={map_offset:04X} size={width}x{height} "
                  f"origin=({x},{y}) dataOff={data_offset:04X} data={data:04X}{suffix}")
        return

    if args.headers:
        headers = []
        record_items = sorted(records)
        record_bytes = {item: struct.pack("<28H", *records[item]) for item in record_items}

        def hardware_info(blob, offset, map_offset):
            payload = blob[offset:]
            if map_offset < 0 or map_offset + 8 > len(payload):
                return None
            x = gba_load_s16(payload, map_offset)
            y = gba_load_s16(payload, map_offset + 2)
            data_offset = gba_load32(payload, map_offset + 4)
            if None in (x, y, data_offset):
                return None
            return x, y, data_offset, map_offset + data_offset

        if args.zero_lz:
            # The exact four-byte LZ header is selective enough to index first,
            # avoiding an items^2 * 56 brute force.  A header at offsets 0..20
            # is wholly within one record; later offsets need its successor.
            target = b"\x10\x00\x00\x00"
            for offset in range(0x38):
                lz_at = offset + 0x20
                first_match = record_items
                second_match = record_items
                if lz_at < 0x38:
                    take = min(4, 0x38 - lz_at)
                    first_match = [item for item in record_items
                                   if record_bytes[item][lz_at:lz_at + take] == target[:take]]
                    if take < 4:
                        second_match = [item for item in record_items
                                        if record_bytes[item][:4 - take] == target[take:]]
                else:
                    at = lz_at - 0x38
                    second_match = [item for item in record_items
                                    if record_bytes[item][at:at + 4] == target]

                if offset <= 0x14:
                    for first in first_match:
                        blob = record_bytes[first]
                        map_offset = struct.unpack_from("<I", blob, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", blob, offset + 0x14)
                        if 0x24 <= map_offset <= 0xD17 and 8 <= width <= 0x1000 and 8 <= height <= 0x1000:
                            headers.append((map_offset, first, None, offset, width, height, None))
                    continue

                for first in first_match:
                    for second in second_match:
                        pair_bytes = record_bytes[first] + record_bytes[second]
                        map_offset = struct.unpack_from("<I", pair_bytes, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", pair_bytes, offset + 0x14)
                        if 0x24 <= map_offset <= 0xD17 and 8 <= width <= 0x1000 and 8 <= height <= 0x1000:
                            details = hardware_info(pair_bytes, offset, map_offset)
                            headers.append((map_offset, first, second, offset, width, height, details))
        else:
            for first in record_items:
                first_bytes = record_bytes[first]
                for second in record_items:
                    pair_bytes = first_bytes + record_bytes[second]
                    for offset in range(0, 0x38, 4):
                        map_offset = struct.unpack_from("<I", pair_bytes, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", pair_bytes, offset + 0x14)
                        if not (0x18 <= map_offset <= 0xD17):
                            continue
                        if not (8 <= width <= 0x1000 and 8 <= height <= 0x1000):
                            continue
                        details = hardware_info(pair_bytes, offset, map_offset)
                        headers.append((map_offset, first, second, offset, width, height, details))
        headers.sort(key=lambda row: (row[0], row[1], -1 if row[2] is None else row[2], row[3]))
        qualifier = "initializable zero-LZ" if args.zero_lz else "plausible two-record"
        print(f"{qualifier} header windows={len(headers)}")
        for map_offset, first, second, offset, width, height, details in headers[:args.limit]:
            second_text = "*" if second is None else str(second)
            suffix = ""
            if details is not None:
                x, y, data_offset, data = details
                suffix = (f" origin=({x},{y}) hwDataOff={data_offset:08X}"
                          f" hwData={data:08X} parity={data & 1}")
            print(f"  map={map_offset:04X} size={width}x{height} records=({first},{second_text}) A+{offset}{suffix}")
            print("    A source: " + fmt_source(sources[first][0]))
            if second is not None:
                print("    B source: " + fmt_source(sources[second][0]))
        return
    if args.rop_chain:
        if args.rop_chain in ("txqueue", "txrepeat", "txrepeat-pop", "txrepeat-gadget", "txpatch"):
            # All three retail clients place Radar_DrawMap's line buffer at
            # 03007CAC, so IWRAM mirrors after line 425.  The western link
            # state is 0x58 bytes later than JP; select its queue lines and
            # self-written literals when searching an EN/PAL heap snapshot.
            western = args.raw_region != "JP"
            connected_line = 482 if western else 438
            queue_line = 498 if western else 454
            count_line = 626 if western else 582
            count_address_low = 0x0190 if western else 0x0138
            queue_add = 0x84 if western else 0x2C
            txqueue_targets = {"JP": 0x020101BB, "EN": 0x020150C5, "PAL": 0x02015281}
            target = (0x03000001 if args.rop_chain not in ("txqueue", "txrepeat-gadget")
                      else txqueue_targets[args.raw_region])
            if args.rop_chain == "txrepeat-gadget":
                # Enter the retail halfword-invert epilogue at 0200215C.  It
                # turns an initial zero halfword into r0=0000FFFF and pops a
                # second target.  The retail store epilogue at 020045D8 then
                # writes that value to the high half of sTxCount, making the
                # signed count negative.  Its final pop returns to a one-word
                # BX-r6 loop written at the IWRAM mirror.
                target = 0x0200215D
            if args.gadget is not None:
                target = args.gadget
            stage_line = 426 + (((target & 0x7FFF) & ~1) // 2)
            halves = (args.tx_word & 0xFFFF, (args.tx_word >> 16) & 0xFFFF)
            if args.rop_chain != "txpatch" and any(value & 0x8000 for value in halves):
                parser.error("--tx-word halfwords must be below 0x8000; high-bit radar tokens expand as RLE")
            required = {
                296: target & 0xFFFF,
                297: (target >> 16) & 0xFFFF,
                connected_line: 1,  # connected=1, handshake step=0
            }
            if args.rop_chain == "txqueue":
                required.update({count_line: args.tx_count, count_line + 1: 0})
                for packet in range(args.tx_count):
                    required[queue_line + packet * 2] = halves[0]
                    required[queue_line + packet * 2 + 1] = halves[1]
            elif args.rop_chain == "txrepeat":
                required.update({
                    stage_line + 0: 0x4803, stage_line + 1: 0x2100,
                    stage_line + 2: 0x43C9, stage_line + 3: 0x6001,
                    stage_line + 4: 0x4A02, stage_line + 5: 0x4710,
                    stage_line + 6: 0, stage_line + 7: 0,
                    stage_line + 8: count_address_low, stage_line + 9: 0x0300,
                    stage_line + 10: target & 0xFFFF,
                    stage_line + 11: (target >> 16) & 0xFFFF,
                    queue_line: halves[0], queue_line + 1: halves[1],
                    count_line: 0, count_line + 1: 0,
                })
                if args.any_tx55:
                    required[queue_line] = frozenset(
                        value for value in range(0x8000) if (value & 0xFF) == 0x55
                    )
                    required[queue_line + 1] = frozenset(range(0x8000))
            elif args.rop_chain == "txrepeat-pop":
                # Radar_DrawMap restores r4-r7 immediately before its return.
                # Supplying those stack words lets the wrapped stage shrink to
                # MVN/STR/BX: r4=0, r5=&sTxCount, r6=stage|1.
                required.update({
                    288: 0, 289: 0,
                    290: count_address_low, 291: 0x0300,
                    292: 0x0001, 293: 0x0300,
                    294: 0, 295: 0,
                    stage_line + 0: 0x43E4,
                    stage_line + 1: 0x602C,
                    stage_line + 2: 0x4730,
                    queue_line: halves[0], queue_line + 1: halves[1],
                    count_line: 0, count_line + 1: 0,
                })
            elif args.rop_chain == "txrepeat-gadget":
                gba_blob = args.gba_image.read_bytes() if args.gba_image else Path(
                    {"JP": "orig/GCCJGC/files/dvd/gba/ffcc_cli.bin",
                     "EN": "orig/GCCE01/files/dvd/gba/ffcc_cli.bin",
                     "PAL": "orig/GCCP01/files/dvd/gba/ffcc_cli.bin"}[args.raw_region]
                ).read_bytes()
                low_rom_offsets = frozenset(
                    offset for offset in range(0, min(len(gba_blob), 0x10000), 2)
                    if struct.unpack_from("<H", gba_blob, offset)[0] < 0x8000
                )
                required.update({
                    # Radar_DrawMap epilogue: r5 points at any low-halfword
                    # value in the first 64 KiB of the immutable client.  Its
                    # MVN therefore sets bit 15 before the later STRH installs
                    # the high half of a negative signed transmit count.
                    290: low_rom_offsets, 291: 0x0200,
                    # 0200215C pops r4-r6 and then r1 as its branch target.
                    298: 0, 299: 0,
                    300: count_address_low + 2, 301: 0x0300,
                    302: 0x0001, 303: 0x0300,
                    304: 0x45D9, 305: 0x0200,
                    # 020045D8 pops its final branch target into r0.
                    310: 0x0001, 311: 0x0300,
                    426: 0x4730,  # bx r6; r6 points back here
                    queue_line: halves[0], queue_line + 1: halves[1],
                    count_line: 0, count_line + 1: 0,
                })
            else:
                if args.tx_word & 0xFF00:
                    parser.error("--rop-chain txpatch currently requires packet byte 1 to be zero")
                if args.tx_count > 2:
                    parser.error("--rop-chain txpatch currently supports --tx-count 1 or 2")
                next_halves = (args.tx_next_word & 0xFFFF,
                               (args.tx_next_word >> 16) & 0xFFFF)
                if args.tx_count == 2 and any(value & 0x8000 for value in next_halves):
                    parser.error("--tx-next-word halfwords must be below 0x8000")
                high = args.tx_word >> 16
                required.update({
                    426: 0x2000 | (high >> 8),
                    427: 0x0200,
                    428: 0x3000 | (high & 0xFF),
                    429: 0x0400,
                    430: 0x3000 | (args.tx_word & 0xFF),
                    431: 0x4679, 432: 0x3100 | queue_add, 433: 0x6008,
                    434: 0x467A, 435: 0x3201, 436: 0x4710, 437: 0,
                    queue_line: args.tx_word & 0xFF, queue_line + 1: 0,
                    count_line: args.tx_count, count_line + 1: 0,
                })
                if args.tx_count == 2:
                    required[queue_line + 2], required[queue_line + 3] = next_halves
            output_limit = count_line + 2

            def advance_tx(words, start, position, token_budget):
                consumed = 0
                for token in words[start:]:
                    if consumed >= token_budget:
                        break
                    consumed += 1
                    if token & 0x8000:
                        count = token & 0x3FF
                        value = 0x1000 | ((token >> 10) & 3)
                    else:
                        count = 1
                        value = token
                    stop = min(position + count, output_limit)
                    for output_index in range(position, stop):
                        wanted = required.get(output_index)
                        if (wanted is not None and
                                (value not in wanted if isinstance(wanted, frozenset)
                                 else wanted != value)):
                            return None
                    position += count
                    if position >= output_limit:
                        return output_limit, consumed
                return position, consumed

            record_items = sorted(records)
            solutions = []
            best = {}
            starts = range(28) if args.start_word is None else (args.start_word,)
            for item in record_items:
                for start in starts:
                    result = advance_tx(records[item], start, 0, 255)
                    if result is None:
                        continue
                    position, consumed = result
                    if position >= output_limit:
                        solutions.append((consumed, start, (item,)))
                        continue
                    key = (position, 1)
                    old = best.get(key)
                    if old is None or consumed < old[0]:
                        best[key] = (consumed, start, (item,))

            for depth in range(1, args.max_records):
                frontier = [(position, data) for (position, used), data in best.items()
                            if used == depth]
                if not frontier:
                    break
                next_best = {}
                for position, (used_tokens, start, path) in frontier:
                    for item in record_items:
                        remaining = 255 - used_tokens
                        if remaining <= 0:
                            continue
                        result = advance_tx(records[item], 0, position, remaining)
                        if result is None:
                            continue
                        new_position, consumed = result
                        new_tokens = used_tokens + consumed
                        new_path = path + (item,)
                        if new_position >= output_limit:
                            solutions.append((new_tokens, start, new_path))
                            continue
                        key = (new_position, depth + 1)
                        old = next_best.get(key)
                        if old is None or new_tokens < old[0]:
                            next_best[key] = (new_tokens, start, new_path)
                best.update(next_best)

            solutions.sort(key=lambda entry: (len(entry[2]), entry[0], entry[1], entry[2]))
            print(f"multi-record {args.rop_chain} chains={len(solutions)} region={args.raw_region} "
                  f"target={target:08X} "
                  f"word={args.tx_word:08X} count={args.tx_count}"
                  + (f" next={args.tx_next_word:08X}" if args.rop_chain == "txpatch" and args.tx_count == 2 else ""))
            if not solutions and best:
                position, used = max(best, key=lambda key: (key[0], -key[1]))
                used_tokens, start, path = best[(position, used)]
                next_required = next(
                    ((index, value) for index, value in sorted(required.items())
                     if index >= position), None
                )
                print(f"  furthest={position} records={path} start=A+{start} "
                      f"tokens={used_tokens} next-required={next_required}")
            for token_count, start, path in solutions[:args.limit]:
                extra = ""
                if args.any_tx55:
                    stream = records[path[0]][start:]
                    for item in path[1:]:
                        stream += records[item]
                    expanded, _ = expand_row(stream, 0, output_limit)
                    if len(expanded) > queue_line + 1:
                        actual_word = expanded[queue_line] | (expanded[queue_line + 1] << 16)
                        extra = f" tx-word={actual_word:08X}"
                print(f"  records={path} start=A+{start} tokens={token_count}{extra}")
                for slot, item in enumerate(path):
                    print(f"    {slot}: item={item} " + fmt_source(sources[item][0]))
            return

        if args.rop_chain == "selfletter":
            # Radar's line buffer begins at 03007CAC with the retail stack
            # used by the harness.  Once an oversized row crosses 03008000,
            # IWRAM mirrors back to 03000000.  The same row can therefore:
            #   * install the saved registers and return PC at line 288,
            #   * repair sTxCount at mirrored lines 582/583, and
            #   * write a six-instruction stage at mirrored 03000000
            #     (line 426), before later repairing the transmit count.
            # The stage loads its own value and Link_SendLetterReply address,
            # yielding an unchecked signed DeleteItemIdx on the GameCube.
            western = args.raw_region != "JP"
            count_line = 626 if western else 582
            send_letter_reply = 0x02003015 if western else 0x02001E31
            letter_value = ((args.letter_index & 0xFFFF) << 16) | args.letter_item
            output_limit = 1200 if args.capture_regs else count_line + 8
            required = {
                296: 0x0001, 297: 0x0300,  # return to mirrored self-written code
                426: 0x2000,              # movs r0, #0 (letter)
                427: 0x2101,              # movs r1, #1 (item answer)
                428: 0x2200,              # movs r2, #0 (not gil)
                429: 0x4B02,              # ldr r3, [pc, #8]
                430: 0x4C02,              # ldr r4, [pc, #8]
                431: 0x4720,              # bx r4
                434: letter_value & 0xFFFF,
                435: letter_value >> 16,
                436: send_letter_reply & 0xFFFF,
                437: send_letter_reply >> 16,
                count_line: 0, count_line + 1: 0,  # sTxCount = 0
            }
            if args.capture_regs:
                required = {index: required[index]
                            for index in (296, 297, count_line, count_line + 1)}

            def advance_self(words, start, position, token_budget):
                consumed = 0
                for token in words[start:]:
                    if consumed >= token_budget:
                        break
                    consumed += 1
                    if token & 0x8000:
                        count = token & 0x3FF
                        value = 0x1000 | ((token >> 10) & 3)
                    else:
                        count = 1
                        value = token
                    stop = min(position + count, output_limit)
                    for output_index in range(position, stop):
                        wanted = required.get(output_index)
                        if wanted is not None and wanted != value:
                            return None
                    position += count
                    if position >= output_limit:
                        return output_limit, consumed
                return position, consumed

            record_items = sorted(records)
            solutions = []
            best = {}
            initial_depth = 1
            if (args.capture_regs and args.raw_region == "JP" and
                    1532 in records and 6266 in records):
                # The first-PC scan proves this shortest prefix reaches
                # 03000001.  Seeding it directly makes the long mirrored-row
                # continuation search tractable.
                start = 26
                prefix = records[1532][start:] + records[6266]
                result = advance_self(prefix, 0, 0, 255)
                if result is not None:
                    position, consumed = result
                    best[(position, 2)] = (consumed, start, (1532, 6266))
                    initial_depth = 2
            else:
                starts = range(28) if args.start_word is None else (args.start_word,)
                for item in record_items:
                    for start in starts:
                        result = advance_self(records[item], start, 0, 255)
                        if result is None:
                            continue
                        position, consumed = result
                        path = (item,)
                        if position >= output_limit:
                            solutions.append((consumed, start, path))
                            continue
                        key = (position, 1)
                        old = best.get(key)
                        if old is None or consumed < old[0]:
                            best[key] = (consumed, start, path)

            for depth in range(initial_depth, args.max_records):
                frontier = [(position, data) for (position, used), data in best.items()
                            if used == depth]
                if not frontier:
                    break
                next_best = {}
                for position, (used_tokens, start, path) in frontier:
                    for item in record_items:
                        remaining = 255 - used_tokens
                        if remaining <= 0:
                            continue
                        result = advance_self(records[item], 0, position, remaining)
                        if result is None:
                            continue
                        new_position, consumed = result
                        new_tokens = used_tokens + consumed
                        new_path = path + (item,)
                        if new_position >= output_limit:
                            solutions.append((new_tokens, start, new_path))
                            continue
                        key = (new_position, depth + 1)
                        old = next_best.get(key)
                        if old is None or new_tokens < old[0]:
                            next_best[key] = (new_tokens, start, new_path)
                best.update(next_best)

            solutions.sort(key=lambda entry: (len(entry[2]), entry[0], entry[1], entry[2]))
            if args.capture_regs:
                staged = []
                for token_count, start, path in solutions:
                    words = records[path[0]][start:]
                    for item in path[1:]:
                        words += records[item]
                    expanded, _ = expand_row(words, 0, output_limit)
                    for stage in range(426, output_limit - 32):
                        if expanded[stage] != 0x4720:
                            continue
                        # PC-relative LDR uses Align(instruction + 4, 4).
                        def literal_index(instruction_index, instruction):
                            byte_address = (instruction_index - 426) * 2
                            literal_address = ((byte_address + 4) & ~3) + (instruction & 0xFF) * 4
                            return 426 + literal_address // 2
                        for function_pc in range(max(426, stage - 64), stage):
                            function_ins = expanded[function_pc]
                            if function_ins & 0xFF00 != 0x4C00:
                                continue
                            function_at = literal_index(function_pc, function_ins)
                            if function_at <= stage or function_at + 1 >= len(expanded):
                                continue
                            function = expanded[function_at] | (expanded[function_at + 1] << 16)
                            # Enter Link_SendEquipSlot after it has installed
                            # command byte 0x1E.  The packet word itself is
                            # loaded from Radar's overwritten caller stack.
                            packet_sender = (0x020032F5 if western
                                             else 0x02002111)
                            if function != packet_sender:
                                continue
                            if any(index != function_pc and expanded[index] not in
                                   (0x0000, 0x1000, 0x1001, 0x1002, 0x1003)
                                   for index in range(426, stage)):
                                continue
                            raw_word = expanded[304] | (expanded[305] << 16)
                            staged.append((token_count, start, path, stage, 0, raw_word))
                            break
                        else:
                            continue
                        break
                solutions = [(tokens, start, path) for tokens, start, path, _, _, _ in staged]
                staged_details = {(start, path): (stage, scratch, raw_word)
                                  for _, start, path, stage, scratch, raw_word in staged}
            print(f"multi-record selfletter chains={len(solutions)} "
                  f"attachment={letter_value:08X}")
            for token_count, start, path in solutions[:args.limit]:
                words = records[path[0]][start:]
                for item in path[1:]:
                    words += records[item]
                expanded, _ = expand_row(words, 0, output_limit)
                assert all(expanded[index] == value for index, value in required.items())
                print(f"  records={path} start=A+{start} tokens={token_count}")
                if args.capture_regs:
                    stage, scratch, raw_word = staged_details[(start, path)]
                    print(f"    fallthrough-stage={stage} scratch={scratch:08X} "
                          f"raw={raw_word:08X}")
                    print("    stage=" + " ".join(
                        f"{value:04X}" for value in expanded[426:442]))
                for slot, item in enumerate(path):
                    print(f"    {slot}: item={item} " + fmt_source(sources[item][0]))
            return

        # Radar's first overwritten PC is line[296].  The GilScreen gadgets
        # load r4-r7 from line[302..309] and the next PC from line[310].  The
        # cmdwrite gadget first pops r3, shifting those positions by one word.
        # A legal JP name at DETAIL_BUF turns the registers into the selected
        # outbound command.
        if args.rop_chain in ("raw", "routeletter"):
            raw_addresses = {
                "JP": (0x02010251, 0x020014C1, 0x02035801),
                "EN": (0x020150C5, 0x02002699, 0x0203B801),
                "PAL": (0x02015281, 0x02002699, 0x0203D801),
            }
            if args.rop_chain == "raw":
                gadget, send_function, name_entry = raw_addresses[args.raw_region]
            else:
                gadget, send_function, name_entry = 0x02010251, 0x02001E31, 0x02035803
            if args.gadget is not None:
                gadget = args.gadget
            if args.name_entry is not None:
                name_entry = args.name_entry
            common_required = {
                296: gadget & 0xFFFF,
                297: gadget >> 16,
            }
            if not args.capture_regs:
                common_required.update({
                    310: name_entry & 0xFFFF,
                    311: name_entry >> 16,
                })
            output_limit = 312
            if args.compact_pop:
                if args.rop_chain != "raw" or args.raw_region != "EN":
                    parser.error("--compact-pop currently requires --rop-chain raw --raw-region EN")
                gadget = args.gadget if args.gadget is not None else 0x020102D3
                common_required = {
                    296: gadget & 0xFFFF,
                    297: gadget >> 16,
                    298: send_function & 0xFFFF,  # popped r4
                    299: send_function >> 16,
                    304: name_entry & 0xFFFF,     # popped r1 / next PC
                    305: name_entry >> 16,
                }
                if args.raw_word is not None:
                    common_required[302] = args.raw_word & 0xFFFF  # popped r6
                    common_required[303] = (args.raw_word >> 16) & 0xFFFF
                output_limit = 306
            if args.compact_pop_chain:
                if args.rop_chain != "raw" or args.raw_region != "EN":
                    parser.error("--compact-pop-chain currently requires --rop-chain raw --raw-region EN")
                common_required = {
                    296: 0x02D3, 297: 0x0201,  # compact first pop
                    304: gadget & 0xFFFF, 305: gadget >> 16,
                    310: send_function & 0xFFFF, 311: send_function >> 16,
                    318: name_entry & 0xFFFF, 319: name_entry >> 16,
                }
                if args.raw_word is not None:
                    common_required[314] = args.raw_word & 0xFFFF
                    common_required[315] = (args.raw_word >> 16) & 0xFFFF
                output_limit = 320
            if args.packet_pop:
                if args.rop_chain != "raw":
                    parser.error("--packet-pop requires --rop-chain raw")
                if args.packet_pop_mask not in (0x0F, 0x19, 0x23, 0x2D, 0x37, 0x41, 0x4B, 0x55):
                    parser.error("--packet-pop-mask must be a legal favorite level")
                # In the observed three-character JP packet, data[0xBC] is
                # favorite bytes 3..7 followed by little-endian Gil.  Gil's
                # low byte BD makes instruction three POP {...,pc}; favorite
                # value 65 selects POP {r0,r6,pc}.  Radar leaves SP at
                # line[298], so the overflowing row supplies r0 and the PC.
                gadget = args.gadget if args.gadget is not None else 0x03000519
                packet_pc_index = 298 + 2 * args.packet_pop_mask.bit_count()
                common_required = {
                    296: gadget & 0xFFFF,
                    297: gadget >> 16,
                    packet_pc_index: send_function & 0xFFFF,
                    packet_pc_index + 1: send_function >> 16,
                }
                if args.raw_word is not None:
                    common_required[298] = args.raw_word & 0xFFFF
                    common_required[299] = args.raw_word >> 16
                output_limit = packet_pc_index + 2
            if args.direct_infowin:
                if args.rop_chain != "raw":
                    parser.error("--direct-infowin currently requires --rop-chain raw")
                common_required = {
                    296: 0x02016661 & 0xFFFF,
                    297: 0x02016661 >> 16,
                }
                if not args.capture_regs:
                    common_required.update({
                        298: send_function & 0xFFFF,
                        299: send_function >> 16,
                        302: name_entry & 0xFFFF,
                        303: name_entry >> 16,
                    })
                output_limit = 304
            if args.infowin_chain or args.infowin_name:
                if args.rop_chain != "raw":
                    parser.error("--infowin-chain/--infowin-name currently require --rop-chain raw")
                common_required[310] = 0x02016661 & 0xFFFF
                common_required[311] = 0x02016661 >> 16
                final_target = name_entry if args.infowin_name else send_function
                common_required[316] = final_target & 0xFFFF
                common_required[317] = final_target >> 16
                output_limit = 318
            if args.rop_chain == "raw" and args.raw_word is not None:
                common_required[306] = args.raw_word & 0xFFFF
                common_required[307] = (args.raw_word >> 16) & 0xFFFF

            def advance(words, start, position, token_budget):
                consumed = 0
                for token in words[start:]:
                    if consumed >= token_budget:
                        break
                    consumed += 1
                    if token & 0x8000:
                        count = token & 0x3FF
                        value = 0x1000 | ((token >> 10) & 3)
                    else:
                        count = 1
                        value = token
                    stop = min(position + count, output_limit)
                    for output_index in range(position, stop):
                        wanted = required.get(output_index)
                        if wanted is not None and wanted != value:
                            return None
                    position += count
                    if position >= output_limit:
                        return output_limit, consumed
                return position, consumed

            # The old solver assumed the complete row lived in exactly three
            # records.  A real inventory can contribute 64 records, while the
            # radar row-length byte caps one row at 255 input tokens.  Treat
            # each record as an automaton edge over expanded output position;
            # for a fixed position, the path using fewer tokens dominates.
            record_items = sorted(records)
            solutions = []
            if (args.computed_call or args.capture_regs or args.infowin_chain or
                    args.infowin_name or args.direct_infowin or args.packet_pop or
                    args.compact_pop or args.compact_pop_chain):
                if args.rop_chain != "raw":
                    parser.error("--computed-call currently requires --rop-chain raw")

                def advance_computed(words, start, position, captured, token_budget):
                    captured = list(captured)
                    consumed = 0
                    for token in words[start:]:
                        if consumed >= token_budget:
                            break
                        consumed += 1
                        if token & 0x8000:
                            count = token & 0x3FF
                            value = 0x1000 | ((token >> 10) & 3)
                        else:
                            count = 1
                            value = token
                        stop = min(position + count, output_limit)
                        for output_index in range(position, stop):
                            wanted = common_required.get(output_index)
                            if wanted is not None and wanted != value:
                                return None
                            if 298 <= output_index < 316:
                                captured[output_index - 298] = value
                        position += count
                        if position >= output_limit:
                            return output_limit, consumed, tuple(captured)
                    return position, consumed, tuple(captured)

                best = {}
                starts = range(28) if args.start_word is None else (args.start_word,)
                for item in record_items:
                    for start in starts:
                        result = advance_computed(records[item], start, 0, (None,) * 18, 255)
                        if result is None:
                            continue
                        position, consumed, captured = result
                        path = (item,)
                        if position >= output_limit:
                            regs = [((captured[i] or 0) |
                                     ((captured[i + 1] or 0) << 16))
                                    for i in range(4, 12, 2)]
                            for branch_reg, base in (("r4", regs[0]), ("r7", regs[3])):
                                safe_store = (0x0202E000 <= regs[0] < 0x02040000 or
                                              0x03000000 <= regs[0] < 0x03008000 or
                                              0x06000000 <= regs[0] < 0x06018000)
                                new_r4 = ((captured[14] or 0) |
                                          ((captured[15] or 0) << 16))
                                name_call = ((new_r4 + regs[2]) & 0xFFFFFFFF == send_function or
                                             (regs[3] + regs[2]) & 0xFFFFFFFF == send_function)
                                if (args.direct_infowin or args.packet_pop or
                                        args.compact_pop or args.compact_pop_chain or
                                        (args.infowin_chain and safe_store) or
                                        (args.infowin_name and safe_store and name_call) or
                                        args.capture_regs or
                                        (base + regs[2]) & 0xFFFFFFFF == send_function):
                                    solutions.append((consumed, start, path, branch_reg))
                            continue
                        best[(position, captured, 1)] = (consumed, start, path)
                for depth in range(1, args.max_records):
                    frontier = [(position, captured, data)
                                for (position, captured, used), data in best.items() if used == depth]
                    if not frontier:
                        break
                    next_best = {}
                    for position, captured, (used_tokens, start, path) in frontier:
                        for item in record_items:
                            remaining = 255 - used_tokens
                            if remaining <= 0:
                                continue
                            result = advance_computed(records[item], 0, position, captured, remaining)
                            if result is None:
                                continue
                            new_position, consumed, new_captured = result
                            new_tokens = used_tokens + consumed
                            new_path = path + (item,)
                            if new_position >= output_limit:
                                regs = [((new_captured[i] or 0) |
                                         ((new_captured[i + 1] or 0) << 16))
                                        for i in range(4, 12, 2)]
                                for branch_reg, base in (("r4", regs[0]), ("r7", regs[3])):
                                    safe_store = (0x0202E000 <= regs[0] < 0x02040000 or
                                                  0x03000000 <= regs[0] < 0x03008000 or
                                                  0x06000000 <= regs[0] < 0x06018000)
                                    new_r4 = ((new_captured[14] or 0) |
                                              ((new_captured[15] or 0) << 16))
                                    name_call = ((new_r4 + regs[2]) & 0xFFFFFFFF == send_function or
                                                 (regs[3] + regs[2]) & 0xFFFFFFFF == send_function)
                                    if (args.direct_infowin or args.packet_pop or
                                            args.compact_pop or args.compact_pop_chain or
                                            (args.infowin_chain and safe_store) or
                                            (args.infowin_name and safe_store and name_call) or
                                            args.capture_regs or
                                            (base + regs[2]) & 0xFFFFFFFF == send_function):
                                        solutions.append((new_tokens, start, new_path, branch_reg))
                                continue
                            key = (new_position, new_captured, depth + 1)
                            old = next_best.get(key)
                            if old is None or new_tokens < old[0]:
                                next_best[key] = (new_tokens, start, new_path)
                    best.update(next_best)

            if (not args.computed_call and not args.infowin_chain and
                    not args.infowin_name and not args.direct_infowin and
                    not args.packet_pop and not args.compact_pop and
                    not args.compact_pop_chain):
                branch_choices = (("r4", 302), ("r7", 308))
            else:
                branch_choices = ()
            for branch_reg, register_index in branch_choices:
                required = dict(common_required)
                required[register_index] = send_function & 0xFFFF
                required[register_index + 1] = send_function >> 16
                best = {}
                for item in record_items:
                    words = records[item]
                    starts = range(len(words)) if args.start_word is None else (args.start_word,)
                    for start in starts:
                        result = advance(words, start, 0, 255)
                        if result is None:
                            continue
                        position, consumed = result
                        path = (item,)
                        if position >= output_limit:
                            solutions.append((consumed, start, path, branch_reg))
                            continue
                        key = (position, 1)
                        if key not in best or consumed < best[key][0]:
                            best[key] = (consumed, start, path)

                for depth in range(1, args.max_records):
                    frontier = [(position, used, data) for (position, used), data in best.items()
                                if used == depth]
                    if not frontier:
                        break
                    next_best = {}
                    for position, _, (used_tokens, start, path) in frontier:
                        for item in record_items:
                            remaining = 255 - used_tokens
                            if remaining <= 0:
                                continue
                            result = advance(records[item], 0, position, remaining)
                            if result is None:
                                continue
                            new_position, consumed = result
                            new_tokens = used_tokens + consumed
                            new_path = path + (item,)
                            if new_position >= output_limit:
                                solutions.append((new_tokens, start, new_path, branch_reg))
                                continue
                            key = (new_position, depth + 1)
                            old = next_best.get(key)
                            if old is None or new_tokens < old[0]:
                                next_best[key] = (new_tokens, start, new_path)
                    best.update(next_best)

            solutions.sort(key=lambda entry: (len(entry[2]), entry[0], entry[1], entry[2], entry[3]))
            qualifier = f" word={args.raw_word:08X}" if args.raw_word is not None else ""
            print(f"multi-record {args.rop_chain} chains={len(solutions)} region={args.raw_region}{qualifier}")
            for token_count, start, path, branch_reg in solutions[:args.limit]:
                words = records[path[0]][start:]
                for item in path[1:]:
                    words += records[item]
                expanded, _ = expand_row(words, 0, output_limit)
                if args.packet_pop:
                    raw_word = expanded[298] | (expanded[299] << 16)
                    packet_pc_index = 298 + 2 * args.packet_pop_mask.bit_count()
                    next_pc = expanded[packet_pc_index] | (expanded[packet_pc_index + 1] << 16)
                    print(f"  records={path} start=A+{start} tokens={token_count} "
                          f"raw={raw_word:08X} branch=packet-pop/{args.packet_pop_mask:02X} "
                          f"next={next_pc:08X}")
                    continue
                if args.compact_pop:
                    raw_word = expanded[302] | (expanded[303] << 16)
                    next_pc = expanded[304] | (expanded[305] << 16)
                    print(f"  records={path} start=A+{start} tokens={token_count} "
                          f"raw={raw_word:08X} branch=compact-pop next={next_pc:08X} "
                          "name='0F G'")
                    continue
                if args.compact_pop_chain:
                    raw_word = expanded[314] | (expanded[315] << 16)
                    next_pc = expanded[318] | (expanded[319] << 16)
                    print(f"  records={path} start=A+{start} tokens={token_count} "
                          f"raw={raw_word:08X} branch=compact-pop-chain "
                          f"next={next_pc:08X} name='0F G'")
                    continue
                regs = [expanded[index] | (expanded[index + 1] << 16)
                        for index in range(302, min(310, output_limit), 2)]
                if args.direct_infowin:
                    raw_word = expanded[300] | (expanded[301] << 16)
                    name = "(F G"
                    detail = f"raw={raw_word:08X}"
                    print(f"  records={path} start=A+{start} tokens={token_count} "
                          f"{detail} branch=direct name={name!r}")
                    if args.capture_regs:
                        new_r4 = expanded[298] | (expanded[299] << 16)
                        next_pc = expanded[302] | (expanded[303] << 16)
                        print(f"    r4={new_r4:08X} next={next_pc:08X}")
                    continue
                if args.rop_chain == "raw":
                    raw_word = (regs[1] if args.infowin_chain or args.infowin_name else
                                expanded[306] | (expanded[307] << 16))
                    if args.infowin_name:
                        new_r4 = expanded[312] | (expanded[313] << 16)
                        name = "4D G" if (new_r4 + regs[2]) & 0xFFFFFFFF == send_function else "7D8G"
                    elif args.infowin_chain:
                        name = "(direct second pop)"
                    elif args.computed_call:
                        name = "0F4D G" if branch_reg == "r4" else "0F7D8G"
                    else:
                        name = "0F G" if branch_reg == "r4" else "0F8G"
                    detail = f"raw={raw_word:08X}"
                else:
                    name = "ãˆkk3D G" if branch_reg == "r4" else "ãˆkk3D8G"
                    letter_value = ((args.letter_index & 0xFFFF) << 16) | args.letter_item
                    load_reg = regs[1]
                    add_reg = regs[2]
                    load_address = (load_reg + 52) & 0xFFFFFFFF
                    required_word = (letter_value - add_reg) & 0xFFFFFFFF
                    layouts = []
                    record_start = 68  # fixed 64-recipe overlay used by the preserved-map chain
                    stream_offset = load_address - 0x02032800 - record_start
                    if stream_offset >= 0:
                        slot, within = divmod(stream_offset, 0x38)
                        if slot < 64 and within <= 0x34 and not (within & 3):
                            blob_index = defaultdict(list)
                            for source_item in record_items:
                                blob = struct.pack("<28H", *records[source_item])
                                for offset in range(0, 0x38 - 3, 4):
                                    blob_index[(offset, struct.unpack_from("<I", blob, offset)[0])].append(source_item)
                            fixed = {2: 435}
                            fixed.update({4 + index: item for index, item in enumerate(path)})
                            for source_item in blob_index.get((within, required_word), ()):
                                if fixed.get(slot, source_item) == source_item:
                                    layouts.append((slot, source_item, load_address, required_word))
                    detail = (f"attachment={letter_value:08X} load={load_address:08X}/"
                              f"{required_word:08X} layout={layouts[0] if layouts else None}")
                print(f"  records={path} start=A+{start} tokens={token_count} "
                      f"{detail} branch={branch_reg} name={name!r}")
                if args.capture_regs:
                    next_pc = expanded[310] | (expanded[311] << 16)
                    print(f"    next={next_pc:08X}")
                    saved = [expanded[index] | (expanded[index + 1] << 16)
                             for index in range(288, 296, 2)]
                    print("    radar-saved=" + " ".join(
                        f"r{4 + index}={value:08X}" for index, value in enumerate(saved)))
                    print("    regs=" + " ".join(
                        f"r{4 + index}={value:08X}" for index, value in enumerate(regs)))
                for slot, item in enumerate(path):
                    print(f"    {slot}: item={item} " + fmt_source(sources[item][0]))
            return

        event_chain = args.rop_chain == "event"
        item_chain = args.rop_chain == "item"
        letter_chain = args.rop_chain == "letter"
        route_letter_chain = args.rop_chain == "routeletter"
        cmdwrite_chain = args.rop_chain == "cmdwrite"
        equipwrite_chain = args.rop_chain == "equipwrite"
        route_equipwrite_chain = args.rop_chain == "routeequipwrite"
        any_letter_chain = letter_chain or route_letter_chain
        gadget = (0x0200570D if route_equipwrite_chain else
                  0x02002555 if cmdwrite_chain else
                  0x02010251 if event_chain or any_letter_chain else 0x02010201)
        send_function = (0x02001E01 if event_chain else
                         0x02001F39 if item_chain else
                         0x02002129 if args.rop_chain == "cmdslot" else
                         0x020020F9 if equipwrite_chain else
                         0x02001E31)
        name_entry = 0x02035803 if route_letter_chain or cmdwrite_chain or route_equipwrite_chain else 0x02035801
        letter_value = ((args.letter_index & 0xFFFF) << 16) | args.letter_item
        cmd_value = args.cmd_value & 0xFFFF
        record_items = sorted(records)
        record_blobs = {item: struct.pack("<28H", *records[item]) for item in record_items}
        word_index = defaultdict(list)
        word_low_index = defaultdict(list)
        half_index = defaultdict(list)
        if route_letter_chain or cmdwrite_chain or route_equipwrite_chain:
            for item, blob in record_blobs.items():
                for offset in range(0, len(blob) - 3, 4):
                    word = struct.unpack_from("<I", blob, offset)[0]
                    word_index[(offset, word)].append(item)
                    word_low_index[(offset, word & 0xFF)].append((item, word))
                if cmdwrite_chain:
                    for offset in range(0, len(blob) - 1, 2):
                        half_index[(offset, struct.unpack_from("<H", blob, offset)[0])].append(item)
        first_stages = []
        for first in record_items:
            for second in record_items:
                words = records[first] + records[second]
                for start, _, target, _ in pc_hits(words):
                    if target == gadget:
                        first_stages.append((first, second, start))
        print(f"epilogue-gadget first stages={len(first_stages)}")

        chains = []
        for first, second, start in first_stages:
            for third in record_items:
                needed = 302 if route_equipwrite_chain else 314 if cmdwrite_chain else 312
                expanded, _ = expand_row(records[first] + records[second] + records[third], start, needed)
                if len(expanded) < needed:
                    continue
                if expanded[296] | (expanded[297] << 16) != gadget:
                    continue
                reg_start = 288 if route_equipwrite_chain else 304 if cmdwrite_chain else 302
                regs = [expanded[index] | (expanded[index + 1] << 16)
                        for index in range(reg_start, reg_start + 8, 2)]
                pc_index = 300 if route_equipwrite_chain else 312 if cmdwrite_chain else 310
                next_pc = expanded[pc_index] | (expanded[pc_index + 1] << 16)
                arg_ok = True
                if item_chain:
                    arg_ok = regs[2] & 0xFF == 0xA6
                elif args.rop_chain == "cmdslot":
                    arg_ok = regs[2] & 0xFFFF == 72
                elif equipwrite_chain:
                    arg_ok = ((regs[1] & 0xFF) == (args.equip_index & 0xFF) and
                              (regs[2] & 0xFF) == (args.equip_value & 0xFF))
                if route_equipwrite_chain:
                    if next_pc != name_entry:
                        continue
                    if (regs[0] & 0xFF) != (args.equip_index & 0xFF):
                        continue
                    if ((regs[3] + regs[2]) & 0xFFFFFFFF) != 0x02018DB9:
                        continue
                    load_address = (regs[1] + 52) & 0xFFFFFFFF
                    layouts = []
                    buffer_offset = load_address - 0x02032800
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3
                        stream_offset = buffer_offset - record_start
                        if stream_offset < 0:
                            continue
                        slot, within = divmod(stream_offset, 0x38)
                        if slot >= count or within > 0x34 or within & 3:
                            continue
                        for source_item, source_word in word_low_index.get(
                                (within, args.equip_value & 0xFF), ()):
                            if slot < 3 and source_item != (first, second, third)[slot]:
                                continue
                            layouts.append((count, slot, source_item, load_address, source_word))
                    if layouts:
                        chains.append((first, second, third, start, regs,
                                       "えkk7D8G", layouts[0]))
                elif cmdwrite_chain:
                    if next_pc != name_entry:
                        continue
                    restored_r3 = expanded[302] | (expanded[303] << 16)
                    if ((regs[3] + regs[2]) & 0xFFFFFFFF) != 0x0200AD4F:
                        continue
                    if regs[2] & 0xFF != args.cmd_index:
                        continue
                    base_address = (regs[1] + 52) & 0xFFFFFFFF
                    value_address = (restored_r3 + regs[0] + 0xF4) & 0xFFFFFFFF
                    layouts = []
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3

                        def locate(address, size):
                            stream_offset = address - 0x02032800 - record_start
                            if stream_offset < 0:
                                return None
                            slot, within = divmod(stream_offset, 0x38)
                            if slot >= count or within > 0x38 - size or within & (size - 1):
                                return None
                            return slot, within

                        base_loc = locate(base_address, 4)
                        value_loc = locate(value_address, 2)
                        if base_loc is None or value_loc is None:
                            continue
                        base_slot, base_within = base_loc
                        value_slot, value_within = value_loc
                        base_items = set(word_index.get((base_within, restored_r3), ()))
                        value_items = set(half_index.get((value_within, cmd_value), ()))
                        chain_items = (first, second, third)
                        if base_slot < 3:
                            base_items &= {chain_items[base_slot]}
                        if value_slot < 3:
                            value_items &= {chain_items[value_slot]}
                        if base_slot == value_slot:
                            both = base_items & value_items
                            if not both:
                                continue
                            base_item = value_item = min(both)
                        else:
                            if not base_items or not value_items:
                                continue
                            base_item = min(base_items)
                            value_item = min(value_items)
                        layouts.append((count, base_slot, base_item, base_address, restored_r3,
                                        value_slot, value_item, value_address))
                    if layouts:
                        chains.append((first, second, third, start, regs,
                                       "えkk7D8G", layouts[0]))
                elif route_letter_chain:
                    if next_pc != name_entry:
                        continue
                    load_address = (regs[1] + 52) & 0xFFFFFFFF
                    if not (0x02032800 <= load_address <= 0x020357FC) or load_address & 3:
                        continue
                    required_word = (letter_value - regs[2]) & 0xFFFFFFFF
                    layouts = []
                    buffer_offset = load_address - 0x02032800
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3
                        stream_offset = buffer_offset - record_start
                        if stream_offset < 0:
                            continue
                        slot, within = divmod(stream_offset, 0x38)
                        if slot >= count or within > 0x34 or within & 3:
                            continue
                        for source_item in word_index.get((within, required_word), ()):
                            if slot < 3 and source_item != (first, second, third)[slot]:
                                continue
                            layouts.append((count, slot, source_item, load_address, required_word))
                    if not layouts:
                        continue
                    if regs[0] == send_function:
                        chains.append((first, second, third, start, regs, "えkk3D G", layouts[0]))
                    if regs[3] == send_function:
                        chains.append((first, second, third, start, regs, "えkk3D8G", layouts[0]))
                elif letter_chain:
                    if next_pc != name_entry:
                        continue
                    # "kB3D" computes r3 = r6 - r5.  If the callable is in
                    # r7, "cB3D" can instead compute r3 = r6 - r4.  The
                    # harness uses 0x00480001/2, but any popped pair with this
                    # modular difference produces the requested attachment.
                    if (regs[0] == send_function and
                            (regs[2] - regs[1]) & 0xFFFFFFFF == letter_value):
                        chains.append((first, second, third, start, regs, "kB3D G", None))
                    if regs[3] == send_function:
                        if (regs[2] - regs[1]) & 0xFFFFFFFF == letter_value:
                            chains.append((first, second, third, start, regs, "kB3D8G", None))
                        if (regs[2] - regs[0]) & 0xFFFFFFFF == letter_value:
                            chains.append((first, second, third, start, regs, "cB3D8G", None))
                else:
                    if regs[0] == send_function and next_pc == name_entry and arg_ok:
                        chains.append((first, second, third, start, regs, None, None))
                    if regs[3] == send_function and next_pc == name_entry and arg_ok:
                        chains.append((first, second, third, start, regs, None, None))

        print(f"complete smith-to-name {args.rop_chain} chains={len(chains)}")
        for first, second, third, start, regs, chain_name, layout in chains[:args.limit]:
            branch_reg = "r4" if regs[0] == send_function else "r7"
            if any_letter_chain:
                name = chain_name
                argument = args.letter_index
            elif cmdwrite_chain:
                name = chain_name
                argument = f"index={args.cmd_index} value={cmd_value:04X}"
            elif route_equipwrite_chain:
                name = chain_name
                argument = f"index={args.equip_index & 0xFF:02X} value={args.equip_value & 0xFF:02X}"
            elif equipwrite_chain:
                name = "(F1F G" if branch_reg == "r4" else "(F1F8G"
                argument = f"index={args.equip_index & 0xFF:02X} value={args.equip_value & 0xFF:02X}"
            elif args.rop_chain == "cmdslot":
                name = "qB G" if branch_reg == "r4" else "qB8G"
                argument = -72
            else:
                name = "1F2F G" if branch_reg == "r4" else "1F2F8G"
                argument = regs[2] & 0xFF
            print(f"  records=({first},{second},{third}) start=A+{start} "
                  f"name={name!r} arg={argument} branch={branch_reg}")
            print("    regs=" + " ".join(f"r{4 + i}={value:08X}" for i, value in enumerate(regs)))
            print("    A source: " + fmt_source(sources[first][0]))
            print("    B source: " + fmt_source(sources[second][0]))
            print("    C source: " + fmt_source(sources[third][0]))
            if layout is not None and cmdwrite_chain:
                (count, base_slot, base_item, base_address, base_value,
                 value_slot, value_item, value_address) = layout
                print(f"    list count={count} base slot={base_slot} record={base_item} "
                      f"loads {base_value:08X} at {base_address:08X}")
                print("    base source: " + fmt_source(sources[base_item][0]))
                print(f"    value slot={value_slot} record={value_item} "
                      f"loads {cmd_value:04X} at {value_address:08X}")
                print("    value source: " + fmt_source(sources[value_item][0]))
            elif layout is not None:
                count, slot, source_item, load_address, required_word = layout
                print(f"    list count={count} slot={slot} record={source_item} "
                      f"loads {required_word:08X} at {load_address:08X}")
                print("    load source: " + fmt_source(sources[source_item][0]))
        return

    hits = []
    record_items = sorted(records)
    for first in record_items:
        for second in record_items:
            words = records[first] + records[second]
            for start, address_at, target, _ in pc_hits(words):
                hits.append((target, first, second, start, address_at, words[start:address_at + 2]))

    hits.sort()
    print(f"two-record EWRAM PC hits={len(hits)}")
    for target, first, second, start, address_at, tokens in hits[:args.limit]:
        where = f"A+{start}" if start < 28 else f"B+{start - 28}"
        address_where = f"A+{address_at}" if address_at < 28 else f"B+{address_at - 28}"
        print(f"target={target:08X} records=({first},{second}) tokens={where}..{address_where + '+1'}")
        print("  " + " ".join(f"{word:04X}" for word in tokens))
        print("  A source: " + fmt_source(sources[first][0]))
        print("  B source: " + fmt_source(sources[second][0]))


if __name__ == "__main__":
    main()
