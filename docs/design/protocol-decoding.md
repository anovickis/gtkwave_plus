# Protocol-aware decoding — design pass

Status: proposal only, nothing implemented. Builds on the existing
Transaction Filter Process (`src/ttranslate.c`, `docs/quickstart/filters.md`).

## 1. What exists today

A trace (or a Combined vector, F4) can be piped through an external
executable (Edit > Data Format > Transaction Filter Process).

* **Input** (stdin): a reduced VCD. `$comment` lines carry `data_start`,
  `name`, `min_time`, `max_time`, `max_seqn`, `args`, then one
  `$comment seqn N <full name>` plus `$var` per *distinct underlying bit
  node* of the single selected trace (`save_nodes_to_export_generic`, the
  `WAVE_EXPORT_TRANS` branch stops after the first trace). Nodes are
  ordered by a splay tree over node pointers, not by bit position or
  signal name, so a filter must map roles through the `seqn` names.
* **Output** (stdout): `$name`, `#time text` (optional `?color?` prefix,
  empty text = gap), `M<A-Z>time label` markers, `$next` for extra output
  rows, `$finish`.
* Result is a string-region `GwBitVector` with the original kept in
  `transaction_cache`. One process per filter, run synchronously, whole
  time range at once.

## 2. Limits that matter for protocols

1. Input is one trace. A bus protocol (valid/ready/addr/data/resp) must
   first be hand-combined, and roles are identified by parsing names.
2. Output is flat text: no fields, no hierarchy, so no "show only writes
   to 0x40", no click-to-expand, no search.
3. No navigation: cannot jump to next/previous transaction or error.
4. Per-trace config only; not saved with roles, not reusable across
   instances (4 identical AXI ports means 4 manual setups).
5. Blocking, re-executed on every refresh; no caching.
6. Windows pipe handling is duplicated and untested here.

## 3. Proposal

### 3.1 Decoder model: keep the process boundary, widen the contract

Do **not** embed a scripting runtime. Keep external executables (language
agnostic, crash-isolated) but add a v2 protocol, negotiated by a first
output/input line, so v1 filters keep working unchanged.

* **Role binding.** A decoder ships a small manifest (`*.decoder`, INI):
  name, executable, and required/optional *roles* with widths
  (`clk`, `valid`, `ready`, `addr:32`, `data:*`, `resp:2` ...). The user
  binds roles to signals in a dialog; the bound set is sent as
  `$comment role <name> <seqn>` lines, so no name parsing and no Combine
  step. Bindings are saved in the .gtkw (`[protocol_decode]`).
* **Structured output.** Add a record form alongside `#time text`:
  `T <start> <end> <kind> <summary>` followed by `F <name> <value>`
  field lines and `E <severity> <text>` for protocol errors. GTKWave
  draws the summary on the bar; fields go in a tooltip and an optional
  child-row expansion (reusing the group/collapse machinery).
* **Instances.** "Apply to all matching" binds one manifest to every
  scope that has the roles under a naming pattern (`*_axi_{aw,w,b}*`),
  using the existing SST scope matching.
* **Async + cache.** Run in a GTask, show a "decoding…" placeholder,
  cache by (manifest, bindings, dump mtime) so reloads and Auto Update
  only redecode on change.

### 3.2 Host-side features enabled by structured records

* Next/previous transaction, next error (keys in the Search menu).
* Transaction list window (sortable table: time, kind, fields) with
  filter text; selecting a row moves the cursor. This is the
  Event Sequence view's sibling and can share its table code.
* Transaction-aware derived signals later: expose `<decoder>.valid_count`
  etc. through the derived-signal evaluator.

### 3.3 First protocols

Bundle reference decoders in `share/decoders/`, written in C or Python
(Python only if present; tests skip otherwise):

1. **APB** — simplest (psel/penable/pready), proves role binding and
   structured output. Ship first.
2. **AXI4-Lite**, then **AXI4/AXI-Stream** (channel interleaving and IDs
   are what really stress the model: out-of-order responses need
   transaction *linking* across channels, i.e. an `L <id> <id>` record).
3. UART/SPI/I2C as low-risk crowd pleasers.

## 4. Testing

* Decoder contract tests with canned stdin/stdout pairs (no GTK),
  including malformed output (must not crash, shows an error row).
* Golden transaction streams from small Verilator-generated FSTs for APB.
* Record parser fuzzed with valgrind like the other GTK-free modules.
* Help-vs-menu test keeps `share/help/features.md` honest.

## 5. Risks

* Large dumps: sending all history to every decoder is O(N) per
  instance; mitigate with the cache and, later, time-window requests.
* Role naming ambiguity across vendors (`awvalid` vs `AWVALID`) —
  fuzzy auto-bind with confirmation, never silent.
* Scope creep: transaction linking is the hard part; defer it past APB.

## 6. Suggested slicing

1. v2 contract + role binding + APB decoder + saved bindings.
2. Structured display (tooltip/expansion) + next/prev navigation.
3. Transaction list window; AXI-Lite.
4. Apply-to-all instances; AXI with linking.
