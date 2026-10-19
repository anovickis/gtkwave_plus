// Fast, inline signal color/radix editor: a GtkPopover offering the common
// subset of what the full "Trace Properties" dialog (showchange.c) exposes,
// without leaving the waveform view. Applies to the current trace selection,
// exactly like the equivalent "/Color Format/..." and "/Data Format/..."
// context menu items (menu.c) -- this widget is a faster path to the same
// backend calls, not a separate implementation of them.

#include "signal_popover.h"
#include "globals.h"
#include "gw-color.h"
#include "menu.h"
#include "bus_ops.h"
#include "gate_schematic_view.h"
#include "netlist_query_ui.h"
#include "signal_event_report.h"
#include "event_sequence_view.h"
#include "cause_trace_view.h"
#include "source_link.h"
#include "schematic_view.h"
#include "derived_signals.h"
#include "status.h"

typedef void (*MenuAction)(gpointer, guint, GtkWidget *);

typedef struct
{
    const char *name;
    gboolean has_color;
    GwRainbowColor color;
    MenuAction action;
} ColorEntry;

static const ColorEntry COLOR_ENTRIES[] = {
    {"Normal", FALSE, 0, menu_colorformat_0},
    {"Red", TRUE, GW_RAINBOW_COLOR_RED, menu_colorformat_1},
    {"Orange", TRUE, GW_RAINBOW_COLOR_ORANGE, menu_colorformat_2},
    {"Yellow", TRUE, GW_RAINBOW_COLOR_YELLOW, menu_colorformat_3},
    {"Green", TRUE, GW_RAINBOW_COLOR_GREEN, menu_colorformat_4},
    {"Blue", TRUE, GW_RAINBOW_COLOR_BLUE, menu_colorformat_5},
    {"Indigo", TRUE, GW_RAINBOW_COLOR_INDIGO, menu_colorformat_6},
    {"Violet", TRUE, GW_RAINBOW_COLOR_VIOLET, menu_colorformat_7},
};

typedef struct
{
    const char *label;
    MenuAction action;
} RadixEntry;

static const RadixEntry RADIX_ENTRIES[] = {
    {"Hex", menu_dataformat_hex},
    {"Dec", menu_dataformat_dec},
    {"Signed", menu_dataformat_signed},
    {"Bin", menu_dataformat_bin},
    {"Oct", menu_dataformat_oct},
    {"ASCII", menu_dataformat_ascii},
};

// Same backend the classic right-click popup menu's "/Data Format/Analog/..."
// submenu already calls (menu.c) -- this popover just didn't have an entry
// point for it yet. Not restricted to real-typed signals; works on any
// selected trace, same as the classic menu.
static const RadixEntry ANALOG_ENTRIES[] = {
    {"Off", menu_dataformat_analog_off},
    {"Step", menu_dataformat_analog_step},
    {"Interp", menu_dataformat_analog_interpol},
    {"Interp+", menu_dataformat_analog_interpol_step},
    {"Screen", menu_dataformat_analog_resize_screen},
    {"All Data", menu_dataformat_analog_resize_all},
};

// Apply the action tied to a button, then close the popover it lives in.
static void on_action_button_clicked(GtkButton *button, gpointer user_data)
{
    MenuAction action = (MenuAction)user_data;
    action(NULL, 0, NULL);

    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover != NULL) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

static GtkWidget *make_color_button(const ColorEntry *entry)
{
    GtkWidget *button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, entry->name);

    GtkWidget *label = gtk_label_new(NULL);
    if (entry->has_color) {
        gchar *hex = gw_color_to_hex(gw_rainbow_color_to_color(entry->color));
        gchar *markup =
            g_strdup_printf("<span foreground=\"%s\" size=\"large\">&#x2B24;</span>", hex);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
        g_free(hex);
    } else {
        gtk_label_set_markup(GTK_LABEL(label), "<span size=\"large\">&#x25CB;</span>");
    }

    gtk_container_add(GTK_CONTAINER(button), label);
    g_signal_connect(button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)entry->action);

    return button;
}

