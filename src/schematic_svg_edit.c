#include "schematic_svg_edit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARROW_LENGTH 10.0
#define ARROW_HALF_WIDTH 3.5
#define BOUNDS_MARGIN 2.0
#define PARALLEL_SPACING 22.0

static gboolean is_moved(const SchematicSvgOffset *o)
{
    return o != NULL && (o->dx != 0.0 || o->dy != 0.0);
}

// Where the line from `from`'s center toward (tx, ty) leaves `from`'s box.
static void box_anchor(const SchematicSvgNode *from,
                       const SchematicSvgOffset *off,
                       gdouble tx,
                       gdouble ty,
                       gdouble *ax,
                       gdouble *ay)
{
    gdouble cx = from->cx + (off != NULL ? off->dx : 0.0);
    gdouble cy = from->cy + (off != NULL ? off->dy : 0.0);
    gdouble dx = tx - cx;
    gdouble dy = ty - cy;
    gdouble hw = from->w / 2.0;
    gdouble hh = from->h / 2.0;

    if (dx == 0.0 && dy == 0.0) {
        *ax = cx;
        *ay = cy;
        return;
    }

    gdouble tx_scale = dx != 0.0 ? hw / fabs(dx) : G_MAXDOUBLE;
    gdouble ty_scale = dy != 0.0 ? hh / fabs(dy) : G_MAXDOUBLE;
    gdouble t = MIN(tx_scale, ty_scale);
    *ax = cx + dx * t;
    *ay = cy + dy * t;
}

// "A&#45;&gt;B" (also tolerates a literal "A->B") -> ids. Caller frees.
static gboolean parse_edge_title(const gchar *title, gchar **tail, gchar **head)
{
    const gchar *sep = strstr(title, "&#45;&gt;");
    gsize sep_len = 9;
    if (sep == NULL) {
        sep = strstr(title, "->");
        sep_len = 2;
    }
    if (sep == NULL) {
        return FALSE;
    }
    *tail = g_strndup(title, (gsize)(sep - title));
    *head = g_strdup(sep + sep_len);
    return TRUE;
}

static gchar *fmt(gdouble v)
{
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof(buf), "%.2f", v);
    return g_strdup(buf);
}

// Replaces the attribute `attr="..."` inside tag text; returns a new string.
static gchar *replace_attr(const gchar *tag, const gchar *attr, const gchar *value)
{
    gchar *needle = g_strdup_printf(" %s=\"", attr);
    const gchar *start = strstr(tag, needle);
    if (start == NULL) {
        g_free(needle);
        return g_strdup(tag);
    }
    const gchar *vstart = start + strlen(needle);
    const gchar *vend = strchr(vstart, '"');
    if (vend == NULL) {
        g_free(needle);
        return g_strdup(tag);
    }
    gchar *prefix = g_strndup(tag, (gsize)(vstart - tag));
    gchar *out = g_strconcat(prefix, value, vend, NULL);
    g_free(prefix);
    g_free(needle);
    return out;
}

// First "M x,y" and last coordinate pair of a path's `d`, to find where the
// label used to sit relative to the old curve.
static gboolean path_endpoints(const gchar *tag, gdouble *x0, gdouble *y0, gdouble *x1, gdouble *y1)
{
    const gchar *d = strstr(tag, " d=\"M");
    if (d == NULL) {
        return FALSE;
    }
    d += 5;
    const gchar *end = strchr(d, '"');
    if (end == NULL) {
        return FALSE;
    }
    gchar *body = g_strndup(d, (gsize)(end - d));
    gdouble vals[2] = {0, 0};
    gdouble first[2] = {0, 0};
    gboolean have_first = FALSE;
    guint n = 0;
    for (gchar *p = body; *p != '\0';) {
        if (g_ascii_isdigit(*p) || *p == '-' || *p == '.') {
            gchar *next;
            gdouble v = g_ascii_strtod(p, &next);
            vals[n & 1] = v;
            n++;
            if (n == 2 && !have_first) {
                first[0] = vals[0];
                first[1] = vals[1];
                have_first = TRUE;
            }
            p = next;
        } else {
            p++;
        }
    }
    g_free(body);
    if (n < 4) {
        return FALSE;
    }
    *x0 = first[0];
    *y0 = first[1];
    *x1 = vals[0];
    *y1 = vals[1];
    return TRUE;
}

