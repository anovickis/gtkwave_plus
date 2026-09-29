# Known gaps / feature TODOs

Tracking known gaps relative to commercial waveform viewers (Verdi/nWave),
raised during day-to-day use of this fork. Not scoped or prioritized yet.

- **No source browsing.** Cross-probing jumps to a single file:line (when
  the loaded file has embedded source-stem info), but there's no way to
  browse the RTL source tree more broadly from within gtkwave.
- **No signal bit-slicing.** Can't select a sub-range of bits from a
  multi-bit bus (e.g. `bus[7:4]`) and display it as its own trace.
- **No SystemVerilog struct/union support.** Struct/union-typed signals
  don't display as an expandable tree of named fields.
- **No logical/arithmetic operations on signals.** No way to create a
  derived/virtual signal from an expression over existing signals (e.g.
  `sig_a AND sig_b`).
- **Search is hard to use.** The existing signal search UX needs rework.
- **No cone-of-logic (fan-in) view.** No way to see everything that
  transitively feeds a signal's value.
- **No driver view.** No way to jump to/see the immediate driver(s) of a
  signal (the assign/always block that sets it).
- **No fan-out view.** The inverse of cone-of-logic: no way to see
  everything a signal transitively affects downstream.
- **No load/reader view.** The inverse of driver view: no way to see
  everything that reads/consumes a signal.
- **No schematic view.** No visual schematic/structural diagram of the
  logic around a signal.
