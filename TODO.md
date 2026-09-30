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
- **Recolor schematic net edges by logic value + step to next/prev
  transition.** `recolor_dot_edges()` colors each labeled net edge by its
  value at the annotated time (red=X-like, dark orange=Z, forestgreen=
  actively driven-high, default=clean zero or unresolvable), same "X-
  like"/"driven-high" definition `strace.c`'s `ST_X`/`ST_HIGH` and List X
  already use. "Prev Transition"/"Next Transition" toolbar buttons move
  the marker to the root signal's previous/next real transition without
  leaving the schematic (`signal_step_transition_time()`, alongside the
  existing `signal_value_at_time()`) -- the existing marker-changed
  handler already re-renders on that. Verified via a standalone GLib-only
  test harness (fake lookup, matching the code's own designed-for-that
  structure) against a real slang-netlist DOT sample, then fed the
  recolored output through the real `dot` binary and visually confirmed
  the render. Didn't end up using the gdb-forced-call harness this time --
  a forced GUI call was slow enough that gdb's own output buffering (into
  a redirected log file) made it look hung when it wasn't; the standalone
  harness was more direct anyway for logic explicitly designed to be
  lookup-injectable.
- **Bus Operations: expand-as-sub-bus + reverse bit order.** "Create Bus"
  already existed as Edit > Combine Down/Up (`combine_traces()`); this
  closed the actual original gap. "Expand as Sub-Bus..." (new popover
  dialog) slices a contiguous bit range out of an existing bus into its
  own narrower bus trace. "Reverse Bit Order" is a one-click toggle of
  the existing `TR_REVERSE` flag. Both exposed in a new "Bus Operations"
  popover section, alongside "Create Bus (Down)/(Up)" (the existing F4/F5
  shortcuts, given a discoverable home). The real complexity: gtkwave has
  two unrelated "multi-bit signal" representations -- a real constructed/
  auto-coalesced vector (`GwBitVector`) and, far more commonly, a
  natively-declared multi-bit VCD var (a `GwNode` with `extvals` set,
  same split List X's scan needed) -- only found this mid-verification,
  since the first version rejected the common case outright. Fixed by
  normalizing the `extvals` case through `combine_traces(1, t)` (the same
  machinery already used elsewhere for "make this one trace a vector")
  before slicing. Known, accepted, documented limitation: slicing a
  natively-declared signal leaks the excluded bits' throwaway expanded
  nodes (bounded, one bus-width's worth of small allocations per
  operation -- see the commit for why a correct partial-free would need
  new shared-library plumbing disproportionate to this feature).
  Verified via gdb against the real 4-bit ripple-counter bus: forced past
  the dialog's modal `gtk_dialog_run()` (breakpoint + `return (gint)-5`
  to fake a GTK_RESPONSE_OK without ever entering the real modal loop),
  confirmed the sliced sub-bus's value exactly matches the corresponding
  bits of the source bus at a known time, and confirmed Reverse Bit Order
  flips it correctly.
