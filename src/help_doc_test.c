#include <glib.h>
#include <string.h>

#include "help_doc.h"

static const gchar *help_path = NULL;
static const gchar *menu_path = NULL;

static const gchar *SAMPLE =
    "ignored preamble\n"
    "\n"
    "# Basics\n"
    "## Zooming In\n"
    "keywords: magnify, scale\n"
    "Zoom with the **Zoom** menu and the `F2` key.\n"
    "Second line of the same paragraph.\n"
    "\n"
    "### Steps\n"
    "1. Open the menu\n"
    "2. Pick a level\n"
    "   and confirm.\n"
    "- bullet one\n"
    "- bullet two\n"
    "\n"
    "See also: [[Markers Guide]], [[Zooming In]]\n"
    "\n"
    "## Markers Guide\n"
    "A marker pins a time. Markers can be named.\n"
    "\n"
    "# Advanced\n"
    "## Derived Things\n"
    "keywords: expression\n"
    "Computed from an expression.\n"
    "\n"
    "## Unrelated\n"
    "Nothing to see about zoom here at all... actually zoom appears once.\n";

static HelpDoc *parse_ok(const gchar *text)
{
    GError *err = NULL;
    HelpDoc *doc = help_doc_parse(text, &err);
    g_assert_no_error(err);
    g_assert_nonnull(doc);
    return doc;
}

/* ------------------------------------------------------------- parsing -- */

static void test_structure(void)
{
    HelpDoc *doc = parse_ok(SAMPLE);
    g_assert_cmpuint(help_doc_n_topics(doc), ==, 4);

    GPtrArray *cats = help_doc_categories(doc);
    g_assert_cmpuint(cats->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(cats, 0), ==, "Basics");
    g_assert_cmpstr(g_ptr_array_index(cats, 1), ==, "Advanced");
    g_ptr_array_unref(cats);

    const HelpTopic *t = help_doc_topic(doc, 0);
    g_assert_cmpstr(t->title, ==, "Zooming In");
    g_assert_cmpstr(t->category, ==, "Basics");
    g_assert_cmpuint(t->keywords->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(t->keywords, 0), ==, "magnify");
    g_assert_cmpstr(g_ptr_array_index(t->keywords, 1), ==, "scale");

    // paragraph (two lines joined), heading, 2 numbered (second continued),
    // 2 bullets, see-also.
    g_assert_cmpuint(t->blocks->len, ==, 7);
    const HelpBlock *b = g_ptr_array_index(t->blocks, 0);
    g_assert_cmpint(b->kind, ==, HELP_BLOCK_PARAGRAPH);
    g_assert_cmpstr(b->text, ==,
                    "Zoom with the **Zoom** menu and the `F2` key. Second line of the same "
                    "paragraph.");
    b = g_ptr_array_index(t->blocks, 1);
    g_assert_cmpint(b->kind, ==, HELP_BLOCK_HEADING);
    g_assert_cmpstr(b->text, ==, "Steps");
    b = g_ptr_array_index(t->blocks, 2);
    g_assert_cmpint(b->kind, ==, HELP_BLOCK_NUMBERED);
    g_assert_cmpuint(b->number, ==, 1);
    b = g_ptr_array_index(t->blocks, 3);
    g_assert_cmpuint(b->number, ==, 2);
    g_assert_cmpstr(b->text, ==, "Pick a level and confirm."); // continuation line joined
    b = g_ptr_array_index(t->blocks, 4);
    g_assert_cmpint(b->kind, ==, HELP_BLOCK_BULLET);
    g_assert_cmpstr(b->text, ==, "bullet one");
    b = g_ptr_array_index(t->blocks, 6);
    g_assert_cmpint(b->kind, ==, HELP_BLOCK_SEE_ALSO);
    g_assert_cmpstr(b->text, ==, "[[Markers Guide]], [[Zooming In]]");

    g_assert_nonnull(help_doc_find_topic(doc, "markers guide")); // case-insensitive
    g_assert_null(help_doc_find_topic(doc, "nope"));

    GPtrArray *links = help_topic_links(t);
    g_assert_cmpuint(links->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(links, 0), ==, "Markers Guide");
    g_ptr_array_unref(links);

    help_doc_free(doc);
}

