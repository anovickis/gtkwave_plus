/*
 * gtkwave-mcp: a Model Context Protocol (MCP) stdio server that bridges to
 * an already-running `gtkwave --wcp` instance.
 *
 * MCP clients (Claude Code, Claude Desktop, and other MCP-aware agents)
 * spawn this process and speak newline-delimited JSON-RPC 2.0 over its
 * stdin/stdout -- the standard MCP "stdio transport". This bridge
 * connects out, as an ordinary WCP client, to a gtkwave instance's
 * existing WCP TCP port (see src/wcp_gtkwave.c / lib/libwcp), and
 * exposes each WCP command as one MCP tool. A tool call's `arguments`
 * object is merged directly into a `{"type":"command","command":<name>}`
 * WCP envelope and forwarded as-is -- WCP's own server does the real
 * argument validation, so this bridge doesn't need to duplicate it; it
 * just relays one WCP response line back as the tool result.
 *
 * Usage: gtkwave-mcp --port <wcp-port> [--host <host>]
 * (matches `gtkwave --wcp --wcp-port=<wcp-port>`, already running).
 */

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2024-11-05"

typedef struct
{
    const char *name;
    const char *description;
    const char *schema_json; /* JSON Schema object, as text */
} McpTool;

/* One tool per WCP command (see lib/libwcp/wcp_protocol.c's
 * supported_commands[] / wcp_parse_command()) -- field names in each
 * schema are exactly the WCP command's own JSON field names, since a
 * tool call's arguments are merged into the WCP envelope unchanged. */
