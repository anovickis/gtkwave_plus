#!/usr/bin/env python3
"""
fsdb_native_to_vcd.py -- clean-room-reverse-engineered native FSDB reader.

Usage: python3 fsdb_native_to_vcd.py <input.fsdb>

Exit-code contract (the C caller in gtkwave relies on this exactly):
  - Exit 0: every scope/variable in the file decoded with FULL confidence.
    A complete, valid VCD is printed to stdout. Nothing is printed to
    stderr in this case.
  - Exit non-zero (1): the file has ANY feature this reader doesn't fully
    understand yet. Nothing is printed to stdout. A one-line diagnostic
    goes to stderr. The caller should fall back to the fsdb2vcd-based
    conversion path (contrib/fsdb2vcd) instead.

This is deliberately all-or-nothing. FSDB is Synopsys's proprietary format;
this reader was built by clean-room reverse engineering (self-generated
VCD -> vcd2fsdb -> byte-level analysis, never Synopsys's SDK or docs) across
many rounds, documented in FINDINGS.md alongside the research this was
consolidated from. Several real features are only partially understood --
rather than risk emitting a subtly-wrong waveform, this script refuses
anything it isn't sure about and lets the (much slower, but universally
correct) fsdb2vcd conversion path handle it.

Known, deliberate gaps (all cause a clean refusal, never a wrong answer):
  - Vector (multi-bit) signals with more than 3 real transitions.
  - Vector signals with 2 or 3 transitions whose start value is not zero.
    (A single-transition (N=1) vector with a non-zero start value IS
    supported, but only when the start value is recoverable with full
    confidence: width=3/4 via a literal start-value bit-array, or a small
    set of width=4/8 "special" start values recognized by exact byte
    signature. Other N=1 shapes -- width=8 with a start value outside that
    signature set, and widths other than 3/4/8 -- still decline cleanly.)
  - Vector widths other than 3, 4, or 8 bits.
  - Two or more vector variables sharing one compressed value block.
  - 4-state (0/1/x/z) values.
  - Scalar deltas (time between transitions) greater than 255 in whatever
    the file's internal time unit is.
  - Scope nesting 2+ levels deep, or multiple top-level scopes.
  - Anything the byte-level parser doesn't recognize at all.

Known limitation that is NOT a correctness risk, just an approximation:
this format's own time unit/scale has never been decoded (nothing in the
research to date found where -- or whether -- FSDB stores something like
VCD's $timescale). The relative spacing between events is decoded exactly;
the VCD emitted here always declares `$timescale 1ns $end` as a fixed,
arbitrary label for that spacing, which does not affect correctness of the
waveform itself, only the units a viewer displays.

See the fsdb_re research directory's FINDINGS.md for the full derivation
history this consolidates. That work is not duplicated here -- this file
only re-implements the specific, validated rules needed to go from bytes
to a VCD, restricted to the subset understood with enough confidence for
an unconditional "trust exit 0" contract.
"""
import re
import sys
import zlib


class IncompleteDecode(Exception):
    """Raised anywhere in this module for "not fully understood" -- the
    top-level entry point catches this (and only this) and turns it into
    the documented "refuse, don't guess" exit path."""


# =============================================================================
# Hierarchy / variable-table discovery
#
# Verbatim port of fsdb_hierarchy.py's logic (see that file's own docstring
# for the byte-level derivation). Only the *reuse* is new here.
# =============================================================================

VAR_TABLE_ANCHOR = re.compile(rb'\xbf([\x00-\x20])')
SCOPE_TABLE_END = re.compile(
    rb'\x00\x00\x01\x01\x00\x00\x00((?:\xfd\x00)+)\xff\x11\x00\x00\x11\x00\x00')
TABLE_END_MARKER = b'\xff\x11\x00\x00\x11\x00\x00'
_NAME_LIKE = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{1,30}')


def _read_cstr_ending_at(data, end_pos):
    if end_pos < 1 or data[end_pos - 1] != 0x00:
        raise IncompleteDecode("expected NUL terminator at %d" % (end_pos - 1))
    start = end_pos - 1
    while start > 0 and 0x20 <= data[start - 1] < 0x7f:
        start -= 1
    name = data[start:end_pos - 1].decode('ascii')
    if not name:
        raise IncompleteDecode("empty name ending at %d" % end_pos)
    return name, start


def _check_no_unaccounted_names(data, region_start, already_found):
    search_start = max(0, region_start - 400)
    _check_no_unaccounted_names_between(data, search_start, region_start, already_found)


def _check_no_unaccounted_names_between(data, region_start, region_end, already_found):
    for mm in _NAME_LIKE.finditer(data, region_start, region_end):
        candidate = mm.group().decode('ascii')
        if candidate not in already_found:
            raise IncompleteDecode(
                "unaccounted-for name-like content ('%s') near the scope "
                "table -- likely multiple top-level scopes, not supported" %
                candidate)


_INTERMEDIATE_SCOPE_TAIL = b'\x00\x01\x00'


