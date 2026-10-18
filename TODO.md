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
- **Compress Time Range: made touch-gesture pinch-zoom compression-
  aware.** Pinch-zoom's anchor-preserving math used the plain
  `tims.start + x*nspx` formula directly, ignoring any active
  compressed span, so pinching near one would visibly jump/drift
  instead of staying pinned under the user's fingers. New
  `gw_solve_start_for_pixel_anchor()`: given a pixel position and the
  real time it should keep representing, solves for `tims.start` by
  walking backward from the anchor time by the anchor pixel count,
  span-by-span -- a genuinely different (and harder) problem than the
  existing `gw_time_to_pixel()`/`gw_pixel_to_time()` (those map a
  *known* base; this solves for the base itself). An initial version
  tried to shortcut this by evaluating `gw_pixel_to_time()` with
  `tims.start` forced to 0 and subtracting the result back out -- wrong
  (which gap/marker/normal pixel-region a point falls into depends on
  the base itself), caught only by directly verifying the round-trip.
  That same verification surfaced a second, pre-existing bug in
  `gw_time_to_pixel()`/`gw_pixel_to_time()` themselves: neither
  correctly handled `tims.start` falling *inside* a compressed span
  (only before/after one), making them mutually inconsistent whenever
  that happened -- reachable before too (e.g. "Go To" a time inside a
  compressed span) but newly routine once pinch-zoom could legitimately
  land there. Fixed both. Verified via gdb against real GLOBALS state:
  confirmed the common (base-before-span) case is byte-for-byte
  unchanged, confirmed the mid-span-base fix makes the two functions
  exact inverses again, confirmed a full solve-then-verify round trip
  holds exactly for both a zoom-in (anchor landing inside a compressed
  marker) and a zoom-out (anchor before the span) scenario.
- **Compress Time Range: made page/scroll-increment math
  compression-aware.** The other documented gap: every "how much time is
  one page/scroll-step" computation (21 call sites across 13 files) used
  the plain `wavewidth * nspx` bulk approximation directly, undercounting
  whenever a compressed span fell within that pixel width -- most
  visibly, this was the actual Page Up/Down and scrollbar-trough-click
  increment (`zoombuttons.c`'s `fix_wavehadj()`). New
  `gw_time_page_increment()`: the real duration `wavewidth` pixels
  represents via `gw_pixel_to_time(wavewidth) - tims.start`, falling
  back to the exact plain formula when nothing's compressed. Mechanically
  replaced all 21 occurrences. Verified via gdb: confirmed no regression
  when nothing's compressed, confirmed a real compressed span made the
  new function return a larger, hand-verified value than the old
  formula, confirmed the real `GtkAdjustment`'s `page_increment`
  property reflects the fix.
- **Source browsing: let users manually switch between known source
  files.** The embedded source cross-probe panel could only ever show
  whatever file an external cross-probe jump last pointed it at -- no
  way to browse to a *different* file while looking at one. New
  `GtkComboBoxText` above the path label, populated from
  `GLOBALS->netlist_source_files` (hidden until that's configured),
  reusing the existing `source_panel_show_file_line()` pipeline
  unchanged. Deliberately scoped to this one increment, not a full
  project explorer (file tree, tabs, per-file history) -- that needs a
  real module-to-source-file mapping slang-netlist doesn't expose today.
  Verified via gdb against a real two-file design: confirmed the combo
  lists both files with the right one pre-selected, then selected the
  other file via the real `gtk_combo_box_set_active_id()` call (firing
  the real "changed" signal end-to-end) and confirmed both the path
  label and the actual loaded buffer content reflect the switch.
- **Analog Waveform view: exposed it in the quick signal popover.**
  Research found the TODO's premise ("currently absent") was outdated --
  the analog rendering engine, `TR_ANALOG_*` flags, and full menu/dialog
  UI already existed and predate this fork; it works on any trace, not
  just real-typed ones. The one real gap: `signal_popover.c` (the newer
  GTK3 popover this session has been extending all along) had no Analog
  entries at all, unlike its already-covered Hex/Dec/Signed/Bin/Oct/
  ASCII row. New "Analog" row (Off/Step/Interp/Interp+/Screen/All Data)
  calling the exact same `menu_dataformat_analog_*()` backend the
  classic popup menu already uses -- reused the existing
  `RadixEntry`/`make_radix_button`/`on_action_button_clicked` machinery
  unchanged. Verified via gdb: opened the real popover, walked its live
  widget tree to the real Analog buttons, clicked the real "Step" button
  via `gtk_button_clicked()` and confirmed the trace's flags gained
  exactly `TR_ANALOG_STEP`, clicked "Off" and confirmed it cleared back.
- **Gate-level schematic: fixed bit-range-suffixed queries
  ("Nothing there to show").** Chased a suspected "nested-instance path"
  bug that turned out to be a false lead (an artifact of an earlier ad
  hoc manual yosys test, not a real bug -- the real auto-detect/prefix-
  stripping code already handles nested instances correctly), but
  retesting the real code path against a real nested signal surfaced a
  genuine, different bug: Yosys's `select` command has no syntax for a
  Verilog-style bit-range suffix (`name[msb:lsb]`) -- confirmed directly
  (`select top/value` works, `select top/value[2:1]` silently matches
  nothing). The popover's "Bit range:" entry builds that exact
  bracket-suffixed path for all four Drivers/Loads/Cone/Schematic
  actions; it works for the first three (slang-netlist does support a
  bit-range suffix) but the gate-level schematic (a completely different,
  Yosys-based path) was apparently never actually exercised with a real
  range before. Fixed by stripping the suffix before building the Yosys
  select expression, with a clear status note explaining the bus-wide
  fallback (Yosys has no bit-slicing selector to use instead). Verified
  via gdb: the exact previously-failing nested, bit-range-suffixed query
  now succeeds and opens a real schematic window, with both the
  DUT-auto-detect and the new bit-range-limitation status notes shown.
- **Extended MCP/WCP with netlist query, List X, Time-Span Comments,
  Undo.** Explicitly flagged as follow-up when MCP shipped: it only
  wrapped the pre-existing WCP command set, leaving every backend built
  afterward unreachable from any socket protocol. New WCP commands
  (`netlist_drivers`/`netlist_loads`/`netlist_cone`, `list_x`,
  `time_span_comment_add`/`time_span_comments_list`, `undo`) plus
  matching MCP tool table entries (the bridge's generic pass-through
  needed no new logic). The netlist query commands deliberately never
  auto-prompt the GUI's HDL-source-config dialog (a WCP client can't
  respond to one popping up) -- fail cleanly with `not_configured`
  instead. Needed a new public `list_x_scan_all()` in
  `list_x_view.c/.h` (previously GUI-only). Verified via gdb + the real
  `gtkwave-mcp` bridge against a real design: confirmed real
  slang-netlist-backed driver/load/fan-in data with correct source
  locations, confirmed the `not_configured` refusal, confirmed `list_x`
  finds all 4 known X-intervals in a real fixture, confirmed
  `time_span_comment_add`/list round-trips exactly, confirmed `undo`
  correctly reports `nothing_to_undo`.
- **Fixed WCP `add_items` (and every other caller of
  `gw_dump_file_lookup_symbol`) silently failing on a plain multi-bit
  signal name.** A real, repeatedly-hit gap, first found during Search
  UX rework and re-confirmed during later testing: a natively-declared
  multi-bit signal's own facs symbol name already embeds its bit-range
  suffix (e.g. "top.bus[3:0]") at load time, not discoverable from the
  plain declared name. Fixed at the root in
  `lib/libgtkwave/src/gw-dump-file.c` rather than in any one caller:
  when a bare lookup fails and isn't already bracket-suffixed, search
  for exactly one real symbol whose name is the given name immediately
  followed by a bracketed range, and auto-resolve to it (ambiguous
  matches are left unresolved). Verified via the real `gtkwave-mcp`
  bridge: bare multi-bit names now resolve and add correctly via the
  real WCP `add_items` command; confirmed no regression for plain
  scalars and no false-positive matching for a genuinely nonexistent
  name.
- **Added "Manage Compressed Spans..." list window.** Every other "list
  of things" feature this session built (List X, Time-Span Comments,
  Group Manager) got one; Compress Time Range didn't -- the only way to
  see/remove an active span was clicking its tiny on-canvas "+" marker.
  New Edit > Manage Compressed Spans..., with "Go To"/"Uncompress" per
  row, reusing the existing `time_compress_span_count()`/`span_get()`/
  `remove_span_at()` accessors unchanged. Verified via gdb: opened the
  real window, confirmed the real row's label text, clicked the real
  "Uncompress" button and confirmed the span was actually removed and
  the window correctly rebuilds to "No compressed spans."