static const McpTool MCP_TOOLS[] = {
    {"get_item_list",
     "List every currently displayed trace/marker, as WCP item ids.",
     "{\"type\":\"object\",\"properties\":{}}"},
    {"get_item_info",
     "Get name/type for one or more WCP item ids.",
     "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}}},\"required\":[\"ids\"]}"},
    {"set_item_color",
     "Set the display color of one item (by WCP id).",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
     "\"color\":{\"type\":\"string\"}},\"required\":[\"id\",\"color\"]}"},
    {"add_variables",
     "Add signals to the wave view by full hierarchical name.",
     "{\"type\":\"object\",\"properties\":{\"variables\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}}},\"required\":[\"variables\"]}"},
    {"add_items",
     "Add variables or scopes to the wave view by hierarchical path.",
     "{\"type\":\"object\",\"properties\":{\"items\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}},\"recursive\":{\"type\":\"boolean\"}},"
     "\"required\":[\"items\"]}"},
    {"add_markers",
     "Add one or more time markers.",
     "{\"type\":\"object\",\"properties\":{\"markers\":{\"type\":\"array\","
     "\"items\":{\"type\":\"object\",\"properties\":{\"time\":{\"type\":"
     "\"integer\"},\"name\":{\"type\":\"string\"},\"move_focus\":{\"type\":"
     "\"boolean\"}},\"required\":[\"time\",\"move_focus\"]}}},"
     "\"required\":[\"markers\"]}"},
    {"remove_items",
     "Remove one or more displayed items by WCP id.",
     "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\","
     "\"items\":{\"type\":\"string\"}}},\"required\":[\"ids\"]}"},
    {"focus_item",
     "Scroll the wave view so one item (by WCP id) is visible.",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},"
     "\"required\":[\"id\"]}"},
    {"clear", "Remove every displayed item.", "{\"type\":\"object\",\"properties\":{}}"},
    {"set_viewport_to",
     "Center the viewport on a timestamp.",
     "{\"type\":\"object\",\"properties\":{\"timestamp\":{\"type\":\"integer\"}},"
     "\"required\":[\"timestamp\"]}"},
    {"set_viewport_range",
     "Set the viewport to an explicit time range.",
     "{\"type\":\"object\",\"properties\":{\"start\":{\"type\":\"integer\"},"
     "\"end\":{\"type\":\"integer\"}},\"required\":[\"start\",\"end\"]}"},
    {"zoom_to_fit",
     "Zoom a viewport to show the entire waveform.",
     "{\"type\":\"object\",\"properties\":{\"viewport_idx\":{\"type\":\"integer\"}},"
     "\"required\":[\"viewport_idx\"]}"},
    {"load",
     "Load a waveform file, replacing what's currently loaded.",
     "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"}},"
     "\"required\":[\"source\"]}"},
    {"reload", "Reload the current waveform file from disk.", "{\"type\":\"object\",\"properties\":{}}"},
    {"netlist_drivers",
     "Query the combinational drivers of a signal (requires HDL sources configured "
     "once via the GUI's Show Drivers/Loads/Cone first). Returns an 'edges' array of "
     "{path, bits, description, location}.",
     "{\"type\":\"object\",\"properties\":{\"signal_path\":{\"type\":\"string\"}},"
     "\"required\":[\"signal_path\"]}"},
    {"netlist_loads",
     "Query the combinational fan-out loads of a signal (same config requirement as "
     "netlist_drivers). optional max_depth (0=unlimited) and module_boundary_only limit "
     "the transitive result set.",
     "{\"type\":\"object\",\"properties\":{\"signal_path\":{\"type\":\"string\"},"
     "\"max_depth\":{\"type\":\"integer\"},\"module_boundary_only\":{\"type\":\"boolean\"}},"
     "\"required\":[\"signal_path\"]}"},
    {"netlist_cone",
     "Query the transitive combinational fan-in cone of a signal (same config "
     "requirement and limit options as netlist_loads).",
     "{\"type\":\"object\",\"properties\":{\"signal_path\":{\"type\":\"string\"},"
     "\"max_depth\":{\"type\":\"integer\"},\"module_boundary_only\":{\"type\":\"boolean\"}},"
     "\"required\":[\"signal_path\"]}"},
    {"list_x",
     "Scan every signal in the whole loaded design (not just displayed ones) for "
     "unknown/X-like values. Returns an 'entries' array of {signal_path, "
     "has_full_path, start, end} and a 'truncated' flag.",
     "{\"type\":\"object\",\"properties\":{}}"},
    {"time_span_comment_add",
     "Add a [start, end) range comment, distinct from point markers.",
     "{\"type\":\"object\",\"properties\":{\"start\":{\"type\":\"integer\"},"
     "\"end\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"}},"
     "\"required\":[\"start\",\"end\",\"text\"]}"},
    {"time_span_comments_list",
     "List every time-span comment. Returns a 'comments' array of {start, end, text}.",
     "{\"type\":\"object\",\"properties\":{}}"},
    {"undo",
     "Undo the last destructive trace-list operation (Ungroup, Delete, Combine, "
     "Expand).",
     "{\"type\":\"object\",\"properties\":{}}"},
    {"redo", "Redo the last undone trace-list operation.", "{\"type\":\"object\",\"properties\":{}}"},
};

static GDataInputStream *g_wcp_in = NULL;
static GOutputStream *g_wcp_out = NULL;

static JsonNode *parse_json_or_null(const gchar *text)
{
    if (text == NULL) {
        return NULL;
    }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonNode *copy = root != NULL ? json_node_copy(root) : NULL;
    g_object_unref(parser);
    return copy;
}

static void write_stdout_line(const gchar *text)
{
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_response(JsonNode *id_node, JsonNode *result_node)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    json_builder_add_value(b, id_node != NULL ? json_node_copy(id_node) : json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(b, "result");
    json_builder_add_value(b, result_node);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *out = json_generator_to_data(gen, NULL);
    write_stdout_line(out);
    g_free(out);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);
}

static void send_error(JsonNode *id_node, gint code, const gchar *message)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    json_builder_add_value(b, id_node != NULL ? json_node_copy(id_node) : json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code");
    json_builder_add_int_value(b, code);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
    json_builder_end_object(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *out = json_generator_to_data(gen, NULL);
    write_stdout_line(out);
    g_free(out);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);
}

static JsonNode *build_text_result(const gchar *text, gboolean is_error)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "content");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "text");
    json_builder_set_member_name(b, "text");
    json_builder_add_string_value(b, text);
    json_builder_end_object(b);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "isError");
    json_builder_add_boolean_value(b, is_error);
    json_builder_end_object(b);

    JsonNode *result = json_node_copy(json_builder_get_root(b));
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);
    return result;
}

