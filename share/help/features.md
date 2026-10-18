# Getting Started

## Using This Help
keywords: help, search, index, topics, find, manual, shift+f1, documentation
This is the built-in guide to the features of this build of GTKWave: what each one does, where to find it, and how to use it. It is also a plain text file (`share/help/features.md`), so it can be read or searched outside the program.

### Finding what you need
- Open it from **Help > Features and Usage** or press **Shift+F1** (plain F1 is already **Time > Move To Time**).
- Type in the search box to search every topic at once. All words must match, and a word matches the start of a longer word (`sched` finds `schedule`). Matches in a topic's title count most, then its keywords, then its headings, then its text.
- With the search box empty, the left side shows the index: every topic grouped by category.
- Words you searched for are highlighted in the topic text.
- A **See also** link jumps to another topic. **Back** and **Forward** retrace the topics you visited.
- **Ctrl+F** moves to the search box. **Escape** clears the search; when it is already empty, Escape closes the window. **Alt+Left** and **Alt+Right** go back and forward.

### Conventions in this guide
- A bold path such as **Edit > Undo** means a choice in the menu bar: the **Edit** menu, then **Undo**.
- Keys are written like **Ctrl+Z**.
- "The primary marker" and "the baseline marker" are the two time markers several features read a time span from; see [[Markers]].

See also: [[Window Overview]], [[Signal Popover]], [[Keyboard Shortcuts]]

## Window Overview
keywords: window, layout, panes, sst, hierarchy, signal list, wave pane, toolbar, from, to, marker, cursor
The main window has four areas.
- **SST (top left)**: the design hierarchy as a tree of scopes. Select a scope to see its signals.
- **Signal list (below the SST)**: the signals in the selected scope, with **Append**, **Insert** and **Replace** buttons that add the highlighted ones to the waveform.
- **Name column and wave pane (right)**: the displayed signals and their values over time. A time ruler runs along the top.
- **Toolbar**: cut/copy/paste, zoom, time navigation, and the **From** and **To** boxes that set the visible time range. The **Marker** and **Cursor** readouts on the right show the times of the primary marker and of the pointer.

Right-clicking a signal name opens a menu whose **Quick Format...** item opens the [[Signal Popover]], where most of the analysis features in this guide live. The source panel can be shown with **View > Show Source Panel** (see [[Source Panel]]).

See also: [[Signal Popover]], [[Zooming and Scrolling]], [[Save Files and Sessions]]

## Signal Popover
keywords: popover, quick format, right-click, context menu, signal actions, color, format, analog, trace properties, bit range
The popover is the quick-action panel for a signal.

### Opening it
Right-click a signal name in the name column and choose **Quick Format...**. It acts on the highlighted signal.

### What it contains
- **Color**: pick a trace color.
- **Format**: Hex, Dec, Signed, Bin, Oct, ASCII and other radixes; see [[Data Formats and Colors]].
- **Analog**: Off, Step, Interp, Interp+, Screen, All Data; see [[Analog Waveforms]].
- **Trace Properties...**: the full properties dialog.
- **Bit range** (only for a bus): an optional range such as `[3:0]` appended to the signal path for Show Drivers, Show Loads and Show Cone of Logic. Leave it empty to query the whole bus.
- **Show Drivers**, **Show Loads**: see [[Show Drivers and Show Loads]].
- **Show Cone of Logic**: opens the gate-level schematic; see [[Gate-Level Schematic]].
- **RTL Schematic**: opens the pre-synthesis dataflow diagram; see [[RTL Dataflow Schematic]].
- **Event Report**: see [[Signal Event Report]].
- **Event Sequence**: see [[Event Sequence]].
- **Derived Signal...**: see [[Derived Signals]].
- **Bus Operations**: **Create Bus (Down)**, **Create Bus (Up)**, **Expand as Sub-Bus...**, **Reverse Bit Order**, **Edit Bus Composition...**; see [[Bus Operations]].

See also: [[Window Overview]], [[Configuring HDL Sources]]

## Save Files and Sessions
keywords: save file, gtkw, session, restore, reload, write save file, read save file, persist, auto update
A save file records how the waveform window looks so it can be restored later.

### Using save files
- **File > Write Save File** (Ctrl+S) and **Write Save File As** (Shift+Ctrl+S) write one.
- **File > Read Save File** (Ctrl+O) loads one onto the current dump. A save file can also be given on the command line with `-a file.gtkw`.
- **File > Reload Waveform** (Shift+Ctrl+R) re-reads the dump from disk and restores the displayed signals, markers and zoom. See [[Auto Update]] for doing this automatically.

### What is saved
Displayed signals and their radix, color and flags; groups; aliases; markers and zoom; and these features:
- compressed time spans ([[Compress Time Range]]),
- time-span comments ([[Time-Span Comments]]),
- derived signals, saved as their expression and recomputed on load ([[Derived Signals]]),
- hand-moved schematic node positions ([[Moving Schematic Nodes]]).

### Notes
- Older save files simply lack the newer lines; they still load.
- Windows such as the schematic views and results lists are not saved.

