#include "help_doc.h"

#include <ctype.h>
#include <string.h>

G_DEFINE_QUARK(help-doc-error-quark, help_doc_error)

// How much a match is worth, by where the word appears.
#define WEIGHT_TITLE 10.0
#define WEIGHT_KEYWORD 6.0
#define WEIGHT_HEADING 4.0
#define WEIGHT_BODY 1.0

typedef struct
{
    guint topic;
    gdouble weight;
} Posting;

struct _HelpDoc
{
    GPtrArray *topics; // HelpTopic*
    GPtrArray *categories; // gchar*
    GHashTable *by_title; // lowercase title -> HelpTopic*
    GHashTable *postings; // word -> GArray of Posting
    GPtrArray *words; // sorted unique words (gchar*, borrowed), for prefix lookup
};

/* ------------------------------------------------------------- freeing -- */

static void block_free(gpointer data)
{
    HelpBlock *b = data;
    g_free(b->text);
    g_free(b);
}

static void topic_free(gpointer data)
{
    HelpTopic *t = data;
    g_free(t->title);
    g_free(t->category);
    g_ptr_array_unref(t->keywords);
    g_ptr_array_unref(t->blocks);
    g_free(t);
}

void help_doc_free(HelpDoc *doc)
{
    if (doc == NULL) {
        return;
    }
    g_ptr_array_unref(doc->topics);
    g_ptr_array_unref(doc->categories);
    g_hash_table_unref(doc->by_title);
    g_hash_table_unref(doc->postings);
    g_ptr_array_unref(doc->words);
    g_free(doc);
}

/* ------------------------------------------------------------ tokenizing -- */

// Lowercase alphanumeric runs. Anything else separates words, so "shift+f1"
// is the words "shift" and "f1".
static GPtrArray *tokenize(const gchar *text)
{
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    GString *cur = g_string_new(NULL);
    for (const gchar *p = text;; p++) {
        if (*p != '\0' && (isalnum((guchar)*p) || (guchar)*p >= 0x80)) {
            g_string_append_c(cur, (gchar)g_ascii_tolower(*p));
        } else {
            if (cur->len > 0) {
                g_ptr_array_add(words, g_strdup(cur->str));
                g_string_truncate(cur, 0);
            }
            if (*p == '\0') {
                break;
            }
        }
    }
    g_string_free(cur, TRUE);
    return words;
}

gchar **help_query_terms(const gchar *query)
{
    GPtrArray *words = tokenize(query != NULL ? query : "");
    g_ptr_array_add(words, NULL);
    return (gchar **)g_ptr_array_free(words, FALSE);
}

/* -------------------------------------------------------------- indexing -- */

static void postings_free(gpointer data)
{
    g_array_unref(data);
}

static void add_posting(HelpDoc *doc, const gchar *word, guint topic, gdouble weight)
{
    GArray *list = g_hash_table_lookup(doc->postings, word);
    if (list == NULL) {
        list = g_array_new(FALSE, FALSE, sizeof(Posting));
        g_hash_table_insert(doc->postings, g_strdup(word), list);
    }
    // One entry per (word, topic): the best weight seen, plus a small credit
    // for each further occurrence so a word used a lot ranks a little higher.
    for (guint i = 0; i < list->len; i++) {
        Posting *p = &g_array_index(list, Posting, i);
        if (p->topic == topic) {
            p->weight = MAX(p->weight, weight) + 0.1;
            return;
        }
    }
    Posting p = {topic, weight};
    g_array_append_val(list, p);
}

static void index_text(HelpDoc *doc, guint topic, const gchar *text, gdouble weight)
{
    GPtrArray *words = tokenize(text);
    for (guint i = 0; i < words->len; i++) {
        add_posting(doc, g_ptr_array_index(words, i), topic, weight);
    }
    g_ptr_array_unref(words);
}

static gint cmp_str_ptr(gconstpointer a, gconstpointer b)
{
    return strcmp(*(gchar *const *)a, *(gchar *const *)b);
}

static void build_index(HelpDoc *doc)
{
    for (guint i = 0; i < doc->topics->len; i++) {
        const HelpTopic *t = g_ptr_array_index(doc->topics, i);
        index_text(doc, i, t->title, WEIGHT_TITLE);
        index_text(doc, i, t->category, WEIGHT_HEADING);
        for (guint k = 0; k < t->keywords->len; k++) {
            index_text(doc, i, g_ptr_array_index(t->keywords, k), WEIGHT_KEYWORD);
        }
        for (guint b = 0; b < t->blocks->len; b++) {
            const HelpBlock *blk = g_ptr_array_index(t->blocks, b);
            index_text(doc, i, blk->text,
                       blk->kind == HELP_BLOCK_HEADING ? WEIGHT_HEADING : WEIGHT_BODY);
        }
    }

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, doc->postings);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_ptr_array_add(doc->words, key);
    }
    g_ptr_array_sort(doc->words, cmp_str_ptr);
}

