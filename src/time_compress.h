#pragma once

#include <config.h>
#include <gtkwave.h>

// "Compress Time Range": one or more real time spans get visually
// collapsed to a small fixed-width "+" marker on the waveform canvas, so
// the rest of the timeline renders denser without touching the loaded
// data itself. This is the canonical time<->pixel mapping every
// rendering/interaction call site must go through once any span is
// compressed -- see the comment on gw_time_to_pixel() below for why a
// single shared pair of functions is required rather than each call site
// doing its own `(t - tims.start) * pxns` arithmetic (the pre-existing
// convention, still fine as a *reference* for the uncompressed case,
// which these functions fall back to exactly when no spans are active).

// Marks [start, end) (start < end) as compressed, merging with any
// existing overlapping/adjacent span. No-ops if start >= end.
void time_compress_add_span(GwTime start, GwTime end);

// Removes whichever compressed span (if any) contains time `t`. Used for
// a one-click "uncompress this one" action.
void time_compress_remove_span_at(GwTime t);

// Clears all compressed spans ("Uncompress All").
void time_compress_clear(void);

gboolean time_compress_has_spans(void);

// Iteration accessors for drawing the "+" overlay (GwCompressedSpan itself
// is private to time_compress.c).
guint time_compress_span_count(void);
void time_compress_span_get(guint index, GwTime *out_start, GwTime *out_end);

// Menu actions (Edit > Compress Time Range / Edit > Uncompress All).
// Compresses the span between the primary and baseline markers -- both
// must be enabled, since silently compressing the whole loaded time
// range by default (as most other primary<->baseline-ranged features in
// this app fall back to when the baseline isn't set) would be actively
// counterproductive for this specific feature.
void time_compress_action_compress_range(void);
void time_compress_action_uncompress_all(void);

// Fixed on-screen pixel width a compressed span occupies, regardless of
// its real duration -- wide enough to hold a small "+" glyph.
gdouble time_compress_marker_width_px(void);

// If `t` falls inside a currently-compressed span, returns that span's
// [start, end) via *out_start/*out_end and returns TRUE. Used by the
// overlay-drawing code and by the "click the + to uncompress" handler.
gboolean time_compress_span_at_pixel(gdouble pixel_from_viewport_start, GwTime *out_start,
                                    GwTime *out_end);

// Converts an absolute GwTime to a pixel offset from the current
// viewport's left edge (GLOBALS->tims.start) -- the same value
// `(t - GLOBALS->tims.start) * GLOBALS->pxns` computed everywhere in
// src/gw-wave-view*.c and src/wavewindow.c before this function existed,
// except any currently-compressed span between tims.start and `t` counts
// for only its fixed marker width instead of its real pixel-duration.
// Falls back to the exact plain formula when there are no compressed
// spans, so this is a safe drop-in replacement at every such call site.
gdouble gw_time_to_pixel(GwTime t);

// Inverse of gw_time_to_pixel(): given a pixel offset from the viewport's
// left edge, returns the absolute GwTime it corresponds to. Drop-in
// replacement for `GLOBALS->tims.start + (GwTime)(x * GLOBALS->nspx)`.
// A pixel offset that lands inside a compressed span's fixed-width marker
// maps to a proportional point within that span's real time range.
GwTime gw_pixel_to_time(gdouble pixel_from_viewport_start);

// Solves for the GLOBALS->tims.start value that would make `anchor_pixel`
// map (via gw_pixel_to_time(), at the CURRENT zoom/nspx and compressed-span
// list) to `anchor_time`. The compression-aware analog of the plain
// `start = anchor_time - anchor_pixel * nspx` formula anchor-preserving
// zoom code (e.g. pinch-zoom) needs. Does not read or modify
// GLOBALS->tims.start itself -- it walks backward from `anchor_time` by
// `anchor_pixel` pixels through the compressed-span list directly, since
// which gap/marker/normal pixel-region a point falls into depends on the
// (unknown, being-solved-for) base itself and so can't be recovered by
// evaluating gw_pixel_to_time() at some other placeholder base.
GwTime gw_solve_start_for_pixel_anchor(gdouble anchor_pixel, GwTime anchor_time);

// The real time-duration that GLOBALS->wavewidth pixels represents,
// starting from the current viewport's left edge (GLOBALS->tims.start) --
// drop-in replacement for the `(GwTime)(((gdouble)GLOBALS->wavewidth) *
// GLOBALS->nspx)` "page increment" idiom duplicated across menu.c,
// shiftbuttons.c, zoombuttons.c, pagebuttons.c, edgebuttons.c, strace.c,
// logfile.c, wcp_gtkwave.c, list_x_view.c, time_span_comments.c,
// gw-wave-view.c, main.c and wavewindow.c. That plain formula
// underestimates a page's real duration whenever a compressed span falls
// within it (the compressed portion covers far more real time than its
// fixed marker width implies) -- this instead asks how much time
// gw_pixel_to_time() actually walks through over that pixel span,
// falling back to the exact plain formula when nothing is compressed.
GwTime gw_time_page_increment(void);

// Opens a list window of every currently-compressed span, each with a
// "Go To" (jump marker + viewport to the span's start) and "Uncompress"
// button -- the only way to see/remove a span today is clicking its tiny
// on-canvas "+" marker, unlike every other "list of things" feature this
// session built.
void time_compress_show_spans_list(void);