- **Compress Time Range.** Turned out to be far from "self-contained" --
  investigated first and found gtkwave has no canonical time<->pixel
  mapping function at all, just `(t - tims.start) * pxns`/
  `tims.start + x * nspx` inlined ad hoc across ~15 files, 50+ call
  sites, in 3-4 slightly different forms. Asked the user how to scope it
  given that; they chose the full real feature. Introduced
  `gw_time_to_pixel()`/`gw_pixel_to_time()` (`time_compress.c`/`.h`) as
  the canonical piecewise mapping (falls back to the exact plain formula
  when nothing's compressed, so it's a safe drop-in), and mechanically
  replaced every call site that affects actual rendering or click/drag
  position: the core trace-rendering loops
  (`gw-wave-view-traces.c`), marker drawing and the actual
  "where did the user click" resolver (`gw-wave-view.c`,
  `wavewindow.c`'s `resolve_pixel_time()`), and the viewport-width-to-
  end-time recompute that's the specific line making the same pixel
  width show more time once compression is active. Deliberately left
  page/scroll-increment math (a pre-existing bulk approximation, not a
  per-pixel position) and touch-gesture pinch-zoom (self-referential,
  would need real algebraic rework) uncompressed-approximate --
  reasoned, documented scope boundaries, not oversights. New
  Edit > Compress Time Range / Uncompress All menu items; compress
  requires both primary and baseline markers set (span between them),
  deliberately not defaulting to "compress everything" the way most
  other primary<->baseline-ranged features in this app do when baseline
  isn't set. Real gotcha hit along the way: `GLOBALS` is initialized
  *positionally* in `globals.c`, not via designated initializers --
  adding a new struct field without a matching positional entry shifted
  every subsequent field by one slot, caught immediately by
  `-Werror=int-conversion` as a wall of unrelated-looking errors rather
  than silently corrupting state. Verified both mathematically (gdb,
  exact round-trip of the new functions against real
  `tims`/`pxns`/`nspx` values before/after adding a span) and visually
  (real screenshot showing the "+" marker rendered correctly, and
  confirmed `tims.end` -- how much timeline the same pixel-width
  viewport shows -- grew by very close to the predicted amount after
  compressing a 30-unit span to the 14px marker).
- **Group Manager.** Built on the existing group data model (begin/end
  `TR_GRP_BEGIN`/`TR_GRP_END` sentinel pairs, nesting already supported
  via `t_grp` ancestor-chain pointers) rather than inventing a new one --
  what was actually missing was a dialog and a way to move
  already-existing traces into an already-existing group (`create_group()`
  only ever builds a brand new group from the current highlight; nothing
  like "move to group" existed anywhere). New Edit > Group Manager...
  lists every group indented by nesting depth, with Move Selected Here
  (reuses `CutBuffer()`/`PasteBuffer()`, the same primitives
  `create_group()` uses, guarded against moving a group into itself or
  one of its own subgroups via a cheap walk up the existing `t_grp`
  chain), Rename, and Ungroup (frees synthetic sentinels but keeps a real
  signal that's wearing `TR_GRP_BEGIN` via the "Expand" bit-blast
  feature). Verified against the real design via gdb, driving the actual
  dialog through real `gtk_button_clicked()` calls on the real widgets
  (found by walking the live GTK container tree) rather than calling
  internal logic directly. Confirmed move-into-a-nested-group lands the
  signal as that group's last child with correct `t_grp`; confirmed
  Ungroup removes the sentinels and correctly repoints every former
  member's `t_grp` to the outer group; confirmed the self-move guard
  rejects moving a group into itself without corrupting the list.
- **Highlight incomplete/unresolved nets** (dashed line) in the
  schematic. A node whose label names a real signal leaf but that
  couldn't be resolved to a full path (e.g. it fell outside a depth/
  module-boundary limit) was previously indistinguishable from a
  genuinely structural node -- both just showed no value. New
  `SchematicNodeInfo.unresolved` flag + `style=dashed` in
  `annotate_dot()`, plus more accurate click/context-menu status
  messages. Verified via the same standalone GLib-only test-harness
  technique as the earlier edge-recoloring work, then fed through the
  real `dot` binary to confirm valid syntax.
- **Persist Compress Time Range spans across session save/reload.**
  `GLOBALS->compressed_spans` was never wired into `savefile.c`, so the
  whole feature evaporated the moment a session was saved and reopened.
  New `[compressed_span] <start> <end>` directive, one line per span,
  written/read the same way other whole-session globals are; old
  savefiles without it just don't have the lines. Verified via gdb:
  wrote two spans to a real temp file via `write_save_helper()`,
  confirmed the exact directive lines, cleared state, reloaded via
  `read_save_helper()`, confirmed both spans came back with exact
  matching values.
