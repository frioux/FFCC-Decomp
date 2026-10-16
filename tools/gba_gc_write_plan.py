#!/usr/bin/env python3
"""Build the GC-side packet plan for the FFCC two-port arbitrary write.

The plan assumes radar/name execution can call the retail GBA Link_Write
function with a controlled r0.  It uses channel 2's character-name overflow
to set channel 3's signed packet counter, then uses two channel-3 name
continuations to replace Joybus receive-buffer 3's length.  A channel-3
letter continuation consequently writes three chosen bytes at any MEM1
address.
"""

import argparse


REGIONS = {
    "PAL": {"joybus": 0x802EAAC0, "cflat": 0x80299F60, "game": 0x8021EEC0},
    "EN": {"joybus": 0x802E9A80, "cflat": 0x80298F20, "game": 0x8021DE80},
    "JP": {"joybus": 0x803063B0, "cflat": 0x802B5860, "game": 0x8023A7C0},
}

RECV3_LENGTH_OFFSET = 0xC98
RECV3_PAYLOAD_OFFSET = 0xCA0
CHANNEL3_WRITE_COUNT = 0xC02C  # signed -16340; starts one byte before length


def number(text: str) -> int:
    return int(text, 0)


def packet(op: int, a: int = 0, b: int = 0, c: int = 0) -> int:
    return op | (a << 8) | (b << 16) | (c << 24)


def show_packet(port: int, word: int, note: str) -> None:
    wire = word.to_bytes(4, "little").hex(" ").upper()
    print(f"  port {port}: 0x{word:08X}   [{wire}]   {note}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--region", choices=REGIONS, required=True)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument("--target", type=number,
                        help="address at which the three payload bytes begin")
    choice.add_argument("--cflat-pointer", type=number,
                        help="runtime value read from CFlat+0x0C")
    choice.add_argument("--script",
                        help="stage a direct CGame script change (run while paused)")
    parser.add_argument("--data", default="00000e",
                        help="three payload bytes, hex (default: 00000e)")
    parser.add_argument("--global-index", type=number, default=59,
                        help="CFlat G index with --cflat-pointer (default: 59)")
    parser.add_argument("--value", type=number, default=14,
                        help="24-bit G value with --cflat-pointer (default: 14)")
    args = parser.parse_args()

    region = REGIONS[args.region]
    recv_length = region["joybus"] + RECV3_LENGTH_OFFSET
    recv_payload = region["joybus"] + RECV3_PAYLOAD_OFFSET

    if args.cflat_pointer is not None:
        if not 0 <= args.value <= 0xFFFFFF:
            parser.error("--value must fit in 24 bits")
        global_address = args.cflat_pointer + args.global_index * 4
        target = global_address + 1
        data = args.value.to_bytes(4, "big")[1:]
        target_note = (
            f"G{args.global_index} at 0x{global_address:08X}; this preserves its "
            "existing high byte, which must be 00"
        )
    elif args.script is not None:
        try:
            script = args.script.encode("ascii")
        except UnicodeEncodeError:
            parser.error("--script must be ASCII")
        if len(script) >= 256:
            parser.error("--script must be shorter than 256 bytes")
        target = region.get("game", 0) + 0xCB28
        if target == 0xCB28:
            parser.error("internal region table has no Game address")
        # m_newGameFlag=1, m_nextScriptFlags=0, then m_nextScript.m_name.
        # The flag becomes live before the final name packets, so these words
        # must be delivered while the main game loop is paused.
        data = b"\x00\x00\x00\x01\x00\x00\x00\x00" + script + b"\x00"
        data += b"\x00" * (-len(data) % 3)
        target_note = (
            f"CGame pending-script block; loads {args.script!r} after unpause"
        )
    else:
        target = args.target
        try:
            data = bytes.fromhex(args.data)
        except ValueError as exc:
            parser.error(str(exc))
        if len(data) != 3:
            parser.error("--data must contain exactly three bytes")
        target_note = "generic three-byte target"

    length = (target - recv_payload) & 0xFFFFFFFF
    length_bytes = length.to_bytes(4, "big")

    print(f"Region: {args.region}")
    print(f"Joybus recv3 length:  0x{recv_length:08X}")
    print(f"Joybus recv3 payload: 0x{recv_payload:08X}")
    print(f"Write target:          0x{target:08X} ({target_note})")
    print(f"Payload:               {data.hex(' ').upper()}")
    print(f"Forged signed length:  0x{length:08X}")
    print()
    print("Raw Link_Write calls, allowing GC processing between every word:")
    show_packet(2, packet(0x1C), "character-name header; packetCount = 1")
    for index in range(9):
        show_packet(2, packet(0x5C), f"zero continuation {index + 1}/9")
    show_packet(2, packet(0x5C, 0x01, 0x00, 0xC0),
                "continuation 10: channel 3 active/result/count high")
    show_packet(2, packet(0x5C, 0x2C, 0x00, 0x00),
                "continuation 11: channel 3 count low/CRC")
    show_packet(3, packet(0x5C, 0x00, length_bytes[0], length_bytes[1]),
                "count C02C: preceding byte + receive length[0:2]")
    show_packet(3, packet(0x5C, length_bytes[2], length_bytes[3], 0x00),
                "count C02D: receive length[2:4] + cmdFlags=0")
    for offset in range(0, len(data), 3):
        chunk = data[offset:offset + 3]
        show_packet(3, packet(0x55, *chunk),
                    f"orphan letter continuation: write target+0x{offset:X}")
    show_packet(3, packet(0x15), "optional cleanup: clear the forged receive buffer")


if __name__ == "__main__":
    main()