/* --------------------------------------------------------------- parsing -- */

static gboolean parse_numbered_prefix(const gchar *line, guint *number, const gchar **rest)
{
    const gchar *p = line;
    if (!isdigit((guchar)*p)) {
        return FALSE;
    }
    guint n = 0;
    while (isdigit((guchar)*p)) {
        n = n * 10 + (guint)(*p - '0');
        p++;
    }
    if (p[0] != '.' || p[1] != ' ') {
        return FALSE;
    }
    *number = n;
    *rest = p + 2;
    return TRUE;
}

static void add_block(HelpTopic *t, HelpBlockKind kind, const gchar *text, guint number)
{
    HelpBlock *b = g_new0(HelpBlock, 1);
    b->kind = kind;
    b->text = g_strdup(text);
    b->number = number;
    g_ptr_array_add(t->blocks, b);
}

// Emits the pending paragraph (if any) as a block of `topic`.
static void flush_paragraph(HelpTopic *topic, GString *para)
{
    if (para->len > 0 && topic != NULL) {
        if (g_str_has_prefix(para->str, "See also: ")) {
            add_block(topic, HELP_BLOCK_SEE_ALSO, para->str + strlen("See also: "), 0);
        } else {
            add_block(topic, HELP_BLOCK_PARAGRAPH, para->str, 0);
        }
    }
    g_string_truncate(para, 0);
}

static HelpBlock *last_block(const HelpTopic *topic)
{
    return topic->blocks->len > 0 ? g_ptr_array_index(topic->blocks, topic->blocks->len - 1)
                                  : NULL;
}

