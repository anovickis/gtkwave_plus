# Known gaps / feature TODOs

Tracking known gaps relative to commercial waveform viewers (Verdi/nWave).
Single prioritized worklist, in the order I'd actually tackle it -- not a
flat category dump. Rationale given wherever the ranking isn't obvious.

## Done

- **Driver view.** Right-click a signal -> Quick Format... -> Show
  Drivers.
- **Fan-out / load view.** Show Loads (full transitive fan-out cone, not
  just immediate reads).
- **Cone-of-logic (fan-in) view.** Show Cone of Logic (text list) and Show
  Schematic (rendered diagram), both via GwNetlistQuery -> slang-netlist.
- **Schematic view (first cut).** Graphviz-rendered fan-in cone with live
  value annotation on each node, re-rendered whenever the cursor/primary
  marker moves.
- **Signal Event Report.** Rising/falling edge counts, duty cycle, longest
  continuous-high span, over the primary<->baseline marker span.
- **Live value annotation in the source cross-probe panel** when the
  cursor moves, for signals already displayed in the waveform.
- **Gate-level schematic (SVG), via Yosys.** A second, distinct schematic
  view -- real synthesized AND/XOR/NOT/DFF cells instead of the RTL-level
  Assignment/Conditional dataflow graph the first one shows. No live value
  annotation (Yosys's post-techmap names don't map back to RTL signals).
  Known gap: requires the configured top module to be synthesizable RTL --
  a plain testbench top with an unguarded `$finish` in an `initial` block
  will fail to elaborate; auto-detecting the real DUT under a testbench
  wrapper isn't implemented.
- **Depth/module-boundary-limited tracing on the fan-in/fan-out queries.**
  `GwNetlistQueryLimits{max_depth, module_boundary_only}`, applied
  client-side (no slang-netlist changes) and surfaced as a Max depth
  spinner + "Stop at module boundary" checkbox + Refresh button in the
  Show Loads / Show Cone of Logic results windows, defaulting to
  unlimited so existing behavior is unchanged unless a user opts in.
  Bit-range limiting was *not* added here -- it turned out `--drivers
  name[bit-range]` already works untouched by just passing a bit-range-
  suffixed signal_path straight through (no code change needed); Show
  Drivers' UI doesn't expose that yet, but the backend already supports it.
- **Make the schematic actually clickable.** Real per-node pixel bounding
  boxes parsed from Graphviz's `-Tplain` output (not a flat PNG), hit-
  tested against mouse clicks via a `GtkEventBox` over the `GtkImage`.
  Left-click reports the node's signal path/label in the status bar,
  double-click on a signal node re-renders the schematic centered on it
  (push/pop-into-instance navigation), right-click opens a context menu
  scoped to that node (Show Drivers / Show Loads). Structural nodes
  (Assignment/Conditional, no backing signal) report as non-clickable
  rather than silently no-opping. Along the way, found and fixed a real
  pre-existing crash in `bsearch_node()` (out-of-bounds `harray[1]` read
  for any node with only one history entry, e.g. a signal that never
  transitions) -- this is what unblocked verifying the clicking code
  end-to-end.
  Not done yet, deferred to the polish pass below: collapse/expand a
  module to a black box, and Add to Waveform / drag a schematic signal
  into the wave pane.
- **Search UX rework.** Extended the existing Pattern Search dialog
  (`strace.c`) rather than building a new one: a new `ST_VALUE_EXACT`
  search type does case-insensitive exact/wildcard matching (`*`/`?`) on
  a signal's ascii value, scalar or vector; a "Stable for >=" spinner
  rejects a candidate match unless it holds for at least N time units
  (`candidate_is_stable()`, shared by Fwd/Bkwd and Mark); a "Jump to
  occurrence #" spinner + Go button searches from the start of the time
  range and stops directly on match #N. Verified via gdb forced calls
  against a live design (regression-checked the existing Fwd/Bkwd path
  first, then each new piece) since driving the actual GTK comboboxes
  isn't practical headless.
  Known gap found along the way, not fixed (pre-existing, unrelated to
  this change): WCP's `add_items` failed to resolve a multi-bit vector
  signal path (e.g. `tb.value`) even though the identical scalar-signal
  form worked fine. Root-caused later (see List X below): it needs
  gtkwave's own internal bit-range-suffixed name with no space before the
  bracket (`tb.value[3:0]`, not `tb.value` or `tb.value [3:0]`) --
  a real usability gap (this internal naming isn't discoverable from the
  VCD's own declared name), not a crash; still not fixed, since
  `add_items`'s contract for what a caller should pass is a product
  decision, not obviously mine to make unilaterally.
- **List X.** Search > List X...: scans every currently-displayed,
  non-blank trace's full history for unknown/X-like values (same
  definition `strace.c`'s `ST_X` search type uses), lists each contiguous
  bad-value interval with a "Go To" (jump marker + viewport) and "Trace
  Drivers" (seeds the existing `netlist_query_show_drivers()` backend
  from the bad signal, disabled with a tooltip when no full hierarchical
  path is resolvable). Covers both a real constructed/auto-coalesced
  vector (`GwBitVector`) and, more commonly, a natively-declared
  multi-bit VCD var (a `GwNode` with `extvals` set) -- these are NOT the
  same representation despite both being "multi-bit," so both needed
  their own scan path. Verified via gdb against a synthetic VCD fixture
  with known X regions; caught and fixed two real bugs in the process
  (see the commit) -- a missing lower time-bound that matched gtkwave's
  synthetic pre-dump-zero sentinel entries as false positives, and an
  initial version that skipped every `extvals` node entirely (assuming
  it meant real/string-only), which silently dropped ordinary
  natively-declared multi-bit signals, the common case.

## Up next, in order

1. **Recolor schematic net lines by logic value** (not just the label
   text) + toolbar buttons to step the annotated time to the next/previous
   transition without leaving the schematic. Natural polish pass now that
   clicking lands, and reuses the live-annotation plumbing already built.

2. **Bus Operations suite** (Create/Edit/Adjust/Reverse Bus,
   expand-as-sub-bus). Supersedes the plain "no bit-slicing" gap --
   worth doing this instead of a narrower slicing-only feature.

3. **Compress Time Range**: collapse uninteresting time spans to a "+"
   marker. Self-contained, no dependency on anything above.

4. **Group Manager**: a real dialog for nested signal groups, bulk
   "move to group".

5. **Highlight incomplete/unresolved nets** (dashed line) in the
   schematic -- the clickable schematic's per-node metadata already exists
   to hang this off of; low effort at this point.

6. **Struct/union display** (expandable field tree) and **derived/virtual
   signal expressions**. Bigger standalone features, no urgent driver
   from anything else on this list -- pick up when there's a specific
   need.

7. **Editable/exportable schematic mode** (reposition instances,
   print/capture, collapse/expand a module to a black box, drag a
   schematic signal into the wave pane), **configurable color themes**,
   **zoom/pan + a navigation-only "last view" undo**. Polish tier -- do
   opportunistically alongside #1 rather than as standalone efforts.

8. **Event Sequence pane** (sub-timestamp/delta-cycle detail). Valuable
   but the highest-effort item here (needs a real sub-timestamp data
   model, not just a UI); lowest priority-to-effort ratio on this list.

## Noted, not detailed

Auto Update (live-tail a still-running simulation), time-span Comments
(distinct from point markers), Waveform Compare / structured two-run
diffing, Dense Block Drawing (fast struct/MDA rendering), Analog Waveform
view. Also still open from the original gap list: no source browsing
beyond single-file cross-probe.
