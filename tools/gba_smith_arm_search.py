#!/usr/bin/env python3
"""Generate retail-representable PAL Smith ARM chains, then verify them elsewhere.

This is a fast candidate generator for the exact ARM entry established by the
JoyBus harness.  Every action is one complete 0x38-byte Smith record obtainable
with a zero-material wrong craft in the supplied MEM1 snapshot.  Unicorn is
used only to prune millions of impossible record combinations; any reported
chain must still pass ``gba_joybus_harness`` on the untouched retail client.
"""

import argparse
import struct
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_R12,
    UC_ARM_REG_SP,
)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gba_smith_radar_scan import Mem1, smith_record, wrong_craft_sources


EWRAM = 0x02000000
IWRAM = 0x03000000
IO = 0x04000000
LINK_WRITE = 0x02002698
REG_JOY_TRANS = 0x04000154
S_TX_COUNT = 0x03000190
DMA0 = 0x040000B0
CPU_STATE_WORDS = 16
REGISTERS = (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR,
)


def craftable_records(mem, item_table, max_material_deletes=0):
    sources = {}
    for tribe in range(4):
        for item_id, rows in wrong_craft_sources(
                mem, item_table, tribe, max_material_deletes).items():
            sources.setdefault(item_id, []).extend((tribe, *row) for row in rows)
    records = {}
    aliases = {}
    for item_id in sorted(sources):
        words = smith_record(mem, item_table, item_id, 100)
        if words is None or item_id > 0x7FFF:
            continue
        raw = struct.pack("<28H", *words)
        if raw not in aliases:
            records[item_id] = raw
            aliases[raw] = [item_id]
        else:
            aliases[raw].append(item_id)
    return records, aliases, sources


class ArmSearch:
    def __init__(self, client):
        self.client = client
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(EWRAM, 0x40000)
        self.uc.mem_map(IWRAM, 0x8000)
        self.uc.mem_map(IO, 0x1000)
        # Bad candidate stores are rejected by the write hook.  Mapping the
        # two common wrap pages keeps such a rejected store from poisoning
        # Unicorn's next run with an unmapped-memory exception.
        self.uc.mem_map(0x00000000, 0x1000)
        self.uc.mem_map(0xFFFFF000, 0x1000)
        self.uc.mem_write(EWRAM, client)
        self.status = None
        self.end = 0
        self.allowed_start = 0
        self.allowed_end = 0
        self.writes = []
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        self.pristine_context = self.uc.context_save()

    def _code(self, uc, address, size, _):
        if LINK_WRITE <= address < LINK_WRITE + 0x7C:
            self.status = ("link-write", address)
            uc.emu_stop()
        elif address == self.end:
            self.status = ("end", address)
            uc.emu_stop()
        elif not self.allowed_start <= address < self.allowed_end:
            self.status = ("branch", address)
            uc.emu_stop()

    def _write(self, uc, _access, address, size, value, _):
        try:
            old = bytes(uc.mem_read(address, size))
        except UcError:
            old = None
        self.writes.append((address, old))
        if address <= REG_JOY_TRANS < address + size:
            self.status = ("joy-write", address, size, value)
        elif address <= S_TX_COUNT < address + size:
            self.status = ("count-write", address, size, value)
        elif address < DMA0 + 12 and address + size > DMA0:
            control_enable = ((size == 4 and address <= DMA0 + 8 and
                               address + size >= DMA0 + 12 and value & 0x80000000) or
                              (size == 2 and address == DMA0 + 10 and value & 0x8000))
            if control_enable and uc.mem_read(DMA0 + 4, 4) == REG_JOY_TRANS.to_bytes(4, "little"):
                self.status = ("dma-joy", address, size, value)
                uc.emu_stop()
            return
        else:
            self.status = ("memory-write", address, size, value)
        uc.emu_stop()

    def restore_writes(self):
        for address, old in reversed(self.writes):
            if old is not None:
                self.uc.mem_write(address, old)
        self.writes.clear()

    def set_state(self, state):
        # Select the ARM mode before restoring banked SP/LR.
        self.uc.reg_write(UC_ARM_REG_CPSR, state[CPU_STATE_WORDS - 1])
        for register, value in zip(REGISTERS[:-1], state[:CPU_STATE_WORDS - 1]):
            self.uc.reg_write(register, value)
        dma = state[CPU_STATE_WORDS:] if len(state) > CPU_STATE_WORDS else (0, 0, 0)
        for index, value in enumerate(dma):
            self.uc.mem_write(DMA0 + index * 4, value.to_bytes(4, "little"))

    def state(self):
        cpu = tuple(self.uc.reg_read(register) & 0xFFFFFFFF for register in REGISTERS)
        dma = tuple(int.from_bytes(self.uc.mem_read(DMA0 + index * 4, 4), "little")
                    for index in range(3))
        return cpu + dma

    def run_first(self, raw, target, record_offset):
        payload = raw[record_offset:]
        self.uc.mem_write(target, payload)
        self.uc.ctl_remove_cache(target, target + len(payload))
        state = (
            0x00040000, 0x30000010, 0x040000B0, 0x00001000,
            0, 0, 0x00010000, 0x35012700, 0, 0, 0, 0,
            0x040000B0, 0x03007F00, 0x0203FF01, 0x4000001F,
            0, 0, 0,
        )
        end = target + (len(payload) & ~3)
        return self.run(state, target, end, payload[-1])

    def run_next(self, state, start, raw):
        self.uc.mem_write(start + 1, raw)
        self.uc.ctl_remove_cache(start, start + 1 + len(raw))
        return self.run(state, start, start + 56, raw[-1])

    def run(self, state, start, end, tail):
        self.status = None
        self.writes.clear()
        self.end = end
        self.allowed_start = start
        self.allowed_end = end
        self.uc.context_restore(self.pristine_context)
        self.set_state(state)
        self.uc.reg_write(UC_ARM_REG_PC, start)
        try:
            begin = start | (1 if state[CPU_STATE_WORDS - 1] & 0x20 else 0)
            self.uc.emu_start(begin, 0, count=120)
        except OSError:
            # Some nonsensical ARM words can trip a Unicorn backend bug even
            # though they would merely kill this candidate on hardware.  A
            # fresh engine keeps that rejected candidate from contaminating
            # the remaining search.
            self.__init__(self.client)
            return ("engine-reset", start), None, tail
        except UcError:
            self.status = self.status or ("fault", self.uc.reg_read(UC_ARM_REG_PC))
        result = self.status
        next_state = self.state()
        self.restore_writes()
        # Keep system mode, but admit a legitimate BLX transition to Thumb.
        # Random MSR-like record words can select reserved modes that are
        # neither useful on hardware nor safe to feed back into Unicorn.
        if result and result[0] == "end" and (next_state[CPU_STATE_WORDS - 1] & 0x1F) == 0x1F:
            return result, next_state, tail
        if result and result[0] in ("link-write", "joy-write", "count-write", "dma-joy"):
            return result, next_state, tail
        return result, None, tail


