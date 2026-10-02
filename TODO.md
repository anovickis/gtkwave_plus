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
  filter/expand/dissolve "Cone Window" interaction model (see the
  scoped plan below -- this round's `submod`/one-hop-selection findings
  are its foundation, but the click-to-expand/dissolve UI itself isn't
  built yet).

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
   explicitly asked to dissolve). Scoped and empirically validated this
   session (both load-bearing mechanisms confirmed working against the
   real toy-CPU test design, not just read about):
   - **"Start small"**: seed the diagram from a single-hop selection
     (Yosys `select <signal> %ci`, immediate drivers only) instead of
     today's full transitive `%ci*` cone. Confirmed: a 1-hop selection
     around `alu_result` produced a 3-cell, 356x184px diagram (just the
     `alu` instance, kept as one black box, with clean named ports) --
     vs. today's always-render-everything ~450-cell, 5399x16100px cone.
   - **"Expand"**: clicking a boundary port connector (an `inputExt`/
     `outputExt` symbol, i.e. a cut wire) should pull in whatever's
     immediately upstream/downstream of that specific net and re-render.
     Mechanism: re-run Yosys with the growing set of previously-included
     object names unioned with a fresh one-hop `%ci`/`%co` query seeded
     from the clicked port's net name, then re-`submod`/re-render.
     Yosys has no interactive/incremental session mode, so this means a
     fresh Yosys invocation per expand-click (re-synthesizing from
     source each time) rather than incrementally mutating a live
     process -- confirmed fast enough for a modest design in this
     session's own ad hoc testing (well under a second), but not
     stress-tested against a large real design yet.
   - **"Dissolve"**: a sub-module instance (any Yosys cell whose type is
     a user module, not a `$_AND_`-style primitive) starts as a single
     black box -- confirmed: selecting a non-flattened module instance
     and `submod`-extracting it still renders as one labeled box via
     netlistsvg's generic-cell fallback shape, not its internal gates.
     Clicking it should add its instance path to a persisted
     "dissolved" set and re-run with `flatten <that instance's
     selection>` (confirmed `flatten` accepts a selection argument, not
     just whole-design) instead of today's unconditional global
     `flatten;`, then re-render -- which will itself introduce new
     boundary ports around what used to be the instance's black-box
     edge, ready for further expand/dissolve.
   - **New persistent per-window state** (`GateSchematicState`):
     `included_objects` (explicit Yosys object names pulled in so far,
     seeded with just the root signal) and `dissolved_instances`
     (instance paths to flatten, starts empty); a single rebuild
     function re-derives the whole Yosys script from both sets on every
     interaction.
   - **Filter**: a visible way to prune the current diagram back down
     (remove an included object, re-render) -- the inverse of expand;
     not yet designed in detail, lowest priority of the three verbs.
   - Click semantics become three-way depending on what's clicked: a
     boundary port connector -> expand; an undissolved black-box
     instance -> dissolve; an already-dissolved primitive gate -> the
     existing click-to-source behavior, unchanged.
   - Known related gap, NOT solved by this plan: selecting/cross-probing
     an individual **wire** (as opposed to a gate/port box) still isn't
     possible -- netlistsvg's SVG gives no net identity to the
     connecting lines themselves, only to the boxes at either end. Would
     need patching netlistsvg or a different renderer; out of scope
     here.
   - A parallel, much smaller opportunity for the RTL (slang-netlist)
     schematic view: `GwNetlistQueryLimits.max_depth` already exists
     (shipped earlier this project for the Show Drivers/Loads results
     windows) but isn't wired into `schematic_view.c`'s own cone
     rendering, which always queries unlimited depth. Passing a shallow
     depth + a similar "double-click an edge node to re-query one level
     deeper" interaction would give a cheaper, smaller-lift version of
     the same "start small, expand" idea for the RTL view specifically,
     reusing existing infrastructure instead of Yosys selection algebra.
     Worth doing as a standalone smaller item, independent of the gate-
     view work above.

4. **RTL schematic layout editing** (reposition instances by hand,
   print/capture an edited layout). What's left of the original
   "Editable schematic mode" bundle that the Cone Window plan above
   doesn't cover -- needs a real editable-layout data model, since
   Graphviz's auto-layout has no notion of a user-repositioned node.
   Real standalone work, not a quick pickup. (Configurable color themes,
   navigation-only "last view" undo, print/capture (Export Image), and
   drag-to-wave-pane, all originally bundled in this item, are now done
   -- see above. Zoom/pan was already done before this list existed.
   Collapse/expand a module to a black box, also originally bundled
   here, is now covered by item 3's dissolve mechanism instead.)

5. **Auto Update** (live-tail a still-running simulation). A genuinely
    different workflow (active-sim debugging vs. post-hoc analysis),
    real nWave feature. Investigated this session: no existing "re-scan
    the currently-open dump file for appended data" capability at all
    (confirmed no stub, no timer, no half-built path). VCD's parser
    (`lib/libgtkwave/src/gw-vcd-loader.c`) closes its file handle and
    discards its VCD-id-to-node symbol table after the initial load, and
    each node's history is a frozen/immutable `GwVlist` after that --
    genuinely not resumable as built. FST is more favorable (its reader
    context stays alive for the dump file's lifetime and its block-table
    design is naturally growable) but still needs a "resume block scan
    from last position" variant. Real implementation needs new
    "append more history to an already-imported node" APIs in
    `GwVcdFile`/`GwFstFile`, a `GwTimeRange` mutability/replacement path,
    and a new id-remapping/resumable-parser-state object for VCD --
    roughly 6-10 files in `lib/libgtkwave` plus a new driver in `src/`.
    Medium-to-large, not a bounded single-session task; deferred rather
    than rushed, same reasoning as Waveform Compare.

6. **Event Sequence pane** (sub-timestamp/delta-cycle detail). Valuable
    but needs a real new sub-timestamp data model; highest effort on
    this list.

7. **Dense Block Drawing** (fast struct/MDA rendering) -- performance/
    scale feature for struct-heavy designs. Investigated this session:
    SystemVerilog struct/union signals aren't a distinct `GwVarType`
    today -- they're flattened into ordinary wide-bitvector nodes and
    drawn through the same `draw_hptr_trace_vector()`/`draw_vptr_trace()`
    path as any other bus (`gw-wave-view-traces.c`), which already skips
    drawing a text value (falls back to a plain colored block) once a
    segment's pixel width drops below `GLOBALS->vector_padding` -- so the
    "too narrow, simplify" precedent already exists, just not tuned as a
    distinct "dense" mode. The TODO's actual ask is *performance* for
    struct-heavy designs, though, and the current loop still issues one
    draw call per history entry regardless of width; a real fix needs
    pre-aggregating sub-pixel-width consecutive segments into batched
    draws before the draw loop runs, inside a large (~300-line),
    performance-critical, universally-used rendering function every bus
    trace in every session goes through. Deferred as too high-risk for a
    single self-contained patch to a hot path this central; a narrower,
    purely-cosmetic threshold tweak was considered and rejected since it
    wouldn't deliver the performance win the item actually asks for.

8. **Transaction/protocol-aware decoding** (a pluggable bus-protocol
    decoder on top of the existing `TR_TTRANSLATED` transaction-trace
    support). A genuinely new capability class; valuable but large,
    speculative scope.