static void bounds_include(gdouble *minx, gdouble *miny, gdouble *maxx, gdouble *maxy,
                           gdouble x0, gdouble y0, gdouble x1, gdouble y1)
{
    *minx = MIN(*minx, x0);
    *miny = MIN(*miny, y0);
    *maxx = MAX(*maxx, x1);
    *maxy = MAX(*maxy, y1);
}

// Rewrites one edge group so it runs between the nodes' (possibly moved)
// boxes. `group` is the whole `<g ...>...</g>` text.
static gchar *rewrite_edge(const gchar *group,
                           const SchematicSvgNode *tail,
                           const SchematicSvgOffset *tail_off,
                           const SchematicSvgNode *head,
                           const SchematicSvgOffset *head_off,
                           gdouble lateral_nx,
                           gdouble lateral_ny,
                           gdouble lateral)
{
    gdouble tcx = tail->cx + (tail_off ? tail_off->dx : 0.0);
    gdouble tcy = tail->cy + (tail_off ? tail_off->dy : 0.0);
    gdouble hcx = head->cx + (head_off ? head_off->dx : 0.0);
    gdouble hcy = head->cy + (head_off ? head_off->dy : 0.0);

    gdouble x1, y1, x2, y2;
    box_anchor(tail, tail_off, hcx, hcy, &x1, &y1);
    box_anchor(head, head_off, tcx, tcy, &x2, &y2);

    gdouble dx = x2 - x1;
    gdouble dy = y2 - y1;
    gdouble len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        return g_strdup(group);
    }
    gdouble ux = dx / len;
    gdouble uy = dy / len;

    // Leave/enter along the dominant axis, like dot's own top-to-bottom
    // splines, so a moved node's edges still read as flow. Computed against
    // the straight anchor-to-anchor line; the arrowhead is fitted afterwards.
    gdouble arrow = MIN(ARROW_LENGTH, len / 2.0);
    gdouble bx = x2 - ux * arrow;
    gdouble by = y2 - uy * arrow;

    gdouble c1x, c1y, c2x, c2y;
    if (fabs(dy) >= fabs(dx) * 0.5) {
        gdouble k = fabs(by - y1) / 2.0;
        gdouble sgn = (by >= y1) ? 1.0 : -1.0;
        c1x = x1;
        c1y = y1 + sgn * k;
        c2x = bx;
        c2y = by - sgn * k;
    } else {
        gdouble k = fabs(bx - x1) / 2.0;
        gdouble sgn = (bx >= x1) ? 1.0 : -1.0;
        c1x = x1 + sgn * k;
        c1y = y1;
        c2x = bx - sgn * k;
        c2y = by;
    }

    // Edges sharing the same node pair (e.g. a signal and its feedback)
    // would otherwise be drawn on top of each other: bow each one sideways
    // by its own amount, along a direction common to the pair so the two
    // directions fan out to opposite sides.
    c1x += lateral_nx * lateral;
    c1y += lateral_ny * lateral;
    c2x += lateral_nx * lateral;
    c2y += lateral_ny * lateral;

    // Arrowhead follows the curve's real end tangent, tip on the box.
    gdouble tx_ = x2 - c2x, ty_ = y2 - c2y;
    gdouble tlen = sqrt(tx_ * tx_ + ty_ * ty_);
    if (tlen < 1e-6) {
        tx_ = ux;
        ty_ = uy;
        tlen = 1.0;
    }
    ux = tx_ / tlen;
    uy = ty_ / tlen;
    bx = x2 - ux * arrow;
    by = y2 - uy * arrow;

    gdouble old_x0 = 0, old_y0 = 0, old_x1 = 0, old_y1 = 0;
    gboolean have_old = path_endpoints(group, &old_x0, &old_y0, &old_x1, &old_y1);
    gdouble old_mx = (old_x0 + old_x1) / 2.0;
    gdouble old_my = (old_y0 + old_y1) / 2.0;
    // Midpoint of the cubic at t = 0.5.
    gdouble new_mx = (x1 + 3 * c1x + 3 * c2x + bx) / 8.0;
    gdouble new_my = (y1 + 3 * c1y + 3 * c2y + by) / 8.0;

    GString *out = g_string_new(NULL);
    const gchar *p = group;
    while (*p != '\0') {
        const gchar *lt = strchr(p, '<');
        if (lt == NULL) {
            g_string_append(out, p);
            break;
        }
        g_string_append_len(out, p, (gssize)(lt - p));
        const gchar *gt = strchr(lt, '>');
        if (gt == NULL) {
            g_string_append(out, lt);
            break;
        }

        gchar *tag = g_strndup(lt, (gsize)(gt - lt + 1));

        if (g_str_has_prefix(tag, "<path ")) {
            gchar *x1s = fmt(x1), *y1s = fmt(y1), *c1xs = fmt(c1x), *c1ys = fmt(c1y);
            gchar *c2xs = fmt(c2x), *c2ys = fmt(c2y), *bxs = fmt(bx), *bys = fmt(by);
            gchar *d = g_strdup_printf("M%s,%s C%s,%s %s,%s %s,%s", x1s, y1s, c1xs, c1ys, c2xs,
                                       c2ys, bxs, bys);
            gchar *nt = replace_attr(tag, "d", d);
            g_string_append(out, nt);
            g_free(nt);
            g_free(d);
            g_free(x1s); g_free(y1s); g_free(c1xs); g_free(c1ys);
            g_free(c2xs); g_free(c2ys); g_free(bxs); g_free(bys);
        } else if (g_str_has_prefix(tag, "<polygon ")) {
            // Arrowhead: tip on the head box, base ARROW_LENGTH back.
            gdouble nx = -uy, ny = ux;
            gdouble ax = bx + nx * ARROW_HALF_WIDTH, ay = by + ny * ARROW_HALF_WIDTH;
            gdouble cxp = bx - nx * ARROW_HALF_WIDTH, cyp = by - ny * ARROW_HALF_WIDTH;
            gchar *axs = fmt(ax), *ays = fmt(ay), *txs = fmt(x2), *tys = fmt(y2);
            gchar *cxs = fmt(cxp), *cys = fmt(cyp);
            gchar *pts = g_strdup_printf("%s,%s %s,%s %s,%s %s,%s", axs, ays, txs, tys, cxs, cys,
                                         axs, ays);
            gchar *nt = replace_attr(tag, "points", pts);
            g_string_append(out, nt);
            g_free(nt);
            g_free(pts);
            g_free(axs); g_free(ays); g_free(txs); g_free(tys); g_free(cxs); g_free(cys);
        } else if (g_str_has_prefix(tag, "<text ") && have_old) {
            // Keep the label the same distance from the curve's middle.
            const gchar *xa = strstr(tag, " x=\"");
            const gchar *ya = strstr(tag, " y=\"");
            if (xa != NULL && ya != NULL) {
                gdouble ox = g_ascii_strtod(xa + 4, NULL);
                gdouble oy = g_ascii_strtod(ya + 4, NULL);
                gchar *nxs = fmt(ox + (new_mx - old_mx));
                gchar *nys = fmt(oy + (new_my - old_my));
                gchar *t1 = replace_attr(tag, "x", nxs);
                gchar *t2 = replace_attr(t1, "y", nys);
                g_string_append(out, t2);
                g_free(t1); g_free(t2); g_free(nxs); g_free(nys);
            } else {
                g_string_append(out, tag);
            }
        } else {
            g_string_append(out, tag);
        }
        g_free(tag);
        p = gt + 1;
    }
    return g_string_free(out, FALSE);
}