See also: [[Undo and Redo]], [[Auto Update]]

## Requirements and Optional Tools
keywords: requirements, dependencies, slang-netlist, yosys, graphviz, dot, netlistsvg, node, npm, gdk-pixbuf, svg, install, missing
The waveform viewing features need nothing beyond the program itself. The design-analysis features call external tools, which must be on your `PATH`.

- **slang-netlist**: Show Drivers, Show Loads and the RTL schematic ([[Show Drivers and Show Loads]], [[RTL Dataflow Schematic]]), and the net information used by [[Event Sequence]] causality ranking.
- **Yosys**: the gate-level schematic ([[Gate-Level Schematic]]).
- **netlistsvg** (needs Node.js; install with `npm install -g netlistsvg`): optional, but it gives the gate-level view real gate symbols and orthogonal wiring, and it is required for clicking boundary ports to expand the cone ([[Expanding the Cone Net by Net]]). Without it the gate-level view falls back to a plainer Graphviz drawing.
- **Graphviz (`dot`)**: both schematic views.
- **gdk-pixbuf with SVG support**: hand-moved schematic layouts ([[Moving Schematic Nodes]]). Without it the schematic shows Graphviz's own layout and says so.

Features that need an HDL design also need to be told where the sources are; see [[Configuring HDL Sources]].

See also: [[Troubleshooting Netlist Features]]

# Viewing and Navigation

## Zooming and Scrolling
keywords: zoom, scroll, pan, fit, page, fetch, discard, move to time, center zooms, from, to
- **Time > Zoom > Zoom Amount** (F2) and **Zoom Base** (Shift+F2) set the zoom directly; the toolbar has zoom in, out and fit buttons.
- **Time > Move To Time** (F1) jumps the view to a time you type.
- **Time > Page** (Alt+7 and Alt+8) and **Time > Fetch** (Alt+1 and Alt+2) move the view by a page or by the fetch size; **Time > Fetch > Fetch Size** (F7) sets it. **Time > Discard** (Alt+3) steps back.
- **View > Center Zooms** (F8) keeps the zoom centered rather than anchored at the left edge.
- The **From** and **To** boxes in the toolbar set the visible range numerically.
- **View > Fullscreen** (F11) toggles fullscreen.

With compressed time spans active, page increments account for them; see [[Compress Time Range]].

See also: [[Markers]], [[Keyboard Shortcuts]]

## Markers
keywords: marker, primary, baseline, named marker, cursor, copy primary, max marker, span
Markers pin times so you can measure and so other features have a range to work on.
- The **primary marker** is the main marker, placed by clicking in the wave pane. The **Marker** readout in the toolbar shows its time.
- The **baseline marker** (the "B marker") is a second one. **Markers > Copy Primary->B Marker** (B) sets it from the primary marker. The difference between the two is the span measured.
- **Markers > Drop Named Marker** (Alt+N) leaves a labeled marker at the current position.
- **View > Toggle Max-Marker** switches the time display between the marker time and the maximum time.

### Features that use the marker span
- [[Compress Time Range]] needs both markers.
- [[Time-Span Comments]] annotate the span between them.
- [[Signal Event Report]] reports over the span, or the whole dump if there is no baseline marker.
- The schematic views and the source panel show signal values at the primary marker ([[RTL Dataflow Schematic]], [[Source Panel]]).
- [[Event Sequence]] opens at the primary marker's time.

See also: [[Zooming and Scrolling]]

## Data Formats and Colors
keywords: radix, hex, decimal, binary, octal, ascii, signed, format, color, data format, invert, reverse bits, filter, translate, enum, fixed point, gray
Each signal has its own display format.

### Choosing a format
- In the [[Signal Popover]], the **Format** row has Hex, Dec, Signed, Bin, Oct and ASCII buttons.
- In the menu: **Edit > Data Format** has Hex (Alt+X), Decimal (Alt+D), Binary (Alt+B), Octal (Alt+O), ASCII, Time, Enum, Signed Decimal and more.
- Other options in that menu: **Right Justify** (Alt+J), **Invert** (Alt+I), **Reverse Bits** (Alt+V), **Gray Filters**, **Popcnt**, **Find First One**, **Fixed Point Shift**, **Range Fill**, and the **Translate Filter File / Process** and **Transaction Filter Process** entries for external translation.
- A bus defaults to hex for wide values and binary for narrow ones.

### Colors
**Edit > Color Format** (Normal, Red, Orange, Yellow, Green, Blue, Indigo, Violet, Cycle) or the popover's **Color** row.

See also: [[Analog Waveforms]], [[Signal Popover]]

## Analog Waveforms
keywords: analog, step, interpolated, annotated, screen data, all data, resize, height, waveform plot
Any multi-bit signal can be drawn as an analog curve instead of a bus.