HelpDoc *help_doc_parse(const gchar *text, GError **error)
{
    HelpDoc *doc = g_new0(HelpDoc, 1);
    doc->topics = g_ptr_array_new_with_free_func(topic_free);
    doc->categories = g_ptr_array_new_with_free_func(g_free);
    doc->by_title = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    doc->postings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, postings_free);
    doc->words = g_ptr_array_new();

    gchar *category = NULL;
    HelpTopic *topic = NULL;
    GString *para = g_string_new(NULL);
    gboolean ok = TRUE;
    gboolean topic_started = FALSE; // anything after the title yet? (keywords: must be first)

    gchar **lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; ok && lines[i] != NULL; i++) {
        guint lineno = i + 1;
        const gchar *line = lines[i];
        gchar *trimmed = g_strstrip(g_strdup(line));
        gboolean blank = trimmed[0] == '\0';
        gboolean indented = !blank && (line[0] == ' ' || line[0] == '\t');

        guint number = 0;
        const gchar *numbered_rest = NULL;
        gboolean is_category = g_str_has_prefix(line, "# ");
        gboolean is_topic = g_str_has_prefix(line, "## ");
        gboolean is_heading = g_str_has_prefix(line, "### ");
        gboolean is_bullet = !indented && g_str_has_prefix(trimmed, "- ");
        gboolean is_numbered_item = !indented && parse_numbered_prefix(trimmed, &number, &numbered_rest);
        gboolean is_see_also = !indented && g_str_has_prefix(trimmed, "See also:");

        // Only a plain line keeps a paragraph going.
        gboolean plain_text = !blank && !is_category && !is_topic && !is_heading && !is_bullet &&
                              !is_numbered_item && !is_see_also;
        HelpBlock *prev = topic != NULL ? last_block(topic) : NULL;
        gboolean continues_item = indented && para->len == 0 && prev != NULL &&
                                  (prev->kind == HELP_BLOCK_BULLET ||
                                   prev->kind == HELP_BLOCK_NUMBERED);
        if (!plain_text || continues_item) {
            if (!continues_item) {
                flush_paragraph(topic, para);
            }
        }

        if (is_category) {
            g_free(category);
            category = g_strstrip(g_strdup(line + 2));
            g_ptr_array_add(doc->categories, g_strdup(category));
            topic = NULL;
        } else if (is_topic) {
            gchar *title = g_strstrip(g_strdup(line + 3));
            gchar *key = g_ascii_strdown(title, -1);
            if (category == NULL) {
                g_set_error(error, HELP_DOC_ERROR, HELP_DOC_ERROR_SYNTAX,
                            "line %u: topic \"%s\" appears before any category", lineno, title);
                ok = FALSE;
            } else if (g_hash_table_contains(doc->by_title, key)) {
                g_set_error(error, HELP_DOC_ERROR, HELP_DOC_ERROR_SYNTAX,
                            "line %u: duplicate topic title \"%s\"", lineno, title);
                ok = FALSE;
            }
            if (ok) {
                topic = g_new0(HelpTopic, 1);
                topic->title = title;
                topic->category = g_strdup(category);
                topic->keywords = g_ptr_array_new_with_free_func(g_free);
                topic->blocks = g_ptr_array_new_with_free_func(block_free);
                topic->index = doc->topics->len;
                g_ptr_array_add(doc->topics, topic);
                g_hash_table_insert(doc->by_title, key, topic);
                topic_started = FALSE;
            } else {
                g_free(title);
                g_free(key);
            }
        } else if (topic == NULL || blank) {
            // Text outside any topic is ignored; blank lines only end paragraphs.
        } else if (!topic_started && g_str_has_prefix(trimmed, "keywords:")) {
            gchar **parts = g_strsplit(trimmed + strlen("keywords:"), ",", -1);
            for (guint k = 0; parts[k] != NULL; k++) {
                g_strstrip(parts[k]);
                if (parts[k][0] != '\0') {
                    g_ptr_array_add(topic->keywords, g_strdup(parts[k]));
                }
            }
            g_strfreev(parts);
            topic_started = TRUE;
        } else {
            topic_started = TRUE;
            if (is_heading) {
                gchar *heading = g_strstrip(g_strdup(line + 4));
                add_block(topic, HELP_BLOCK_HEADING, heading, 0);
                g_free(heading);
            } else if (is_bullet) {
                add_block(topic, HELP_BLOCK_BULLET, trimmed + 2, 0);
            } else if (is_numbered_item) {
                add_block(topic, HELP_BLOCK_NUMBERED, numbered_rest, number);
            } else if (continues_item) {
                HelpBlock *item = last_block(topic);
                gchar *joined = g_strconcat(item->text, " ", trimmed, NULL);
                g_free(item->text);
                item->text = joined;
            } else {
                // A plain line, or the start of a See-also paragraph.
                if (para->len > 0) {
                    g_string_append_c(para, ' ');
                }
                g_string_append(para, trimmed);
            }
        }
        g_free(trimmed);
    }
    flush_paragraph(topic, para);

    if (ok) {
        for (guint i = 0; i < doc->topics->len; i++) {
            const HelpTopic *t = g_ptr_array_index(doc->topics, i);
            if (t->blocks->len == 0) {
                g_set_error(error, HELP_DOC_ERROR, HELP_DOC_ERROR_SYNTAX,
                            "topic \"%s\" has no content", t->title);
                ok = FALSE;
                break;
            }
        }
    }

    g_strfreev(lines);
    g_string_free(para, TRUE);
    g_free(category);

    if (!ok) {
        help_doc_free(doc);
        return NULL;
    }
    build_index(doc);
    return doc;
}

/* -------------------------------------------------------------- accessors -- */

guint help_doc_n_topics(const HelpDoc *doc)
{
    return doc->topics->len;
}

const HelpTopic *help_doc_topic(const HelpDoc *doc, guint index)
{
    return index < doc->topics->len ? g_ptr_array_index(doc->topics, index) : NULL;
}

GPtrArray *help_doc_categories(const HelpDoc *doc)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < doc->categories->len; i++) {
        g_ptr_array_add(out, g_ptr_array_index(doc->categories, i));
    }
    return out;
}

const HelpTopic *help_doc_find_topic(const HelpDoc *doc, const gchar *title)
{
    gchar *key = g_ascii_strdown(title, -1);
    const HelpTopic *t = g_hash_table_lookup(doc->by_title, key);
    g_free(key);
    return t;
}

/* ----------------------------------------------------------------- search -- */