static void expect_parse_error(const gchar *text, const gchar *needle)
{
    GError *err = NULL;
    HelpDoc *doc = help_doc_parse(text, &err);
    g_assert_null(doc);
    g_assert_error(err, HELP_DOC_ERROR, HELP_DOC_ERROR_SYNTAX);
    if (strstr(err->message, needle) == NULL) {
        g_test_message("error was: %s", err->message);
    }
    g_assert_nonnull(strstr(err->message, needle));
    g_clear_error(&err);
}

static void test_parse_errors(void)
{
    expect_parse_error("## Orphan\ntext\n", "before any category");
    expect_parse_error("# C\n## A\ntext\n## a\nmore\n", "duplicate"); // case-insensitive
    expect_parse_error("# C\n## Empty\n\n## Next\ntext\n", "no content");
}

static void test_inline_spans(void)
{
    GPtrArray *s = help_inline_parse("plain **bold** and `code` then [[Some Topic]] end");
    g_assert_cmpuint(s->len, ==, 7);
    HelpSpan *sp = g_ptr_array_index(s, 0);
    g_assert_cmpstr(sp->text, ==, "plain ");
    g_assert_false(sp->bold || sp->code);
    sp = g_ptr_array_index(s, 1);
    g_assert_cmpstr(sp->text, ==, "bold");
    g_assert_true(sp->bold);
    sp = g_ptr_array_index(s, 3);
    g_assert_cmpstr(sp->text, ==, "code");
    g_assert_true(sp->code);
    sp = g_ptr_array_index(s, 5);
    g_assert_cmpstr(sp->link, ==, "Some Topic");
    sp = g_ptr_array_index(s, 6);
    g_assert_cmpstr(sp->text, ==, " end");
    g_ptr_array_unref(s);

    // Unterminated markers are literal text, not swallowed.
    s = help_inline_parse("a **never closed and `neither and [[nor");
    g_assert_cmpuint(s->len, ==, 1);
    sp = g_ptr_array_index(s, 0);
    g_assert_cmpstr(sp->text, ==, "a **never closed and `neither and [[nor");
    g_ptr_array_unref(s);

    s = help_inline_parse("");
    g_assert_cmpuint(s->len, ==, 0);
    g_ptr_array_unref(s);
}

/* -------------------------------------------------------------- search -- */

static const HelpTopic *top_hit(HelpDoc *doc, const gchar *query)
{
    GPtrArray *hits = help_doc_search(doc, query);
    const HelpTopic *t = hits->len > 0 ? ((HelpHit *)g_ptr_array_index(hits, 0))->topic : NULL;
    g_ptr_array_unref(hits);
    return t;
}

static void test_search_basics(void)
{
    HelpDoc *doc = parse_ok(SAMPLE);

    // Title beats body: "zoom" is in "Zooming In"'s title/body and once in "Unrelated".
    const HelpTopic *t = top_hit(doc, "zoom");
    g_assert_nonnull(t);
    g_assert_cmpstr(t->title, ==, "Zooming In");

    GPtrArray *hits = help_doc_search(doc, "zoom");
    g_assert_cmpuint(hits->len, ==, 2); // Zooming In, Unrelated
    g_ptr_array_unref(hits);

    // A keyword finds a topic whose text never says the word.
    g_assert_cmpstr(top_hit(doc, "magnify")->title, ==, "Zooming In");
    g_assert_cmpstr(top_hit(doc, "expression")->title, ==, "Derived Things");

    // Prefix matching, any case.
    g_assert_cmpstr(top_hit(doc, "MARK")->title, ==, "Markers Guide");
    g_assert_cmpstr(top_hit(doc, "mag")->title, ==, "Zooming In");

    // AND: every word must match.
    hits = help_doc_search(doc, "marker named");
    g_assert_cmpuint(hits->len, ==, 1);
    g_ptr_array_unref(hits);
    hits = help_doc_search(doc, "marker nonexistentword");
    g_assert_cmpuint(hits->len, ==, 0);
    g_ptr_array_unref(hits);

    // Empty / punctuation-only queries match nothing rather than everything.
    hits = help_doc_search(doc, "");
    g_assert_cmpuint(hits->len, ==, 0);
    g_ptr_array_unref(hits);
    hits = help_doc_search(doc, "   +-  ");
    g_assert_cmpuint(hits->len, ==, 0);
    g_ptr_array_unref(hits);

    // Category names are searchable too.
    hits = help_doc_search(doc, "advanced");
    g_assert_cmpuint(hits->len, >=, 1);
    g_ptr_array_unref(hits);

    help_doc_free(doc);
}