- In the [[Signal Popover]], use the **Analog** row: **Off**, **Step**, **Interp**, **Interp+** (interpolated and annotated), **Screen** and **All Data**.
- The same choices are in **Edit > Data Format > Analog**: Off, Step, Interpolated, Interpolated Annotated, and under **Resizing**, **Screen Data** or **All Data**.
- **Screen** scales the curve to the values visible on screen; **All Data** scales to the full history.
- **Edit > Insert Analog Height Extension** makes a trace taller so the curve has room.

See also: [[Data Formats and Colors]]

## Compress Time Range
keywords: compress, time range, span, gap, long idle, marker, uncompress, manage compressed spans, plus marker
Collapses a long stretch of uninteresting time into a narrow marker so the activity either side fits on screen together.

### Using it
1. Set the primary and baseline markers around the span to hide (see [[Markers]]). Both are required; it does not default to compressing everything.
2. **Edit > Compress Time Range**. The span becomes a narrow "+" marker on the time axis.
3. **Edit > Manage Compressed Spans...** lists the spans, each with **Go To** and **Uncompress**. **Edit > Uncompress All** removes them all. Clicking a span's "+" marker also uncompresses it.

### Notes
- Clicks, markers, drawing, zoom by pinch and page scrolling all account for compressed spans.
- Spans are saved in save files; see [[Save Files and Sessions]].

See also: [[Zooming and Scrolling]], [[Time-Span Comments]]

## Time-Span Comments
keywords: time span, comment, annotation, range note, document, debug session
Range annotations for documenting a debug session, distinct from point markers.

### Using them
1. Set the primary and baseline markers around the stretch of time ([[Markers]]).
2. **Edit > Add Time-Span Comment...** and type the text.
3. **Edit > Time-Span Comments...** lists them. Each row has **Go To**, **Edit...** (changes the text; the time span stays) and **Delete**.

Comments are saved in save files ([[Save Files and Sessions]]) and can also be added over the automation interface ([[WCP and the MCP Server]]).

See also: [[Compress Time Range]]

## Keyboard Shortcuts
keywords: keyboard, shortcut, hotkey, accelerator, keys, f1, ctrl, alt, shift
Shortcuts as bound in this build.

### File
- **Ctrl+N** new window, **Ctrl+T** new tab, **Ctrl+W** close, **Ctrl+Q** quit.
- **Ctrl+O** read save file, **Ctrl+S** write save file, **Shift+Ctrl+S** write save file as.
- **Shift+Ctrl+R** reload waveform, **Ctrl+P** print to file, **L** read sim logfile.

### Edit
- **Ctrl+Z** undo, **Ctrl+Shift+Z** redo, **Ctrl+X / Ctrl+C / Ctrl+V** cut, copy, paste, **Ctrl+Delete** delete.
- **Ctrl+B** insert blank, **Alt+A** alias highlighted trace, **Shift+Alt+A** remove aliases.
- **F3** expand, **F4** combine down, **F5** combine up.
- **G** create group, **T** toggle group open/close, **F12** expand all groups, **Shift+F12** collapse all groups.
- **Alt+X / Alt+D / Alt+B / Alt+O** hex, decimal, binary, octal; **Alt+J**, **Alt+I**, **Alt+V** right justify, invert, reverse bits (add Shift for off).
- **Alt+R** highlight regexp, **Shift+Alt+R** unhighlight, **Ctrl+F** show-change first highlighted, **H** toggle trace hierarchy.

### Search, time and view
- **Alt+S** signal search regexp.
- **F1** move to time, **F2** zoom amount, **Shift+F2** zoom base, **F7** fetch size, **Alt+1 / Alt+2** fetch back / forward, **Alt+3** discard, **Alt+7 / Alt+8** page back / forward.
- **B** copy primary to baseline marker, **Alt+N** drop named marker.
- **F8** center zooms, **F11** fullscreen, **Alt+G** grid, **Alt+F1** show base symbols.

### Help
- **Shift+F1** this guide.

See also: [[Using This Help]]

# Organizing Signals

## Groups and Group Manager
keywords: group, group manager, nest, ungroup, rename group, move into group, collapse, expand groups
Groups fold related signals under one collapsible heading.

- **Edit > Create Group** (G) groups the highlighted signals. **Toggle Group Open|Close** (T), **Expand All Groups** (F12) and **Collapse All Groups** (Shift+F12) fold them.
- **Edit > Group Manager...** lists every group, indented by nesting depth, with:
  - **Move Selected Here**: moves the highlighted signals into the chosen group. Moving a group into itself or one of its own subgroups is refused.
  - **Rename**.
  - **Ungroup**: dissolves the group, leaving its members in place in the group above.
- Groups and their nesting are saved in save files.

See also: [[Undo and Redo]], [[Bus Operations]]

## Bus Operations
keywords: bus, combine, create bus, sub-bus, expand, slice, reverse bit order, edit composition, bits, vector
Build and reshape multi-bit signals from the [[Signal Popover]]'s **Bus Operations** section.

