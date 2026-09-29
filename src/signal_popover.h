#pragma once

#include <gtk/gtk.h>

// Show the quick per-signal color/radix popover, anchored to `relative_to`
// (or a sane fallback if NULL) and pointing at (x, y) in that widget's
// coordinate space.
void signal_popover_show(GtkWidget *relative_to, gdouble x, gdouble y);