gchar *schematic_svg_apply_offsets(const gchar *svg,
                                   GHashTable *nodes,
                                   GHashTable *offsets,
                                   gdouble orig_w,
                                   gdouble orig_h,
                                   gdouble px_per_pt,
                                   SchematicSvgBounds *out_bounds,
                                   GError **error)
{
    GRegex *group_re =
        g_regex_new("<g id=\"(node|edge)[0-9]+\" class=\"(node|edge)\">(.*?)</g>",
                    G_REGEX_DOTALL,
                    0,
                    NULL);
    GRegex *title_re = g_regex_new("<title>([^<]*)</title>", 0, 0, NULL);

    // Bounds start as the original drawing (inner coords: x in [0, W],
    // y in [-H, 0]) and grow to cover every moved node.
    gdouble minx = 0.0, miny = -orig_h, maxx = orig_w, maxy = 0.0;

    GHashTableIter it;
    gpointer key, value;
    if (offsets != NULL) {
        g_hash_table_iter_init(&it, offsets);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            const SchematicSvgOffset *off = value;
            const SchematicSvgNode *n = g_hash_table_lookup(nodes, key);
            if (n == NULL || !is_moved(off)) {
                continue;
            }
            bounds_include(&minx, &miny, &maxx, &maxy,
                           n->cx + off->dx - n->w / 2.0, n->cy + off->dy - n->h / 2.0,
                           n->cx + off->dx + n->w / 2.0, n->cy + off->dy + n->h / 2.0);
        }
    }
    minx -= BOUNDS_MARGIN;
    miny -= BOUNDS_MARGIN;
    maxx += BOUNDS_MARGIN;
    maxy += BOUNDS_MARGIN;

    // How many edges will be redrawn between each unordered node pair, so
    // parallel ones can be spread apart (see rewrite_edge()).
    GHashTable *pair_total = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *pair_next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    {
        GMatchInfo *pmi = NULL;
        g_regex_match(group_re, svg, 0, &pmi);
        while (g_match_info_matches(pmi)) {
            gchar *kind = g_match_info_fetch(pmi, 1);
            gchar *body = g_match_info_fetch(pmi, 3);
            GMatchInfo *tmi = NULL;
            if (g_str_equal(kind, "edge") && g_regex_match(title_re, body, 0, &tmi)) {
                gchar *title = g_match_info_fetch(tmi, 1);
                gchar *t = NULL, *h = NULL;
                if (parse_edge_title(title, &t, &h)) {
                    const SchematicSvgOffset *to = offsets ? g_hash_table_lookup(offsets, t) : NULL;
                    const SchematicSvgOffset *ho = offsets ? g_hash_table_lookup(offsets, h) : NULL;
                    if (is_moved(to) || is_moved(ho)) {
                        gchar *pair_key = strcmp(t, h) <= 0 ? g_strdup_printf("%s|%s", t, h)
                                                       : g_strdup_printf("%s|%s", h, t);
                        guint n = GPOINTER_TO_UINT(g_hash_table_lookup(pair_total, pair_key));
                        g_hash_table_insert(pair_total, pair_key, GUINT_TO_POINTER(n + 1));
                    }
                }
                g_free(t);
                g_free(h);
                g_free(title);
            }
            g_match_info_free(tmi);
            g_free(kind);
            g_free(body);
            g_match_info_next(pmi, NULL);
        }
        g_match_info_free(pmi);
    }

    GString *out = g_string_new(NULL);
    GMatchInfo *mi = NULL;
    gsize pos = 0;
    gsize svg_len = strlen(svg);
    gboolean ok = TRUE;

    g_regex_match(group_re, svg, 0, &mi);
    while (g_match_info_matches(mi)) {
        gint start, end;
        g_match_info_fetch_pos(mi, 0, &start, &end);
        g_string_append_len(out, svg + pos, (gssize)(start - (gint)pos));

        gchar *group = g_match_info_fetch(mi, 0);
        gchar *kind = g_match_info_fetch(mi, 1);
        gchar *body = g_match_info_fetch(mi, 3);

        gchar *title = NULL;
        GMatchInfo *tmi = NULL;
        if (g_regex_match(title_re, body, 0, &tmi)) {
            title = g_match_info_fetch(tmi, 1);
        }
        g_match_info_free(tmi);

        if (title != NULL && g_str_equal(kind, "node")) {
            const SchematicSvgOffset *off = offsets ? g_hash_table_lookup(offsets, title) : NULL;
            if (is_moved(off)) {
                gchar *dxs = fmt(off->dx), *dys = fmt(off->dy);
                // Insert the transform into the opening <g ...> tag.
                const gchar *tag_end = strchr(group, '>');
                gchar *head_part = g_strndup(group, (gsize)(tag_end - group));
                g_string_append_printf(out, "%s transform=\"translate(%s %s)\"%s", head_part, dxs,
                                       dys, tag_end);
                g_free(head_part);
                g_free(dxs);
                g_free(dys);
            } else {
                g_string_append(out, group);
            }
        } else if (title != NULL && g_str_equal(kind, "edge")) {
            gchar *tail_id = NULL, *head_id = NULL;
            gboolean rewritten = FALSE;
            if (parse_edge_title(title, &tail_id, &head_id)) {
                const SchematicSvgNode *tn = g_hash_table_lookup(nodes, tail_id);
                const SchematicSvgNode *hn = g_hash_table_lookup(nodes, head_id);
                const SchematicSvgOffset *to = offsets ? g_hash_table_lookup(offsets, tail_id) : NULL;
                const SchematicSvgOffset *ho = offsets ? g_hash_table_lookup(offsets, head_id) : NULL;
                if (tn != NULL && hn != NULL && (is_moved(to) || is_moved(ho))) {
                    gchar *edge_key = strcmp(tail_id, head_id) <= 0
                                     ? g_strdup_printf("%s|%s", tail_id, head_id)
                                     : g_strdup_printf("%s|%s", head_id, tail_id);
                    guint total = GPOINTER_TO_UINT(g_hash_table_lookup(pair_total, edge_key));
                    guint idx = GPOINTER_TO_UINT(g_hash_table_lookup(pair_next, edge_key));
                    g_hash_table_insert(pair_next, g_strdup(edge_key), GUINT_TO_POINTER(idx + 1));
                    g_free(edge_key);

                    // Perpendicular to the pair's canonical (smaller id ->
                    // larger id) direction, shared by every edge of the pair.
                    const SchematicSvgNode *a = strcmp(tail_id, head_id) <= 0 ? tn : hn;
                    const SchematicSvgOffset *ao = strcmp(tail_id, head_id) <= 0 ? to : ho;
                    const SchematicSvgNode *b2 = strcmp(tail_id, head_id) <= 0 ? hn : tn;
                    const SchematicSvgOffset *bo = strcmp(tail_id, head_id) <= 0 ? ho : to;
                    gdouble cdx = (b2->cx + (bo ? bo->dx : 0)) - (a->cx + (ao ? ao->dx : 0));
                    gdouble cdy = (b2->cy + (bo ? bo->dy : 0)) - (a->cy + (ao ? ao->dy : 0));
                    gdouble clen = sqrt(cdx * cdx + cdy * cdy);
                    gdouble pnx = clen > 1e-6 ? -cdy / clen : 0.0;
                    gdouble pny = clen > 1e-6 ? cdx / clen : 0.0;
                    gdouble lateral = total > 1 ? ((gdouble)idx - ((gdouble)total - 1.0) / 2.0) *
                                                      PARALLEL_SPACING
                                                : 0.0;

                    gchar *rw = rewrite_edge(group, tn, to, hn, ho, pnx, pny, lateral);
                    g_string_append(out, rw);
                    g_free(rw);
                    rewritten = TRUE;
                }
            }
            if (!rewritten) {
                g_string_append(out, group);
            }
            g_free(tail_id);
            g_free(head_id);
        } else {
            g_string_append(out, group);
        }

        g_free(title);
        g_free(body);
        g_free(kind);
        g_free(group);
        pos = (gsize)end;
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_string_append_len(out, svg + pos, (gssize)(svg_len - pos));

    gdouble width = maxx - minx;
    gdouble height = maxy - miny;
    gchar *result = g_string_free(out, FALSE);

    // Root <svg> size in pixels + viewBox; root group translated so the
    // new top-left is the origin; background rect covering the new bounds.
    GRegex *svg_re = g_regex_new("<svg width=\"[^\"]*\" height=\"[^\"]*\"\\s*viewBox=\"[^\"]*\"",
                                 0, 0, NULL);
    gchar *ws = fmt(width * px_per_pt), *hs = fmt(height * px_per_pt);
    gchar *ww = fmt(width), *hh = fmt(height);
    gchar *svg_head = g_strdup_printf("<svg width=\"%spx\" height=\"%spx\" viewBox=\"0.00 0.00 %s %s\"",
                                      ws, hs, ww, hh);
    gchar *step1 = g_regex_replace_literal(svg_re, result, -1, 0, svg_head, 0, NULL);
    g_free(result);
    g_free(svg_head);
    g_free(ws); g_free(hs); g_free(ww); g_free(hh);
    g_regex_unref(svg_re);

    GRegex *root_re = g_regex_new("(<g id=\"graph0\" class=\"graph\" transform=\")[^\"]*(\")", 0, 0, NULL);
    gchar *mx = fmt(-minx), *my = fmt(-miny);
    gchar *root_tf = g_strdup_printf("\\1scale(1 1) rotate(0) translate(%s %s)\\2", mx, my);
    gchar *step2 = g_regex_replace(root_re, step1, -1, 0, root_tf, 0, NULL);
    g_free(step1);
    g_free(root_tf);
    g_free(mx); g_free(my);
    g_regex_unref(root_re);

    GRegex *bg_re = g_regex_new("(<g id=\"graph0\"[^>]*>\\s*(?:<title>[^<]*</title>\\s*)?<polygon fill=\"white\" stroke=\"none\" points=\")[^\"]*(\")",
                                0, 0, NULL);
    gchar *x0 = fmt(minx), *y0 = fmt(miny), *x1s = fmt(maxx), *y1s = fmt(maxy);
    gchar *bg_pts = g_strdup_printf("\\1%s,%s %s,%s %s,%s %s,%s %s,%s\\2", x0, y0, x0, y1s, x1s,
                                    y1s, x1s, y0, x0, y0);
    gchar *step3 = g_regex_replace(bg_re, step2, -1, 0, bg_pts, 0, NULL);
    g_free(step2);
    g_free(bg_pts);
    g_free(x0); g_free(y0); g_free(x1s); g_free(y1s);
    g_regex_unref(bg_re);

    g_regex_unref(group_re);
    g_regex_unref(title_re);
    g_hash_table_unref(pair_total);
    g_hash_table_unref(pair_next);

    if (!strstr(step3, "translate(") || !strstr(step3, "<svg width=")) {
        ok = FALSE;
    }
    if (!ok) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "The SVG isn't in the shape Graphviz normally writes");
        g_free(step3);
        return NULL;
    }

    if (out_bounds != NULL) {
        out_bounds->min_x = minx;
        out_bounds->min_y = miny;
        out_bounds->width = width;
        out_bounds->height = height;
    }
    return step3;
}

void schematic_svg_node_pixel_box(const SchematicSvgNode *node,
                                  const SchematicSvgOffset *offset,
                                  const SchematicSvgBounds *bounds,
                                  gdouble px_per_pt,
                                  gdouble *x,
                                  gdouble *y,
                                  gdouble *w,
                                  gdouble *h)
{
    gdouble dx = offset != NULL ? offset->dx : 0.0;
    gdouble dy = offset != NULL ? offset->dy : 0.0;
    *x = (node->cx + dx - node->w / 2.0 - bounds->min_x) * px_per_pt;
    *y = (node->cy + dy - node->h / 2.0 - bounds->min_y) * px_per_pt;
    *w = node->w * px_per_pt;
    *h = node->h * px_per_pt;
}