def score_state(state):
    regs = state[:15]
    score = 0
    if state[CPU_STATE_WORDS - 1] & 0x20:
        score += 500
    score += 5000 * sum(value != 0 for value in state[CPU_STATE_WORDS:])
    dma_source, dma_destination, dma_control = state[CPU_STATE_WORDS:]
    if 0x02000000 <= dma_source < 0x02040000:
        score += 3000
    dma_distance = abs(dma_destination - REG_JOY_TRANS)
    score += max(0, 12000 - dma_distance // 16)
    if dma_destination == REG_JOY_TRANS:
        score += 20000
    if dma_control & 0x80000000:
        score += 10000
    if regs[0] == 0x00040055:
        score += 10000
    elif regs[0] & 0xFF == 0x55:
        score += 1000
    if LINK_WRITE in regs or LINK_WRITE + 1 in regs:
        score += 5000
    link_distance = min(abs(value - (LINK_WRITE + 1)) for value in regs)
    score += max(0, 2000 - link_distance // 16)
    destination_distance = abs(regs[12] - REG_JOY_TRANS)
    score += max(0, 12000 - destination_distance // 16)
    if regs[12] == REG_JOY_TRANS:
        score += 20000
    if 0x02000000 <= regs[11] < 0x02040000:
        score += 5000
    store_bases = (REG_JOY_TRANS, REG_JOY_TRANS + 0x6A,
                   REG_JOY_TRANS + 0x329, REG_JOY_TRANS + 0xB00)
    distance = min(abs(regs[2] - target) for target in store_bases)
    score += max(0, 2000 - min(distance, 2000))
    if regs[2] in (DMA0 + 4, DMA0 + 8):
        score += 10000
    if ((0x02000000 <= regs[5] < 0x0203FFC0) or
            (0x03000000 <= regs[5] < 0x03007FC0)):
        # Record 4146 has the cleanest crafted ARM->Thumb transition, but its
        # Thumb tail begins with three LDM r5 operations.
        score += 1000
    for value in regs:
        if value in (REG_JOY_TRANS, S_TX_COUNT, 0x03000090):
            score += 2000
        if 0x02000000 <= value < 0x02038000:
            score += 40
        if 0x04000000 <= value < 0x04000400:
            score += 20
    if state[2] == 0x040000B0 or state[12] == 0x040000B0:
        score += 10
    return score


def source_text(source):
    tribe_names = ("clavat", "lilty", "yuke", "selkie")
    tribe, gil, price, material, count = source
    return f"{tribe_names[tribe]} gil={gil} price={price} material={material} count={count}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mem1", type=Path, required=True)
    parser.add_argument("--item-table", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--client", type=Path,
                        default=Path("orig/GCCP01/files/dvd/gba/ffcc_cli.bin"))
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--beam", type=int, default=2000)
    parser.add_argument("--max-material-deletes", type=int, default=0)
    parser.add_argument("--branch-slot", type=lambda value: int(value, 0), default=0xE1,
                        choices=(0xE0, 0xE1, 0xE2),
                        help="legal PAL name byte selecting DOWNLOAD_BUF landing (default: E1)")
    args = parser.parse_args()

    mem = Mem1(args.mem1.read_bytes())
    records, aliases, sources = craftable_records(
        mem, args.item_table, args.max_material_deletes)
    actions = list(records.items())
    search = ArmSearch(args.client.read_bytes())
    target = 0x0200020C + args.branch_slot * 0x400
    source_offset = target - 0x02038000 + 31
    first_record, record_offset = divmod(source_offset, 56)
    print(f"items={sum(len(value) for value in aliases.values())} unique-records={len(actions)} "
          f"slot={args.branch_slot:02X} target={target:08X} "
          f"first-record={first_record} byte={record_offset}")

    states = {}
    hits = []
    fixed_layout = {9: 407, 17: 1369, 18: 13568, 19: 416}
    first_actions = ([(fixed_layout[first_record], records[fixed_layout[first_record]])]
                     if first_record in fixed_layout else actions)
    for item_id, raw in first_actions:
        result, state, tail = search.run_first(raw, target, record_offset)
        if result and result[0] in ("link-write", "joy-write", "count-write", "dma-joy"):
            hits.append(((item_id,), result, state))
        elif state is not None:
            key = (state, tail)
            states.setdefault(key, (item_id,))
    print(f"depth=1 fallthrough-states={len(states)} hits={len(hits)}")

    boundary = target + ((56 - record_offset) & ~3)
    for depth in range(2, args.depth + 1):
        inventory_record = first_record + depth - 1
        if inventory_record >= 64:
            print(f"stopping before inventory record {inventory_record}: Smith inventory capacity reached")
            break
        stage_actions = ([(fixed_layout[inventory_record], records[fixed_layout[inventory_record]])]
                         if inventory_record in fixed_layout else actions)
        next_states = {}
        for (state, _tail), chain in states.items():
            for item_id, raw in stage_actions:
                result, new_state, tail = search.run_next(state, boundary, raw)
                if result and result[0] in ("link-write", "joy-write", "count-write", "dma-joy"):
                    hit = (chain + (item_id,), result, search.state())
                    hits.append(hit)
                    print(f"HIT depth={depth} chain={hit[0]} result={result} r0={hit[2][0]:08X}")
                elif new_state is not None:
                    key = (new_state, tail)
                    next_states.setdefault(key, chain + (item_id,))
        ranked = sorted(next_states.items(), key=lambda row: score_state(row[0][0]), reverse=True)
        states = dict(ranked[:args.beam])
        boundary += 56
        best = score_state(ranked[0][0][0]) if ranked else -1
        dma_counts = tuple(sum(state[CPU_STATE_WORDS + index] != 0
                               for state, _tail in next_states)
                           for index in range(3))
        print(f"depth={depth} unique-states={len(next_states)} kept={len(states)} "
              f"hits={len(hits)} best-score={best} dma-states={dma_counts}")
        if hits or not states:
            break

    for chain, result, state in hits:
        print(f"chain={','.join(map(str, chain))} result={result} r0={state[0]:08X}")
        for item_id in chain:
            print(f"  item={item_id} {source_text(sources[item_id][0])}")
    if not hits:
        for (state, tail), chain in sorted(
                states.items(), key=lambda row: score_state(row[0][0]), reverse=True)[:10]:
            shown = " ".join(f"r{index}={value:08X}" for index, value in enumerate(state[:13]))
            print(f"candidate score={score_state(state)} chain={','.join(map(str, chain))} "
                  f"tail={tail:02X} cpsr={state[CPU_STATE_WORDS - 1]:08X} {shown} "
                  f"dma={state[CPU_STATE_WORDS]:08X},{state[CPU_STATE_WORDS + 1]:08X},"
                  f"{state[CPU_STATE_WORDS + 2]:08X}")


if __name__ == "__main__":
    main()