- **Added Redo**, completing Undo's documented single-level limitation.
  Refactored the single snapshot stack into two symmetric ones (undo/
  redo): `undo_stack_undo()` now pushes the pre-undo state onto the
  redo stack before restoring (so Redo can bring it back), and a fresh
  destructive op clears the redo stack, same as any plain linear undo/
  redo history. New Edit > Redo (Ctrl+Shift+Z), plus a matching WCP
  `redo` command and MCP tool entry. Verified via gdb + the real
  `gtkwave-mcp` bridge: full delete/undo/redo round-trip symmetry
  (can_undo/can_redo flip correctly at each step, traces actually
  restored/re-deleted), then confirmed a fresh destructive op correctly
  clears the redo stack.
- **Added Edit to Time-Span Comments** (was Add/Delete only). Each row
  in the list window now has an "Edit..." button alongside Go To/
  Delete, opening the same dialog pattern Add uses but pre-filled with
  the comment's current text; saves in place via a new
  `time_span_comments_set_text()` (time span itself stays fixed, only
  the text changes). Verified live via gdb: opened the real list
  window, clicked the real Edit button (forcing the modal's OK return
  via a breakpoint), confirmed the dialog was pre-filled with the
  existing text, changed it, and confirmed via
  `time_span_comments_get()` that the text changed, start/end were
  preserved, and the count stayed at 1 -- then confirmed the rebuilt
  list window's row label showed the new text.
- **Added "Add to Waveform" to the RTL schematic view's context menu.**
  Right-clicking a resolved node (Show Drivers/Show Loads) now also
  offers Add to Waveform: resolves the full path via
  `gw_dump_file_lookup_symbol()` (same lookup bus_ops.c/wcp_gtkwave.c
  use) and adds it via `AddNodeTraceReturn()`, same as the WCP
  `add_items` path. Gate-level (yosys) schematic view is out of scope
  -- no click/hit-testing machinery exists there to hang a menu off of.
  Verified live via gdb: a direct call with a resolvable VCD path added
  the trace (confirmed name and count); opened the real schematic
  view, drove the real right-click handler with a synthetic event at a
  real node's coordinates, confirmed the real popup menu's items by
  label (including the new "Add to Waveform" item), and activated it
  via `gtk_menu_item_activate()`, confirming it invoked the handler
  with the node's exact full path.
- **Fixed schematic popups getting stuck at native size, never auto-
  fitting.** User-reported: both schematic popups (RTL fan-in cone and
  gate-level) could still render dark/unreadable with the diagram tiny
  in a corner, no effective pan/zoom -- the same symptom the earlier
  e0e95f3 zoom/centering fix was supposed to have already resolved.
  Root cause: the one-shot "fit on first real size-allocate" handler in
  both views disconnected itself unconditionally after a single call to
  zoom_to_fit(), regardless of whether that call actually computed a
  fit -- a toplevel's very first size-allocate can legitimately report
  a placeholder size (e.g. 1x1) before the window manager finishes
  negotiating its real geometry, and disconnecting on that miss left
  the diagram stuck at native 1.0 zoom forever with no further retry.
  Fixed by having zoom_to_fit()/gate_zoom_to_fit() return whether a fit
  was actually applied, and only disconnecting the one-shot handler
  when that's true. Verified via gdb: reconnected a fresh instance of
  the handler, forced the scrolled window's allocation to a bogus 1x1
  and emitted a real "size-allocate" signal (confirmed zoom unchanged),
  restored its real allocation and emitted again (confirmed the same
  handler was still connected and now correctly fit, zoom 1.0 -> 1.061),
  then emitted a third time with yet another valid size (confirmed zoom
  unchanged, proving it had by then correctly disconnected itself so a
  later resize doesn't fight the user's own zoom choice). A window-
  manager-less headless display made this hard to verify by eye --
  this was confirmed at the logic level instead.