def _parse_intermediate_scope_name(data, next_start):
    """`next_start` is the file offset where the following entity (the leaf
    scope name, in the only depth this is used for) begins. Parses one
    "true intermediate" scope name (has both a parent and a child scope)
    immediately before it, per the formula confirmed in FINDINGS.md (fork
    rounds N+5/N+6, dense-swept over 13 independent name lengths with zero
    exceptions):
        [[ptr_field(name_len)] [name, NOT NUL-terminated]
         [mystery_byte][second_byte][0x00, 0x01, 0x00]]
    where ptr_field(len) is `bytes([0x60+len, 0x00])` for len<=3 or
    `bytes([0x60, 0x00, len-3])` for len>=4, and mystery_byte/second_byte
    jointly encode n = name_len+3 as a radix-8 split (byte_hi = n // 8 =
    second_byte, byte_lo = 0x60 | ((n % 8) << 2) = mystery_byte).

    Returns (name, ptr_field_start). Raises IncompleteDecode if the bytes
    don't match this exact shape -- this formula is only confirmed for a
    single intermediate level (a 3-level top>mid>sub nest); anything that
    doesn't fit is refused, not guessed."""
    tail_start = next_start - 5
    if tail_start < 0 or data[tail_start + 2:tail_start + 5] != _INTERMEDIATE_SCOPE_TAIL:
        raise IncompleteDecode("intermediate scope tail marker not found")
    mystery_byte, second_byte = data[tail_start], data[tail_start + 1]
    if (mystery_byte & 0x03) != 0 or mystery_byte < 0x60:
        raise IncompleteDecode("intermediate scope mystery byte shape mismatch")
    n = second_byte * 8 + ((mystery_byte - 0x60) >> 2)
    name_len = n - 3
    if name_len < 1 or name_len > 64:
        raise IncompleteDecode("intermediate scope name length out of range")

    name_end = tail_start
    name_start = name_end - name_len
    if name_start < 0:
        raise IncompleteDecode("intermediate scope name runs off start of data")
    name_bytes = data[name_start:name_end]
    if not all(0x20 <= b < 0x7f for b in name_bytes):
        raise IncompleteDecode("intermediate scope name not printable ASCII")
    name = name_bytes.decode('ascii')

    if name_len <= 3:
        ptr_field = bytes([0x60 + name_len, 0x00])
    else:
        if name_len - 3 > 0xff:
            raise IncompleteDecode("intermediate scope name too long for pointer field")
        ptr_field = bytes([0x60, 0x00, name_len - 3])
    ptr_start = name_start - len(ptr_field)
    if ptr_start < 0 or data[ptr_start:name_start] != ptr_field:
        raise IncompleteDecode("intermediate scope pointer-field mismatch")

    return name, ptr_start


def parse_scope(data):
    matches = list(SCOPE_TABLE_END.finditer(data))
    if not matches:
        var_anchor = _find_var_table_anchor(data)
        if var_anchor is not None:
            _check_no_unaccounted_names(data, var_anchor.start(), [])
        return []
    if len(matches) > 1:
        raise IncompleteDecode(
            "%d scope-table closings found (multiple/sibling top-level "
            "scopes) -- not supported" % len(matches))
    m = matches[0]
    depth = len(m.group(1)) // 2
    leaf_name, leaf_start = _read_cstr_ending_at(data, m.start())

    if depth == 1:
        _check_no_unaccounted_names(data, leaf_start, [leaf_name])
        return [leaf_name]

    if depth == 3:
        # Exactly one "true intermediate" scope (has both a parent and a
        # child) sits between the outermost and leaf scopes -- the shape
        # FINDINGS.md fully characterized (rounds N+5/N+6). depth==2 (no
        # true intermediate at all) has a documented, unresolved byte-level
        # discrepancy from every formula tried (see FINDINGS.md, fork round
        # N+11) and is deliberately NOT attempted here -- it falls through
        # to the generic decline below.
        mid_name, ptr_start = _parse_intermediate_scope_name(data, leaf_start)
        outer_name, outer_start = _read_cstr_ending_at(data, ptr_start)
        names = [outer_name, mid_name, leaf_name]
        _check_no_unaccounted_names(data, outer_start, names)
        return names

    raise IncompleteDecode(
        "scope nesting depth %d not supported (only 1 and 3 confirmed)" % depth)


def _find_var_table_anchor(data):
    for m in VAR_TABLE_ANCHOR.finditer(data):
        pos = m.end()
        nul = data.find(b'\x00', pos)
        if nul < 0 or nul == pos:
            continue
        name_bytes = data[pos:nul]
        if len(name_bytes) > 64:
            continue
        if all(0x20 <= b < 0x7f for b in name_bytes):
            return m
    return None


def parse_variables(data):
    """Returns (variables, table_end_pos), where variables is a list of
    (name, kind) in declaration order (kind is 'scalar' or
    'unsupported:<tag>[:...]'), and table_end_pos is the file offset right
    after the parsed table's own TABLE_END_MARKER -- used by the caller to
    sanity-check that nothing else was left unparsed after a non-scalar
    variable (parse_variables always stops there; see module docstring)."""
    m = _find_var_table_anchor(data)
    if m is None:
        raise IncompleteDecode("variable name/handle table anchor not found")

    pos = m.end()
    variables = []
    while True:
        nul = data.find(b'\x00', pos)
        if nul < 0:
            raise IncompleteDecode("unterminated variable name at %d" % pos)
        name = data[pos:nul].decode('ascii', errors='replace')
        pos = nul + 1

        if pos >= len(data):
            raise IncompleteDecode("truncated variable table after name %r" % name)
        subtag = data[pos]
        if subtag == 0x10:
            pos += 3
            variables.append((name, 'scalar'))
        else:
            marker_pos = data.find(TABLE_END_MARKER, pos + 1)
            if marker_pos < 0:
                raise IncompleteDecode("no table-end marker found after variable %r" % name)
            shares_table = marker_pos != pos + 1 + 3
            tag = 'unsupported:0x%02x' % subtag
            if not shares_table:
                variables.append((name, tag))
                return variables, marker_pos + len(TABLE_END_MARKER)

            cursor = pos + 1 + 3
            shared_names = []
            ok = True
            c = cursor
            while True:
                if data[c:c + len(TABLE_END_MARKER)] == TABLE_END_MARKER:
                    break
                if c + 1 >= len(data):
                    ok = False
                    break
                c += 1
                nul2 = data.find(b'\x00', c)
                if nul2 < 0 or nul2 == c or nul2 - c > 64:
                    ok = False
                    break
                name2_bytes = data[c:nul2]
                if not all(0x20 <= b < 0x7f for b in name2_bytes):
                    ok = False
                    break
                name2 = name2_bytes.decode('ascii', errors='replace')
                c = nul2 + 1
                if c >= len(data) or data[c] != 0x11:
                    ok = False
                    break
                c = c + 1 + 3
                shared_names.append(name2)
                if c > marker_pos:
                    ok = False
                    break

            # Whether or not the extra names structurally parsed, this is a
            # shared (2+ vector variables, one payload) file -- out of scope
            # for the strict "full history" contract either way (see
            # decode_vector_variable's own docstring). Record it as such;
            # the caller rejects the whole file.
            if ok and shared_names:
                tag += ':shares-table-with-other-variable(s):' + ','.join(shared_names)
                variables.append((name, tag))
                for nm in shared_names:
                    variables.append((nm, 'unsupported:0x11:shared-secondary-of:' + name))
            else:
                tag += ':shares-table-with-other-variable(s)'
                variables.append((name, tag))
            return variables, marker_pos + len(TABLE_END_MARKER)

        if data[pos:pos + len(TABLE_END_MARKER)] == TABLE_END_MARKER:
            return variables, pos + len(TABLE_END_MARKER)
        if pos < len(data) and data[pos] == 0xff:
            pos += 1
        if pos >= len(data) or not (0x00 <= data[pos] <= 0x20):
            raise IncompleteDecode(
                "unexpected byte 0x%02x at %d after variable %r" %
                (data[pos], pos, name))
        pos += 1