- **Create Bus (Down)** and **Create Bus (Up)** combine the highlighted signals into one bus (also **Edit > Combine Down** F4 and **Combine Up** F5).
- **Expand as Sub-Bus...** slices a contiguous bit range out of a bus into its own narrower bus. Enter the range in the dialog.
- **Reverse Bit Order** flips the bit order of the bus display (a toggle).
- **Edit Bus Composition...** shows the bus's bits. **Move Up**, **Move Down** and **Remove** change the selected bit; **Add Bit...** accepts a signal path (a single-bit signal; not another bus). **Apply** creates a new bus named `<original>_edited` and leaves the original alone.
- **Edit > Expand** (F3) splits a bus into its individual bits.

See also: [[Undo and Redo]], [[Derived Signals]]

## Undo and Redo
keywords: undo, redo, ctrl+z, history, restore, delete, ungroup, combine, expand
**Edit > Undo** (Ctrl+Z) and **Edit > Redo** (Ctrl+Shift+Z) reverse and re-apply destructive changes to the signal list: delete, ungroup, combine up/down and expand.

- Up to 10 steps are kept.
- A new destructive change clears the redo history.
- Undo restores the signal list, groups, markers and colors. It never reopens the dump file.

See also: [[Groups and Group Manager]], [[Save Files and Sessions]]

## Aliases, Blanks and Comments
keywords: alias, rename trace, blank, comment, spacing, label, remove aliases
- **Edit > Alias Highlighted Trace** (Alt+A) gives the highlighted trace a display name of your choice. **Remove Highlighted Aliases** (Shift+Alt+A) restores the real name.
- **Edit > Insert Blank** (Ctrl+B) adds an empty row for spacing. **Edit > Insert Comment** adds a text row.
- Aliases, blanks and comments are saved in save files.

See also: [[Save Files and Sessions]]

## Derived Signals
keywords: derived, virtual signal, expression, computed, formula, and, or, xor, add, equals, ternary, new signal
A derived signal is computed from an expression over existing signals and shown as a normal trace: radix, color, analog mode and markers all work on it.

### Creating one
1. Optionally highlight the signals you want to use.
2. Right-click a signal, **Quick Format...**, then **Derived Signal...**.
3. Type the **Expression**. The dialog lists the highlighted signals with short names you can use instead of full paths: the leaf name when it is unique (`count`), or `s1`, `s2`, ... Full names such as `top.u.count` always work.
4. Optionally give a **Name**; otherwise the expression text is the name.
5. Click **Add** or press Enter. A problem is shown in the dialog without closing it.

### Examples
- `a & b` is high when both are high.
- `count + 1`, `count == 3` and `(a | b) && !reset`.
- `state == 4'd2 ? data : 8'h00`.
- `{hi, lo}` joins two buses; `data[7:4]` takes the upper nibble.

### How it behaves
- The result is evaluated across the whole dump. An operand counts as unknown (X) before its first change.
- It is saved in the save file as its expression and recomputed on load, so it survives **Reload Waveform** and [[Auto Update]].
- To change one, delete the trace and add it again.

### Limits
- Operands must be bit or vector signals (not real or string).
- A derived signal cannot use another derived signal.
- Arithmetic and comparisons work on up to 64 bits; the other operators have no width limit.

See also: [[Derived Signal Expression Language]], [[Bus Operations]], [[Save Files and Sessions]]

## Derived Signal Expression Language
keywords: expression, operators, precedence, verilog, literal, concatenation, replication, select, x, z, unknown, width, reduction, shift, ternary
The expression language follows Verilog's operators and precedence, with unsigned values and four states (0, 1, X, Z).

### Operands
- **Signals**: `top.u.count`, or in quotes for unusual characters: `"top.bus[7:0]"`. A bus can be written with or without its range (`top.bus` or `top.bus[7:0]`).
- **Literals**: `42`, `8'hFF`, `4'b10x1`, `12'o777`, `'d5`, `8'd200`. Unsized literals are just wide enough for their value; `x` and `z` digits are allowed in binary, octal and hex.
- **Concatenation** `{a, b}`, **replication** `{4{a}}`, **bit select** `a[3]`, **part select** `a[7:4]` (bit numbers count from the least significant bit).

### Operators, loosest binding first
- `?:`, then `||`, `&&`, `|`, `^` and `^~`/`~^`, `&`, `==` `!=`, `<` `<=` `>` `>=`, `<<` `>>`, `+` `-`, `*` `/` `%`, and unary operators.
- Unary: `~` bitwise not, `!` logical not, `-` negate, and the reductions `&` `|` `^` `~&` `~|` `~^` which collapse a bus to one bit.

### Width rules
- `+ - * / % & | ^` and `?:` produce a result as wide as the wider operand. There is no carry bit: results wrap, so `count + 1` wraps at the width of `count`. Widen it by joining zeros: `{1'b0, count} + 1`.
- Shifts keep the left operand's width.
- Comparisons, `&&`, `||`, `!` and the reductions produce one bit.
- Concatenation adds widths; replication multiplies them.