// First index in the sorted word list whose word is >= `prefix`.
static guint lower_bound(const HelpDoc *doc, const gchar *prefix)
{
    guint lo = 0, hi = doc->words->len;
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        if (strcmp(g_ptr_array_index(doc->words, mid), prefix) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// A block's text with the inline markup (**bold**, `code`, [[links]])
// removed, for showing in search results.
static gchar *strip_markup(const gchar *text)
{
    GString *out = g_string_new(NULL);
    for (const gchar *p = text; *p != '\0';) {
        if ((p[0] == '*' && p[1] == '*') || (p[0] == '[' && p[1] == '[') ||
            (p[0] == ']' && p[1] == ']')) {
            p += 2;
        } else if (*p == '`') {
            p++;
        } else {
            g_string_append_c(out, *p);
            p++;
        }
    }
    return g_string_free(out, FALSE);
}

static gchar *make_snippet(const HelpTopic *t, gchar *const *terms)
{
    // Prefer the first body block containing one of the terms.
    for (guint b = 0; b < t->blocks->len; b++) {
        const HelpBlock *blk = g_ptr_array_index(t->blocks, b);
        gchar *plain = strip_markup(blk->text);
        gchar *lower = g_ascii_strdown(plain, -1);
        for (guint k = 0; terms[k] != NULL; k++) {
            const gchar *hit = strstr(lower, terms[k]);
            if (hit != NULL) {
                gsize pos = (gsize)(hit - lower);
                gsize start = pos > 50 ? pos - 50 : 0;
                // Don't begin mid-word.
                while (start > 0 && start < pos && !isspace((guchar)plain[start - 1])) {
                    start++;
                }
                gsize len = strlen(plain);
                gsize end = MIN(len, pos + 90);
                gchar *excerpt = g_strndup(plain + start, end - start);
                gchar *out = g_strconcat(start > 0 ? "\xe2\x80\xa6" : "", excerpt,
                                         end < len ? "\xe2\x80\xa6" : "", NULL);
                g_free(excerpt);
                g_free(lower);
                g_free(plain);
                return out;
            }
        }
        g_free(lower);
        g_free(plain);
    }
    // Matched only in the title/keywords: start of the first paragraph.
    for (guint b = 0; b < t->blocks->len; b++) {
        const HelpBlock *blk = g_ptr_array_index(t->blocks, b);
        if (blk->kind == HELP_BLOCK_PARAGRAPH) {
            gchar *plain = strip_markup(blk->text);
            gsize len = strlen(plain);
            gchar *excerpt = g_strndup(plain, MIN(len, 120));
            gchar *out = g_strconcat(excerpt, len > 120 ? "\xe2\x80\xa6" : "", NULL);
            g_free(excerpt);
            g_free(plain);
            return out;
        }
    }
    return g_strdup("");
}

static gint cmp_hits(gconstpointer a, gconstpointer b)
{
    const HelpHit *x = *(HelpHit *const *)a;
    const HelpHit *y = *(HelpHit *const *)b;
    if (x->score != y->score) {
        return x->score > y->score ? -1 : 1;
    }
    return (gint)x->topic->index - (gint)y->topic->index;
}

static void hit_free(gpointer data)
{
    HelpHit *h = data;
    g_free(h->snippet);
    g_free(h);
}

GPtrArray *help_doc_search(const HelpDoc *doc, const gchar *query)
{
    GPtrArray *hits = g_ptr_array_new_with_free_func(hit_free);
    gchar **terms = help_query_terms(query);
    guint n_terms = g_strv_length(terms);
    if (n_terms == 0) {
        g_strfreev(terms);
        return hits;
    }

    guint n_topics = doc->topics->len;
    gdouble *score = g_new0(gdouble, n_topics);
    guint *matched_terms = g_new0(guint, n_topics);

    for (guint t = 0; t < n_terms; t++) {
        const gchar *term = terms[t];
        gsize len = strlen(term);
        // Best weight per topic for THIS term, across every word it matches.
        gdouble *best = g_new0(gdouble, n_topics);

        if (len < 2) {
            // A lone character is too common to prefix-match: exact only.
            GArray *list = g_hash_table_lookup(doc->postings, term);
            for (guint i = 0; list != NULL && i < list->len; i++) {
                const Posting *p = &g_array_index(list, Posting, i);
                best[p->topic] = MAX(best[p->topic], p->weight * 1.5);
            }
        } else {
            for (guint w = lower_bound(doc, term); w < doc->words->len; w++) {
                const gchar *word = g_ptr_array_index(doc->words, w);
                if (strncmp(word, term, len) != 0) {
                    break;
                }
                // An exact word beats a mere prefix; a plural counts as exact
                // ("signal" is a full match for "Signals").
                gsize wl = strlen(word);
                gdouble factor = (wl == len || (wl == len + 1 && word[len] == 's')) ? 1.5 : 1.0;
                GArray *list = g_hash_table_lookup(doc->postings, word);
                for (guint i = 0; i < list->len; i++) {
                    const Posting *p = &g_array_index(list, Posting, i);
                    best[p->topic] = MAX(best[p->topic], p->weight * factor);
                }
            }
        }

        for (guint i = 0; i < n_topics; i++) {
            if (best[i] > 0) {
                score[i] += best[i];
                matched_terms[i]++;
            }
        }
        g_free(best);
    }

    // The whole query as a phrase in a title is the strongest signal.
    gchar *lower_query = g_ascii_strdown(query, -1);
    g_strstrip(lower_query);

    for (guint i = 0; i < n_topics; i++) {
        if (matched_terms[i] != n_terms) {
            continue; // AND: every word must match
        }
        const HelpTopic *topic = g_ptr_array_index(doc->topics, i);
        gchar *lower_title = g_ascii_strdown(topic->title, -1);
        if (strstr(lower_title, lower_query) != NULL) {
            // The query as a phrase in the title: strong, and the more of the
            // title the phrase covers the more specific (better) the match,
            // so "derived signal" prefers "Derived Signals" over a longer
            // title that merely begins the same way.
            GPtrArray *title_words = tokenize(topic->title);
            gdouble coverage = title_words->len > 0 ? (gdouble)n_terms / (gdouble)title_words->len
                                                    : 0.0;
            score[i] += 25.0 + 12.0 * MIN(coverage, 1.0);
            g_ptr_array_unref(title_words);
        }
        g_free(lower_title);

        HelpHit *h = g_new0(HelpHit, 1);
        h->topic = topic;
        h->score = score[i];
        h->snippet = make_snippet(topic, terms);
        g_ptr_array_add(hits, h);
    }
    g_ptr_array_sort(hits, cmp_hits);

    g_free(lower_query);
    g_free(score);
    g_free(matched_terms);
    g_strfreev(terms);
    return hits;
}

/* ----------------------------------------------------------------- links -- */

static void collect_links(const gchar *text, GPtrArray *out)
{
    const gchar *p = text;
    while ((p = strstr(p, "[[")) != NULL) {
        const gchar *end = strstr(p + 2, "]]");
        if (end == NULL) {
            break;
        }
        g_ptr_array_add(out, g_strndup(p + 2, (gsize)(end - (p + 2))));
        p = end + 2;
    }
}

GPtrArray *help_topic_links(const HelpTopic *topic)
{
    GPtrArray *links = g_ptr_array_new_with_free_func(g_free);
    for (guint b = 0; b < topic->blocks->len; b++) {
        const HelpBlock *blk = g_ptr_array_index(topic->blocks, b);
        collect_links(blk->text, links);
    }
    return links;
}

/* ---------------------------------------------------------------- inline -- */

static void span_free(gpointer data)
{
    HelpSpan *s = data;
    g_free(s->text);
    g_free(s->link);
    g_free(s);
}

static void flush_plain(GPtrArray *spans, GString *plain)
{
    if (plain->len > 0) {
        HelpSpan *s = g_new0(HelpSpan, 1);
        s->text = g_strdup(plain->str);
        g_ptr_array_add(spans, s);
        g_string_truncate(plain, 0);
    }
}

GPtrArray *help_inline_parse(const gchar *text)
{
    GPtrArray *spans = g_ptr_array_new_with_free_func(span_free);
    GString *plain = g_string_new(NULL);
    const gchar *p = text;

    while (*p != '\0') {
        const gchar *close = NULL;
        HelpSpan *s = NULL;

        if (p[0] == '*' && p[1] == '*' && (close = strstr(p + 2, "**")) != NULL) {
            flush_plain(spans, plain);
            s = g_new0(HelpSpan, 1);
            s->text = g_strndup(p + 2, (gsize)(close - (p + 2)));
            s->bold = TRUE;
            p = close + 2;
        } else if (p[0] == '`' && (close = strchr(p + 1, '`')) != NULL) {
            flush_plain(spans, plain);
            s = g_new0(HelpSpan, 1);
            s->text = g_strndup(p + 1, (gsize)(close - (p + 1)));
            s->code = TRUE;
            p = close + 1;
        } else if (p[0] == '[' && p[1] == '[' && (close = strstr(p + 2, "]]")) != NULL) {
            flush_plain(spans, plain);
            s = g_new0(HelpSpan, 1);
            s->text = g_strndup(p + 2, (gsize)(close - (p + 2)));
            s->link = g_strdup(s->text);
            p = close + 2;
        }

        if (s != NULL) {
            g_ptr_array_add(spans, s);
        } else {
            g_string_append_c(plain, *p);
            p++;
        }
    }
    flush_plain(spans, plain);
    g_string_free(plain, TRUE);
    return spans;
}
