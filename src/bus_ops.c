#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "bus_ops.h"
#include "globals.h"
#include "menu.h"
#include "signalwindow.h"
#include "status.h"

// Builds a new vector trace containing just bit-index range [lo_idx,
// hi_idx] (inclusive, 0-based, in `t`'s own currently-DISPLAYED bit
// order -- i.e. index 0 is the first column gtkwave draws for this bus,
// already accounting for TR_REVERSE) and adds it to the display. Mirrors
// menu.c's combine_traces()'s existing t->vector branch (same per-bit
// shift/invert attribute composition, same bits2vector()/AddVector()
// backend), just restricted to a slice instead of the whole bus.
//
// Handles both bus representations this codebase has (see the identical
// split in src/list_x_view.c's scan_vector_trace()/scan_node_trace()):
// a real constructed/auto-coalesced vector (GwBitVector, t->vector true)
// is sliced directly; a natively-declared multi-bit VCD var (a GwNode
// with extvals set, t->vector false, the common case) has no per-bit
// GwNode array of its own to slice, so it's first normalized into one via
// combine_traces(1, t) -- the same gw_node_expand()-based machinery
// combine_traces() already uses elsewhere for "make this one trace a
// vector" (src/analyzer.c:266, src/ttranslate.c:204) -- and that
// temporary vector's bits are sliced instead.
//
// Known limitation: gw_node_expand() (which combine_traces() calls
// internally for the natively-declared case) allocates one throwaway
// GwNode per bit and hands ownership of the whole array to its caller;
// nothing in this codebase has ever needed to keep only *part* of that
// array before, so the bits excluded from the slice (outside
// [lo_idx,hi_idx]) are intentionally leaked here rather than risk an
// incorrect free of shared/still-referenced node internals. Bounded to
// one bus-width's worth of small allocations per sub-bus created from a
// natively-declared signal -- not a growing or repeated leak.
static void expand_subbus(GwTrace *t, int lo_idx, int hi_idx)
{
    GwBitVector *full_vec;
    gboolean is_temp_vec;

    if (t->vector) {
        full_vec = t->n.vec;
        is_temp_vec = FALSE;
    } else {
        full_vec = combine_traces(1, t);
        is_temp_vec = TRUE;
        if (full_vec == NULL) {
            status_text(
                "GTKWAVE | Could not expand this signal into individual bits to slice.\n");
            return;
        }
    }

    GwBits *bits = full_vec->bits;
    if (bits == NULL) {
        status_text("GTKWAVE | This bus has no bit-level information to slice.\n");
        return;
    }

    int nnbits = bits->nnbits;
    if (lo_idx < 0) {
        lo_idx = 0;
    }
    if (hi_idx >= nnbits) {
        hi_idx = nnbits - 1;
    }
    if (lo_idx > hi_idx) {
        int tmp = lo_idx;
        lo_idx = hi_idx;
        hi_idx = tmp;
    }
    int count = hi_idx - lo_idx + 1;
    if (count <= 0 || count > BITATTRIBUTES_MAX) {
        status_text("GTKWAVE | Invalid bit range for sub-bus.\n");
        return;
    }

    // A freshly-normalized temp vector already has `t`'s own shift/invert
    // baked into its per-bit attribs by combine_traces() itself (see its
    // non-vector branch); applying them again here would double them up.
    // Its bit order is also already the plain gw_node_expand() order --
    // TR_REVERSE only has an established meaning for a real, existing
    // vector trace's own bits->nodes[] (combine_traces()'s t->vector
    // branch), not for a single expanded node.
    gboolean reversed = !is_temp_vec && (t->flags & TR_REVERSE) != 0;
    GwTime base_shift = is_temp_vec ? 0 : t->shift;
    guint64 base_flags = is_temp_vec ? 0 : t->flags;
    GwBitAttributes *oldba = bits->attribs;

    GwBits *b = calloc_2(1, sizeof(GwBits) + (gsize)count * sizeof(GwNode *));
    b->attribs = malloc_2((gsize)count * sizeof(GwBitAttributes));
    gboolean attrib_reqd = FALSE;

    for (int i = 0; i < count; i++) {
        int display_idx = lo_idx + i;
        int storage_idx = reversed ? (nnbits - 1 - display_idx) : display_idx;

        GwNode *node = bits->nodes[storage_idx];
        if (node->expansion) {
            node->expansion->refcnt++;
        }
        b->nodes[i] = node;

        b->attribs[i].shift = base_shift + (oldba != NULL ? oldba[storage_idx].shift : 0);
        b->attribs[i].flags =
            base_flags ^ (oldba != NULL ? (oldba[storage_idx].flags & TR_INVERT) : 0);

        if (b->attribs[i].shift || (b->attribs[i].flags & TR_INVERT)) {
            attrib_reqd = TRUE;
        }
    }

    if (!attrib_reqd) {
        free_2(b->attribs);
        b->attribs = NULL;
    }

    b->nnbits = count;

    const gchar *base_name = t->name_full != NULL ? t->name_full : t->name;
    gchar *slice_name =
        g_strdup_printf("%s[%d:%d]", base_name != NULL ? base_name : "bus", hi_idx, lo_idx);
    b->name = malloc_2(strlen(slice_name) + 1);
    strcpy(b->name, slice_name);
    g_free(slice_name);

    GwBitVector *v = bits2vector(b);
    if (v == NULL) {
        free_2(b->name);
        if (b->attribs != NULL) {
            free_2(b->attribs);
        }
        free_2(b);
        status_text("GTKWAVE | Failed to build sub-bus.\n");
        return;
    }
    v->bits = b; /* matches combine_traces()'s ownership hand-off */

    AddVector(v, NULL);
    free_2(v->bits->name);
    v->bits->name = NULL;

    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
}