### Unknown values
- Bitwise and logical operators work bit by bit: `0 & x` is 0, `1 | x` is 1, and `1'bx && 1'b0` is 0.
- Arithmetic, comparisons and shift amounts are pessimistic: if any operand bit is X or Z, the whole result is X.
- `c ? a : b` with an unknown `c` keeps the bits on which `a` and `b` agree and makes the rest X.
- Selecting a bit beyond the width gives X. Division by zero gives X.

### Errors
The dialog reports a syntax error with its column, an unknown signal by name, or an operand too wide for arithmetic (over 64 bits).

See also: [[Derived Signals]]

# Finding Problems

## Pattern Search
keywords: pattern search, search, find value, exact value, wildcard, stable for, occurrence, jump to, fwd, bkwd, mark, edge, rising, falling
**Search > Pattern Search 1** and **Pattern Search 2** open the pattern-search dialog, which finds times where signals match a pattern.

### Using it
1. Open **Search > Pattern Search 1** (or 2). The dialog lists the displayed signals (up to 500), each with a drop-down for what to look for and, where relevant, a box for the value.
2. For each signal you want to constrain, choose a search type: an edge, a particular value, and so on. The exact-value type matches the signal's displayed value (scalar or bus) without regard to case, with `*` (any run of characters) and `?` (any one character) as wildcards.
3. The start and end boxes limit the search to part of the dump: **Start of Time** or a named marker, and a named marker or **End of Time**.
4. **Fwd** and **Bkwd** move to the next or previous match. **Mark** marks the matches (the dialog shows how many), and **Clear** removes the marks.
5. **Jump to occurrence #** with **Go** searches from the start of the range and stops on the Nth match.
6. **Stable for >=** rejects a match unless it holds for at least that many time units, which filters out glitches.

See also: [[List X]], [[Signal Event Report]]

## List X
keywords: list x, unknown, x values, undefined, uninitialized, find x, bad values, trace drivers, go to
**Search > List X...** finds every stretch of time where a signal has an unknown (X-like) value.

### Using it
1. Open **Search > List X...**. It scans the displayed signals and also the whole design hierarchy, so you do not need to display a signal first.
2. Each row is one contiguous interval for one signal, with its time range.
3. **Go To** moves the marker and view to the interval.
4. **Trace Drivers** opens [[Show Drivers and Show Loads]] for the signal (disabled when no full hierarchical path can be worked out).

### Notes
- Multi-bit signals are treated as unknown if any bit is.
- It works for both ordinary multi-bit signals and constructed buses.

See also: [[Pattern Search]], [[Show Drivers and Show Loads]]

## Signal Event Report
keywords: event report, edges, rising, falling, duty cycle, transitions, statistics, high time, activity
Counts activity on signals over a time span.

### Using it
1. Highlight one or more signals.
2. In the [[Signal Popover]], click **Event Report**.
3. A table lists, for each signal: rising edges, falling edges, total transitions, duty cycle (percentage of time high) and the longest continuous high period.

### The time span
If the baseline marker is set, the span between the primary and baseline markers is used; otherwise the whole dump. See [[Markers]].

### Notes
Multi-bit signals only report their total transition count.

See also: [[Event Sequence]], [[Pattern Search]]

## Event Sequence
keywords: event sequence, delta cycle, order, causality, same timestamp, simultaneous, dump order, ranking, causes, race
Shows what changed at one instant, in what order, and what plausibly caused what. Useful for zero-delay races and delta-cycle questions.

### Using it
1. Right-click a signal, **Quick Format...**, then **Event Sequence**. It opens at the primary marker's time.
2. Type another time and press **Go**, use **At Marker** to return to the marker, or **Prev** and **Next** to step between times at which any displayed signal changes.
3. Each row is one net that changed. **Seq** is its position in the dump's own order, **Signal** and **Value** are what changed, and **Note** says how many alternate names the net has and flags a change dumped before its own cause.
4. Tick **Only displayed signals** to limit the list to what is in the waveform.
5. Tick **Rank by causality** to order by what feeds what. This needs the design; see [[Configuring HDL Sources]]. **Level** 0 means no other changed signal feeds it; **Caused by** lists the changed signals that do.

### Where the order comes from
- **Dump order** is recovered by re-reading the dump for that one timestamp. It works for VCD (including gzipped) and not for FST, whose reader does not keep the order within a time step; the Seq column is blank there.
- **Causality** is inferred from the design: a signal's combinational inputs, plus a register's clock and reset.

### Limits
- Causality is structure, not simulator evaluation order: it shows what could have caused a change.
- The netlist tool treats registers as a hard boundary, so a register's own output is not linked to the logic it feeds: a clock edge is linked to the register, but not onward to logic fed by that register.
- Signals the netlist tool cannot resolve are shown unranked. At most 64 signals are looked up per request.
- Dumps read from standard input, bzip2 and GHW files are not supported.

See also: [[Signal Event Report]], [[Show Drivers and Show Loads]], [[Markers]]

# Design Analysis

## Configuring HDL Sources
keywords: hdl, sources, source files, top module, configure, slang, yosys, design, verilog, systemverilog, first use
The design-analysis features need the design's source files and the name of its top module. You are asked once per session, the first time you use one.