- **Expose bit-range querying in Show Drivers/Loads/Cone/Schematic.**
  The netlist-query backend already accepted a bit-range-suffixed
  signal_path with no code change needed -- this closed the missing UI
  entry point. New optional "Bit range:" entry in the popover, shown
  only for a bus/multi-bit target (covers both multi-bit
  representations), appended onto the signal path when non-empty.
  Verified via gdb: confirmed the range is appended correctly, confirmed
  a real Drivers query succeeds with it applied (window titled
  "Drivers of tb.dut.u_counter.count_q[2:1]"), confirmed an empty range
  still queries the whole signal as before.
- **Fix schematic popup windows: center content, add Zoom In/Out/Fit/1:1.**
  User-reported: both schematic popups (RTL fan-in cone and gate-level)
  rendered the diagram tiny and stuck in the top-left corner, with no
  zoom control -- a plain GtkImage-in-GtkScrolledWindow with a fixed
  default window size unrelated to the image's natural size. Centered
  the image on both axes, and added a real Zoom In/Out/Fit/1:1 toolbar
  (rescales a retained original pixbuf, auto-fits once via a one-shot
  `size-allocate` handler, later refreshes preserve the user's chosen
  zoom). `schematic_view.c`'s click-to-node hit-testing now divides
  click coordinates by the current zoom before testing against node
  bounding boxes (always stored in 1.0-zoom space). Verified via gdb:
  confirmed zoom_to_fit() computes a real non-1.0 scale and produces a
  correctly-sized displayed pixbuf, and confirmed the click-coordinate
  fix is both correct and necessary -- a synthetic click at a known
  node's *scaled* position resolves correctly; the same node's
  *unscaled* 1.0-zoom coordinates correctly miss.