static void test_search_ranking_and_snippets(void)
{
    HelpDoc *doc = parse_ok(SAMPLE);

    GPtrArray *hits = help_doc_search(doc, "zoom");
    const HelpHit *first = g_ptr_array_index(hits, 0);
    const HelpHit *second = g_ptr_array_index(hits, 1);
    g_assert_cmpfloat(first->score, >, second->score);
    // The snippet is an excerpt of the text around the match.
    g_assert_nonnull(strstr(first->snippet, "Zoom"));
    g_assert_nonnull(strstr(second->snippet, "zoom"));
    g_ptr_array_unref(hits);

    // The whole query as a phrase in a title outranks scattered matches.
    HelpDoc *d2 = parse_ok("# C\n## Alpha Beta\ntext about gamma.\n\n## Other\nalpha and beta are "
                           "both mentioned here many times: alpha beta alpha beta.\n");
    g_assert_cmpstr(top_hit(d2, "alpha beta")->title, ==, "Alpha Beta");
    help_doc_free(d2);

    // A topic matched only by its title still gets a usable snippet.
    hits = help_doc_search(doc, "derived");
    g_assert_cmpuint(hits->len, ==, 1);
    g_assert_cmpstr(((HelpHit *)g_ptr_array_index(hits, 0))->snippet, ==,
                    "Computed from an expression.");
    g_ptr_array_unref(hits);

    // A long block is excerpted with ellipses, not dumped whole.
    GString *long_text = g_string_new("# C\n## Long\n");
    for (guint i = 0; i < 60; i++) {
        g_string_append(long_text, "filler ");
    }
    g_string_append(long_text, "NEEDLE ");
    for (guint i = 0; i < 60; i++) {
        g_string_append(long_text, "filler ");
    }
    g_string_append(long_text, "\n");
    HelpDoc *d3 = parse_ok(long_text->str);
    hits = help_doc_search(d3, "needle");
    const gchar *snip = ((HelpHit *)g_ptr_array_index(hits, 0))->snippet;
    g_assert_nonnull(strstr(snip, "NEEDLE"));
    g_assert_cmpuint(strlen(snip), <, 250);
    g_assert_true(g_str_has_prefix(snip, "\xe2\x80\xa6"));
    g_assert_true(g_str_has_suffix(snip, "\xe2\x80\xa6"));
    g_ptr_array_unref(hits);
    help_doc_free(d3);
    g_string_free(long_text, TRUE);

    help_doc_free(doc);
}

static void test_snippets_have_no_markup(void)
{
    HelpDoc *doc = parse_ok("# C\n## Topic\nThe **Dissolve** action and `code` see [[Other]] now.\n\n"
                            "## Other\ntext.\n");
    GPtrArray *hits = help_doc_search(doc, "dissolve");
    g_assert_cmpuint(hits->len, ==, 1);
    const gchar *snip = ((HelpHit *)g_ptr_array_index(hits, 0))->snippet;
    g_assert_cmpstr(snip, ==, "The Dissolve action and code see Other now.");
    g_ptr_array_unref(hits);

    // Also when the match is only in the title, so the snippet is the lead-in.
    hits = help_doc_search(doc, "topic");
    snip = ((HelpHit *)g_ptr_array_index(hits, 0))->snippet;
    g_assert_null(strstr(snip, "**"));
    g_assert_null(strstr(snip, "[["));
    g_ptr_array_unref(hits);
    help_doc_free(doc);
}

static void test_single_character_terms(void)
{
    // "x" must not prefix-match every word starting with x (or anything):
    // a lone character only matches itself as a whole word.
    HelpDoc *doc = parse_ok("# C\n## List X\nfinds x values.\n\n## Xylophone\nmusic.\n");
    GPtrArray *hits = help_doc_search(doc, "x");
    g_assert_cmpuint(hits->len, ==, 1);
    g_assert_cmpstr(((HelpHit *)g_ptr_array_index(hits, 0))->topic->title, ==, "List X");
    g_ptr_array_unref(hits);
    help_doc_free(doc);
}

