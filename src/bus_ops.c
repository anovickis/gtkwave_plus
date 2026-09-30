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

// One constituent bit of a bus composition being edited: the underlying
// node plus its per-bit shift/invert attributes, same pairing GwBits
// itself uses (see gw-bits.h) -- a plain owned copy so the dialog can
// freely reorder/add/remove entries without touching any real GwBits
// array until the user confirms.
typedef struct
{
    GwNode *node;
    GwBitAttributes attrib;
} BusBitEntry;

// Reads out `t`'s current constituent bits as a fresh, editable list, in
// the same DISPLAY order and with the same per-bit attribute composition
// expand_subbus() above uses -- see its comment for why both bus
// representations need separate handling. Returns NULL (with a status
// message already shown) if `t` has no bit-level information to edit.
static GPtrArray *build_entries_from_trace(GwTrace *t)
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
                "GTKWAVE | Could not expand this signal into individual bits to edit.\n");
            return NULL;
        }
    }

    GwBits *bits = full_vec->bits;
    if (bits == NULL) {
        status_text("GTKWAVE | This bus has no bit-level information to edit.\n");
        return NULL;
    }

    int nnbits = bits->nnbits;
    gboolean reversed = !is_temp_vec && (t->flags & TR_REVERSE) != 0;
    GwTime base_shift = is_temp_vec ? 0 : t->shift;
    guint64 base_flags = is_temp_vec ? 0 : t->flags;
    GwBitAttributes *oldba = bits->attribs;

    GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
    for (int display_idx = 0; display_idx < nnbits; display_idx++) {
        int storage_idx = reversed ? (nnbits - 1 - display_idx) : display_idx;

        BusBitEntry *e = g_new0(BusBitEntry, 1);
        e->node = bits->nodes[storage_idx];
        e->attrib.shift = base_shift + (oldba != NULL ? oldba[storage_idx].shift : 0);
        e->attrib.flags =
            base_flags ^ (oldba != NULL ? (oldba[storage_idx].flags & TR_INVERT) : 0);
        g_ptr_array_add(entries, e);
    }

    return entries;
}

// Builds a brand-new bus trace from the edited composition and adds it
// to the display -- deliberately non-destructive (mirrors
// Expand-as-Sub-Bus): the source trace `t` is never modified or removed,
// since safely mutating an existing GwBitVector's node ownership in
// place has no established precedent in this codebase (see
// expand_subbus()'s own comment on the same tradeoff).
static void apply_composition(GwTrace *t, GPtrArray *entries)
{
    int count = (int)entries->len;
    if (count <= 0) {
        status_text("GTKWAVE | A bus needs at least one bit.\n");
        return;
    }
    if (count > BITATTRIBUTES_MAX) {
        status_text("GTKWAVE | Too many bits for a single bus.\n");
        return;
    }

    GwBits *b = calloc_2(1, sizeof(GwBits) + (gsize)count * sizeof(GwNode *));
    b->attribs = malloc_2((gsize)count * sizeof(GwBitAttributes));
    gboolean attrib_reqd = FALSE;

    for (int i = 0; i < count; i++) {
        BusBitEntry *e = g_ptr_array_index(entries, i);
        if (e->node->expansion) {
            e->node->expansion->refcnt++;
        }
        b->nodes[i] = e->node;
        b->attribs[i] = e->attrib;
        if (e->attrib.shift || (e->attrib.flags & TR_INVERT)) {
            attrib_reqd = TRUE;
        }
    }

    if (!attrib_reqd) {
        free_2(b->attribs);
        b->attribs = NULL;
    }
    b->nnbits = count;

    const gchar *base_name = t->name_full != NULL ? t->name_full : t->name;
    gchar *new_name = g_strdup_printf("%s_edited", base_name != NULL ? base_name : "bus");
    b->name = malloc_2(strlen(new_name) + 1);
    strcpy(b->name, new_name);
    g_free(new_name);

    GwBitVector *v = bits2vector(b);
    if (v == NULL) {
        free_2(b->name);
        if (b->attribs != NULL) {
            free_2(b->attribs);
        }
        free_2(b);
        status_text("GTKWAVE | Failed to build the edited bus.\n");
        return;
    }
    v->bits = b;

    AddVector(v, NULL);
    free_2(v->bits->name);
    v->bits->name = NULL;

    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
}

typedef struct
{
    GPtrArray *entries; /* owned BusBitEntry*, not yet applied */
    GtkWidget *listbox;
    GtkWidget *add_entry;
} EditCompositionState;

static void refresh_composition_listbox(EditCompositionState *state)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(state->listbox));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    for (guint i = 0; i < state->entries->len; i++) {
        BusBitEntry *e = g_ptr_array_index(state->entries, i);
        gchar *label_text =
            g_strdup_printf("[%u] %s", i, e->node->nname != NULL ? e->node->nname : "?");
        GtkWidget *label = gtk_label_new(label_text);
        g_free(label_text);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_margin_start(label, 4);
        gtk_widget_set_margin_end(label, 4);
        gtk_list_box_insert(GTK_LIST_BOX(state->listbox), label, -1);
    }
    gtk_widget_show_all(state->listbox);
}