### The dialog
- **Choose Source Files...** selects the Verilog or SystemVerilog files that make up the design.
- **Top module** is pre-filled with a guess from the dump's top scope. It must be the module the waveform was dumped from.

### Testbench wrappers
If the top is really a testbench, a plain signal declared directly in it may not exist in the elaborated design. The tools then retry one level down inside the module the testbench instantiates, and say so in the status bar. This is a best-effort text scan.

### Who uses it
[[Show Drivers and Show Loads]], [[Gate-Level Schematic]], [[RTL Dataflow Schematic]], [[Event Sequence]] causality ranking, and the source file picker in [[Source Panel]].

See also: [[Requirements and Optional Tools]], [[Troubleshooting Netlist Features]]

## Show Drivers and Show Loads
keywords: drivers, loads, fan-out, fan-in, who drives, who reads, netlist query, depth, module boundary, source location, bit range
Answer "what drives this signal?" and "what does this signal feed?".

### Using them
1. Right-click a signal, **Quick Format...**, and click **Show Drivers** or **Show Loads**. For a bus, you can first enter a **Bit range** such as `[3:0]`.
2. A window lists the results. **Show Loads** lists the full transitive fan-out, not just the immediate readers. Click a row's source location to open that line in the [[Source Panel]].

### Limiting a big result
The **Show Loads** window has a **Max depth** spinner, a **Stop at module boundary** checkbox and a **Refresh** button. By default there is no limit.

### Notes
- Needs slang-netlist and the sources ([[Configuring HDL Sources]]).
- The same queries are available to scripts; see [[WCP and the MCP Server]].

See also: [[Gate-Level Schematic]], [[List X]]

## Gate-Level Schematic
keywords: gate-level, schematic, cone of logic, fan-in, yosys, gates, flip-flop, mux, synthesized, netlistsvg, depth, start small
**Show Cone of Logic** draws the logic that feeds a signal as real synthesized gates, flip-flops and multiplexers.

### Opening it
Right-click a signal, **Quick Format...**, **Show Cone of Logic**. For a bus, a **Bit range** is widened to the whole bus (the status bar says so).

### Starting small
The cone starts shallow so the diagram stays readable. The **Depth** spinner sets how many hops from the signal are shown (0 means unlimited). Boundary ports mark where the cone was cut.

### Reading it
- Boundary ports are connector arrows at the cut edges. Sub-module instances appear as single boxes.
- Click a gate to jump to its source line in the [[Source Panel]].
- Mouse wheel scrolls; **Shift+wheel** scrolls sideways; **Ctrl+wheel** zooms. The toolbar has **-**, **+**, **Zoom to Fit** and **1:1**.
- **Export Image...** saves the diagram as a PNG at full resolution.

### Going further
- Grow the cone one net at a time: [[Expanding the Cone Net by Net]].
- Open up sub-modules or remove things: [[Dissolve, Filter Out and Collapse]].

### Notes
- Needs Yosys and the sources ([[Configuring HDL Sources]]). The top module must be synthesizable RTL; a testbench top is handled by the retry described there.
- Real gate symbols and clickable boundary ports need netlistsvg ([[Requirements and Optional Tools]]).

See also: [[RTL Dataflow Schematic]], [[Schematic Window Controls]]

## Expanding the Cone Net by Net
keywords: expand, boundary port, net, grow cone, start small, one hop, shrink expansion, incremental, upstream
Instead of deepening the whole diagram, grow it from one spot.

### Using it
1. In the [[Gate-Level Schematic]], double-click an **input boundary port**. Only that net's driver is pulled in, one hop upstream; every other branch stays as it was. New boundary ports appear where the new logic is cut.
2. Repeat on whichever new port interests you.
3. To undo one, right-click anywhere and choose **Shrink expansion at "..."** for that net.
4. Double-clicking the **output** port (the signal you queried) does nothing; it explains this in the status bar.

### Notes
- The **Depth** spinner still sets how deep the queried signal itself is shown; expansions are added on top of it.
- Expansions follow the net by name. If a net can no longer be found (it was renamed by optimization, or sat inside an instance you then collapsed), it is dropped with a message.
- Expansions last as long as the window.
- Requires netlistsvg; see [[Requirements and Optional Tools]].

See also: [[Dissolve, Filter Out and Collapse]]

## Dissolve, Filter Out and Collapse
keywords: dissolve, flatten, filter out, collapse, restore, sub-module, instance, black box, multi-select, shift-click
Reshape the gate-level diagram around sub-modules and clutter.

### Selecting
Click a box to select it. **Shift+click** adds or removes boxes from the selection. Right-click opens the menu.

### The menu
- **Dissolve "name"**: opens a sub-module instance into its internal gates. Double-clicking an instance does the same.
- **Filter Out**: removes the selected objects from the cone. Boundary ports cannot be filtered.
- **Collapse "name"**: puts a dissolved instance back into one box.
- **Restore "name"**: brings back something you filtered out.
- **Shrink expansion at "..."**: see [[Expanding the Cone Net by Net]].

