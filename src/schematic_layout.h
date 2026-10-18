#pragma once

#include <glib.h>
#include <stdio.h>

// Hand-made node position overrides for the RTL schematic view, layered on
// top of Graphviz's own layout. Stored as an OFFSET from wherever Graphviz
// put the node (in points, y positive = down), not an absolute position,
// so an override still means "a bit to the right of where it would be"
// after the depth limit, a marker move's wider value labels, or a changed
// cone nudges the automatic layout.
//
// Keyed by (root signal path, Graphviz node id). Node ids come from
// slang-netlist's elaboration, so each entry also records the node's
// label and is ignored if the label no longer matches -- a changed design
// that renumbers nodes quietly drops stale overrides instead of moving
// the wrong box.
//
// Process-wide and independent of any one window, so closing and
// reopening a schematic (or an Auto Update reload) keeps the layout, and
// written to/read from save files so it survives across sessions.

void schematic_layout_set(const gchar *root,
                          const gchar *node_id,
                          const gchar *label,
                          gdouble dx,
                          gdouble dy);

// TRUE and fills dx/dy only if an override exists for node_id AND its
// recorded label equals `label`.
gboolean schematic_layout_get(const gchar *root,
                              const gchar *node_id,
                              const gchar *label,
                              gdouble *dx,
                              gdouble *dy);

guint schematic_layout_count(const gchar *root);
void schematic_layout_clear(const gchar *root);
void schematic_layout_clear_all(void);

// One "[schematic_offset] ..." line per override, for write_save_helper().
void schematic_layout_write(FILE *out);

// Parses the part of a save-file line after "[schematic_offset] ". Returns
// FALSE (storing nothing) if malformed.
gboolean schematic_layout_read(const gchar *args);
