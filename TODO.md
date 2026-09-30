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
  marker moves. Static image only so far -- see #2 below.
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

## Up next, in order

1. **Make the schematic actually clickable**, using node bounding boxes
   from Graphviz's own `-Txdot`/`-Tplain` output instead of a flat PNG.
   This is the real foundation the whole "static image" complaint traces
   back to, and it's a smaller lift than it looks -- Graphviz already
   computes per-node positions, so this is an overlay/hit-testing problem,
   not a new rendering engine. Directly unlocks, cheaply, once done:
   - Double-click a net to isolate its driver->load path (step with n/N)
   - Right-click menus scoped per element type (signal/instance/net)
   - Real push/pop-into-instance navigation (replaces "always re-render
     the whole cone")
   - Collapse/expand a module to a black box
   - Add to Waveform / drag a schematic signal into the wave pane

2. **Search UX rework**: exact/wildcard bus-value search, a "stable for
   >= N time units" constraint, jump to the Nth occurrence. This was the
   original "search is hard to use" complaint from day one and has zero
   dependency on the netlist/schematic work, so it can happen in parallel
   with #1 whenever there's a natural break.

3. **List X / Trace X**: scan the loaded waveform for X (unknown) regions,
   list them, then trace backward from a specific occurrence to its
   root-cause driver. High ROI -- reuses the existing driver-query backend
   as-is, just a different entry point (seeded from a bad value instead of
   a signal name).

4. **Recolor schematic net lines by logic value** (not just the label
   text) + toolbar buttons to step the annotated time to the next/previous
   transition without leaving the schematic. Natural polish pass once #1
   lands, and reuses the live-annotation plumbing already built.

5. **Bus Operations suite** (Create/Edit/Adjust/Reverse Bus,
   expand-as-sub-bus). Supersedes the plain "no bit-slicing" gap --
   worth doing this instead of a narrower slicing-only feature.

6. **Compress Time Range**: collapse uninteresting time spans to a "+"
   marker. Self-contained, no dependency on anything above.

7. **Group Manager**: a real dialog for nested signal groups, bulk
   "move to group".

8. **Highlight incomplete/unresolved nets** (dashed line) in the
   schematic -- worth doing once #1's node metadata exists to hang it off
   of; low effort at that point.

9. **Struct/union display** (expandable field tree) and **derived/virtual
   signal expressions**. Bigger standalone features, no urgent driver
   from anything else on this list -- pick up when there's a specific
   need.

10. **Editable/exportable schematic mode** (reposition instances,
    print/capture), **configurable color themes**, **zoom/pan + a
    navigation-only "last view" undo**. Polish tier -- do opportunistically
    alongside #1/#4 rather than as standalone efforts.

11. **Event Sequence pane** (sub-timestamp/delta-cycle detail). Valuable
    but the highest-effort item here (needs a real sub-timestamp data
    model, not just a UI); lowest priority-to-effort ratio on this list.

## Noted, not detailed

Auto Update (live-tail a still-running simulation), time-span Comments
(distinct from point markers), Waveform Compare / structured two-run
diffing, Dense Block Drawing (fast struct/MDA rendering), Analog Waveform
view. Also still open from the original gap list: no source browsing
beyond single-file cross-probe.