static GtkWidget *make_radix_button(const RadixEntry *entry)
{
    GtkWidget *button = gtk_button_new_with_label(entry->label);
    g_signal_connect(button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)entry->action);

    return button;
}

// Best-effort fully-qualified hierarchical path for a trace: its real
// alias/full name if it has one (name_full -- only ever set for a signal
// explicitly aliased, or restored from a saved session that had one; see
// AddNodeTraceReturn()/AddVector() in analyzer.c, which only populate it
// when an `aliasname` argument was actually passed), otherwise the
// underlying node/vector's own name (nd->nname / vec->bvname) -- the
// SAME discriminant trace_is_bus() below already uses, and the one true
// full name every ordinary, non-aliased trace actually has (confirmed
// directly against a live instance: an un-aliased signal just dragged
// in from the SST tree has name_full == NULL, always, for every normal
// add path -- AddVector()/AddNode()'s own callers all pass a NULL
// aliasname except savefile.c restoring a previously-saved alias).
// Unlike t->name, this is never shortened by GLOBALS->hier_max_level
// (see hier_extract() in analyzer.c), so it's safe to feed into a
// netlist backend that needs the real hierarchical path.
static const gchar *trace_full_path(GwTrace *t)
{
    if (t->name_full != NULL) {
        return t->name_full;
    }
    if (t->vector) {
        return t->n.vec != NULL ? t->n.vec->bvname : NULL;
    }
    return t->n.nd != NULL ? t->n.nd->nname : NULL;
}

// Driver/load queries act on exactly one signal; picks the first
// highlighted, named trace (the same highlight set the color/radix actions
// above act on in bulk), since a fan-in/fan-out query only makes sense for
// a single target.
static GwTrace *find_target_trace(void)
{
    GwTrace *t = GLOBALS->traces.first;
    while (t != NULL) {
        if ((t->flags & TR_HIGHLIGHT) && !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) &&
           trace_full_path(t) != NULL) {
            return t;
        }
        t = t->t_next;
    }
    return NULL;
}

static void close_ancestor_popover(GtkWidget *widget)
{
    GtkWidget *popover = gtk_widget_get_ancestor(widget, GTK_TYPE_POPOVER);
    if (popover != NULL) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

// True for either representation a multi-bit signal can have in this
// codebase -- a real constructed/auto-coalesced vector (GwBitVector) or,
// far more commonly, a natively-declared multi-bit VCD var (a GwNode with
// extvals set) -- same split src/list_x_view.c's scan and src/bus_ops.c's
// slicing both needed. Gates whether the bit-range entry below is shown
// at all (a scalar signal has no range to narrow).
static gboolean trace_is_bus(GwTrace *t)
{
    if (t == NULL) {
        return FALSE;
    }
    if (t->vector) {
        return TRUE;
    }
    return t->n.nd != NULL && t->n.nd->extvals;
}

// The netlist-query backend (GwNetlistQuery -> slang-netlist) has always
// accepted a bit-range-suffixed signal_path (e.g. "count_q[3:0]") -- that
// part needed no code change. What was missing was a UI entry point: the
// popover only ever passed the whole-signal name_full through. When
// `range_entry` holds non-empty text, appends it as "[text]"; otherwise
// (including when there's no range_entry at all, i.e. a scalar target)
// behaves exactly as before.
static gchar *build_query_signal_path(GwTrace *t, GtkEntry *range_entry)
{
    const gchar *full_path = trace_full_path(t);
    if (range_entry != NULL) {
        const gchar *range_text = gtk_entry_get_text(range_entry);
        if (range_text != NULL && range_text[0] != '\0') {
            return g_strdup_printf("%s[%s]", full_path, range_text);
        }
    }
    return g_strdup(full_path);
}

static void on_show_drivers_clicked(GtkButton *button, gpointer user_data)
{
    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its drivers.\n");
        return;
    }
    gchar *signal_path = build_query_signal_path(t, GTK_ENTRY(user_data));
    netlist_query_show_drivers(signal_path);
    g_free(signal_path);
}

