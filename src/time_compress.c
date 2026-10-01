#include <config.h>
#include <gtkwave.h>
#include "time_compress.h"
#include "globals.h"
#include "currenttime.h"
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

        // seg_start is span->start in the common case (base is entirely
        // before this span), but can be `cursor` itself if `base` falls
        // inside the span -- in which case the span's OWN marker pixel
        // width must be shrunk proportionally to just the portion still
        // ahead of cursor (remaining_dur/span_dur), or a viewport whose
        // left edge sits mid-span would double-count the part of the
        // span that's already behind it. A real, reachable case once
        // compression-aware code (e.g. pinch-zoom) can compute a new
        // tims.start that lands inside a span, not just adjacent to one.
        GwTime seg_start = MAX(span->start, cursor);
        if (seg_start > cursor) {
            accumulated_px += (gdouble)(seg_start - cursor) * pxns;
            cursor = seg_start;
        }

        GwTime seg_end = MIN(span->end, t);
        if (seg_end > cursor) {
            GwTime span_dur = span->end - span->start;
            GwTime remaining_dur = span->end - seg_start;
            gdouble marker_px = span_dur > 0
                                    ? COMPRESS_MARKER_WIDTH_PX * (gdouble)remaining_dur /
                                          (gdouble)span_dur
                                    : 0.0;
            gdouble frac =
                remaining_dur > 0 ? (gdouble)(seg_end - cursor) / (gdouble)remaining_dur : 0.0;
            accumulated_px += marker_px * frac;
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

        // See gw_time_to_pixel()'s matching comment: effective_start is
        // span->start normally, but `cursor` itself if `base` already
        // falls inside this span, in which case only the remaining
        // portion of the span still gets a (proportionally smaller)
        // marker pixel width.
        GwTime effective_start = MAX(span->start, cursor);
        GwTime gap = (span->start > cursor) ? (span->start - cursor) : 0;
        gdouble gap_px = (gdouble)gap * pxns;

        if (remaining_px <= gap_px) {
            return cursor + (GwTime)(remaining_px * nspx);
        }
        remaining_px -= gap_px;
        cursor = effective_start;

        GwTime span_dur = span->end - span->start;
        GwTime remaining_dur = span->end - effective_start;
        gdouble marker_px = span_dur > 0
                                ? COMPRESS_MARKER_WIDTH_PX * (gdouble)remaining_dur /
                                      (gdouble)span_dur
                                : 0.0;

        if (remaining_px <= marker_px) {
            gdouble frac = marker_px > 0 ? remaining_px / marker_px : 0.0;
            return cursor + (GwTime)(frac * (gdouble)remaining_dur);
        }
        remaining_px -= marker_px;
        cursor = span->end;
    }

    return cursor + (GwTime)(remaining_px * nspx);
}

GwTime gw_solve_start_for_pixel_anchor(gdouble anchor_pixel, GwTime anchor_time)
{
    gdouble pxns = GLOBALS->pxns;
    gdouble nspx = GLOBALS->nspx;

    if (anchor_pixel < 0) {
        anchor_pixel = 0;
    }
    if (!time_compress_has_spans()) {
        return anchor_time - (GwTime)(anchor_pixel * nspx);
    }

    // Walks backward from `anchor_time` by `anchor_pixel` pixels,
    // span-by-span in reverse chronological order -- the correct inverse
    // of gw_pixel_to_time()/gw_time_to_pixel() for solving the unknown
    // base itself. An earlier version of this function instead evaluated
    // gw_pixel_to_time() with tims.start forced to 0 and subtracted the
    // result -- that's invalid here: which gap/marker/normal region a
    // given pixel falls into depends on base itself (a span's gap width
    // is `span->start - base`), so forcing base to an arbitrary value
    // can put the same pixel in a completely different region than the
    // real base would, giving a wrong answer with no warning.
    gdouble remaining_px = anchor_pixel;
    GwTime cursor = anchor_time;

    for (gint i = (gint)GLOBALS->compressed_spans->len - 1; i >= 0; i--) {
        GwCompressedSpan *span = g_ptr_array_index(GLOBALS->compressed_spans, i);

        GwTime seg_end = MIN(span->end, cursor);
        if (seg_end <= span->start) {
            continue; /* span is entirely at-or-after cursor -- not reached yet walking backward */
        }

        GwTime gap_after_span = cursor - seg_end;
        gdouble gap_px = (gdouble)gap_after_span * pxns;

        if (remaining_px <= gap_px) {
            return cursor - (GwTime)(remaining_px * nspx);
        }
        remaining_px -= gap_px;
        cursor = seg_end;

        GwTime span_dur = span->end - span->start;
        GwTime seg_dur = seg_end - span->start;
        gdouble marker_px =
            span_dur > 0 ? COMPRESS_MARKER_WIDTH_PX * (gdouble)seg_dur / (gdouble)span_dur : 0.0;

        if (remaining_px <= marker_px) {
            gdouble frac = marker_px > 0 ? remaining_px / marker_px : 0.0;
            return cursor - (GwTime)(frac * (gdouble)seg_dur);
        }
        remaining_px -= marker_px;
        cursor = span->start;
    }

    return cursor - (GwTime)(remaining_px * nspx);
}