typedef struct
{
    GwTrace *trace;
    GtkWidget *hi_spin;
    GtkWidget *lo_spin;
} SubBusDialogState;

static void on_hi_spin_changed(GtkSpinButton *widget, gpointer user_data)
{
    SubBusDialogState *state = user_data;
    gint hi = gtk_spin_button_get_value_as_int(widget);
    gint lo = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->lo_spin));
    if (lo > hi) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->lo_spin), hi);
    }
}

static void on_lo_spin_changed(GtkSpinButton *widget, gpointer user_data)
{
    SubBusDialogState *state = user_data;
    gint lo = gtk_spin_button_get_value_as_int(widget);
    gint hi = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->hi_spin));
    if (hi < lo) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->hi_spin), lo);
    }
}

// Cheap upper bound on the bit width without actually expanding anything
// (that only happens once the user confirms a range, in expand_subbus())
// -- covers both bus representations, see expand_subbus()'s intro comment.
static int trace_bit_width(GwTrace *t)
{
    if (t->vector) {
        return (t->n.vec != NULL && t->n.vec->bits != NULL) ? t->n.vec->bits->nnbits : 0;
    }
    if (t->n.nd != NULL && t->n.nd->extvals) {
        int msb = t->n.nd->msi;
        int lsb = t->n.nd->lsi;
        return ABS(msb - lsb) + 1;
    }
    return 0;
}

void bus_ops_show_expand_subbus_dialog(GwTrace *t)
{
    if (t == NULL) {
        status_text("GTKWAVE | Select a bus/vector signal first.\n");
        return;
    }

    int nnbits = trace_bit_width(t);
    if (nnbits == 0) {
        status_text("GTKWAVE | Expand as Sub-Bus needs a bus (multi-bit) signal.\n");
        return;
    }
    if (nnbits <= 1) {
        status_text("GTKWAVE | This bus only has one bit, nothing to slice.\n");
        return;
    }

    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    gchar *title = g_strdup_printf("Expand as Sub-Bus: %s", t->name != NULL ? t->name : "");
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, parent, GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK,
        NULL);
    g_free(title);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    gchar *intro_text = g_strdup_printf(
        "Pick a bit range within this %d-bit bus's own currently-displayed bit order "
        "(0 = first/leftmost) to build as a new, narrower bus.",
        nnbits);
    GtkWidget *intro = gtk_label_new(intro_text);
    g_free(intro_text);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_widget_set_halign(intro, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *range_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("High index:"), FALSE, FALSE, 0);
    GtkWidget *hi_spin = gtk_spin_button_new_with_range(0, nnbits - 1, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(hi_spin), nnbits - 1);
    gtk_box_pack_start(GTK_BOX(range_row), hi_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("Low index:"), FALSE, FALSE, 0);
    GtkWidget *lo_spin = gtk_spin_button_new_with_range(0, nnbits - 1, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(lo_spin), 0);
    gtk_box_pack_start(GTK_BOX(range_row), lo_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), range_row, FALSE, FALSE, 0);

    SubBusDialogState state = {.trace = t, .hi_spin = hi_spin, .lo_spin = lo_spin};
    g_signal_connect(hi_spin, "value-changed", G_CALLBACK(on_hi_spin_changed), &state);
    g_signal_connect(lo_spin, "value-changed", G_CALLBACK(on_lo_spin_changed), &state);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        gint hi = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hi_spin));
        gint lo = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(lo_spin));
        expand_subbus(t, lo, hi);
    }

    gtk_widget_destroy(dialog);
}

void bus_ops_reverse_bit_order(GwTrace *t)
{
    if (t == NULL) {
        status_text("GTKWAVE | Select a bus/vector signal first.\n");
        return;
    }
    if (!t->vector) {
        status_text("GTKWAVE | Reverse Bit Order needs a bus (multi-bit) signal.\n");
        return;
    }

    t->flags ^= TR_REVERSE;
    redraw_signals_and_waves();
}