static void test_tokenizing(void)
{
    gchar **t = help_query_terms("Shift+F1, Move-To  Time!");
    g_assert_cmpuint(g_strv_length(t), ==, 5);
    g_assert_cmpstr(t[0], ==, "shift");
    g_assert_cmpstr(t[1], ==, "f1");
    g_assert_cmpstr(t[4], ==, "time");
    g_strfreev(t);

    HelpDoc *doc = parse_ok("# C\n## Keys\nPress Shift+F1 for help.\n");
    GPtrArray *hits = help_doc_search(doc, "shift+f1");
    g_assert_cmpuint(hits->len, ==, 1);
    g_ptr_array_unref(hits);
    help_doc_free(doc);
}

/* ---------------------------------------------- the real help document -- */

static HelpDoc *load_real(void)
{
    gchar *text = NULL;
    GError *err = NULL;
    g_assert_nonnull(help_path);
    g_assert_true(g_file_get_contents(help_path, &text, NULL, &err));
    HelpDoc *doc = help_doc_parse(text, &err);
    if (doc == NULL) {
        g_test_message("features.md: %s", err->message);
    }
    g_assert_nonnull(doc);
    g_free(text);
    return doc;
}

static void test_real_document_is_well_formed(void)
{
    if (help_path == NULL) {
        g_test_skip("no help file given");
        return;
    }
    HelpDoc *doc = load_real();
    g_assert_cmpuint(help_doc_n_topics(doc), >=, 30);

    GPtrArray *cats = help_doc_categories(doc);
    g_assert_cmpuint(cats->len, >=, 5);
    for (guint c = 0; c < cats->len; c++) {
        // Every category has at least one topic.
        guint n = 0;
        for (guint i = 0; i < help_doc_n_topics(doc); i++) {
            if (g_str_equal(help_doc_topic(doc, i)->category, g_ptr_array_index(cats, c))) {
                n++;
            }
        }
        if (n == 0) {
            g_test_message("empty category: %s", (const gchar *)g_ptr_array_index(cats, c));
        }
        g_assert_cmpuint(n, >, 0);
    }
    g_ptr_array_unref(cats);

    for (guint i = 0; i < help_doc_n_topics(doc); i++) {
        const HelpTopic *t = help_doc_topic(doc, i);
        if (t->keywords->len < 3) {
            g_test_message("topic '%s' needs at least 3 keywords to be findable", t->title);
        }
        g_assert_cmpuint(t->keywords->len, >=, 3);
        g_assert_cmpuint(t->blocks->len, >=, 1);
    }
    help_doc_free(doc);
}

static void test_real_document_links_resolve(void)
{
    if (help_path == NULL) {
        g_test_skip("no help file given");
        return;
    }
    HelpDoc *doc = load_real();
    guint total = 0;
    for (guint i = 0; i < help_doc_n_topics(doc); i++) {
        const HelpTopic *t = help_doc_topic(doc, i);
        GPtrArray *links = help_topic_links(t);
        for (guint k = 0; k < links->len; k++) {
            total++;
            const gchar *target = g_ptr_array_index(links, k);
            if (help_doc_find_topic(doc, target) == NULL) {
                g_test_message("'%s' links to missing topic '%s'", t->title, target);
            }
            g_assert_nonnull(help_doc_find_topic(doc, target));
        }
        g_ptr_array_unref(links);
    }
    g_assert_cmpuint(total, >, 40);
    help_doc_free(doc);
}