### Notes
- Dissolving or collapsing renumbers the anonymous gates Yosys creates. A filter or expansion that names one of those anonymous gates could then point at a different gate, so such entries are dropped with a status message when the set of dissolved instances changes. Entries naming declared signals or instances are kept.
- Inside a dissolved instance the cone is still limited by the **Depth** spinner and by the expansions you have made; expand individual ports to see further.

See also: [[Gate-Level Schematic]]

## RTL Dataflow Schematic
keywords: rtl schematic, dataflow, slang-netlist, assignment, conditional, value annotation, live values, edge colors, transition, back, depth
The pre-synthesis view: a diagram of the elaborated design's dataflow (Assignment, Conditional, Case, Const and signal nodes), with live values.

### Opening it
Right-click a signal, **Quick Format...**, **RTL Schematic**.

### Reading it
- Each named node shows the signal's value at the primary marker and updates as the marker moves ([[Markers]]).
- Nets are colored by value: red for X-like, dark orange for Z, green for driven high; the colors can be changed ([[Configuration Options]]).
- A dashed box marks a signal the tool named but could not resolve to a full path (for example, beyond the depth limit).
- **Depth** limits the hops shown (0 is unlimited); a node at the edge says it has more upstream, and double-clicking it shows more.
- **Prev Transition** and **Next Transition** move the marker to the root signal's previous or next transition without leaving the diagram.

### Navigating
- Click a node to see its path in the status bar. Double-click a signal node to re-center the diagram on it; **Back** returns.
- Right-click a node for **Show Drivers**, **Show Loads** and **Add to Waveform**.
- Drag a resolved node onto the signal or wave pane to add it as a trace.

### Notes
- Needs slang-netlist, Graphviz and the sources ([[Configuring HDL Sources]]).
- You can rearrange the boxes; see [[Moving Schematic Nodes]].

See also: [[Gate-Level Schematic]], [[Schematic Window Controls]]

## Moving Schematic Nodes
keywords: move nodes, layout, reposition, drag, reset layout, edit layout, arrange, schematic layout
Rearrange the boxes of the [[RTL Dataflow Schematic]] by hand, on top of Graphviz's automatic layout.

### Using it
1. Click **Move Nodes** on the schematic toolbar.
2. Drag any box, including Assignment and Conditional boxes. Only that box moves; the connections to it are redrawn.
3. Click **Move Nodes** again to leave move mode. **Reset Layout** puts every moved box back where Graphviz placed it.
4. **Export Image...** includes your layout.

### What to know
- While **Move Nodes** is on, clicking a node to inspect it, double-clicking to navigate and dragging to the wave pane are switched off. Right-click still works.
- Positions are remembered per signal, kept when the window is closed and reopened, and written to save files ([[Save Files and Sessions]]).
- A moved box is remembered by its place in the design's structure (not by its number, which changes on every run), so it stays put as the marker moves and across sessions. If the design changes, the entries stop matching and are ignored.
- Redrawn connections are simple curves with no routing around other boxes. Connections you did not touch are unchanged.
- Needs gdk-pixbuf with SVG support; without it the layout is Graphviz's own and the status bar says so.

See also: [[Schematic Window Controls]]

## Schematic Window Controls
keywords: schematic, zoom, fit, scroll, export image, png, depth, colors, toolbar, wheel
Controls shared by both schematic windows.

- **Zoom**: **-**, **+**, **Zoom to Fit** and **1:1** on the toolbar; **Ctrl+wheel** also zooms. A window fits itself when it first opens, then keeps your zoom.
- **Scrolling**: wheel scrolls up and down, **Shift+wheel** scrolls sideways.
- **Depth**: how many hops of fan-in are shown; 0 means all.
- **Export Image...**: saves the full-resolution diagram as a PNG, using a name based on the signal path. It saves the diagram, not the current zoom or pan.
- **Edge colors** in the RTL view can be set in `.gtkwaverc`; see [[Configuration Options]].

See also: [[Gate-Level Schematic]], [[RTL Dataflow Schematic]]

## Source Panel
keywords: source, source panel, cross-probe, code, definition, instantiation, file, line, value annotation, stems
An embedded view of the design source, linked to the waveform.

### Using it
- **View > Show Source Panel** shows it.
- Clicking a source location in [[Show Drivers and Show Loads]] or a gate in the [[Gate-Level Schematic]] opens that file and line here.
- Once the HDL sources are configured, a file list above the path lets you switch to another source file ([[Configuring HDL Sources]]).
- For signals already displayed, their values at the cursor are shown beside the source and update as the cursor moves.
- **Search > Open Source Definition** and **Open Source Instantiation** open the source for a scope selected in the hierarchy; they use the stems file read with **File > Read Verilog Stemsfile** (or the `-t` option).

See also: [[Configuring HDL Sources]]

# Live Dumps and Automation

## Auto Update
keywords: auto update, live, tail, running simulation, follow, reload, fst, growing dump, poll
**File > Auto Update** keeps the display current while a simulation is still writing its dump.