- **Back navigation for the RTL schematic view.** Double-clicking a node
  now navigates the same window to that signal's cone in place, pushing
  the signal navigated away from onto a per-window history stack, rather
  than spawning a new top-level window per hop (found while picking this
  up: double-click had regressed to the latter at some point, even
  though the "Make the schematic actually clickable" entry above already
  described in-place "push/pop-into-instance navigation" as the
  intended behavior). A new "Back" toolbar button (disabled until
  there's history to pop) returns to the previous signal. Verified via
  gdb against a live instance: drove the real `on_image_button_press`
  double-click handler with a synthetic event at a real node's
  coordinates (first at the same signal, then after retargeting the
  node's resolved path to a distinct signal), confirmed the window
  title/`signal_path`/history stack/Back-button sensitivity all updated
  correctly, then drove the real `on_back_clicked` handler and confirmed
  it popped back to the exact prior signal and disabled itself again.
- **Configurable schematic edge colors.** The X/Z/driven-high edge
  colors the RTL schematic's recoloring feature annotates nets with
  were hardcoded ("red"/"darkorange"/"forestgreen"). Added three new
  `.gtkwaverc` options (`schematic_color_x`/`schematic_color_z`/
  `schematic_color_active`, documented in `gtkwaverc.5`) following the
  existing `rc.c` key-value pattern, falling back to the previous
  hardcoded defaults when unset. Verified the rc-parsing path end-to-end
  via gdb against a live instance (`insert_rc_variable()` with a real
  `"schematic_color_x #112233"` line, confirmed `GLOBALS->
  schematic_color_x` was set); verified the color-selection logic itself
  (defaults preserved, overrides take effect, idle nets still get no
  color) via a standalone compiled unit test of the copied logic, since
  the real function is `static` inside a translation unit that pulls in
  the whole app.
- **Export Image for both schematic views (print/capture).** A new
  "Export Image..." toolbar button on the RTL and gate-level schematic
  windows saves the diagram to a PNG file via a native save dialog
  (`GtkFileChooserNative`, same pattern as the HDL-source-file picker in
  `netlist_query_ui.c`), with a suggested filename derived from the
  signal path. Always saves the original full-resolution render, not
  whatever zoom/pan is currently on screen. Verified via gdb against a
  live instance: confirmed the suggested filename matched the signal
  path exactly (`top.max_reached_reg.png`); forced the dialog's
  `gtk_native_dialog_run()` to return Cancel (breakpoint + forced
  return, same technique as the Time-Span Comments Edit dialog
  verification) and confirmed the function exits cleanly with no file
  written; separately called the exact same `gdk_pixbuf_save()` used by
  the Accept branch directly on the live `original_pixbuf` and confirmed
  a valid full-resolution PNG was written to disk. Did not force the
  Accept branch itself through the dialog (headless Xvfb has no window
  manager, and an earlier attempt at driving a different nested-call
  loop in this same session wedged a gdb/gtkwave pair badly enough to
  need a hard kill -- not worth the risk for boilerplate that's a
  verbatim copy of the already-shipped `choose_source_files()` pattern).
- **Drag a schematic signal into the wave pane.** A resolved node on the
  RTL schematic can now be dragged off the diagram and dropped onto the
  main signal/wave pane to add it as a trace, alongside the existing
  right-click "Add to Waveform" item. Reuses the existing
  `WAVE_DRAG_TARGET_TCL`/`WAVE_DRAG_INFO_TCL` drag format the hierarchy
  tree view and search box already drag from (`treesearch.c`'s
  `dnd_setup()`, `tcl_helper.c`'s `make_single_tcl_list_name()`) --
  `GwSignalList` already accepts drops of that format
  (`signal_list.c`), so no destination-side code was needed, only a
  manually-driven drag source on the schematic's event box (press arms
  it, motion past GTK's drag threshold starts it, mirroring
  `GwSignalList`'s own manual drag-begin rather than
  `gtk_drag_source_set()`'s automatic one). Gate-level view untouched --
  it has no click/hit-testing machinery to begin with (same scoping as
  the earlier Add to Waveform item). Verified via gdb against a live
  instance: pressing on a resolved node correctly armed the drag state
  (path/coordinates captured) while leaving the existing single-click
  status-text behavior intact; releasing without exceeding the drag
  threshold correctly cleared it again; a sub-threshold motion left the
  armed state untouched (no premature drag); and the actual payload-
  building call (`make_single_tcl_list_name()`) produced the exact
  expected Tcl-list string (`"{net top max_reached_reg} "`) for the
  live node. Didn't force the real `gtk_drag_begin_with_coordinates()`
  grab/drop through to completion -- same headless-Xvfb/nested-GTK-
  loop hazard as the Export Image item above (confirmed again this
  session: a `finish` through a gdb-issued nested call that hit a
  breakpoint reliably wedges this harness; forcing an immediate
  `return` from the breakpointed frame instead, without ever calling
  `finish`, is the safe pattern and is now the going-forward approach
  for this project rather than something to keep re-discovering).
- **Fixed schematic nodes mislabeled as unresolved for Case/Const/
  Variable.** Found opportunistically while building a more complex
  multi-module test design (a 4-module toy CPU datapath -- sequencer +
  register file + ALU, with a genuine feedback loop) to show off the
  schematic view: `extract_leaf_name()`'s structural-node allowlist
  checked for the literal string `"Constant"`, but slang-netlist's
  `NetlistDot.hpp` always emits `"Const <value>"` (e.g. `"Const
  8'd0"`) -- that comparison never matched anything in practice, so
  every constant node fell through to the generic leaf-name extraction
  and rendered as a dashed-border "unresolved net" instead of a
  structural one. `"Case"` was missing from the allowlist entirely,
  same symptom. Fixed both, plus added `"Variable "` prefix handling
  (a real signal name worth resolving, confirmed from the same
  upstream source, unlike the structural kinds). Verified via gdb
  against a live instance rendering the new design's real schematic:
  before the fix, `Case`/`Const 8'd0` nodes had `unresolved=1`; after,
  `unresolved=0` for both, confirmed on the actual live `SchematicNodeInfo`
  array, not just by eye.
- **Gate-level schematic: real traditional gate symbols, orthogonal
  routing, click-to-source, scroll-wheel conventions.** User feedback on
  the gate-level view (`gate_schematic_view.c`): Yosys's own `show`
  command rendered generic IEEE-style record boxes via Graphviz, not a
  real schematic (wires/gates/muxes/flops). Rebuilt the renderer around
  **netlistsvg** (https://github.com/nturley/netlistsvg, MIT, Node.js +
  `npm install -g netlistsvg`, auto-detected via `g_find_program_in_path`
  with a graceful fallback to an improved dot/Graphviz path when it's
  absent -- see below): real D-shaped AND, shield-shaped OR/XOR, and
  triangle+bubble inverter symbols via its bundled skin, plus automatic
  all-orthogonal, crossing-minimized routing via its ELK.js layout
  engine (no hand-tuned Graphviz splines needed for this path).
  Click-to-source: SVG cell groups carry an explicit pixel-space
  `transform="translate(x,y)"` + `s:width`/`s:height` + `id="cell_<id>"`,
  cross-referenced against the same cell's `attributes.src` in the Yosys
  JSON netlist (via `json-glib`) to jump the source cross-probe panel to
  that element's line -- new hit-testing parser, same
  `GateNodeInfo`/`find_gate_node_at()` struct/lookup the dot-fallback
  path also populates.
  Fallback path (no netlistsvg installed): kept the original
  Yosys-`show`-via-Graphviz pipeline, switched from SVG+rsvg-convert to
  a direct `dot -Tpng` render (exact DPI match with `-Tplain` hit-testing
  coordinates, confirmed empirically -- the old SVG/rsvg-convert
  pipeline had a small but real DPI mismatch) and added
  `inject_ortho_splines()` (`splines="ortho"` + wider node/rank spacing)
  since Yosys's own dot writer already unconditionally emits
  `rankdir="LR"`/`remincross=true` but never sets `splines`.
  **Ports as connectors**: the netlistsvg path's first version pruned
  the fan-in cone via `select ... %ci* %n; delete` (invert + delete
  everything outside the cone), which left every cut wire boundary
  dangling with no port identity -- netlistsvg drew nothing distinctive
  for them. Fixed by switching to Yosys's `submod -name <name>` (carves
  the selection out into a real new module, auto-creating a properly
  directioned, properly NAMED port at every cut wire), confirmed
  empirically to give every boundary cut a real `inputExt`/`outputExt`
  connector arrow symbol. (`submod` requires memories already lowered --
  `-no-mem` isn't a thing, so `memory;` was added ahead of `techmap`,
  which the dot fallback path doesn't need since plain `show` has no
  such restriction.)
  **Scroll-wheel conventions** (both schematic views, for consistency):
  plain wheel scrolls vertically (GTK's own default, untouched),
  Shift+wheel scrolls horizontally (no GTK3 default for this), Ctrl+wheel
  zooms -- a `scroll-event` handler connected normally (not `_after`) on
  the `GtkScrolledWindow` so returning `TRUE` for the Shift/Ctrl cases
  pre-empts the widget's own default handling, `FALSE` for the plain
  case lets it run unmodified.
  Verified via gdb against a live instance (including over the user's
  real X11-forwarded display, not just headless): confirmed the
  `submod`-based JSON netlist's `nodes with href` count and a direct
  `GateNodeInfo` pixel-coordinate spot-check against a saved PNG crop
  landed exactly on the expected `u_alu.result` port-arrow symbol;
  confirmed the real `on_gate_image_button_press()` handler hit-tests
  and returns `TRUE` for a constructed click at a known cell's
  coordinates without crashing or hanging.
  Known gaps, explicitly not done: selecting/cross-probing an individual
  **wire** (netlistsvg's SVG doesn't give connecting-line paths their
  own net identity, only gate/port boxes do -- would need patching
  netlistsvg itself or a different renderer); the incremental
  filter/expand/dissolve "Cone Window" interaction model for the
  gate-level view (see the scoped plan below -- this round's
  `submod`/one-hop-selection findings are its foundation, but the
  click-to-expand/dissolve UI itself isn't built yet).
- **Depth-limited "Cone Window" rendering for the RTL schematic view.**
  The smaller sibling half of the Cone Window plan (see below): the
  view now starts small (a 3-hop fan-in cone by default) and grows on
  request, instead of always rendering the full transitive cone.
  Correction to this item's own original scoping note (written before
  implementation): `GwNetlistQueryLimits` turned out NOT to be reusable
  here as first thought -- it filters a flat `GwNetlistEdge` list by
  *hierarchical path divergence*, which only works because every edge
  there has a resolvable path, whereas a cone's DOT graph is mostly
  unnamed structural nodes (Assignment/Conditional/...) with nothing to
  compare, so that notion of "depth" can't prune them. Built a client-
  side graph-hop BFS instead, directly over the DOT text's own edges
  (reverse-adjacency from `Na -> Nb` lines, walked backward from the
  query root) -- a different, arguably more natural notion of "depth"
  for a diagram (hop count through the graph actually being drawn)
  than hierarchical path proximity. New toolbar "Depth:" spinner (0 =
  unlimited, the old behavior); double-clicking a node marked
  "(double-click: more)" at the current depth boundary instead
  increments the limit by 2 and re-renders, keeping the spinner in
  sync; navigating to a new root (Back, or double-clicking a resolved
  node) resets the depth back to the small default. Verified via gdb
  against a live instance: default depth-3 render of
  `cpu_top.alu_result` came back as 13 nodes (3 correctly marked
  depth_truncated) vs. the unlimited render's 20; double-clicking a
  truncated node bumped the depth to 5 and grew to 19 nodes (1
  remaining truncated) with the spinner's displayed value confirmed in
  sync; driving the spinner directly down to depth 1 correctly shrank
  the render to 2 nodes.
- **Depth-limited "Cone Window" rendering for the gate-level schematic
  view.** The simpler, global-hop-count MVP of item 3 below's "Start
  small"/"Expand" verbs (not the full per-net `included_objects`
  incremental-expansion model that item still describes -- see its
  trimmed-down remaining scope): a newly-opened window now seeds from
  Yosys's `%ci<N>` fan-in-hop selection operator at
  `GATE_SCHEMATIC_DEFAULT_MAX_DEPTH` (2) hops instead of always
  rendering the full `%ci*` transitive cone, with a toolbar "Depth:"
  spinner (0 = unlimited) and double-click-to-grow-by-2 on any
  `inputExt`/`outputExt` boundary port connector -- same interaction
  shape as the RTL view's sibling feature above.
  `netlist_query_show_gate_schematic()` restructured into a two-phase
  create-window-then-refresh shape (`gate_refresh_schematic()`
  extracted out), caching the resolved top_module/select_prefix (from
  the testbench-vs-DUT auto-detection fallback) on first successful
  render so a later depth change doesn't re-scan source files every
  time. Verified via gdb against a live instance: depth 2/4/0 against
  `cpu_top.alu_result`'s fan-in produced 14/47/459 nodes respectively
  (3/19/- flagged as boundary ports), confirming both the hop limit and
  boundary-port flagging work correctly; auto-detection from testbench
  `tb` to DUT `cpu_top` (instance `tb.dut`) also confirmed still
  working through the restructuring.
- **"Dissolve" for the gate-level schematic view.** Sub-module instances
  no longer get unconditionally flattened: a newly-opened cone now
  starts with every instance as a single black-box symbol, and
  double-clicking one adds it to `GateSchematicState::dissolved_instances`
  and re-runs Yosys with a `flatten <that instance's selection>;`
  fragment (instead of the old blanket `flatten;`) to reveal just its
  internals -- confirmed selective: dissolving one instance leaves
  sibling instances un-dissolved. Detection reuses Yosys's own JSON: an
  un-flattened instance cell's `type` is the plain module name (not a
  `$`-prefixed primitive) and its id is exactly the instance name
  `flatten` needs -- new `extract_module_instance_ids_from_json()` and
  `GateNodeInfo::is_module_instance`. Also swapped the old unconditional
  `memory;` (maps a memory array to one flip-flop per word) for
  `memory_collect;` (coalesces to a single `$mem_v2` cell, still
  satisfies submod's "no raw memory cells" rule) -- confirmed directly:
  dissolving `regfile`'s `regs[DEPTH]` array now renders as **one block
  with wide bus ports** instead of `DEPTH` separate boxes, per explicit
  request ("if you have an array of flops, just draw one block and use
  a wide connection on input and output -- ie use busses"). Verified
  live via gdb: a fresh cone defaulted to 3 black-box instances with
  none flattened; dissolving `u_alu` grew 6->14 nodes; dissolving `u_rf`
  (separately, before a deep `u_alu` dissolve could eat the whole depth
  budget -- see known limitation below) grew 9->19 nodes with exactly
  one `u_rf.regs` ($mem_v2) node, not 8 separate per-word flops.
  Known limitation, not yet addressed: the depth spinner is still one
  *global* hop-count budget shared across both un-dissolved instances
  and dissolved ones' now-exposed internals -- dissolving a
  primitive-heavy instance (confirmed with `u_alu`'s bit-blasted
  8-bit case statement: ~180 gates) can consume the entire budget just
  traversing its own internals, never reaching far enough to show
  sibling instances even at a much higher depth. The per-net
  `included_objects` model below would fix this naturally (each
  instance's internal hops would no longer compete with the others for
  one shared budget).
- **Right-click menu + multi-select "Filter Out" for the gate-level
  schematic view.** Reworked the click model directly on request
  ("ideally you right click in schematic on component and it gives you
  options like dissolve, filter... or ability to select 1, 2, 3 etc
  with shift click, and it highlights"): plain click selects one node
  (blue highlight, drawn by a "draw" signal connected with `_after()`
  so it paints on top of the already-rendered image); shift+click
  toggles a node in/out of a multi-selection without navigating
  anywhere; right-click selects the node under the cursor first
  (unless already part of a multi-selection, which is preserved) and
  pops up a context menu -- "Dissolve" for any selected un-dissolved
  instances (batched into one Yosys re-run), "Filter Out" for any
  selected non-boundary-port objects (new `GateSchematicState::
  filtered_out`, subtracted from the fan-in selection before submod/
  show runs), and one "Collapse" entry per already-dissolved instance
  regardless of the current selection (there's no click target left to
  represent a dissolved instance as a whole once it's exposed as plain
  gates). Double-click keeps its existing fast-path behavior unchanged.
  Getting "Filter Out" right for more than one object at once needed
  real Yosys selection-stack-machine semantics, confirmed empirically:
  `select A obj1 obj2 %d` does NOT subtract both objects from A (`%d`
  only pops the top two stack entries, so obj2 only ends up subtracted
  from obj1, not from A, and both get silently re-unioned back into the
  final result) -- a true "A minus (obj1 union obj2 union ...)" needs
  (N-1) `%u` to fold the filtered objects into one set before a final
  `%d` (`build_filter_suffix()`). Verified live via gdb, exercising the
  real signal handlers directly: filtering one instance and filtering
  two in one batch (confirming the `%u`-fold) both correctly shrank the
  cone while leaving the rest intact; over-filtering down to nothing
  surfaced Yosys/netlistsvg's own "no module" error rather than
  crashing (a bit raw/unfriendly, not specially handled -- known minor
  rough edge); a dissolve+collapse round trip via the real menu
  handlers restored the exact original node count and instance; click/
  shift-click/right-click selection semantics all confirmed via
  synthetic `GdkEventButton` calls into the real click handler.
  Known gap: no un-filter gesture yet (unlike dissolve's collapse) --
  `filtered_out` only grows for now.
- **Fixed double-click word selection splitting identifiers at
  underscores** in the embedded source cross-probe panel
  (`source_panel.c`). Root cause: GtkTextView's own built-in double-
  click word-selection (Pango's word-break segmentation) treats `_` as
  a boundary, so double-clicking "rdata_a" only selected a fragment --
  a GTK/Pango default, not anything this file was configuring.
  Overridden via the `"extend-selection"` signal, reusing the same
  `[A-Za-z0-9_]` identifier character class already used elsewhere in
  this file for syntax highlighting. Verified via gdb: calling the new
  handler directly at the exact position GTK's default would normally
  split (the final `a` of `rdata_a`) now selects the whole 7-character
  identifier (offsets 9-16); triple-click's line-select granularity
  still correctly falls through to GTK's own default handling.
- **Fixed "select a signal first" firing for every ordinary (non-
  aliased) signal** in the Quick Format popover's netlist-query buttons
  (Show Drivers/Loads/Cone of Logic/Schematic/Gate Schematic).
  `find_target_trace()`/`build_query_signal_path()` required
  `GwTrace::name_full` to be set, but that field is genuinely only ever
  populated for an explicitly aliased signal or one restored from a
  saved session -- every ordinary add path passes a NULL alias.
  Confirmed directly: a plain signal just dragged in from the SST tree
  has `name_full == NULL`, always. Fixed with a new `trace_full_path()`
  helper that falls back to the underlying node/vector's own name
  (`nd->nname`/`vec->bvname`, never shortened by `hier_max_level` the
  way `t->name` can be) using the same discriminant `trace_is_bus()`
  already relies on. `on_show_gate_schematic_clicked()` had its own
  separate instance of the same bug (used `t->name_full` directly,
  bypassing `build_query_signal_path()` entirely) -- fixed the same
  way. Verified live via gdb: a GwTrace built to match an ordinary
  signal's exact shape (no alias, highlighted, backed by a real node)
  now correctly resolves its full path through both the real Show
  Drivers and Show Gate Schematic click handlers.
- **Fixed RTL netlist queries (Show Drivers/Loads/Cone of Logic/
  Schematic) failing for testbench-level and bit-ranged signals.** Two
  independent bugs, both confirmed by directly exercising the real
  slang-netlist CLI: (1) a plain wire declared directly in the
  testbench wrapper (not inside the DUT instance it elaborates) is
  genuinely absent from slang-netlist's queryable node set when the
  configured top is that wrapper, even though gtkwave's own VCD-side
  table has it, and even though the same leaf path one level deeper
  inside the auto-detected DUT instance resolves fine -- exactly the
  "configured top is a testbench wrapper" problem the Yosys-backed gate
  schematic already solved, but never ported to this path. Moved
  `find_dut_instance()` into shared `netlist_query_ui.c`/`.h` (as
  `netlist_query_find_dut_instance()`) and added the same auto-retry to
  `run_query_and_show()`/`refresh_schematic()`. (2) slang-netlist's
  `--drivers` accepts a bit-range suffix but `--fan-in`/`--fan-out`
  (behind Loads/Cone/Schematic) do not -- added shared
  `netlist_query_has_bit_range_suffix()`/`strip_bit_range_suffix()`
  helpers (mirroring the gate schematic's existing private Yosys-side
  copy) and strip the suffix before those three query kinds, with a
  status note that the whole bus is shown instead. Verified live via
  gdb end to end: "Show Cone of Logic" on a real bus signal with both
  problems at once now strips the range, retries with the DUT instance
  inserted, and returns real populated results with working jump-to-
  source links (confirmed visually via screenshot).
  Known remaining limitation, NOT fixed here: a plain pass-through wire
  at ANY intermediate hierarchy level (not just the outermost
  testbench-to-DUT boundary) has the same "could not find node"
  problem -- confirmed: `tb.dut.step` (a module's own internal
  connecting wire for a sub-instance's output) still fails, while
  `tb.dut.u_seq.step` (the actual driving instance) succeeds directly.
  The one-level DUT-insertion fallback only covers the single most
  common case; a fully general fix would need to walk down through
  every intermediate pass-through instance, which needs port-
  connectivity information this fix doesn't have.
- **"Show Cone of Logic" now opens the real gate-level schematic
  diagram, not a flat list or an RTL flow chart.** Two rounds, per
  direct feedback: (1) it used to run the same fan-in query ("Show
  Schematic" already used) through a text results-list window instead
  of a diagram ("show cone of logic provides a list, we should be
  showing a schematic") -- repointed it at schematic_view.c's RTL
  dataflow-graph diagram and removed the now-redundant list code path
  (`QUERY_KIND_FANIN`, `netlist_query_show_cone()`). (2) that RTL
  diagram was immediately (and correctly) rejected too ("now it opens a
  window labeled schematic, but what we get is a flow chart") -- an
  architectural reality of that view (no gates exist pre-synthesis), not
  a bug in it, and exactly the same complaint from earlier this session
  about that same view. Repointed again, this time at
  gate_schematic_view.c's real Yosys-synthesized gate-level diagram
  (confirmed visually: a black-box `alu` instance, bus-width port
  labels, orthogonal wiring) -- making it identical to the separate
  "Show Gate Schematic (SVG)" button, which was removed as the
  resulting duplication. Window title changed from "Gate-Level
  Schematic: %s" to "Cone of Logic: %s" to match. `schematic_view.c`
  (the RTL view) is untouched and still compiles -- just has no UI
  entry point left for now; it could be wired back up under different,
  unambiguous terminology (e.g. "Show RTL Dataflow Graph") if ever
  wanted again, rather than under "schematic"/"cone of logic".
- **Strip unrelated sibling output ports from the gate-level cone of
  logic.** Per direct feedback: querying `alu_result`'s cone also
  showed `alu_zero`, a completely unrelated output of the same `alu`
  instance with no bearing on `alu_result`'s value -- "it's another
  output and it should not be shown... only things that alter the
  alu_result should be shown". Root cause confirmed directly against a
  real Yosys run: `submod` always gives an included cell a complete,
  valid port set, so pulling in the whole `alu` cell (as the driver of
  `result`) also exposes its unrelated `zero` output as a second
  boundary port -- not specific to one cell either, confirmed the same
  thing happens with a register file's unused read ports at deeper
  depths. Fixed by stripping every output port of the submod-extracted
  cone except the one actually queried (`extract_select_leaf_name()` +
  `opt_clean -purge`), guarded by Yosys's own `-assert-min 1` so a
  never-expected match failure aborts cleanly instead of silently
  stripping the real target too. Scoped to the netlistsvg/JSON path
  only -- the dot-based fallback has no equivalent extracted module to
  prune in the same way (a known, narrower gap on that already-reduced-
  capability path). Verified live via gdb with a real signal from the
  dump file's own symbol table: `alu_result`'s cone now yields exactly
  5 nodes (the instance, the one output, and its 3 true inputs) -- no
  `alu_zero` anywhere in the result.
- **Fixed a crash reading a multi-bit signal's value before its first
  real sample** (`local_trace_asciival()` in `gw-mouseover.c`, the
  backend behind the schematic view's live value annotation and the
  signal-under-the-mouse popup) -- discovered as a direct consequence
  of the fix above, since no multi-bit signal had ever successfully
  reached this value-lookup code via the schematic view before
  "Show Cone of Logic" started rendering real diagrams for bus
  signals. Every loader bootstraps a node's history with a pre-dump
  sentinel entry (time -1/-2) whose value is always a plain
  `GW_BIT_X`-style byte, even for a multi-bit ("extvals") node -- no
  real vector string ever gets allocated for it. The value-lookup code
  didn't account for this, misreading that raw byte as a vector string
  pointer and segfaulting whenever a queried time fell before a multi-
  bit signal's first real transition (e.g. before reset deasserts).
  `list_x_view.c`'s `scan_node_trace()` already had the correct guard
  for this exact case -- applied the same check here. Verified live via
  gdb with a real signal from the dump file's own symbol table (not a
  fabricated trace, ruling out a test-harness artifact): reproduced the
  crash, inspected the live `GwHistEnt` confirming the exact mechanism,
  then confirmed the fix resolves it and the schematic renders correct
  "= x" annotations for not-yet-driven signals.
- **Fixed the gate-level cone of logic for a signal nested two or more
  levels below the dut** (e.g. a wrapper module's own sub-instance's own
  sub-instance, as opposed to a signal declared directly on the dut or
  one hop into it -- an everyday pattern in real RTL, built and tested
  against a deliberately deeper 4-module example design to catch this).
  Root cause confirmed directly against real Yosys runs: Yosys's `/`
  selection syntax only ever resolves ONE level of cell nesting below a
  named module ("top/cellA" resolves, "top/cellA/cellB" and
  "top/cellA.wire_in_cellB" don't), while `make_yosys_select()` builds
  exactly that second, unselectable form whenever the queried signal
  lives two or more levels down -- the dotted form only becomes
  selectable once Yosys's own `flatten` has actually renamed the wire to
  that same form itself. Fixed by unconditionally flattening just the
  immediate child cell of the top module on the path to the queried
  signal before selecting (`build_ancestor_flatten_clause()` in
  `gate_schematic_view.c`) -- confirmed `flatten <cell>` recurses into
  everything beneath it, so one flatten is enough regardless of how much
  deeper the real nesting goes, and flattening the same cell twice (it's
  also separately dissolved) is harmless. Also fixed a related bug this
  surfaced in the existing sibling-output-port-stripping logic: its
  `extract_select_leaf_name()` assumed submod always names the extracted
  port after the queried signal's bare final identifier (e.g. "step" from
  "u_seq.step") -- true for a shallow signal, but confirmed directly that
  at 1+ levels of real nesting submod actually keeps the *whole* dotted
  relative path as the port name (e.g. "u_exec.rf_rdata_b", not
  "rf_rdata_b"). Renamed to `extract_select_relative_path()` and fixed to
  keep the whole path. Verified live via gdb against the new, deeper
  example design: querying `u_exec.rf_rdata_b` (one level into the dut)
  now renders a correct diagram -- the register file's memory array as
  one block with wide buses, its true inputs, and no sibling port leak.
  **Known, NOT fixed, honestly still open**: a signal that's ALSO
  re-declared under a different name one or more levels further out,
  connected the whole way by nothing but a straight port connection
  (e.g. this new design's ALU outputs -- `result`/`zero`/`carry`/
  `overflow`/`negative` -- each immediately re-wired to its own
  "alu_"-prefixed wire the next level up) still fails: confirmed
  directly that Yosys's own `opt` either renames such a wire to the
  *outer* declaration instead of keeping the leaf name, or, for a
  multi-bit bus only ever consumed bit-by-bit by further derived signals
  like `result`, eliminates it as a single named object entirely once
  nothing in the selected cone still needs it as one indivisible bus --
  in which case there's no single correct port name to find by any
  static analysis of the select expression at all. The existing
  `-assert-min 1` guard turns this into a normal, reported failure
  ("yosys did not produce a gate-level netlist for this signal") instead
  of a wrong diagram, so it fails safe, but querying e.g. `u_alu.result`
  or `u_alu.negative` through this specific design's wrapper chain
  doesn't work yet. Would need determining the real post-`submod` port
  name empirically (e.g. tagging the root object and finding it by
  attribute afterward) rather than guessing from the select expression --
  tried via a preliminary "ask Yosys what it's still called" pass first,
  but confirmed that doesn't help either, since the renaming/elimination
  happens *during* `submod` itself, not before it.

- **RTL schematic layout editing** (schematic toolbar: Move Nodes / Reset
  Layout). Per-node position overrides layered on top of Graphviz's own
  layout: with "Move Nodes" on, left-drag any box (structural
  Assignment/Conditional nodes included) and only that box moves; edges
  touching it are redrawn, everything else stays exactly as Graphviz drew
  it. Stored as an offset from where Graphviz put the node (so it survives
  small automatic-layout changes), per root signal, process-wide (survives
  closing/reopening the window and Auto Update reloads), written to/read
  from save files (`[schematic_offset]` lines), and included in Export
  Image. Never touches the default path: with nothing moved and Move Nodes
  off, the diagram is still Graphviz's PNG, byte for byte as before.
  Three things the design had to work around, all found by testing rather
  than assumed:
  * **Graphviz can't pin node positions here.** That needs neato/fdp, and
    this machine (and plenty of installs) ships only the `dot` layout
    plugin. So the edit is done on `dot -Tsvg` (`schematic_svg_edit.c`): a
    moved node's group gets a `transform`, affected edges are re-drawn
    between the new boxes (arrowhead, label and colour kept), and the
    result is rasterized with the gdk-pixbuf SVG loader at an explicit pixel
    size (no DPI guessing). The SVG is cached per refresh, so dragging
    re-renders without spawning dot.
  * **slang-netlist renumbers nodes on every run** (`N26` vs `N28` for the
    same `count_q`), so a node id can't remember a position. Identity is a
    structural key instead (`schematic_node_key.c`): Weisfeiler-Lehman
    colour refinement over the FULL cone (so it doesn't change with the
    depth limit). Verified live: after a drag, a fresh refresh renumbered
    the nodes and the box stayed put; a save file loaded in a new process
    reproduced identical positions under different ids.
  * **Parallel edges collapsed onto one line** after a move (a signal and
    its feedback share a node pair); found by looking at a render, fixed by
    fanning them out sideways, with a regression test.
  **Known gaps:** only the RTL (slang-netlist) schematic, not the Yosys
  gate-level view; while Move Nodes is on, click-to-inspect, double-click
  navigation and drag-to-waveform are off (the right-click menu still
  works); redrawn edges are simple curves with no obstacle avoidance, and
  untouched edges can pass behind a moved box; structurally identical
  twin nodes are told apart by Graphviz's placement order, which isn't
  guaranteed stable; the SVG path needs gdk-pixbuf's SVG loader (it falls
  back to Graphviz's layout with a status message if missing) and its text
  renders in a slightly different font than dot's PNG; a move that grows
  the picture upward/leftward shifts the image under the pointer while
  dragging (offsets stay correct); no undo beyond Reset Layout.

- **Auto Update for FST** (File > Auto Update). Polls the loaded FST every
  2s and, when a still-running simulation has flushed new time to it,
  reloads in place through the existing File > Reload Waveform path
  (traces, markers, zoom restored from its saved state), then keeps the
  view pinned to the newest data if it was showing the end -- a view the
  user scrolled back is left alone. Deliberately built on reload rather
  than the originally planned "append history to an imported node" APIs
  (6-10 files in `lib/libgtkwave`): FST imports lazily, so a reload only
  re-reads the hierarchy plus the displayed signals, and that avoids
  mutating `GwHistEnt` chains that aliases, `harray`, and composite bus
  vectors all hold copies of.
  The poll (`src/auto_update_poll.c`, GTK-free, tested against a real
  still-open `fstWriter`) never trusts that the file is readable or that
  growth is visible: an in-progress FST only opens once its hierarchy has
  been flushed, and then only shows data up to the last *flushed* block,
  in bursts. So it gates on `stat()`, then probes with `fstReaderOpen()`,
  then compares the file's own end time (`!=`, so a rewritten/shorter
  re-run reloads too). An unreadable file just means "try next tick" --
  this matters because the reload path itself blocks in a `sleep()` retry
  loop on a failed load. Verified end to end headless: the loaded range
  grew 170 -> 250 -> 310 -> 350 ... one reload per flushed burst, none
  between, and the pinned view followed (start 0 -> 246 -> 286 ...).
  **Known gaps:** FST only (VCD refused with a message) -- but note the
  reload route may make VCD cheap too, untested; refused with a
  skip-start/skip-end window (a reload would silently drop it); a full
  reload per update re-reads the hierarchy, so a very large design will
  want a longer interval (fixed at 2s, no UI/rc setting yet); transient
  UI state outside the save file resets across a reload; a race remains
  if the file becomes unreadable between the probe and the reload.

- **Event Sequence pane** (signal popover -> "Event Sequence"). For one
  timestamp (defaults to the primary marker; Go / At Marker / Prev / Next,
  where Prev/Next step between times any displayed signal changes), lists
  every value change with two independent orderings, rather than adding a
  sequence field to every `GwHistEnt`:
  1. **Dump order (VCD, incl. .gz):** the dump file is re-scanned on demand
     for just that timestamp (`src/event_sequence.c`, GTK-free, unit-tested
     in `src/event_sequence_test.c`). FST can't give this -- its reader
     walks a time step's changes by signal handle, not write order -- so
     Seq is blank there. GHW, bzip2 and stdin dumps are reported as
     unsupported.
  2. **Causality (any format, needs the netlist; "Rank by causality"):**
     each changed signal's fan-in cone, plus a register's clock/reset
     (new `gw_netlist_query_get_sensitivity()`/`get_registers()`), is
     intersected with the set that changed at that timestamp, giving
     "caused by" and a level (0 = nothing changed feeds it). Flags changes
     the simulator dumped *before* their cause. Capped at 64 netlist
     queries (one process spawn each); the rest show as "?".
  Nets with several names (VCD id-code / FST handle aliases) collapse into
  one row, deepest-scoped name primary. "Only displayed signals" filters
  before ranking so the cap is spent on what you care about.
  **Known gaps:** slang-netlist treats a register as a hard cut in both
  directions (`fan-in(y)` for `y = ~q` doesn't list `q`; `fan-out(q)`
  doesn't list `y`), so `clk -> q` is ranked but `q -> logic fed by q`
  is not -- that logic shows at level 0 next to its stimulus. Wires
  slang-netlist can't resolve (plain testbench-internal wires) are
  unranked ("?"). Causality is inferred structure, not simulator delta
  numbers: it can say what *could* have caused a change, not the
  simulator's actual evaluation order. Values are shown as dumped (no
  radix/translate-filter formatting). Verified end to end in the real app
  (gdb-driven, headless) plus 7 backend and 2 netlist-wrapper tests; an
  end-to-end pass caught a double-free in the view's row rendering that
  the unit tests couldn't reach.

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

3. **Gate-level schematic "Cone Window"** (incremental filter/expand/
   dissolve, matching how Verdi's nSchema and Concept Engineering's own
   StarVision -- a past gtkwave contributor, per `bus_ops.c`'s copyright
   line -- both work: start from one small element, grow the diagram
   one hop at a time on request, keep sub-modules as black boxes until
   explicitly asked to dissolve). "Start small"/"Expand" (global
   `%ci<N>` hop-count depth spinner), "Dissolve" (per-instance
   `flatten`, black-box by default, with a right-click "Collapse" to
   undo it), and "Filter Out" (right-click, multi-select via
   shift-click) are all done -- see Done section above, including
   Dissolve's known shared-depth-budget limitation and Filter's missing
   un-filter gesture. What's left is the richer per-net incremental
   model:
   - **Per-net "Start small"/"Expand"**: rather than one global hop-count
     limit, track the actual set of included Yosys object names
     (`included_objects`, seeded with just the root signal) and grow it
     one specific net at a time -- clicking a particular boundary port
     pulls in only what's immediately upstream/downstream of *that* net
     (a fresh one-hop `%ci`/`%co` query seeded from the clicked port's
     net name, unioned into `included_objects`), rather than today's
     "expand everything at the boundary by 2 more hops". This would also
     fix Dissolve's shared-depth-budget limitation, since each
     instance's internal hops would no longer compete with every other
     instance for one global depth number, and would make "un-filter"
     natural too (just remove the object from `included_objects` again,
     the same shape as Dissolve's existing Collapse). Confirmed during
     scoping: a 1-hop selection around `alu_result` produced a 3-cell,
     356x184px diagram (just the `alu` instance, kept as one black box,
     with clean named ports) vs. the always-render-everything ~450-cell,
     5399x16100px full cone -- and a fresh Yosys invocation per
     expand-click (re-synthesizing from source each time, since Yosys
     has no interactive/incremental session mode) was fast enough in ad
     hoc testing (well under a second), though not stress-tested
     against a large real design yet.
   - Click/selection semantics would stay as they are now (plain-click
     selects, shift-click multi-selects, right-click menu offers
     Dissolve/Filter Out/Collapse as applicable, double-click keeps its
     boundary-expand/single-instance-dissolve fast path) -- this item is
     about the underlying *mechanism* (per-net vs. global-hop-count),
     not the interaction model, which is already in place.
   - Known related gap, NOT solved by this plan: selecting/cross-probing
     an individual **wire** (as opposed to a gate/port box) still isn't
     possible -- netlistsvg's SVG gives no net identity to the
     connecting lines themselves, only to the boxes at either end. Would
     need patching netlistsvg or a different renderer; out of scope
     here.
   - The RTL (slang-netlist) schematic view's own "start small, expand"
     sibling piece is now done -- see above (it has no dissolve concept
     of its own, since it's pre-synthesis and has no gates to black-box
     module instances around).

4. ~~**RTL schematic layout editing**~~ -- done, see the Done section above.

5. **Auto Update** -- FST done (see Done above). Remaining: VCD (its
    loader discards the id table after load, so a true incremental path is
    still hard, but a reload-based route as used for FST is worth trying
    first), a configurable interval, and an incremental append path if the
    full-reload cost proves too high on large designs.

6. ~~**Event Sequence pane**~~ -- done, see the Done section above.

7. **Dense Block Drawing** (fast struct/MDA rendering) -- largely
    already covered; deprioritized. Struct/union signals aren't a
    distinct `GwVarType` -- they're flattened into ordinary wide-bitvector
    nodes drawn through `draw_hptr_trace_vector()`/`draw_vptr_trace()`
    (`gw-wave-view-traces.c`). Correction to an earlier note here: the
    loop does NOT issue one draw call per history entry. A segment that
    collapses to zero pixels (`_x0 == _x1`) already skips ahead with
    `bsearch_node()` to the next pixel (`WAVE_OPT_SKIP`), and text
    conversion (`convert_ascii_vec`) only runs for segments wider than
    `GLOBALS->vector_padding`. What remains is segments 1-4px wide, which
    cost one or two `cairo_stroke` calls each; batching those strokes by
    color is the only remaining win, and it touches a hot path every bus
    trace uses. Not measured: `perf` is blocked here
    (`perf_event_paranoid >= 2`). Revisit only with a profile showing a
    real redraw bottleneck on a dense dump.

8. **Transaction/protocol-aware decoding** (a pluggable bus-protocol
    decoder on top of the existing `TR_TTRANSLATED` transaction-trace
    support). A genuinely new capability class; valuable but large,
    speculative scope.