static void on_show_loads_clicked(GtkButton *button, gpointer user_data)
{
    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its loads.\n");
        return;
    }
    gchar *signal_path = build_query_signal_path(t, GTK_ENTRY(user_data));
    netlist_query_show_loads(signal_path);
    g_free(signal_path);
}

// "Show Cone of Logic" -- the real gate-level schematic (Yosys-
// synthesized AND/OR/XOR/NOT/MUX/DFF shapes, real wires), NOT the RTL
// dataflow-graph view (schematic_view.c's netlist_query_show_schematic()
// -- a pre-synthesis Assignment/Conditional/Case/Merge graph, which
// reads as a flowchart rather than a schematic since no gates exist yet
// at that level, and isn't what's wanted here per direct feedback: "now
// it opens a window labeled schematic, but what we get is a flow
// chart"). gate_schematic_view.c already has its own bit-range-
// stripping and testbench-vs-DUT auto-detection (independent of, and
// just as capable as, the slang-netlist-backed versions in netlist_
// query_ui.c/schematic_view.c), so build_query_signal_path()'s bit-
// range-suffixed path (if any) is handled correctly either way.
static void on_show_schematic_clicked(GtkButton *button, gpointer user_data)
{
    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its cone of logic.\n");
        return;
    }
    gchar *signal_path = build_query_signal_path(t, GTK_ENTRY(user_data));
    netlist_query_show_gate_schematic(signal_path);
    g_free(signal_path);
}

// "RTL Schematic" -- the pre-synthesis dataflow view (schematic_view.c):
// Assignment/Conditional/Case nodes with live values at the marker, edge
// coloring, Prev/Next Transition, and hand-movable boxes (Move Nodes). A
// separate button from "Show Cone of Logic", which deliberately opens the
// gate-level view instead.
static void on_show_rtl_schematic_clicked(GtkButton *button, gpointer user_data)
{
    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its RTL schematic.\n");
        return;
    }
    gchar *signal_path = build_query_signal_path(t, GTK_ENTRY(user_data));
    netlist_query_show_schematic(signal_path);
    g_free(signal_path);
}

static void on_show_event_report_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    close_ancestor_popover(GTK_WIDGET(button));
    signal_event_report_show();
}

static void on_show_event_sequence_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    close_ancestor_popover(GTK_WIDGET(button));
    event_sequence_view_show();
}

static void on_go_to_source_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    if (t == NULL) {
        return;
    }
    gchar *path = g_strdup(trace_full_path(t));
    close_ancestor_popover(GTK_WIDGET(button));
    source_link_go_to_signal(GTK_WIDGET(GLOBALS->signalarea), path);
    g_free(path);
}

static void on_trace_cause_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    if (t == NULL) {
        return;
    }
    gchar *path = g_strdup(trace_full_path(t));
    close_ancestor_popover(GTK_WIDGET(button));
    cause_trace_view_show(path);
    g_free(path);
}

static void on_derived_signal_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    close_ancestor_popover(GTK_WIDGET(button));
    derived_signal_show_dialog();
}

static void on_expand_subbus_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_show_expand_subbus_dialog(t);
}

static void on_define_fields_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_show_define_fields_dialog(t);
}

static void on_reverse_bit_order_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_reverse_bit_order(t);
}

static void on_edit_composition_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_show_edit_composition_dialog(t);
}

static void add_section_label(GtkWidget *box, const char *text)
{
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    g_free(markup);
}

