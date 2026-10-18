#pragma once

#include <glib.h>

// Applies per-node position overrides to a Graphviz `dot -Tsvg` drawing
// WITHOUT re-running a layout engine. Graphviz can only honor pinned node
// positions through neato/fdp, and plenty of installs ship only the `dot`
// layout plugin, so the edit is done on the SVG instead: a moved node's
// group gets a `transform`, and every edge touching a moved node is
// re-drawn between the nodes' new boxes (arrowhead and label included).
// Everything else in the drawing is left byte-for-byte as Graphviz wrote it.
//
// All coordinates are the SVG's own "inner" coordinates in points, y-down
// (Graphviz writes y as negative-up inside a translated root group, so a
// node's center from `-Tplain` (inches, y-up) is (cx*72, -cy*72)).

typedef struct
{
    gdouble cx, cy; // center
    gdouble w, h; // size
} SchematicSvgNode;

typedef struct
{
    gdouble dx, dy; // how far the node was moved (y positive = down)
} SchematicSvgOffset;

typedef struct
{
    gdouble min_x, min_y; // of the edited drawing, inner coordinates
    gdouble width, height;
} SchematicSvgBounds;

// svg: output of `dot -Gpad=0 -Gmargin=0 -Tsvg`.
// nodes: node id (e.g. "N7") -> SchematicSvgNode*, the ORIGINAL layout.
// offsets: node id -> SchematicSvgOffset*; ids absent (or at 0,0) don't move.
// orig_w/orig_h: the original drawing's size in points (`-Tplain` graph
// line, times 72), so the result always covers the original area.
// px_per_pt: pixel scale of the output (96/72 to match the app's 96 DPI).
//
// Returns the edited SVG, sized in pixels (width/height attributes) so a
// rasterizer needs no DPI guess, with *out_bounds filled so a caller can
// map a node's position to pixels as (inner - min) * px_per_pt. NULL with
// error set if `svg` isn't Graphviz-shaped.
gchar *schematic_svg_apply_offsets(const gchar *svg,
                                   GHashTable *nodes,
                                   GHashTable *offsets,
                                   gdouble orig_w,
                                   gdouble orig_h,
                                   gdouble px_per_pt,
                                   SchematicSvgBounds *out_bounds,
                                   GError **error);

// Pixel-space top-left and size of `node` after `offset` (which may be
// NULL) is applied, in the same pixel space as the edited SVG.
void schematic_svg_node_pixel_box(const SchematicSvgNode *node,
                                  const SchematicSvgOffset *offset,
                                  const SchematicSvgBounds *bounds,
                                  gdouble px_per_pt,
                                  gdouble *x,
                                  gdouble *y,
                                  gdouble *w,
                                  gdouble *h);
