#pragma once

#include <glib.h>

// The built-in help: a Markdown-flavoured document (share/help/features.md)
// parsed into categories and topics, with a full-text index for searching.
// GTK-free so it can be tested without a display.
//
// Document format:
//   # Category                     starts a category (shown in the index)
//   ## Topic title                 starts a topic in that category
//   keywords: a, b, c              (first line of a topic, optional) extra
//                                  words that find this topic, not displayed
//   ### Heading                    a sub-heading inside a topic
//   - item / 1. item               list items (indented lines continue them)
//   plain lines                    a paragraph (consecutive lines join)
//   See also: [[Topic]], [[Other]] a paragraph of cross-references
//   inline: **bold**, `code`, [[Topic Title]] (a link to another topic)

typedef enum
{
    HELP_BLOCK_PARAGRAPH,
    HELP_BLOCK_HEADING,
    HELP_BLOCK_BULLET,
    HELP_BLOCK_NUMBERED,
    HELP_BLOCK_SEE_ALSO,
} HelpBlockKind;

typedef struct
{
    HelpBlockKind kind;
    gchar *text; // inline markup left in place; see help_inline_parse()
    guint number; // HELP_BLOCK_NUMBERED: the item's own number
} HelpBlock;

typedef struct
{
    gchar *title;
    gchar *category;
    GPtrArray *keywords; // gchar*
    GPtrArray *blocks; // HelpBlock*
    guint index; // position in the document
} HelpTopic;

typedef struct _HelpDoc HelpDoc;

typedef enum
{
    HELP_DOC_ERROR_SYNTAX,
} HelpDocError;

GQuark help_doc_error_quark(void);
#define HELP_DOC_ERROR (help_doc_error_quark())

// Parses a whole document. Errors (with line numbers): a topic before any
// category, a duplicate topic title, an empty topic.
HelpDoc *help_doc_parse(const gchar *text, GError **error);
void help_doc_free(HelpDoc *doc);

guint help_doc_n_topics(const HelpDoc *doc);
const HelpTopic *help_doc_topic(const HelpDoc *doc, guint index);
// Category names in document order (gchar*, borrowed).
GPtrArray *help_doc_categories(const HelpDoc *doc);
// Case-insensitive exact title match, or NULL.
const HelpTopic *help_doc_find_topic(const HelpDoc *doc, const gchar *title);

typedef struct
{
    const HelpTopic *topic;
    gdouble score;
    gchar *snippet; // a short excerpt around the first match
} HelpHit;

// Full-text search. Every word of `query` must match (AND); a word matches
// any indexed word it is a prefix of. Title matches score highest, then
// keywords, then headings, then body text. Best first; ties keep document
// order. An empty query returns no hits. Free with g_ptr_array_unref().
GPtrArray *help_doc_search(const HelpDoc *doc, const gchar *query);

// The lowercase words of `query`, for highlighting matches in displayed
// text. Free with g_strfreev().
gchar **help_query_terms(const gchar *query);

// Titles of every topic `topic` links to, from [[...]] anywhere in it
// (gchar*, newly allocated; free with g_ptr_array_unref()).
GPtrArray *help_topic_links(const HelpTopic *topic);

/* ----------------------------------------------------- inline markup -- */

typedef struct
{
    gchar *text;
    gboolean bold;
    gboolean code;
    gchar *link; // target topic title for a [[link]], else NULL
} HelpSpan;

// Splits a block's text into styled spans. Unterminated markers are kept
// as plain text. Free with g_ptr_array_unref().
GPtrArray *help_inline_parse(const gchar *text);