static void test_real_document_features_are_findable(void)
{
    if (help_path == NULL) {
        g_test_skip("no help file given");
        return;
    }
    HelpDoc *doc = load_real();

    // Every feature a user might look up by name finds its own topic first.
    static const struct
    {
        const gchar *query;
        const gchar *title;
    } expect[] = {
        {"dissolve", "Dissolve, Filter Out and Collapse"},
        {"filter out", "Dissolve, Filter Out and Collapse"},
        {"derived signal", "Derived Signals"},
        {"verilog operators precedence", "Derived Signal Expression Language"},
        {"auto update", "Auto Update"},
        {"move nodes", "Moving Schematic Nodes"},
        {"reset layout", "Moving Schematic Nodes"},
        {"event sequence", "Event Sequence"},
        {"delta cycle", "Event Sequence"},
        {"list x", "List X"},
        {"compress time range", "Compress Time Range"},
        {"group manager", "Groups and Group Manager"},
        {"undo", "Undo and Redo"},
        {"time-span", "Time-Span Comments"},
        {"drivers", "Show Drivers and Show Loads"},
        {"cone of logic", "Gate-Level Schematic"},
        {"boundary port", "Expanding the Cone Net by Net"},
        {"shrink expansion", "Expanding the Cone Net by Net"},
        {"rtl schematic", "RTL Dataflow Schematic"},
        {"mcp", "WCP and the MCP Server"},
        {"analog", "Analog Waveforms"},
        {"pattern search", "Pattern Search"},
        {"event report", "Signal Event Report"},
        {"schematic_color_x", "Configuration Options"},
        {"keyboard shortcuts", "Keyboard Shortcuts"},
        {"slang-netlist", "Requirements and Optional Tools"},
        {"top module", "Configuring HDL Sources"},
        {"quick format", "Signal Popover"},
        {"save file", "Save Files and Sessions"},
        {"source panel", "Source Panel"},
        {"sub-bus", "Bus Operations"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(expect); i++) {
        const HelpTopic *t = top_hit(doc, expect[i].query);
        if (t == NULL || !g_str_equal(t->title, expect[i].title)) {
            g_test_message("search '%s' -> %s (wanted %s)", expect[i].query,
                           t ? t->title : "(nothing)", expect[i].title);
        }
        g_assert_nonnull(t);
        g_assert_cmpstr(t->title, ==, expect[i].title);
    }
    help_doc_free(doc);
}

/* -------------------- the document vs. the program it describes -- */

static gint cmp_string_ptr(gconstpointer a, gconstpointer b)
{
    return strcmp(*(gchar *const *)a, *(gchar *const *)b);
}

// Maps "<Control>N" style accelerators to a canonical, order-independent
// form ("CTRL+N") so they can be compared with the doc's "Ctrl+N".
static gchar *canonical_accel(const gchar *text, gboolean from_menu_source)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    GString *key = g_string_new(NULL);
    if (from_menu_source) {
        const gchar *p = text;
        while (*p == '<') {
            const gchar *end = strchr(p, '>');
            gchar *raw = g_strndup(p + 1, (gsize)(end - p - 1));
            gchar *mod = g_ascii_strup(raw, -1);
            g_free(raw);
            if (g_str_equal(mod, "CONTROL")) {
                g_free(mod);
                mod = g_strdup("CTRL");
            }
            g_ptr_array_add(parts, mod);
            p = end + 1;
        }
        g_string_append(key, p);
    } else {
        gchar **toks = g_strsplit(text, "+", -1);
        guint n = g_strv_length(toks);
        for (guint i = 0; i + 1 < n; i++) {
            g_ptr_array_add(parts, g_ascii_strup(toks[i], -1));
        }
        g_string_append(key, toks[n - 1]);
        g_strfreev(toks);
    }
    g_ptr_array_sort(parts, cmp_string_ptr);
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < parts->len; i++) {
        g_string_append(out, g_ptr_array_index(parts, i));
        g_string_append_c(out, '+');
    }
    gchar *k = g_ascii_strup(key->str, -1);
    g_string_append(out, k);
    g_free(k);
    g_string_free(key, TRUE);
    g_ptr_array_unref(parts);
    return g_string_free(out, FALSE);
}

typedef struct
{
    GHashTable *paths; // every "/A/B/C" menu literal
    GHashTable *accels; // path -> canonical accelerator
} MenuTable;

