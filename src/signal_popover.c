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

    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(popover));
}