static void handle_initialize(JsonNode *id_node, JsonObject *params)
{
    const gchar *client_version =
        params != NULL && json_object_has_member(params, "protocolVersion")
            ? json_object_get_string_member(params, "protocolVersion")
            : NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "protocolVersion");
    json_builder_add_string_value(b, client_version != NULL ? client_version : MCP_PROTOCOL_VERSION);
    json_builder_set_member_name(b, "capabilities");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "serverInfo");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "gtkwave-wcp-bridge");
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, "0.1.0");
    json_builder_end_object(b);
    json_builder_end_object(b);

    JsonNode *result = json_node_copy(json_builder_get_root(b));
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);

    send_response(id_node, result);
}

static void handle_tools_list(JsonNode *id_node)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_array(b);

    for (guint i = 0; i < G_N_ELEMENTS(MCP_TOOLS); i++) {
        const McpTool *tool = &MCP_TOOLS[i];
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, tool->name);
        json_builder_set_member_name(b, "description");
        json_builder_add_string_value(b, tool->description);
        json_builder_set_member_name(b, "inputSchema");
        JsonNode *schema = parse_json_or_null(tool->schema_json);
        json_builder_add_value(b, schema != NULL ? schema : json_node_new(JSON_NODE_NULL));
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode *result = json_node_copy(json_builder_get_root(b));
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);

    send_response(id_node, result);
}

/* Sends one WCP command (built from a tool name + its MCP arguments
 * object) to the connected gtkwave instance, and returns its one-line
 * JSON response verbatim (owned, g_free by caller), or NULL on a
 * transport-level failure (connection lost, etc. -- not a WCP-level
 * error, which comes back as a normal {"type":"error",...} line). */
static gchar *wcp_roundtrip(const gchar *tool_name, JsonObject *arguments)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "command");
    json_builder_set_member_name(b, "command");
    json_builder_add_string_value(b, tool_name);

    if (arguments != NULL) {
        GList *members = json_object_get_members(arguments);
        for (GList *l = members; l != NULL; l = l->next) {
            const gchar *member_name = l->data;
            JsonNode *value = json_object_get_member(arguments, member_name);
            json_builder_set_member_name(b, member_name);
            json_builder_add_value(b, json_node_copy(value));
        }
        g_list_free(members);
    }
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *request = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);

    gchar *request_line = g_strconcat(request, "\n", NULL);
    g_free(request);

    GError *error = NULL;
    gboolean sent = g_output_stream_write_all(
        g_wcp_out, request_line, strlen(request_line), NULL, NULL, &error);
    g_free(request_line);
    if (!sent) {
        g_clear_error(&error);
        return NULL;
    }

    gchar *response = g_data_input_stream_read_line(g_wcp_in, NULL, NULL, &error);
    if (response == NULL) {
        g_clear_error(&error);
        return NULL;
    }
    return response;
}

static void handle_tools_call(JsonNode *id_node, JsonObject *params)
{
    if (params == NULL || !json_object_has_member(params, "name")) {
        send_error(id_node, -32602, "tools/call requires a 'name'");
        return;
    }

    const gchar *tool_name = json_object_get_string_member(params, "name");
    gboolean known = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(MCP_TOOLS); i++) {
        if (g_str_equal(MCP_TOOLS[i].name, tool_name)) {
            known = TRUE;
            break;
        }
    }
    if (!known) {
        gchar *msg = g_strdup_printf("Unknown tool: %s", tool_name);
        send_response(id_node, build_text_result(msg, TRUE));
        g_free(msg);
        return;
    }

    JsonObject *arguments = NULL;
    if (json_object_has_member(params, "arguments")) {
        JsonNode *args_node = json_object_get_member(params, "arguments");
        if (JSON_NODE_HOLDS_OBJECT(args_node)) {
            arguments = json_node_get_object(args_node);
        }
    }

    gchar *wcp_response = wcp_roundtrip(tool_name, arguments);
    if (wcp_response == NULL) {
        send_response(
            id_node,
            build_text_result("Lost connection to the gtkwave WCP server.", TRUE));
        return;
    }

    gboolean is_error = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, wcp_response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "type") &&
                g_str_equal(json_object_get_string_member(obj, "type"), "error")) {
                is_error = TRUE;
            }
        }
    }
    g_object_unref(parser);

    send_response(id_node, build_text_result(wcp_response, is_error));
    g_free(wcp_response);
}