static MenuTable load_menu(void)
{
    gchar *src = NULL;
    g_assert_true(g_file_get_contents(menu_path, &src, NULL, NULL));
    MenuTable m = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free),
    };

    GRegex *lit = g_regex_new("\"(/[A-Za-z][^\"]*)\"", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match(lit, src, 0, &mi);
    while (g_match_info_matches(mi)) {
        g_hash_table_add(m.paths, g_match_info_fetch(mi, 1));
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(lit);

    GRegex *item = g_regex_new("WAVE_GTKIFE\\(\\s*\"(/[^\"]+)\",\\s*\"([^\"]+)\"", 0, 0, NULL);
    g_regex_match(item, src, 0, &mi);
    while (g_match_info_matches(mi)) {
        gchar *path = g_match_info_fetch(mi, 1);
        gchar *accel = g_match_info_fetch(mi, 2);
        if (!g_hash_table_contains(m.accels, path)) { // the first binding wins
            g_hash_table_insert(m.accels, path, canonical_accel(accel, TRUE));
        } else {
            g_free(path);
        }
        g_free(accel);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(item);
    g_free(src);
    return m;
}

static gboolean menu_has(const MenuTable *m, const gchar *doc_path)
{
    // "File > Auto Update" -> "/File/Auto Update"; a submenu header is a
    // prefix of its items' paths.
    gchar **parts = g_strsplit(doc_path, " > ", -1);
    gchar *joined = g_strjoinv("/", parts);
    gchar *full = g_strconcat("/", joined, NULL);
    gchar *prefix = g_strconcat(full, "/", NULL);
    g_strfreev(parts);
    g_free(joined);

    gboolean found = g_hash_table_contains(m->paths, full);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, m->paths);
    while (!found && g_hash_table_iter_next(&it, &key, NULL)) {
        found = g_str_has_prefix(key, prefix);
    }
    g_free(full);
    g_free(prefix);
    return found;
}

static void test_real_document_matches_the_menus(void)
{
    if (help_path == NULL || menu_path == NULL) {
        g_test_skip("help or menu source not given");
        return;
    }
    gchar *doc_text = NULL;
    g_assert_true(g_file_get_contents(help_path, &doc_text, NULL, NULL));
    MenuTable menu = load_menu();

    // Every **Menu > Item** the guide names must exist in the program.
    GRegex *re = g_regex_new("\\*\\*([A-Za-z][^*]* > [^*]+)\\*\\*", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    guint checked = 0;
    g_regex_match(re, doc_text, 0, &mi);
    while (g_match_info_matches(mi)) {
        gchar *path = g_match_info_fetch(mi, 1);
        checked++;
        if (!menu_has(&menu, path)) {
            g_test_message("help names a menu item that doesn't exist: %s", path);
        }
        g_assert_true(menu_has(&menu, path));
        g_free(path);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    g_assert_cmpuint(checked, >, 40);

    // Every "**Menu > Item** (Key)" shortcut must be the real binding.
    GRegex *with_key =
        g_regex_new("\\*\\*([A-Za-z][^*]* > [^*]+)\\*\\* "
                    "\\(((?:(?:Ctrl|Alt|Shift)\\+)*(?:F[0-9]{1,2}|[A-Z]))\\)",
                    0, 0, NULL);
    guint keys = 0;
    g_regex_match(with_key, doc_text, 0, &mi);
    while (g_match_info_matches(mi)) {
        gchar *path = g_match_info_fetch(mi, 1);
        gchar *shortcut = g_match_info_fetch(mi, 2);
        gchar **parts = g_strsplit(path, " > ", -1);
        gchar *joined = g_strjoinv("/", parts);
        gchar *menu_key = g_strconcat("/", joined, NULL);
        const gchar *actual = g_hash_table_lookup(menu.accels, menu_key);
        gchar *claimed = canonical_accel(shortcut, FALSE);
        if (actual == NULL || !g_str_equal(actual, claimed)) {
            g_test_message("%s: help says (%s), program binds %s", path, shortcut,
                           actual ? actual : "(nothing)");
        }
        g_assert_nonnull(actual);
        g_assert_cmpstr(actual, ==, claimed);
        keys++;
        g_free(claimed);
        g_free(menu_key);
        g_free(joined);
        g_strfreev(parts);
        g_free(path);
        g_free(shortcut);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(with_key);
    g_assert_cmpuint(keys, >, 10);

    g_hash_table_unref(menu.paths);
    g_hash_table_unref(menu.accels);
    g_free(doc_text);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (argc > 1) {
        help_path = argv[1];
    }
    if (argc > 2) {
        menu_path = argv[2];
    }
    g_test_add_func("/help/structure", test_structure);
    g_test_add_func("/help/parse-errors", test_parse_errors);
    g_test_add_func("/help/inline-spans", test_inline_spans);
    g_test_add_func("/help/search-basics", test_search_basics);
    g_test_add_func("/help/search-ranking-and-snippets", test_search_ranking_and_snippets);
    g_test_add_func("/help/snippets-no-markup", test_snippets_have_no_markup);
    g_test_add_func("/help/single-character-terms", test_single_character_terms);
    g_test_add_func("/help/tokenizing", test_tokenizing);
    g_test_add_func("/help/real-doc-well-formed", test_real_document_is_well_formed);
    g_test_add_func("/help/real-doc-links-resolve", test_real_document_links_resolve);
    g_test_add_func("/help/real-doc-features-findable", test_real_document_features_are_findable);
    g_test_add_func("/help/real-doc-matches-menus", test_real_document_matches_the_menus);
    return g_test_run();
}
