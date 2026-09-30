# Known gaps / feature TODOs

Tracking known gaps relative to commercial waveform viewers (Verdi/nWave),
raised during day-to-day use of this fork, plus a systematic gap list
pulled from the installed Verdi nWave/nSchema manuals. Rough priority
order within each section; not formally scoped.

## Done

- **Driver view.** Right-click a signal -> Quick Format... -> Show
  Drivers.
- **Fan-out / load view.** Show Loads (full transitive fan-out cone, not
  just immediate reads).
- **Cone-of-logic (fan-in) view.** Show Cone of Logic (text list) and Show
  Schematic (rendered diagram), both via GwNetlistQuery -> slang-netlist.
- **Schematic view (first cut).** Graphviz-rendered fan-in cone with live
  value annotation on each node, re-rendered whenever the cursor/primary
  marker moves. Static image only so far -- see the interactivity gaps
  below.
- **Signal Event Report.** Rising/falling edge counts, duty cycle, longest
  continuous-high span, over the primary<->baseline marker span.
- **Live value annotation in the source cross-probe panel** when the
  cursor moves, for signals already displayed in the waveform.

## Open -- original gaps

- **No source browsing.** Cross-probing jumps to a single file:line, but
  there's no way to browse the RTL source tree more broadly.
- **No signal bit-slicing.** Can't select a sub-range of bits from a
  multi-bit bus and display it as its own trace. (See also Bus Operations
  below -- a real fix should probably be a superset of this.)
- **No SystemVerilog struct/union support.** Struct/union-typed signals
  don't display as an expandable tree of named fields.
- **No logical/arithmetic operations on signals.** No way to create a
  derived/virtual signal from an expression over existing signals.
- **Search is hard to use.** See the concrete search-UX design pulled from
  Verdi below.

## Open -- schematic view interactivity (from the Verdi nSchema manual)

The current schematic is a single static re-rendered image per click; none
of these need slang-netlist changes, just UI work on top of what's there.

- **Real click-to-expand navigation.** Push into an instance's internals
  in place (replacing the current view) / pop back up, instead of always
  re-rendering the whole cone fresh.
- **Collapse/expand a module to a black box** via a corner icon, showing
  only its connected nets.
- **Double-click a net to isolate its driver->load path**, step through
  multiple loads with n/N.
- **Right-click menus scoped per element type** (signal vs. instance vs.
  net) instead of one static popover.
- **Add to Waveform / drag a schematic signal into the wave pane** --
  schematic currently only cross-probes to *source*, not to the waveform.
- **Recolor net lines by logic value**, not just the value-label text;
  toolbar buttons to step the annotated time to the next/previous
  transition of the selected signal without leaving the schematic.
- **Highlight incomplete/unresolved nets** (dashed line) -- valuable given
  slang-netlist's own coverage is still growing.
- **Manual per-signal recolor + "color instances by scope"** bulk
  highlighting.
- **Depth/bit-range-limited tracing + a tabular trace report.** Real
  scalability concern: unbounded fan-in cones on a big design will get
  unwieldy fast.
- **Zoom/pan controls + a navigation-only "last view" undo**, separate
  from any future structural undo/redo.
- **A separate editable/exportable schematic mode** (reposition instances,
  print/capture to file) rather than exporting the auto-layout directly.
- **Configurable color themes** for schematic elements (nets, symbols,
  annotation values), dark/light aware.

## Open -- general nWave features (from the Verdi manual)

- **Search UX rework**, concretely: exact/wildcard bus-value search, a
  "stable for >= N time units" constraint, jump to the Nth occurrence of a
  match.
- **Event Sequence pane**: list every glitch/sub-transition within a
  single simulation timestamp (delta-cycle detail), Waveform/Grid display
  modes.
- **Compress Time Range**: collapse one or more uninteresting time spans
  to a "+" marker; click to re-expand.
- **Group Manager**: a real dialog for nested signal groups (auto-named
  subgroups, bulk "move to group").
- **Bus Operations suite**: Create Bus (assemble a new bus from arbitrary
  signals/bits), Edit/Adjust/Reverse Bus, Expand-as-sub-bus -- a superset
  of the plain bit-slicing gap above.
- **List X / Trace X**: scan the loaded waveform for X (unknown) regions,
  then trace backward from a specific occurrence to its root-cause driver
  -- reuses the existing driver-query backend, different entry point
  (seeded from a bad value, not a signal name).
- Lower priority, noted but not detailed: Auto Update (live-tail a
  still-running simulation), time-span Comments (distinct from point
  markers), Waveform Compare / structured two-run diffing, Dense Block
  Drawing (fast struct/MDA rendering), Analog Waveform view.
