#include <config.h>
#include <gtkwave.h>
#include "time_compress.h"
#include "globals.h"
#include "signalwindow.h"
#include "status.h"

#define COMPRESS_MARKER_WIDTH_PX 14.0

typedef struct
{
    GwTime start;
    GwTime end; /* exclusive */
} GwCompressedSpan;

static void ensure_array(void)
{
    if (GLOBALS->compressed_spans == NULL) {
        GLOBALS->compressed_spans = g_ptr_array_new_with_free_func(g_free);
    }
}

static gint compare_spans(gconstpointer a, gconstpointer b)
{
    const GwCompressedSpan *sa = *(const GwCompressedSpan **)a;
    const GwCompressedSpan *sb = *(const GwCompressedSpan **)b;
    if (sa->start < sb->start) {
        return -1;
    }
    if (sa->start > sb->start) {
        return 1;
    }
    return 0;
}

void time_compress_add_span(GwTime start, GwTime end)
{
    if (start >= end) {
        return;
    }

    ensure_array();

    /* Merge with any existing span that overlaps or touches [start, end). */
    for (guint i = 0; i < GLOBALS->compressed_spans->len;) {
        GwCompressedSpan *existing = g_ptr_array_index(GLOBALS->compressed_spans, i);
        if (existing->end < start || existing->start > end) {
            i++;
            continue;
        }
        start = MIN(start, existing->start);
        end = MAX(end, existing->end);
        g_ptr_array_remove_index(GLOBALS->compressed_spans, i);
        /* restart the scan since indices shifted and the wider [start,end)
         * may now overlap a span we already passed */
        i = 0;
    }

    GwCompressedSpan *span = g_new0(GwCompressedSpan, 1);
    span->start = start;
    span->end = end;
    g_ptr_array_add(GLOBALS->compressed_spans, span);
    g_ptr_array_sort(GLOBALS->compressed_spans, compare_spans);
}

void time_compress_remove_span_at(GwTime t)
{
    if (GLOBALS->compressed_spans == NULL) {
        return;
    }
    for (guint i = 0; i < GLOBALS->compressed_spans->len; i++) {
        GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, i);
        if (t >= span->start && t < span->end) {
            g_ptr_array_remove_index(GLOBALS->compressed_spans, i);
            return;
        }
    }
}

void time_compress_clear(void)
{
    if (GLOBALS->compressed_spans != NULL) {
        g_ptr_array_set_size(GLOBALS->compressed_spans, 0);
    }
}

gboolean time_compress_has_spans(void)
{
    return GLOBALS->compressed_spans != NULL && GLOBALS->compressed_spans->len > 0;
}

gdouble time_compress_marker_width_px(void)
{
    return COMPRESS_MARKER_WIDTH_PX;
}

guint time_compress_span_count(void)
{
    return GLOBALS->compressed_spans != NULL ? GLOBALS->compressed_spans->len : 0;
}

void time_compress_span_get(guint index, GwTime *out_start, GwTime *out_end)
{
    GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, index);
    if (out_start != NULL) {
        *out_start = span->start;
    }
    if (out_end != NULL) {
        *out_end = span->end;
    }
}

void time_compress_action_compress_range(void)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    GwMarker *baseline = gw_project_get_baseline_marker(GLOBALS->project);

    if (!gw_marker_is_enabled(primary) || !gw_marker_is_enabled(baseline)) {
        status_text(
            "GTKWAVE | Compress Time Range needs both the primary and baseline markers set "
            "-- drop them at the start/end of the span to collapse.\n");
        return;
    }

    GwTime a = gw_marker_get_position(primary);
    GwTime b = gw_marker_get_position(baseline);
    GwTime start = MIN(a, b);
    GwTime end = MAX(a, b);

    if (start == end) {
        status_text("GTKWAVE | Primary and baseline markers are at the same time -- nothing "
                   "to compress.\n");
        return;
    }

    time_compress_add_span(start, end);
    redraw_signals_and_waves();
}

void time_compress_action_uncompress_all(void)
{
    time_compress_clear();
    redraw_signals_and_waves();
}

gboolean time_compress_span_at_pixel(gdouble pixel_from_viewport_start, GwTime *out_start,
                                    GwTime *out_end)
{
    if (!time_compress_has_spans()) {
        return FALSE;
    }

    GwTime t = gw_pixel_to_time(pixel_from_viewport_start);

    for (guint i = 0; i < GLOBALS->compressed_spans->len; i++) {
        GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, i);
        if (t >= span->start && t < span->end) {
            if (out_start != NULL) {
                *out_start = span->start;
            }
            if (out_end != NULL) {
                *out_end = span->end;
            }
            return TRUE;
        }
    }
    return FALSE;
}

gdouble gw_time_to_pixel(GwTime t)
{
    GwTime base = GLOBALS->tims.start;
    gdouble pxns = GLOBALS->pxns;

    if (t <= base || !time_compress_has_spans()) {
        return (gdouble)(t - base) * pxns;
    }

    gdouble accumulated_px = 0.0;
    GwTime cursor = base;

    for (guint i = 0; i < GLOBALS->compressed_spans->len; i++) {
        GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, i);
        if (span->end <= cursor) {
            continue;
        }
        if (span->start >= t) {
            break;
        }

        GwTime seg_start = MAX(span->start, cursor);
        if (seg_start > cursor) {
            accumulated_px += (gdouble)(seg_start - cursor) * pxns;
            cursor = seg_start;
        }

        GwTime seg_end = MIN(span->end, t);
        if (seg_end > cursor) {
            GwTime span_dur = span->end - span->start;
            gdouble frac =
                span_dur > 0 ? (gdouble)(seg_end - cursor) / (gdouble)span_dur : 0.0;
            accumulated_px += COMPRESS_MARKER_WIDTH_PX * frac;
            cursor = seg_end;
        }
    }

    if (t > cursor) {
        accumulated_px += (gdouble)(t - cursor) * pxns;
    }

    return accumulated_px;
}

GwTime gw_pixel_to_time(gdouble x)
{
    GwTime base = GLOBALS->tims.start;
    gdouble nspx = GLOBALS->nspx;
    gdouble pxns = GLOBALS->pxns;

    if (x < 0) {
        x = 0;
    }
    if (!time_compress_has_spans()) {
        return base + (GwTime)(x * nspx);
    }

    gdouble remaining_px = x;
    GwTime cursor = base;

    for (guint i = 0; i < GLOBALS->compressed_spans->len; i++) {
        GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, i);
        if (span->end <= cursor) {
            continue;
        }

        GwTime gap = (span->start > cursor) ? (span->start - cursor) : 0;
        gdouble gap_px = (gdouble)gap * pxns;

        if (remaining_px <= gap_px) {
            return cursor + (GwTime)(remaining_px * nspx);
        }
        remaining_px -= gap_px;
        cursor = span->start;

        GwTime span_dur = span->end - span->start;
        if (remaining_px <= COMPRESS_MARKER_WIDTH_PX) {
            gdouble frac = remaining_px / COMPRESS_MARKER_WIDTH_PX;
            return cursor + (GwTime)(frac * (gdouble)span_dur);
        }
        remaining_px -= COMPRESS_MARKER_WIDTH_PX;
        cursor = span->end;
    }

    return cursor + (GwTime)(remaining_px * nspx);
}
