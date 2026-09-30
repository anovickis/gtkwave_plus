#pragma once

#include <gtk/gtk.h>
#include "analyzer.h"

G_BEGIN_DECLS

#define GW_TYPE_MOUSEOVER (gw_mouseover_get_type())
G_DECLARE_FINAL_TYPE(GwMouseover, gw_mouseover, GW, MOUSEOVER, GtkWindow)

GtkWidget *gw_mouseover_new(void);
void gw_mouseover_update(GwMouseover *self, GwTrace *trace, GwTime t);
void gw_mouseover_update_signal_list(GwMouseover *self, GwTrace *trace, GwTime t);

// Formats a trace's value at an arbitrary time (not necessarily the primary
// marker), the same logic behind the mouseover tooltip. Caller frees the
// result with free_2(). Returns NULL if the trace has no value at that
// time (e.g. before the first transition).
gchar *local_trace_asciival(GwTrace *t, GwTime tim);

G_END_DECLS