def discover(data):
    """Returns (scope_path, variables). Raises IncompleteDecode for
    anything not confidently resolved -- unlike fsdb_hierarchy.py's
    discover(), an unresolved scope path is NOT tolerated here (this
    reader's contract needs a confidently-correct hierarchy, not just
    confidently-correct values)."""
    scope_path = parse_scope(data)
    variables, table_end_pos = parse_variables(data)
    return scope_path, variables, table_end_pos


# =============================================================================
# Scalar (1-bit) decoding -- verbatim port of fsdb_decode_poc.py.
# =============================================================================

def _find_all(data, pattern, start=0):
    out = []
    idx = start
    while True:
        idx = data.find(pattern, idx)
        if idx < 0:
            break
        out.append(idx)
        idx += 1
    return out


def _parse_entry_body(data, pos, n_hint=None):
    if pos + 3 > len(data):
        return None
    eb0, eb1, handle = data[pos], data[pos + 1], data[pos + 2]
    if (eb0 + eb1) & 0xff != 0x33:
        return None
    start_value = eb0 - 0x31
    if start_value not in (0, 1):
        return None
    dict_indices = [handle]
    p = pos + 3
    i = 1
    while True:
        if n_hint is not None and i >= n_hint:
            break
        if p >= len(data):
            return None
        if data[p] == 0xc1 and n_hint is None:
            break
        if p + 2 > len(data):
            return None
        seq, dict_idx = data[p], data[p + 1]
        expected_seq = ((i - 1 + start_value) % 2) + 1
        if seq != expected_seq:
            return None
        dict_indices.append(dict_idx)
        p += 2
        i += 1
    if p >= len(data) or data[p] != 0xc1:
        return None
    return start_value, len(dict_indices), dict_indices, p + 1


def _try_parse_normal_entry(data, pos):
    if pos + 7 > len(data) or data[pos] != 0xc0:
        return None
    n_hint = data[pos + 1] - 1
    if n_hint < 1 or data[pos + 2:pos + 4] != b'\x00\x00':
        return None
    return _parse_entry_body(data, pos + 4, n_hint=n_hint)


def _try_parse_elided_n1_entry(data, pos):
    if pos + 7 > len(data) or data[pos] != 0xc0:
        return None
    if data[pos + 1] != 0x02:
        return None
    handle = data[pos + 5]
    if data[pos + 6] != 0xc1:
        return None
    return 1, [handle], pos + 7


def parse_start_value_table(data, num_vars):
    candidates = _find_all(data, b'\xc0')
    for start in candidates:
        pos = start
        entries = []
        ok = True
        prev_start_value = None
        for _ in range(num_vars):
            parsed = _try_parse_normal_entry(data, pos)
            if parsed is not None:
                start_value, n, dict_indices, next_pos = parsed
                entries.append((start_value, n, dict_indices))
                prev_start_value = start_value
                pos = next_pos
                continue

            elided = _try_parse_elided_n1_entry(data, pos)
            if elided is not None and prev_start_value is not None:
                n, dict_indices, next_pos = elided
                start_value = prev_start_value
                entries.append((start_value, n, dict_indices))
                pos = next_pos
                continue

            ok = False
            break
        if ok and len(entries) == num_vars:
            return entries
    raise IncompleteDecode(
        "could not locate start-value table for %d scalar variable(s)" % num_vars)


def parse_delta_dictionary(data, dict_size):
    prefix = bytes([0xfe, (dict_size + 1) & 0xff, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00])
    idx = data.find(prefix)
    if idx < 0:
        raise IncompleteDecode(
            "could not locate shared delta-time stream (dict_size=%d)" % dict_size)
    pos = idx + len(prefix)
    values = []
    count = 0
    while len(values) < dict_size:
        if count >= 3 and (count - 3) % 4 == 0:
            if pos >= len(data) or data[pos] != 0x00:
                raise IncompleteDecode("expected chunk separator at dict-position %d" % count)
            pos += 1
        if pos >= len(data):
            raise IncompleteDecode("delta dictionary truncated")
        values.append(data[pos])
        pos += 1
        count += 1
    return values


def decode_scalars(data, names):
    """Returns {name: (width=1, [(time, value), ...])} -- full, absolute-
    time histories including the t=0 start value."""
    entries = parse_start_value_table(data, len(names))
    dict_size = 0
    for _, _, dict_indices in entries:
        dict_size = max(dict_size, max(dict_indices))
    dictionary = parse_delta_dictionary(data, dict_size)

    out = {}
    for name, (start_value, n, dict_indices) in zip(names, entries):
        history = [(0, start_value)]
        t = 0
        value = start_value
        for dict_idx in dict_indices:
            if dict_idx < 1 or dict_idx > len(dictionary):
                raise IncompleteDecode("dict index out of range for %r" % name)
            t += dictionary[dict_idx - 1]
            value ^= 1
            history.append((t, value))
        out[name] = (1, history)
    return out