static void on_composition_move_up_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    EditCompositionState *state = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (row == NULL) {
        return;
    }
    gint idx = gtk_list_box_row_get_index(row);
    if (idx <= 0) {
        return;
    }

    gpointer tmp = state->entries->pdata[idx];
    state->entries->pdata[idx] = state->entries->pdata[idx - 1];
    state->entries->pdata[idx - 1] = tmp;

    refresh_composition_listbox(state);
    gtk_list_box_select_row(
        GTK_LIST_BOX(state->listbox),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), idx - 1));
}

static void on_composition_move_down_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    EditCompositionState *state = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (row == NULL) {
        return;
    }
    gint idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || (guint)(idx + 1) >= state->entries->len) {
        return;
    }

    gpointer tmp = state->entries->pdata[idx];
    state->entries->pdata[idx] = state->entries->pdata[idx + 1];
    state->entries->pdata[idx + 1] = tmp;

    refresh_composition_listbox(state);
    gtk_list_box_select_row(
        GTK_LIST_BOX(state->listbox),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), idx + 1));
}

static void on_composition_remove_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    EditCompositionState *state = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (row == NULL) {
        status_text("GTKWAVE | Select a bit to remove first.\n");
        return;
    }
    gint idx = gtk_list_box_row_get_index(row);
    g_ptr_array_remove_index(state->entries, (guint)idx);
    refresh_composition_listbox(state);
}

static void on_composition_add_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    EditCompositionState *state = user_data;
    const gchar *path = gtk_entry_get_text(GTK_ENTRY(state->add_entry));
    if (path == NULL || path[0] == '\0') {
        status_text("GTKWAVE | Enter a signal path to add as a bit first.\n");
        return;
    }

    if (GLOBALS->dump_file == NULL) {
        status_text("GTKWAVE | No dump file loaded.\n");
        return;
    }

    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, path);
    if (sym == NULL || sym->n == NULL) {
        status_text("GTKWAVE | Could not resolve that signal path.\n");
        return;
    }
    if (sym->n->extvals) {
        status_text("GTKWAVE | Only single-bit signals can be added as a bit (not another bus).\n");
        return;
    }

    BusBitEntry *e = g_new0(BusBitEntry, 1);
    e->node = sym->n;
    g_ptr_array_add(state->entries, e);

    gtk_entry_set_text(GTK_ENTRY(state->add_entry), "");
    refresh_composition_listbox(state);
}

void bus_ops_show_edit_composition_dialog(GwTrace *t)
{
    if (t == NULL) {
        status_text("GTKWAVE | Select a bus/vector signal first.\n");
        return;
    }
    if (trace_bit_width(t) == 0) {
        status_text("GTKWAVE | Edit Bus Composition needs a bus (multi-bit) signal.\n");
        return;
    }

    GPtrArray *entries = build_entries_from_trace(t);
    if (entries == NULL) {
        return; /* build_entries_from_trace() already reported why */
    }

    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    gchar *title = g_strdup_printf("Edit Bus Composition: %s", t->name != NULL ? t->name : "");
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title,
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Apply (New Bus)",
                                                    GTK_RESPONSE_OK,
                                                    NULL);
    g_free(title);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *intro = gtk_label_new(
        "Reorder, remove, or add constituent bits below, then Apply to build a new bus with "
        "this composition -- the original bus is left untouched.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_widget_set_halign(intro, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled, 360, 200);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    EditCompositionState state = {.entries = entries, .listbox = listbox, .add_entry = NULL};
    refresh_composition_listbox(&state);

    GtkWidget *reorder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *up_button = gtk_button_new_with_label("Move Up");
    GtkWidget *down_button = gtk_button_new_with_label("Move Down");
    GtkWidget *remove_button = gtk_button_new_with_label("Remove");
    gtk_box_pack_start(GTK_BOX(reorder_row), up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(reorder_row), down_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(reorder_row), remove_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), reorder_row, FALSE, FALSE, 0);

    GtkWidget *add_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *add_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(add_entry), "signal.path to add as a bit");
    gtk_widget_set_hexpand(add_entry, TRUE);
    GtkWidget *add_button = gtk_button_new_with_label("Add Bit");
    gtk_box_pack_start(GTK_BOX(add_row), add_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(add_row), add_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), add_row, FALSE, FALSE, 0);

    state.add_entry = add_entry;

    g_signal_connect(up_button, "clicked", G_CALLBACK(on_composition_move_up_clicked), &state);
    g_signal_connect(
        down_button, "clicked", G_CALLBACK(on_composition_move_down_clicked), &state);
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_composition_remove_clicked), &state);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_composition_add_clicked), &state);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        apply_composition(t, entries);
    }

    gtk_widget_destroy(dialog);
    g_ptr_array_free(entries, TRUE);
}