### Using it
1. Open the FST dump the simulation is writing.
2. Tick **File > Auto Update**. It checks the file every 2 seconds.
3. When newly written time is available it reloads the waveform in place, exactly like **Reload Waveform**: your signals, markers and zoom are restored.
4. If the view was showing the end of the data, it stays pinned to the newest data. If you had scrolled back, your window is left alone.
5. Untick it to stop.

### What to know
- FST only. VCD is refused with a message.
- It will not turn on when you gave a skip-start/skip-end window, because a reload would drop it.
- A growing FST becomes readable only after the simulator has flushed it, and shows only what has been flushed, so updates arrive in bursts.
- Each update re-reads the hierarchy, so a very large design may feel slow.

See also: [[Save Files and Sessions]], [[Derived Signals]]

## WCP and the MCP Server
keywords: wcp, mcp, automation, scripting, remote control, api, ai, agent, json-rpc, gtkwave-mcp, wcp-port
A running GTKWave can be driven from other programs.

### WCP
Start with `gtkwave --wcp` (optionally `--wcp-port=PORT`, default 8765; `--wcp-remote` allows connections from other machines). Commands include adding and removing signals and markers, setting the viewport, loading and reloading, and in this build: netlist drivers, loads and cone, List X, time-span comments, undo and redo.

### MCP
`gtkwave-mcp` is a small bridge that lets an MCP client talk to a running `gtkwave --wcp`. The client starts it and speaks JSON-RPC over its input and output; each tool call is passed to GTKWave as the matching WCP command. The tools are: `get_item_list`, `get_item_info`, `set_item_color`, `add_variables`, `add_items`, `add_markers`, `remove_items`, `focus_item`, `clear`, `set_viewport_to`, `set_viewport_range`, `zoom_to_fit`, `load`, `reload`, `netlist_drivers`, `netlist_loads`, `netlist_cone`, `list_x`, `time_span_comment_add`, `time_span_comments_list`, `undo`, `redo`.

### Notes
- The netlist tools never pop up the HDL-source dialog; they report `not_configured` instead, so configure the sources in the program first ([[Configuring HDL Sources]]).
- State persists between calls because it is the same running GTKWave.

See also: [[Show Drivers and Show Loads]], [[List X]], [[Time-Span Comments]], [[Undo and Redo]]

## Configuration Options
keywords: gtkwaverc, rc, configuration, settings, schematic colors, schematic_color_x, schematic_color_z, schematic_color_active, vector_padding
Options go in `.gtkwaverc`.

- `schematic_color_x`, `schematic_color_z` and `schematic_color_active` set the colors the [[RTL Dataflow Schematic]] gives to X-like nets, Z nets and driven-high nets. They default to red, dark orange and forest green. Use any color Graphviz accepts, such as `#112233`.
- A single option can also be given on the command line, as an rc line in quotes: `gtkwave -4 "schematic_color_x #112233" dump.fst`.

See also: the `gtkwaverc` manual page for the full list of options.

# Troubleshooting

## Troubleshooting Netlist Features
keywords: troubleshooting, error, failed, not found, could not find node, nothing there to show, not synthesizable, finish, testbench, no signal, unresolved
Common messages and what to do.

- **"Could not find 'slang-netlist' / 'yosys' / 'dot'"**: install the tool or add it to `PATH` ([[Requirements and Optional Tools]]).
- **"could not find node"** or an empty result for a signal in a testbench: the signal may not exist in the elaborated design; the tools retry inside the instantiated module. Check the **Top module** ([[Configuring HDL Sources]]).
- **Gate-level view: "yosys did not produce a gate-level netlist"**: the top is not synthesizable RTL (for example a testbench with an unguarded `$finish`), or the signal was merged away by optimization. Query a signal that is a real net in the DUT.
- **A bit range is ignored in the gate-level view**: Yosys cannot select bit slices, so the whole bus is used; the status bar says so.
- **Clicking boundary ports does nothing**: they only exist when netlistsvg is installed ([[Requirements and Optional Tools]]).
- **Dashed box in the RTL schematic**: a signal the tool named but could not resolve to a path.
- **"Couldn't expand ..."**: the net was renamed or removed ([[Expanding the Cone Net by Net]]).
- **Derived signal errors**: see [[Derived Signal Expression Language]].

See also: [[Known Limitations]]

## Known Limitations
keywords: limitations, known issues, unsupported, caveats, limits, not supported
Gathered from the topics above.

- **Event Sequence**: dump order is VCD only; causality does not link a register's output to the logic it feeds; at most 64 signals ranked.
- **Derived signals**: no real or string operands, no derived-from-derived, no editing in place, 64-bit arithmetic.
- **Auto Update**: FST only; a reload per update.
- **Gate-level schematic**: needs synthesizable RTL; dissolving an instance cannot show its whole interior automatically, so expand its ports individually; expansions and filters last as long as the window.
- **Schematic layout**: simple redrawn curves; only the RTL view; needs SVG support in gdk-pixbuf.
- **Compress Time Range**: needs both markers.

See also: [[Troubleshooting Netlist Features]]