# =============================================================================
# Vector (multi-bit) decoding -- verbatim port of the validated subset of
# fsdb_vector_n2_poc.py: N=2 (any of widths 3/4/8) and N=3 (width=4 only,
# non-ambiguous case), plus an N=1 (single transition) decoder ported from
# the separate, standalone fsdb_vector_poc.py prototype (FINDINGS.md rounds
# N+15/N+20/N+21/N+22 -- that prototype was validated against real files but
# never wired into this consolidated script, which is why N=1 vectors with a
# non-zero start value were declining here even though the underlying
# encoding was already solved). N=4, N>=5, multi-variable-shared-payload,
# and any width outside {3,4,8} are all deliberately NOT ported here -- each
# only ever yields a *partial* result in the original research (final value
# only, or an inherently ambiguous shape), which can never satisfy this
# script's all-or-nothing "full history" contract. See module docstring.
# =============================================================================

PREFIX_WIDTH4_START0 = bytes.fromhex('07040004000000304300')
PREFIX_WIDTH3_START0 = bytes.fromhex('07040003000000304300')
PREFIX_WIDTH8_START0 = bytes.fromhex('07040008000000304300')
WIDTH8_BODY_PREFIX = bytes.fromhex('a0000101')
SEPARATOR = b'\x00\x01'

# Every N=1 (single-transition) vector payload shares one overall shape:
#   byte[0]           = idx0 (varies with the transition's START value)
#   byte[1:3]         = constant 04 00
#   byte[3:7]         = bit_width, little-endian uint32
#   byte[7]           = constant 0x30
#   byte[8:-4-width]  = "start-value region" -- content depends on idx0
#   byte[-4-width:-4] = the transition's END value: one byte per bit,
#                       MSB-first, each literally 0x00 or 0x01 (confirmed
#                       unconditionally, for any width/value -- FINDINGS.md
#                       round N+13)
#   byte[-4:]         = trailer, always c1 21 00 00
#
# Two independently-confirmed shapes for the start-value region:
#
# 1. "Generic" shape: idx0=0x12 at width=4 (FINDINGS.md round N+15/N+20,
#    exhaustively confirmed for all 12 non-special width=4 start values), or
#    idx0=0x10 at width=3 -- the start-value region is EXACTLY [width bytes:
#    a literal start-value array, the same one-byte-per-bit MSB-first
#    scheme as the end value] + [00 01 separator]. The width=3 case was
#    previously only characterized as "idx0 is always 0x10, no special/
#    non-special split, no formula found" (FINDINGS.md round N+20) -- nobody
#    had actually tried decoding that region as a literal bit-array; it
#    turns out to be the exact same generic scheme as width=4's idx0=0x12,
#    just keyed by a different (but still constant, not value-dependent)
#    idx0. Directly re-verified here against all 6 real width=3 N=1 corpus
#    files (start values 0, 0, 2, 3, 4, 7) against their .vcd ground truth.
#
# 2. "Special" shape: idx0 in {0x07, 0x08, 0x09}. The start-value region's
#    exact bytes are a reproducible, deterministic function of the start
#    value alone (confirmed: files with the same start value but different
#    end values/delta times produce byte-identical start-value regions),
#    but no general formula for it is known -- only a handful of exact
#    byte-for-byte signatures have been confirmed against real ground truth
#    (FINDINGS.md rounds N+13/N+14/N+20/N+21/N+22). Anything not an exact
#    match against KNOWN_N1_SIGNATURES below would decode the END value
#    correctly but cannot recover the START value; per this project's
#    all-or-nothing discipline, that case declines rather than guesses (see
#    decode_n1_vector_payload).
KNOWN_N1_SIGNATURES = {
    # width=4 special start values {0, 1, 8, 15} (idx0 0x07 for 0/1, 0x08
    # for 8/15 -- FINDINGS.md round N+20's idx0-as-f(start) formula). Each
    # signature confirmed identical across multiple independent real corpus
    # files sharing the same start value but different end values (0:
    # p004/z6s00; 1: t2001/z6s01; 8: n1s8/t2008/z6s08; 15: n1sf/z6s15).
    (4, bytes.fromhex('430008000001')): 0,
    (4, bytes.fromhex('430008010001')): 1,
    (4, bytes.fromhex('014400070001')): 8,
    (4, bytes.fromhex('014000070001')): 15,
    # width=8 special start values {0, 0xff} (idx0 0x07 for 0, 0x08 for
    # 0xff -- FINDINGS.md round N+13/N+14). Confirmed identical across
    # p008/p08b/p08c/r800 (start=0) and p08d/r8ff (start=0xff).
    (8, bytes.fromhex('4300a0000a01')): 0,
    (8, bytes.fromhex('01c0000b0001')): 0xff,
}


def decode_n1_vector_payload(payload):
    """Returns (start_value, end_value, width) for a single-transition
    vector payload, or raises IncompleteDecode. Only ever returns a result
    when BOTH the start and end value are known with full confidence --
    this reader has no use for a partial ("end value only") result, since
    it can never produce a complete, t=0-inclusive VCD history without the
    start value."""
    if len(payload) < 12 or payload[1:3] != b'\x04\x00' or payload[7] != 0x30:
        raise IncompleteDecode("N=1: payload header shape mismatch")
    width = int.from_bytes(payload[3:7], 'little')
    if width not in (3, 4, 8):
        raise IncompleteDecode("N=1: unsupported width %d" % width)
    if payload[-4:] != TRAILER:
        raise IncompleteDecode("N=1: trailer mismatch")

    idx0 = payload[0]

    if (idx0, width) in ((0x12, 4), (0x10, 3)):
        if len(payload) != 14 + 2 * width:
            raise IncompleteDecode(
                "N=1: unexpected payload length for the generic "
                "start-array shape (idx0=0x%02x, width=%d)" % (idx0, width))
        start_array = payload[8:8 + width]
        sep = payload[8 + width:10 + width]
        end_array = payload[10 + width:10 + 2 * width]
        if sep != SEPARATOR:
            raise IncompleteDecode(
                "N=1: missing 00 01 separator after start-value array")
        if any(b not in (0, 1) for b in start_array) or \
                any(b not in (0, 1) for b in end_array):
            raise IncompleteDecode(
                "N=1: start/end array is not all literal 0/1 bytes")
        return _bits_to_int(start_array), _bits_to_int(end_array), width

    if idx0 in (0x07, 0x08, 0x09) and width in (4, 8):
        mystery = payload[8:len(payload) - 4 - width]
        start_value = KNOWN_N1_SIGNATURES.get((width, mystery))
        if start_value is None:
            raise IncompleteDecode(
                "N=1: start-value region %s (idx0=0x%02x, width=%d) is "
                "not a known signature" % (mystery.hex(), idx0, width))
        end_array = payload[-4 - width:-4]
        if any(b not in (0, 1) for b in end_array):
            raise IncompleteDecode(
                "N=1: signature-matched end-value array is not all "
                "literal 0/1 bytes")
        return start_value, _bits_to_int(end_array), width

    raise IncompleteDecode(
        "N=1: no known decode for idx0=0x%02x width=%d" % (idx0, width))
