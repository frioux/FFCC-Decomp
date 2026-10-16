# MWCC register colouring

This note describes how MWCC picks callee-saved registers, as observed with
[cadmic/mwcc-debugger](https://github.com/cadmic/mwcc-debugger) on PAL game code.
It explains when declaration-order sweeps can and cannot move a residual, and
how to read a pure register-numbering diff before spending builds on it.

## How it was observed

mwcc-debugger runs `mwcceppc.exe` under retrowin32 + gdb and dumps the backend
IR between passes, plus the register allocator's interference graph
(`regalloc-{gpr,fpr}-pass-1-all.txt`, with every virtual register, its physical
register and its neighbours) and the assignment order
(`regalloc-*-pass-1-assigned.txt`). The debugger supports GC/2.6. For
`pppYmMelt.cpp` with the unit's exact flags, GC/2.6 produced the same
colouring as the project's GC/2.5, so the dumps describe the shipped
build. One run takes about 90 seconds in WSL2.

## The model

1. **Virtual register numbering.** Parameters get the first virtual registers.
   Next come the function's named locals, in reverse declaration order. The
   frontend temporaries (`@N` objects) follow, newest first.
    - Locals of an inlined function become `@N` temporaries when it is
      expanded. They are numbered in reverse declaration order, so among
      themselves they keep declaration order.
    - Temporaries created later by frontend CSE or copy propagation are newer
      than the inlined locals and get lower virtual registers. A copy of an
      inline argument made by copy propagation is one of these.
2. **High-degree nodes first.** Nodes with at least K neighbours are coloured
   first, in descending virtual-register order. K is 32 for FPRs. For GPRs it
   fell between 28 and 34 in the measured function.
3. **Everything else** is coloured in descending virtual-register order. Each
   node takes the highest free callee-saved register (r31 or f31 downwards).
   Volatile registers are used when the value does not live across a call.

For named locals this gives the familiar declaration-order rule: the first
declared local gets the highest register. Inlined locals do the same among
themselves. The rule breaks across groups. Named locals always sit below the
frontend temporaries and the inlined locals in numbering. A named value can
only rank above them by being high-degree, i.e. overlapping at least K other
values.

Practical consequences:

- Moving a named local in the declaration list is inert when the only locals
  it could swap with are in another register class or never interfere with it.
- When a residual rotates a named local against inlined locals or compiler
  temporaries, declaration order cannot fix it. What can is changing which
  nodes are high-degree (how many values are live at the same time), or which
  values are temporaries and which are named.
- Count `neighbors` in the `-all` dump for the nodes involved. A node just
  under K becomes high-degree when one more value overlaps it, and it then
  jumps to the top of the order.

## Worked example: `pppFrameYmMelt`

`pppFrameYmMelt` inlines `InitPolygonData(PYmMelt*, VERTEX_DATA*, short)`; the
EN MAP keeps it as `InitPolygonData__FP7PYmMeltP11VERTEX_DATAs`. At 99.500%,
all 15 differing rows are register numbers inside the inlined loops.

| Value | Retail | Ours |
| --- | --- | --- |
| `phaseOffset` (inline argument) | r27 | r25 |
| `rowVertex` | r26 | r26 |
| `vertex` | r25 | r27 |
| `x` | f30 | f30 |
| `step` | f29 | f29 |
| `halfWidth` | f28 | f28 |
| `matrixY` (named local of the caller) | f27 | f24 |
| `-halfWidth` copy (inner loop start) | f26 | f26 |
| `rot` | f25 | f27 |
| `z` | f24 | f25 |

In our build no FP node reaches K (`matrixY` has 31 neighbours, `halfWidth` 30,
`x` 22). The FPRs therefore follow pure numbering, and `matrixY`, the only named
FP local, ends up at the bottom. Retail's order fits the model only if zero,
`x`, `step`, `halfWidth` and `matrixY` are high-degree and `-halfWidth`, `rot`
and `z` are not. So retail has more FP values live at once in this region. On
the GPR side, retail's `phaseOffset` temporary has to be older than the inlined
locals, and `vertex` has to be declared before `rowVertex`.

Measured with an out-of-tree scorer (exact unit flags, objdiff against the
retail object), none above 99.500%:

| Experiment | Variants | Result |
| --- | --- | --- |
| Every declaration order of the inline's seven locals, `Mtx` first or last | 10080 | Only `rot`, `halfWidth` and `step` move |
| Caller locals moved to every position, five call spellings | 160 | Byte-identical to the baseline |
| `rot` as variable, in-call expression or block local; `z`/`x` scope; `halfWidth` in one or two statements; parameter instead of the `vertex` local | 336 | Best is the baseline |

Writing `rot` inside the `PSMTXRotRad` call puts `rot`, `-halfWidth` and `z`
exactly on retail's f25, f26 and f24. There `matrixY` reaches 32 neighbours
and jumps to f31 (99.32%). The next thing to try is that form plus FP values
that stay live across the loops, checked against the neighbour counts above.
