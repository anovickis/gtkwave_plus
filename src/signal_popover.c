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
#include "schematic_view.h"
#include "signal_event_report.h"
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

// Driver/load queries act on exactly one signal; picks the first
// highlighted, named trace (the same highlight set the color/radix actions
// above act on in bulk), since a fan-in/fan-out query only makes sense for
// a single target.
static GwTrace *find_target_trace(void)
{
    GwTrace *t = GLOBALS->traces.first;
    while (t != NULL) {
        if ((t->flags & TR_HIGHLIGHT) && !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) &&
           t->name_full != NULL) {
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

static void on_show_drivers_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its drivers.\n");
        return;
    }
    netlist_query_show_drivers(t->name_full);
}

static void on_show_loads_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its loads.\n");
        return;
    }
    netlist_query_show_loads(t->name_full);
}

static void on_show_cone_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its cone of logic.\n");
        return;
    }
    netlist_query_show_cone(t->name_full);
}

static void on_show_schematic_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its schematic.\n");
        return;
    }
    netlist_query_show_schematic(t->name_full);
}

static void on_show_gate_schematic_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));

    if (t == NULL) {
        status_text("GTKWAVE | Select a signal first to show its gate-level schematic.\n");
        return;
    }
    netlist_query_show_gate_schematic(t->name_full);
}

static void on_show_event_report_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    close_ancestor_popover(GTK_WIDGET(button));
    signal_event_report_show();
}

static void on_expand_subbus_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_show_expand_subbus_dialog(t);
}

static void on_reverse_bit_order_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    GwTrace *t = find_target_trace();
    close_ancestor_popover(GTK_WIDGET(button));
    bus_ops_reverse_bit_order(t);
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

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    GtkWidget *props_button = gtk_button_new_with_label("Trace Properties\xe2\x80\xa6");
    gtk_button_set_relief(GTK_BUTTON(props_button), GTK_RELIEF_NONE);
    g_signal_connect(props_button,
                     "clicked",
                     G_CALLBACK(on_action_button_clicked),
                     (gpointer)(MenuAction)menu_showchangeall);
    gtk_box_pack_start(GTK_BOX(box), props_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    GtkWidget *netlist_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *drivers_button = gtk_button_new_with_label("Show Drivers");
    g_signal_connect(drivers_button, "clicked", G_CALLBACK(on_show_drivers_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(netlist_box), drivers_button, TRUE, TRUE, 0);

    GtkWidget *loads_button = gtk_button_new_with_label("Show Loads");
    g_signal_connect(loads_button, "clicked", G_CALLBACK(on_show_loads_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(netlist_box), loads_button, TRUE, TRUE, 0);

    GtkWidget *cone_button = gtk_button_new_with_label("Show Cone of Logic");
    g_signal_connect(cone_button, "clicked", G_CALLBACK(on_show_cone_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(netlist_box), cone_button, TRUE, TRUE, 0);

    GtkWidget *schematic_button = gtk_button_new_with_label("Show Schematic");
    g_signal_connect(schematic_button, "clicked", G_CALLBACK(on_show_schematic_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(netlist_box), schematic_button, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), netlist_box, FALSE, FALSE, 0);

    GtkWidget *gate_schematic_button = gtk_button_new_with_label("Show Gate Schematic (SVG)");
    g_signal_connect(
        gate_schematic_button, "clicked", G_CALLBACK(on_show_gate_schematic_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), gate_schematic_button, FALSE, FALSE, 0);

    GtkWidget *event_report_button = gtk_button_new_with_label("Event Report");
    g_signal_connect(
        event_report_button, "clicked", G_CALLBACK(on_show_event_report_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), event_report_button, FALSE, FALSE, 0);

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

    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(popover));
}