TRAILER = b'\xc1\x21\x00\x00'

WIDTH8_X_SIGNATURES = {
    2: bytes.fromhex('a9004600'),
    4: bytes.fromhex('89006500'),
    6: bytes.fromhex('890001014600'),
    7: bytes.fromhex('890003010101'),
}
CLS03_CLEAN_BYTE0_TO_X = {0x42: 3, 0x41: 4, 0x40: 14}

N3_V1_SIGNATURES = {
    1: bytes.fromhex('0300000183 00'.replace(' ', '')),
    2: bytes.fromhex('030000 01c200'.replace(' ', '')),
    3: bytes.fromhex('03000001020001'),
    4: bytes.fromhex('030000014100'),
    5: bytes.fromhex('03000001e100'),
    6: bytes.fromhex('06000001000101'),
    7: bytes.fromhex('06000001000101'),
    8: bytes.fromhex('0400000101 6600'.replace(' ', '')),
    9: bytes.fromhex('0400000101 4300'.replace(' ', '')),
    10: bytes.fromhex('06000001010001'),
    11: bytes.fromhex('0400000101820 0'.replace(' ', '')),
    12: bytes.fromhex('050000010101'),
    13: bytes.fromhex('06000001010100'),
    14: bytes.fromhex('030000014000'),
    15: bytes.fromhex('03000001600 0'.replace(' ', '')),
}
N3_R_TABLE = {
    1: {bytes.fromhex('4100090100'): 6, bytes.fromhex('4100090101'): 7},
    2: {bytes.fromhex('0a010100'): 6, bytes.fromhex('0a010101'): 7},
    3: {
        bytes.fromhex('6500090001'): 1, bytes.fromhex('85000800'): 2,
        bytes.fromhex('4500470007'): 4, bytes.fromhex('4500440007'): 6,
        bytes.fromhex('45000a010101'): 7, bytes.fromhex('6200090000'): 8,
        bytes.fromhex('6200090001'): 9, bytes.fromhex('82000800'): 10,
        bytes.fromhex('4200470007'): 12, bytes.fromhex('4200430007'): 13,
        bytes.fromhex('4200440007'): 14,
    },
    4: {bytes.fromhex('8500090100'): 6, bytes.fromhex('8500090101'): 7},
    5: {bytes.fromhex('090100'): 6, bytes.fromhex('090101'): 7},
    6: {
        bytes.fromhex('a5000801'): 7, bytes.fromhex('4500630007'): 9,
        bytes.fromhex('450044000801'): 13,
    },
    7: {bytes.fromhex('430045000800'): 6},
    8: {bytes.fromhex('4500090101'): 11, bytes.fromhex('010147000801'): 7},
    9: {
        bytes.fromhex('6500090101'): 11, bytes.fromhex('6100090100'): 6,
        bytes.fromhex('6100090101'): 7,
    },
    10: {
        bytes.fromhex('a5000801'): 11, bytes.fromhex('4500670007'): 6,
        bytes.fromhex('450047000801'): 7,
    },
    11: {bytes.fromhex('640007'): 6, bytes.fromhex('44000801'): 7},
    12: {bytes.fromhex('650047000800'): 6},
    13: {bytes.fromhex('8100090100'): 6, bytes.fromhex('8100090101'): 7},
    15: {
        bytes.fromhex('0100410009010 0'.replace(' ', '')): 6,
        bytes.fromhex('0100410009010 1'.replace(' ', '')): 7,
    },
}
N3_TRUNCATING_V2_11_R = {
    7: bytes.fromhex('6300'), 12: bytes.fromhex('8500'), 15: bytes.fromhex('4500'),
}


def _bits_to_int(b):
    v = 0
    for bit in b:
        if bit not in (0, 1):
            raise IncompleteDecode("not a clean literal bit array")
        v = (v << 1) | bit
    return v


def find_vector_payload(data):
    idx = 0
    while True:
        idx = data.find(b'\x78\x9c', idx)
        if idx < 0:
            return None
        try:
            out = zlib.decompressobj().decompress(data[idx:idx + 8192])
        except zlib.error:
            idx += 1
            continue
        if len(out) >= 8 and out[7] == 0x30 and out[1:3] == b'\x04\x00':
            return out
        idx += 1