static void dispatch(const gchar *line)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, NULL)) {
        g_object_unref(parser);
        send_error(NULL, -32700, "Parse error");
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        send_error(NULL, -32600, "Invalid Request");
        return;
    }

    JsonObject *obj = json_node_get_object(root);
    JsonNode *id_node = json_object_has_member(obj, "id") ? json_object_get_member(obj, "id") : NULL;
    gboolean is_request = id_node != NULL;

    const gchar *method =
        json_object_has_member(obj, "method") ? json_object_get_string_member(obj, "method") : NULL;
    JsonObject *params = NULL;
    if (json_object_has_member(obj, "params")) {
        JsonNode *params_node = json_object_get_member(obj, "params");
        if (JSON_NODE_HOLDS_OBJECT(params_node)) {
            params = json_node_get_object(params_node);
        }
    }

    if (method == NULL) {
        g_object_unref(parser);
        if (is_request) {
            send_error(id_node, -32600, "Invalid Request");
        }
        return;
    }

    if (g_str_equal(method, "initialize")) {
        handle_initialize(id_node, params);
    } else if (g_str_equal(method, "notifications/initialized") ||
              g_str_equal(method, "notifications/cancelled")) {
        /* no response expected */
    } else if (g_str_equal(method, "tools/list")) {
        handle_tools_list(id_node);
    } else if (g_str_equal(method, "tools/call")) {
        handle_tools_call(id_node, params);
    } else if (g_str_equal(method, "ping")) {
        JsonNode *empty = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(empty, json_object_new());
        send_response(id_node, empty);
    } else if (is_request) {
        gchar *msg = g_strdup_printf("Method not found: %s", method);
        send_error(id_node, -32601, msg);
        g_free(msg);
    }

    g_object_unref(parser);
}

int main(int argc, char **argv)
{
    guint16 port = 8765;
    const gchar *host = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--port") && i + 1 < argc) {
            port = (guint16)atoi(argv[++i]);
        } else if (g_str_equal(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (g_str_equal(argv[i], "--help")) {
            printf("Usage: %s --port <wcp-port> [--host <host>]\n", argv[0]);
            printf("Bridges MCP (stdio) to an already-running `gtkwave --wcp` instance.\n");
            return 0;
        }
    }

    GError *error = NULL;
    GSocketClient *client = g_socket_client_new();
    GSocketConnection *conn = g_socket_client_connect_to_host(
        client, host, port, NULL, &error);
    if (conn == NULL) {
        fprintf(stderr,
               "gtkwave-mcp: could not connect to gtkwave WCP server at %s:%u: %s\n",
               host,
               port,
               error != NULL ? error->message : "unknown error");
        return 1;
    }

    GInputStream *raw_in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    g_wcp_out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_wcp_in = g_data_input_stream_new(raw_in);
    g_data_input_stream_set_newline_type(g_wcp_in, G_DATA_STREAM_NEWLINE_TYPE_LF);

    /* Discard the server's greeting line -- WCP always sends one
     * immediately on connect (see wcp_response_greeting()); this bridge
     * advertises its own fixed tool list instead of relaying it. */
    gchar *greeting = g_data_input_stream_read_line(g_wcp_in, NULL, NULL, NULL);
    g_free(greeting);

    gchar *line = NULL;
    gsize line_len = 0;
    while (getline(&line, &line_len, stdin) != -1) {
        g_strchomp(line);
        if (line[0] == '\0') {
            continue;
        }
        dispatch(line);
    }
    free(line);

    g_object_unref(g_wcp_in);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    g_object_unref(client);
    return 0;
}
