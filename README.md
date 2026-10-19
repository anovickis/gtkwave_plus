# GTKWave Plus

A fork of [GTKWave](https://github.com/gtkwave/gtkwave), the GTK+ based wave viewer for Unix and Win32 which reads FST and GHW files as well as standard Verilog VCD/EVCD files.

## What this fork adds

This repository, **gtkwave_plus**, is a fork of [gtkwave/gtkwave](https://github.com/gtkwave/gtkwave) that adds debugging and analysis features aimed at RTL design and verification work. Everything upstream does still works; the additions are listed below. A built-in, searchable guide to all of them (what each does, where it is, how to use it) is in the program under **Help > Features and Usage** (Shift+F1), and as a plain file at [`share/help/features.md`](share/help/features.md).

**Tracing the design from a waveform**
- *Show Drivers / Show Loads*, and a gate-level *Cone of Logic* schematic (Yosys, with real gate symbols when netlistsvg is installed) that you can grow one net at a time, dissolve sub-modules in, and filter.
- An RTL dataflow schematic with live values at the marker, whose boxes you can rearrange by hand.
- *List X*: every stretch where something is unknown, anywhere in the hierarchy, with a jump to its drivers.
- *Event Sequence*: what changed at one timestamp, in dump order, ranked by what could have caused what.

**Working with signals**
- *Derived signals*: Verilog-style expressions over existing signals (`(a & b) | c`, `count + 1`, `state == 3`), computed across the whole dump.
- *Structs and unions*: add a struct scope as an expandable group with its packed value, or describe the fields of a packed struct that arrived as one flat bus.
- Bus operations, a group manager, time-span comments, compressing long idle stretches, undo/redo, and richer pattern search.

**Live and scripted use**
- *Auto Update*: follow an FST dump a running simulation is still writing.
- An MCP bridge and WCP commands so scripts and AI tools can drive a running instance.

### Optional tools

The waveform viewing itself needs nothing extra. The design-analysis features call out to these when present:

| Tool | Used for |
| --- | --- |
| slang-netlist | Show Drivers / Loads, the RTL schematic, Event Sequence causality |
| [Yosys](https://github.com/YosysHQ/yosys) | the gate-level schematic |
| [Graphviz](https://graphviz.org) (`dot`) | both schematics |
| [netlistsvg](https://github.com/nturley/netlistsvg) (needs Node.js) | real gate symbols, and clicking boundary ports to expand the cone |

### Running the tests

```sh
meson test -C build
```

The help guide is tested against the program's own menus, so the text in `share/help/features.md` cannot silently drift from what the menus actually contain; update it whenever a feature or shortcut changes.

### Status

Developed on Linux against GTK 3. The macOS and Windows build paths from upstream are untouched but have not been exercised for the additions. `TODO.md` records what each feature does and, just as importantly, where it stops: its known limits and what is still open.

---

## GTKWave

GTKWave is a fully featured GTK+ based wave viewer for Unix and Win32 which reads FST, and GHW files as well as standard Verilog VCD/EVCD files and allows their viewing.

## Building GTKWave from source

### Installing dependencies

Debian, Ubuntu:

```sh
apt install build-essential meson gperf flex desktop-file-utils libgtk-3-dev libgtk-4-dev \
            libbz2-dev libjudy-dev libgirepository1.0-dev libjson-glib-dev
```

Fedora:

```sh
dnf install meson gperf flex glib2-devel gcc gcc-c++ gtk3-devel gtk4-devel \
            gobject-introspection-devel desktop-file-utils tcl
```

macOS:

```sh
brew install desktop-file-utils shared-mime-info       \
             gobject-introspection gtk-mac-integration \
             meson ninja pkg-config gtk+3 gtk4 json-glib
```

### Building GTKWave


```sh
git clone "https://github.com/anovickis/gtkwave_plus.git"
cd gtkwave_plus
meson setup build && cd build && meson install
```

### Running GTKWave
```sh
gtkwave [path to a .vcd, .fst, .ghw dump file or a .gtkw savefile]
```
For more information about available command line parameters refer to the built in-help (`gtkwave --help`) or the [`gtkwave` man page](https://gtkwave.github.io/gtkwave/man/gtkwave.1.html).