def decode_n2_vector_payload(payload):
    """Returns (intermediate_value, final_value, width), or raises
    IncompleteDecode (covers both "elided" and "unrecognized shape" --
    unlike the research version, this reader has no use for telling those
    apart, since both mean "can't produce a full history")."""
    width = int.from_bytes(payload[3:7], 'little')

    if width == 3:
        if payload[:10] != PREFIX_WIDTH3_START0 or payload[-4:] != TRAILER:
            raise IncompleteDecode("width=3 header/trailer mismatch")
        body = payload[10:-4]
        if body[:3] != b'\x0b\x00\x01' or body[6:8] != b'\x00\x01':
            raise IncompleteDecode("width=3 body shape mismatch")
        x_array, final_array = body[3:6], body[8:11]
        if len(x_array) != 3 or len(final_array) != 3:
            raise IncompleteDecode("width=3 array length mismatch")
        return _bits_to_int(x_array), _bits_to_int(final_array), width

    if width == 8:
        if payload[:10] != PREFIX_WIDTH8_START0 or payload[-4:] != TRAILER:
            raise IncompleteDecode("width=8 header/trailer mismatch")
        body = payload[10:-4]
        if body[:4] != WIDTH8_BODY_PREFIX:
            raise IncompleteDecode("width=8 body prefix mismatch")
        rest = body[4:]
        x, sig_len = None, None
        for candidate_x, sig in WIDTH8_X_SIGNATURES.items():
            if rest[:len(sig)] == sig:
                x, sig_len = candidate_x, len(sig)
                break
        if x is None:
            raise IncompleteDecode("width=8: no known X-signature")
        tail = rest[sig_len:]
        if len(tail) < 3:
            raise IncompleteDecode("width=8 tail too short")
        _prefix_byte, filler, count_byte = tail[0], tail[1], tail[2]
        array = tail[3:]
        if filler != 0x00:
            raise IncompleteDecode("width=8 filler byte mismatch")
        cp = width - len(array)
        if not (0 <= cp <= width) or count_byte != (width + 1) - cp or not array:
            raise IncompleteDecode("width=8 count/array mismatch")
        top_bits = (x >> len(array)) << len(array)
        return x, top_bits | _bits_to_int(array), width

    if payload[:10] != PREFIX_WIDTH4_START0 or payload[-4:] != TRAILER:
        raise IncompleteDecode("width=4 header/trailer mismatch")

    if payload[10] == 0x04 and payload[11:15] == b'\x00\x00\x01\x01':
        body = payload[15:-4]
        if len(body) < 3:
            raise IncompleteDecode("cls=0x04 body too short")
        prefix_byte, filler, count_byte = body[0], body[1], body[2]
        array = body[3:]
        if filler != 0x00 or (prefix_byte & 0x1f) != 0x02:
            raise IncompleteDecode("cls=0x04 shape mismatch")
        cp = (prefix_byte - 0x82) // 0x20
        if not (0 <= cp <= width) or len(array) != width - cp:
            raise IncompleteDecode("cls=0x04 cp/array mismatch")
        if count_byte != (width + 1) - cp:
            raise IncompleteDecode("cls=0x04 count_byte mismatch")
        if width != 4:
            raise IncompleteDecode("cls=0x04 (X=11) only verified at width=4")
        x = 11
        top_bits = (x >> len(array)) << len(array)
        return x, top_bits | _bits_to_int(array), width

    # NOTE: the two bytes immediately before the final-value array (at
    # payload[-6-width:-4-width]) are NOT a fixed \x00\x01 separator, despite
    # looking like one in every file this reader was originally validated
    # against. Cross-checked directly against ground-truth .vcd for every
    # width=4 N=2 corpus file: those two bytes take other values (0x0002,
    # 0x0005, 0xc100, 0x0601, 0xc200, 0xa200, ...) on plenty of real files
    # whose tag/final-value decode is otherwise unambiguous and -- once
    # decoded per the classifier-specific rules below -- byte-for-byte
    # correct. So this reader no longer requires those two bytes to equal
    # any particular value; the final-value array's *position* (the last
    # `width` bytes before the trailer) does not depend on them, and
    # `_bits_to_int` below already rejects anything that isn't a clean
    # literal 0/1 bit array. What those two bytes actually encode remains
    # unknown (see FINDINGS.md) but is evidently irrelevant to recovering
    # the transition values themselves.
    final_array = payload[-4 - width:-4]
    if len(final_array) != width:
        raise IncompleteDecode("final-value array length mismatch")
    final_value = _bits_to_int(final_array)

    tag = payload[10:-6 - width]
    classifier = tag[0] if tag else None

    if classifier == 0x0e:
        if tag[:4] != b'\x0e\x00\x00\x01' or len(tag) != 4 + width:
            raise IncompleteDecode("cls=0x0e tag shape mismatch")
        return _bits_to_int(tag[4:]), final_value, width

    if classifier == 0x03:
        if (len(tag) == 8 and tag[1:4] == b'\x00\x00\x01' and tag[5:7] == b'\x00\x08'):
            byte0, byte3 = tag[4], tag[7]
            if byte0 not in CLS03_CLEAN_BYTE0_TO_X:
                raise IncompleteDecode("cls=0x03 byte0 not in known table")
            x = CLS03_CLEAN_BYTE0_TO_X[byte0]
            if byte3 != (x & 1):
                raise IncompleteDecode("cls=0x03 byte3 cross-check failed")
            return x, final_value, width
        raise IncompleteDecode("cls=0x03 tag shape mismatch (outlier bucket)")

    raise IncompleteDecode("unrecognized N=2 tag classifier")


def _try_resolve_v3_region(v1, rest, width):
    trunc_r = N3_TRUNCATING_V2_11_R.get(v1)
    if trunc_r is not None and rest[:2] == trunc_r:
        tail = rest[2:]
        if len(tail) >= 3:
            prefix_byte, filler, count_byte = tail[0], tail[1], tail[2]
            array = tail[3:]
            cp = width - len(array)
            if (filler == 0x00 and 0 <= cp <= width and
                    count_byte == (width + 1) - cp and
                    prefix_byte == (0x82 | (cp << 5)) & 0xFF and
                    array and all(b in (0, 1) for b in array)):
                v2 = 11
                top_bits = (v2 >> len(array)) << len(array)
                return v2, top_bits | _bits_to_int(array)

    r_table = N3_R_TABLE.get(v1, {})
    for r_bytes, candidate_v2 in r_table.items():
        if rest[:len(r_bytes)] == r_bytes and len(rest) == len(r_bytes) + 6:
            remainder = rest[len(r_bytes):]
            if remainder[:2] != SEPARATOR:
                continue
            final_array = remainder[2:6]
            if len(final_array) != width or any(b not in (0, 1) for b in final_array):
                continue
            return candidate_v2, _bits_to_int(final_array)
    return None