void signal_popover_show(GtkWidget *relative_to, gdouble x, gdouble y)
{
    if (relative_to == NULL) {
        relative_to = GLOBALS->wavewindow != NULL ? GLOBALS->wavewindow : GLOBALS->mainwindow;
    }
    if (relative_to == NULL) {
        return;
    }

    GtkWidget *popover = gtk_popover_new(relative_to);

    GdkRectangle rect = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    add_section_label(box, "Color");

    GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (gsize i = 0; i < G_N_ELEMENTS(COLOR_ENTRIES); i++) {
        gtk_box_pack_start(GTK_BOX(color_box), make_color_button(&COLOR_ENTRIES[i]), FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), color_box, FALSE, FALSE, 0);

    add_section_label(box, "Format");

    GtkWidget *radix_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (gsize i = 0; i < G_N_ELEMENTS(RADIX_ENTRIES); i++) {
        gtk_box_pack_start(GTK_BOX(radix_box), make_radix_button(&RADIX_ENTRIES[i]), FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), radix_box, FALSE, FALSE, 0);

    add_section_label(box, "Analog");

    GtkWidget *analog_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (gsize i = 0; i < G_N_ELEMENTS(ANALOG_ENTRIES); i++) {
        gtk_box_pack_start(
            GTK_BOX(analog_box), make_radix_button(&ANALOG_ENTRIES[i]), FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), analog_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    GtkWidget *props_button = gtk_button_new_with_label("Trace Properties\xe2\x80\xa6");
    gtk_button_set_relief(GTK_BUTTON(props_button), GTK_RELIEF_NONE);
    g_signal_connect(props_button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)(MenuAction)menu_showchangeall);
    gtk_box_pack_start(GTK_BOX(box), props_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // Bit-range entry for the netlist queries below -- the backend
    // (GwNetlistQuery -> slang-netlist) has always accepted a
    // bit-range-suffixed signal_path; this is the missing UI entry point,
    // so only worth showing at all for a bus/multi-bit target.
    GtkWidget *range_entry = NULL;
    if (trace_is_bus(find_target_trace())) {
        GtkWidget *range_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_pack_start(GTK_BOX(range_box), gtk_label_new("Bit range:"), FALSE, FALSE, 0);
        range_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(range_entry), "e.g. 3:1 (whole signal if empty)");
        gtk_widget_set_tooltip_text(
            range_entry,
            "Optional bit range to narrow the queries below to, e.g. \"3:1\" for bits 3 "
            "down to 1. Leave empty to query the whole signal.");
        gtk_box_pack_start(GTK_BOX(range_box), range_entry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), range_box, FALSE, FALSE, 0);
    }

    GtkWidget *netlist_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *drivers_button = gtk_button_new_with_label("Show Drivers");
    g_signal_connect(drivers_button, "clicked", G_CALLBACK(on_show_drivers_clicked), range_entry);
    gtk_box_pack_start(GTK_BOX(netlist_box), drivers_button, TRUE, TRUE, 0);

    GtkWidget *loads_button = gtk_button_new_with_label("Show Loads");
    g_signal_connect(loads_button, "clicked", G_CALLBACK(on_show_loads_clicked), range_entry);
    gtk_box_pack_start(GTK_BOX(netlist_box), loads_button, TRUE, TRUE, 0);

    // "Show Cone of Logic" -- the real gate-level schematic (see
    // on_show_schematic_clicked()'s own comment for why this isn't the
    // RTL dataflow view). A separate "Show Gate Schematic (SVG)" button
    // existed earlier and was removed as pure UI duplication once this
    // button was pointed at the same diagram.
    GtkWidget *schematic_button = gtk_button_new_with_label("Show Cone of Logic");
    g_signal_connect(
        schematic_button, "clicked", G_CALLBACK(on_show_schematic_clicked), range_entry);
    gtk_box_pack_start(GTK_BOX(netlist_box), schematic_button, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), netlist_box, FALSE, FALSE, 0);

    GtkWidget *rtl_button = gtk_button_new_with_label("RTL Schematic");
    gtk_widget_set_tooltip_text(
        rtl_button,
        "Pre-synthesis dataflow diagram of the signal's fan-in, with live values at the marker. "
        "Boxes can be rearranged (Move Nodes).");
    g_signal_connect(rtl_button, "clicked", G_CALLBACK(on_show_rtl_schematic_clicked), range_entry);
    gtk_box_pack_start(GTK_BOX(box), rtl_button, FALSE, FALSE, 0);

    GtkWidget *event_report_button = gtk_button_new_with_label("Event Report");
    g_signal_connect(
        event_report_button, "clicked", G_CALLBACK(on_show_event_report_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), event_report_button, FALSE, FALSE, 0);

    GtkWidget *event_sequence_button = gtk_button_new_with_label("Event Sequence");
    g_signal_connect(
        event_sequence_button, "clicked", G_CALLBACK(on_show_event_sequence_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), event_sequence_button, FALSE, FALSE, 0);

    GtkWidget *source_button = gtk_button_new_with_label("Go to Source");
    gtk_widget_set_tooltip_text(
        source_button,
        "Open the RTL line that declares or drives this signal in the Source Panel");
    g_signal_connect(source_button, "clicked", G_CALLBACK(on_go_to_source_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), source_button, FALSE, FALSE, 0);

    GtkWidget *cause_button = gtk_button_new_with_label("Trace Cause");
    gtk_widget_set_tooltip_text(
        cause_button,
        "Explain why this signal changed at the marker, and trace it back through the design");
    g_signal_connect(cause_button, "clicked", G_CALLBACK(on_trace_cause_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), cause_button, FALSE, FALSE, 0);

    GtkWidget *derived_button = gtk_button_new_with_label("Derived Signal\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(
        derived_button,
        "Add a signal computed from an expression over existing signals, e.g. (a & b) | c, "
        "count + 1, or state == 3");
    g_signal_connect(derived_button, "clicked", G_CALLBACK(on_derived_signal_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), derived_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    add_section_label(box, "Bus Operations");

    GtkWidget *combine_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *combine_down_button = gtk_button_new_with_label("Create Bus (Down)");
    g_signal_connect(combine_down_button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)(MenuAction)menu_combine_down);
    gtk_box_pack_start(GTK_BOX(combine_box), combine_down_button, TRUE, TRUE, 0);

    GtkWidget *combine_up_button = gtk_button_new_with_label("Create Bus (Up)");
    g_signal_connect(combine_up_button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)(MenuAction)menu_combine_up);
    gtk_box_pack_start(GTK_BOX(combine_box), combine_up_button, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), combine_box, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(
        combine_box, "Combine the currently-highlighted signals into one bus (same as the "
                    "existing F4/F5 shortcuts)");

    GtkWidget *bus_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *subbus_button = gtk_button_new_with_label("Expand as Sub-Bus\xe2\x80\xa6");
    g_signal_connect(subbus_button, "clicked", G_CALLBACK(on_expand_subbus_clicked), NULL);
    gtk_widget_set_tooltip_text(subbus_button,
                               "Slice a bit range out of this bus into its own new bus");
    gtk_box_pack_start(GTK_BOX(bus_box), subbus_button, TRUE, TRUE, 0);

    GtkWidget *reverse_button = gtk_button_new_with_label("Reverse Bit Order");
    g_signal_connect(reverse_button, "clicked", G_CALLBACK(on_reverse_bit_order_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bus_box), reverse_button, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), bus_box, FALSE, FALSE, 0);

    GtkWidget *edit_composition_button = gtk_button_new_with_label("Edit Bus Composition\xe2\x80\xa6");
    g_signal_connect(
        edit_composition_button, "clicked", G_CALLBACK(on_edit_composition_clicked), NULL);
    gtk_widget_set_tooltip_text(
        edit_composition_button,
        "Reorder, add, or remove this bus's constituent bits into a new bus");
    gtk_box_pack_start(GTK_BOX(box), edit_composition_button, FALSE, FALSE, 0);

    GtkWidget *define_fields_button = gtk_button_new_with_label("Define Struct Fields\xe2\x80\xa6");
    g_signal_connect(
        define_fields_button, "clicked", G_CALLBACK(on_define_fields_clicked), NULL);
    gtk_widget_set_tooltip_text(
        define_fields_button,
        "Describe the fields of a packed struct carried by this bus (e.g. op:4 imm:8 valid:1). "
        "The bus becomes a group: the whole value on the header row, each field inside it.");
    gtk_box_pack_start(GTK_BOX(box), define_fields_button, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(popover));
}