GwTime gw_time_page_increment(void)
{
    // gw_pixel_to_time(0) is always exactly GLOBALS->tims.start (pixel 0
    // is the viewport's left edge by definition), so this is exactly
    // "how much time do GLOBALS->wavewidth pixels cover, compression-
    // aware" without a redundant second call for the trivial subtrahend.
    return gw_pixel_to_time((gdouble)GLOBALS->wavewidth) - GLOBALS->tims.start;
}

// "Manage Compressed Spans..." list window -- every other "list of
// things" feature this session built (List X, Time-Span Comments, Group
// Manager) got one; this feature didn't, leaving the tiny on-canvas "+"
// marker as the only way to see or remove an active span.

// Moves the primary marker to `t` and scrolls the viewport so it's
// visible -- same simple one-shot idiom src/list_x_view.c's own
// goto_time() uses (see its comment for why the fuller centered-recenter
// math elsewhere isn't needed here either).
static void goto_time(GwTime t)
{
    if (t < GLOBALS->tims.first) {
        t = GLOBALS->tims.first;
    } else if (t > GLOBALS->tims.last) {
        t = GLOBALS->tims.last;
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    gw_marker_set_position(primary_marker, t);
    gw_marker_set_enabled(primary_marker, TRUE);

    GtkAdjustment *hadj = GTK_ADJUSTMENT(GLOBALS->wave_hslider);
    gtk_adjustment_set_value(hadj, (gdouble)t);

    GwTime pageinc = gw_time_page_increment();
    if (t < (GLOBALS->tims.last - pageinc + 1)) {
        GLOBALS->tims.timecache = t;
    } else {
        GLOBALS->tims.timecache = GLOBALS->tims.last - pageinc + 1;
        if (GLOBALS->tims.timecache < GLOBALS->tims.first) {
            GLOBALS->tims.timecache = GLOBALS->tims.first;
        }
    }

    time_update();
    redraw_signals_and_waves();
}

static gchar *format_span(GwTime start, GwTime end)
{
    GwTimeDimension time_dimension = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    GwTime global_time_offset = gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    char start_buf[40];
    char end_buf[40];

    reformat_time(start_buf, start + global_time_offset, time_dimension);
    reformat_time(end_buf, end + global_time_offset, time_dimension);

    return g_strdup_printf("[%s, %s)", start_buf, end_buf);
}

typedef struct
{
    GwTime start;
    GtkWidget *window; /* not owned -- destroyed by on_uncompress_clicked(), not freed here */
} CompressedSpanRowData;

static void compressed_span_row_data_free(gpointer data)
{
    g_free(data);
}

static void on_goto_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    CompressedSpanRowData *data = user_data;
    goto_time(data->start);
}

static void on_uncompress_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    CompressedSpanRowData *data = user_data;
    time_compress_remove_span_at(data->start);
    redraw_signals_and_waves();
    gtk_widget_destroy(data->window);
    time_compress_show_spans_list(); /* cheap full refresh -- small lists, no state to preserve */
}

void time_compress_show_spans_list(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Manage Compressed Spans");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 320);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    guint count = time_compress_span_count();

    if (count == 0) {
        GtkWidget *empty_label = gtk_label_new("No compressed spans.");
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty_label, -1);
    } else {
        for (guint i = 0; i < count; i++) {
            GwTime start, end;
            time_compress_span_get(i, &start, &end);

            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);

            gchar *span_text = format_span(start, end);
            GtkWidget *label = gtk_label_new(span_text);
            g_free(span_text);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

            CompressedSpanRowData *goto_data = g_new0(CompressedSpanRowData, 1);
            goto_data->start = start;
            GtkWidget *goto_button = gtk_button_new_with_label("Go To");
            g_signal_connect_data(goto_button, "clicked", G_CALLBACK(on_goto_clicked), goto_data,
                                  (GClosureNotify)(GDestroyNotify)compressed_span_row_data_free,
                                  (GConnectFlags)0);
            gtk_box_pack_start(GTK_BOX(row_box), goto_button, FALSE, FALSE, 0);

            CompressedSpanRowData *uncompress_data = g_new0(CompressedSpanRowData, 1);
            uncompress_data->start = start;
            uncompress_data->window = window;
            GtkWidget *uncompress_button = gtk_button_new_with_label("Uncompress");
            g_signal_connect_data(uncompress_button, "clicked",
                                  G_CALLBACK(on_uncompress_clicked), uncompress_data,
                                  (GClosureNotify)(GDestroyNotify)compressed_span_row_data_free,
                                  (GConnectFlags)0);
            gtk_box_pack_start(GTK_BOX(row_box), uncompress_button, FALSE, FALSE, 0);

            gtk_list_box_insert(GTK_LIST_BOX(listbox), row_box, -1);
        }
    }

    gtk_widget_show_all(window);
}