- **List X: scan the whole design hierarchy**, not just currently-
  displayed traces. Previously you had to already suspect *and display*
  a signal before List X could find an X in it, defeating the feature's
  actual point. Now also walks `GwFacs` (the flat symbol table for the
  whole loaded dump, same unconditional-scan pattern `tcl_helper.c`/
  `treesearch.c` already use), building a throwaway `GwTrace` per symbol
  to reuse the existing scan logic, deduped by `GwNode*` for VCD
  identifier-code aliasing. Along the way, hit and fixed a real gap: a
  node that's never been displayed keeps its history compressed
  (`mv.mvlfac` set, no `head`/`next` chain yet) until `import_trace()`
  unpacks it -- previously only done by `AddNodeTraceReturn()` the first
  time a node became a visible trace. Auto-coalesced constructed vectors
  (no single `GwFacs` entry, since they're synthesized at display time)
  still go through the old `GLOBALS->traces` walk. Verified via gdb:
  loaded a VCD with two never-displayed signals carrying real X regions,
  called `list_x_show()` directly, and confirmed via the live GTK widget
  tree that all 4 X-like intervals were found with correct paths/ranges.
- **Gate-level schematic: auto-detect the real DUT under a testbench.**
  Two independent bugs, both fixed: (1) `read_verilog -sv` without
  `-defer` fully compiles every source file immediately, and Yosys's
  frontend const-folds/"executes" a fully-constant initial block's
  system tasks (`$finish`) as part of that, regardless of which module
  `-top` later selects -- `-defer` sidesteps it entirely by deferring
  compilation to `hierarchy -top`, which only compiles modules actually
  reachable from the chosen top. (2) The configured top module defaults
  to the VCD's own dump-root scope, which for a testbench-wrapped design
  *is* the testbench -- new `find_dut_instance()` (best-effort plain-
  text scan, comments stripped, keyword-filtered) finds the first real
  module instantiation inside it, and a retry (only on Yosys's specific
  `$finish'/$stop' executed` error text, so a genuine design error still
  surfaces immediately) uses that as the real top with signal paths
  remapped through the detected instance path. Verified via gdb against
  the real tb/top/counter test design: confirmed the first attempt fails
  exactly as expected, confirmed auto-detection finds the right module
  and instance, confirmed the retry opens a real gate-level schematic
  with the correct status message noting the substitution.
- **Add an MCP server to gtkwave (`mcp/gtkwave-mcp`).** A small
  standalone stdio<->WCP bridge (glib/gio/json-glib only, no GTK): MCP
  clients spawn it and speak newline-delimited JSON-RPC 2.0 over its
  stdin/stdout, and it relays each tool call to an already-running
  `gtkwave --wcp` instance as an ordinary WCP client, one tool per WCP
  command. A tool call's `arguments` merge directly into the WCP
  envelope unchanged (identical field names), so the bridge needs no
  per-command translation -- WCP's own server does the real validation
  and the bridge just relays its response line back as the tool result.
  Scoped to the existing WCP surface only; this session's other new
  backends (netlist-query/schematic/search/List-X/bus-ops) aren't
  reachable over any socket protocol yet, WCP or otherwise -- exposing
  those is follow-up work. Verified end-to-end with no gdb involved at
  all (the actual point of the feature): launched a plain `gtkwave --wcp`
  instance, piped real JSON-RPC sequences into the bridge across several
  separate invocations, confirmed initialize/tools/list/tools/call all
  work, confirmed state persists across invocations against the same
  gtkwave process (get_item_list reflecting an earlier call's results),
  and confirmed a real WCP-level error surfaces correctly as
  `isError: true`.
- **Undo for destructive trace-list operations** (Ungroup, Delete,
  Combine Up/Down, Expand). A bounded (10-deep) linear undo stack of
  whole-session-state snapshots, reusing the existing savefile
  serialization (`write_save_helper()`/`read_save_helper()`) rather than
  a hand-rolled trace-list clone -- confirmed safe to call mid-session
  since `parsewavline()`'s own `"dumpfile"` directive handler is a
  deliberate no-op, so replaying a snapshot only restores trace/group/
  marker/color state and never tries to re-open the dump file. New
  Edit > Undo (Ctrl+Z). Verified via gdb: added two real signals via the
  new `gtkwave-mcp` bridge, selected them, called `menu_delete_traces()`
  directly (confirmed the trace list emptied), then `menu_undo()`
  (confirmed both signals came back with their exact original names and
  flags). Scoped to a single-level "redo" via snapshots, not a full
  command-pattern undo/redo tree (no Redo yet -- an accepted, documented
  boundary, not an oversight).
- **Bus Operations: Edit Bus Composition** (reorder/add/remove
  constituent bits of an existing bus). Rounds out the originally-scoped
  "Create/Edit/Adjust/Reverse Bus" item. New popover dialog: Move Up/
  Move Down/Remove on a selected bit, an "Add Bit..." entry that
  resolves a typed signal path (rejecting another multi-bit bus).
  Reuses `expand_subbus()`'s exact per-bit attribute composition and
  dual-representation handling; Apply builds a brand-new
  `"<original>_edited"` bus via the same `bits2vector()`/`AddVector()`
  backend -- deliberately non-destructive, same precedent
  Expand-as-Sub-Bus already set. Verified via gdb forced calls: broke in
  at the real (unmodified) dialog's `gtk_dialog_run()`, directly
  manipulated the real in-memory `entries` array (reorder + remove +
  add a genuinely different scalar signal), then faked the dialog's OK
  response to let the real `apply_composition()` run -- confirmed the
  resulting new trace had exactly that edited composition, in order,
  including the mixed extvals-bit + plain-scalar-bit case. Didn't
  separately drive the GTK button-click handlers themselves (thin,
  low-risk wrappers around the same array ops already exercised
  directly) -- a deliberate scoping choice.
- **Verified Group Manager's renames/nested-structure round-trip
  through save/reload** -- and found a real bug doing it.
  `show_rename_dialog()` freed a group's name with `g_free()` and
  reassigned it with `g_strdup()`, but that field is allocated with
  `malloc_2()` (`create_group()`, `src/menu.c`), which prepends its own
  allocation-chain bookkeeping before the returned pointer -- freeing it
  with plain `g_free()` corrupts the heap. Reproduced a real SIGABRT
  ("double free or corruption") the first time this path ran under gdb:
  renaming *any* group created via the normal "Group" menu action would
  have corrupted the heap on the very first rename, in the released
  build too, not just under gdb. Fixed with `free_2()`/`strdup_2()`, the
  matching pair every other group-sentinel name in this codebase already
  uses. Re-verified once fixed: built a real nested-group structure
  (outer group containing a plain trace, a nested inner group, and
  another plain trace) via `create_group()`, renamed the outer group,
  wrote it to a real temp file via `write_save_helper()`, cleared all
  traces, reloaded via `read_save_helper()`, and confirmed every trace's
  name and the correct nested `t_grp` ancestor-chain pointers matched
  the original structure exactly.
- **Time-Span Comments** (range annotations, distinct from point/named
  markers) -- natural complement to markers/groups for documenting a
  debug session. New "Add Time-Span Comment..." (reads the primary/
  baseline marker span the same way Compress Time Range already does)
  and "Time-Span Comments..." list window (Go To/Delete per row, same
  shape List X already uses). Persistence mirrors Compress Time Range's
  own precedent exactly: a new `[time_span_comment] <start> <end>
  "<text>"` savefile directive. Verified via gdb forced calls: set real
  markers, broke in at the real add dialog's `gtk_dialog_run()`, set the
  real entry's text, faked the OK response -- confirmed the comment was
  added with the exact range/text; verified persistence end-to-end
  (write, clear, reload, confirmed exact round-trip).

## Up next, in order

Re-ranked by value (not just effort) after a deliberate stock-take of
everything shipped so far plus standard Verdi/nWave parity gaps -- see
the conversation history for the full reasoning behind this ordering.

1. **Waveform Compare / structured two-run diffing.** One of the most-
   requested Verdi/nWave parity features. Big lift, big payoff.

2. **Struct/union display** (expandable field tree) and **derived/virtual
   signal expressions**. Bigger standalone features, no urgent driver
   from anything else on this list -- pick up when there's a specific
   need.

3. **Editable/exportable schematic mode** (reposition instances,
   print/capture, collapse/expand a module to a black box, drag a
   schematic signal into the wave pane), **configurable color themes**,
   **zoom/pan + a navigation-only "last view" undo**. Polish tier -- do
   opportunistically alongside other schematic work rather than as a
   standalone effort.

4. **Compress Time Range: make touch-gesture pinch-zoom compression-
    aware.** The one documented gap in that feature -- the existing
    pinch-zoom math is self-referential and would need real algebraic
    rework, not a mechanical substitution.

5. **Auto Update** (live-tail a still-running simulation). A genuinely
    different workflow (active-sim debugging vs. post-hoc analysis),
    real nWave feature.

6. **Event Sequence pane** (sub-timestamp/delta-cycle detail). Valuable
    but needs a real new sub-timestamp data model; highest effort on
    this list.

7. **Source browsing beyond single-file cross-probe** -- multi-file/
    project-wide source navigation.

8. **Dense Block Drawing** (fast struct/MDA rendering) -- performance/
    scale feature for struct-heavy designs.

9. **Analog Waveform view** -- expected baseline EDA-viewer feature,
    currently absent.

10. **Compress Time Range: compression-aware page/scroll-increment
    math.** Precision polish (paging jumps a slightly wrong amount
    across a compressed span); low urgency.

11. **Transaction/protocol-aware decoding** (a pluggable bus-protocol
    decoder on top of the existing `TR_TTRANSLATED` transaction-trace
    support). A genuinely new capability class; valuable but large,
    speculative scope.