def decode_n3_vector_payload(payload):
    """Returns (v1, v2, final_value, width). Deliberately does NOT include
    the adjacent-duplicate-collapse path (AdjacentDuplicateCollapseError in
    the research version) -- that only ever gives 2 recoverable values with
    genuine, provable ambiguity about the true transition count/positions,
    which can't be a "full history" either."""
    width = int.from_bytes(payload[3:7], 'little')
    if width != 4:
        raise IncompleteDecode("N=3 only implemented for width=4")
    if payload[:10] != PREFIX_WIDTH4_START0 or payload[-4:] != TRAILER:
        raise IncompleteDecode("N=3 header/trailer mismatch")

    body = payload[10:-4]
    sig_matches = [v1 for v1, sig in N3_V1_SIGNATURES.items() if body[:len(sig)] == sig]

    resolved = []
    for candidate_v1 in sig_matches:
        rest = body[len(N3_V1_SIGNATURES[candidate_v1]):]
        result = _try_resolve_v3_region(candidate_v1, rest, width)
        if result is not None:
            resolved.append((candidate_v1, result[0], result[1]))

    if len(resolved) != 1:
        raise IncompleteDecode(
            "N=3: %d candidate(s) resolved (need exactly 1)" % len(resolved))
    v1, v2, final_value = resolved[0]
    return v1, v2, final_value, width


def decode_vector_variable(data):
    """Returns (width, [(time, value), ...]) -- a full, absolute-time
    history including t=0 -- or raises IncompleteDecode. Tries N=1, then
    N=2, then N=3. N=1 is tried first since its payload shape is the most
    specific (fixed length for the generic case, exact byte signature for
    the special case) and is cheap to rule out; it also naturally covers
    the N=1/idx0=0x07/zero-start-value files that used to fall through to
    the N=3 attempt and raise a misleading "N=3 header/trailer mismatch" or
    "N=3: 0 candidates resolved" (those files were genuinely single-
    transition, not unresolved N=3 value combinations). N=4, N>=5,
    multi-variable-shared-payload, and any width outside {3,4,8} are not
    attempted (see this module's docstring)."""
    payload = find_vector_payload(data)
    if payload is None:
        raise IncompleteDecode("no vector payload found")

    try:
        start_value, end_value, width = decode_n1_vector_payload(payload)
        delta = parse_delta_dictionary(data, dict_size=1)[0]
        return width, [(0, start_value), (delta, end_value)]
    except IncompleteDecode:
        pass

    # Every currently-supported N=2/N=3 payload shape (widths 3/4/8) starts
    # with idx0 byte 0x07, which FINDINGS.md ties specifically to a start
    # value of 0 (or 1) -- a non-zero start value gets a different idx0
    # (0x08, 0x09, 0x0c, 0x0d, 0x12, ... depending on the value and N). For
    # N=2/N=3 (2 or 3 real transitions), none of those non-0x07 shapes are
    # understood (only the N=1 case above has known decodes for some of
    # them -- see KNOWN_N1_SIGNATURES/decode_n1_vector_payload), so this
    # reader declines the rest with an honest, accurate reason instead of a
    # misleading "N=3 header/trailer mismatch" (confirmed these really are
    # non-zero-start-value, N>=2 vectors by direct inspection of every file
    # that previously hit that message in the corpus).
    if payload[0] != 0x07:
        raise IncompleteDecode(
            "vector payload start-value tag 0x%02x (N=1 single-transition "
            "decode already tried and failed above; for 2+ transitions "
            "only tag 0x07 -- zero start value -- is supported)" % payload[0])

    try:
        x, final_value, width = decode_n2_vector_payload(payload)
    except IncompleteDecode:
        pass
    else:
        # The two inter-transition deltas (0->x and x->final) are only
        # guaranteed EQUAL when the encoder's shared delta-time dictionary
        # deduplicated them into a single size-1 entry -- which is what
        # every N=2 vector file originally used to validate this decoder
        # happened to do. When the two deltas genuinely differ, the encoder
        # instead stores a size-2 dictionary (two independent byte values,
        # in order); direct cross-check against every real N=2 vector's
        # ground-truth .vcd in the corpus confirms these are the only two
        # cases that occur, are mutually exclusive (exactly one of the two
        # dictionary sizes is ever present), and each -- once present --
        # always matches the true transition times exactly (0 mismatches,
        # 0 unresolved across the whole corpus). See FINDINGS.md.
        try:
            delta = parse_delta_dictionary(data, dict_size=1)[0]
            return width, [(0, 0), (delta, x), (delta + delta, final_value)]
        except IncompleteDecode:
            pass
        delta1, delta2 = parse_delta_dictionary(data, dict_size=2)
        return width, [(0, 0), (delta1, x), (delta1 + delta2, final_value)]

    # decode_n3_vector_payload also transparently covers the "N=4 but the
    # 4th transition collapsed into the 3rd" case from the research code
    # (decode_n4's collapse_v4_eq_v3 path): a payload that collapsed that
    # way is byte-identical to a real N=3 payload, and -- per this whole
    # project's confirmed elision rule -- a redundant same-value re-dump is
    # invisible to the encoder at every level, so there is no 4th event to
    # reconstruct here even if the original waveform re-dumped the same
    # value at a later time. Trying decode_n3 directly, with no special
    # casing, is exactly correct for both the genuine-N=3 and the
    # collapsed-N=4 cases alike.
    v1, v2, v3, width = decode_n3_vector_payload(payload)
    delta = parse_delta_dictionary(data, dict_size=1)[0]
    return width, [(0, 0), (delta, v1), (delta * 2, v2), (delta * 3, v3)]


# =============================================================================
# Top-level: decode one file, or raise IncompleteDecode with a specific
# one-line reason.
# =============================================================================

def decode_file(data):
    """Returns (scope_path: list[str], {name: (width, [(time,value),...])})
    for a file where EVERY variable decoded with full confidence. Raises
    IncompleteDecode otherwise -- the whole file, never a partial result."""
    scope_path, variables, table_end_pos = discover(data)

    scalar_names = [n for n, k in variables if k == 'scalar']
    vector_entries = [(n, k) for n, k in variables if k != 'scalar']

    if len(vector_entries) > 1:
        # More than one non-scalar entry only ever happens for the
        # "shares-table" (2+ vector variables, one payload) case -- see
        # parse_variables. Never fully decodable (see module docstring).
        raise IncompleteDecode(
            "file has %d vector variable(s) sharing one value payload -- "
            "not supported" % len(vector_entries))

    results = {}
    if scalar_names:
        results.update(decode_scalars(data, scalar_names))

    if vector_entries:
        name, kind = vector_entries[0]
        if kind != 'unsupported:0x11':
            raise IncompleteDecode(
                "variable %r has unsupported type tag %r" % (name, kind))
        width, history = decode_vector_variable(data)
        results[name] = (width, history)

        # Safety net: parse_variables() always stops scanning the moment it
        # sees a non-scalar entry (see that function's docstring) -- it
        # cannot tell us whether more variables were declared afterward in
        # the real file. Rather than silently emit a VCD that's missing
        # trailing variables, scan the gap between where parsing stopped
        # for the vector variable and the table's own end marker for any
        # unaccounted-for printable-ASCII name-like content; if there's
        # any, refuse rather than risk an incomplete-but-"successful"
        # result. This code path is only reached when parse_variables
        # found TABLE_END_MARKER landing immediately after the vector
        # variable's own fixed-width record (the "not shares_table" case),
        # i.e. table_end_pos is exactly len(TABLE_END_MARKER) bytes past
        # where parsing stopped -- so gap_start below is that exact point,
        # not an approximation.
        #
        # NOTE: this must be a tightly BOUNDED scan (only the true gap),
        # not an unbounded lookback from table_end_pos -- an earlier version
        # of this check used _check_no_unaccounted_names's fixed 400-byte
        # lookback here, which also swept backward across the *already*
        # parsed scope table (and its legitimate scope name(s)), producing
        # false "multiple top-level scopes" declines on perfectly-decodable
        # single-top-level-scope files whenever a scope name happened to
        # fall within that window.
        gap_start = table_end_pos - len(TABLE_END_MARKER)
        _check_no_unaccounted_names_between(data, gap_start, table_end_pos, [name])
    elif scalar_names:
        # All-scalar files: parse_variables() genuinely consumes the whole
        # table up to its real end marker in this case, so no extra
        # completeness check is needed.
        pass
    else:
        raise IncompleteDecode("no variables found in file")

    return scope_path, results


# =============================================================================
# VCD emission
# =============================================================================

_ID_CHARS = [chr(c) for c in range(0x21, 0x7f)]  # '!'..'~', standard VCD identifier alphabet


def _make_id(n):
    """0-indexed -> a short VCD identifier ('!', '"', ..., then 2-char, ...)."""
    base = len(_ID_CHARS)
    if n < base:
        return _ID_CHARS[n]
    s = ''
    n_plus = n
    while True:
        n_plus, r = divmod(n_plus, base)
        s = _ID_CHARS[r] + s
        if n_plus == 0:
            break
        n_plus -= 1
    return s


def _format_value(width, value, ident):
    if width == 1:
        return '%d%s' % (value, ident)
    bits = ''.join(str((value >> i) & 1) for i in range(width - 1, -1, -1))
    return 'b%s %s' % (bits, ident)


def emit_vcd(scope_path, results, out):
    names = list(results.keys())
    ids = {name: _make_id(i) for i, name in enumerate(names)}

    out.write('$date\n   (unknown -- not decoded from FSDB)\n$end\n')
    out.write('$version\n   gtkwave fsdb_native_to_vcd.py\n$end\n')
    # See module docstring: FSDB's own time unit has never been decoded;
    # this is a fixed, arbitrary label, not a decoded value.
    out.write('$timescale 1ns $end\n')

    indent = ''
    for scope in scope_path:
        out.write('%s$scope module %s $end\n' % (indent, scope))
        indent += ' '
    for name in names:
        width, _history = results[name]
        if width == 1:
            out.write('%s$var wire 1 %s %s $end\n' % (indent, ids[name], name))
        else:
            out.write('%s$var wire %d %s %s [%d:0] $end\n' %
                      (indent, width, ids[name], name, width - 1))
    for _ in scope_path:
        indent = indent[:-1]
        out.write('%s$upscope $end\n' % indent)
    out.write('$enddefinitions $end\n')

    # Merge every variable's own (time, value) history into one global,
    # time-ordered event stream.
    events = {}  # time -> [(name, value), ...]
    for name, (width, history) in results.items():
        for t, v in history:
            events.setdefault(t, []).append((name, v, width))

    out.write('$dumpvars\n')
    for name, v, width in events[0]:
        out.write(_format_value(width, v, ids[name]) + '\n')
    out.write('$end\n')

    for t in sorted(events):
        if t == 0:
            continue
        out.write('#%d\n' % t)
        for name, v, width in events[t]:
            out.write(_format_value(width, v, ids[name]) + '\n')


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: fsdb_native_to_vcd.py <input.fsdb>\n")
        return 1

    path = sys.argv[1]
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        sys.stderr.write("native decode incomplete: could not read %r: %s\n" % (path, e))
        return 1

    try:
        scope_path, results = decode_file(data)
    except IncompleteDecode as e:
        sys.stderr.write("native decode incomplete: %s\n" % e)
        return 1
    except Exception as e:  # noqa: BLE001 -- any other exception is also a refusal, not a crash
        sys.stderr.write("native decode incomplete: unexpected error: %r\n" % (e,))
        return 1

    if scope_path is None:
        sys.stderr.write("native decode incomplete: scope path unresolved\n")
        return 1

    emit_vcd(scope_path, results, sys.stdout)
    return 0


if __name__ == '__main__':
    sys.exit(main())
