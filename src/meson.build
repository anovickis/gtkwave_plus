subdir('helpers')

# gtkwave

gtkwave_resources = gnome.compile_resources(
    'gtkwave-resources',
    'io.github.gtkwave.GTKWave.gresource.xml',
    c_name: 'gtkwave',
)

gtkwave_sources = [
    'analyzer.c',
    'auto_update.c',
    'auto_update_poll.c',
    'baseconvert.c',
    'bitvec.c',
    'bsearch.c',
    'bus_ops.c',
    'busy.c',
    'clipping.c',
    'console_panel.c',
    'currenttime.c',
    'debug.c',
    'derived_expr.c',
    'derived_signals.c',
    'edgebuttons.c',
    'entry.c',
    'event_sequence.c',
    'event_sequence_view.c',
    'netlist_graph.c',
    'cause_trace.c',
    'cause_trace_view.c',
    'condition_search.c',
    'condition_search_view.c',
    'source_link.c',
    'dump_file_main.c',
    'fetchbuttons.c',
    'fgetdynamic.c',
    'file.c',
    'fonts.c',
    'gconf.c',
    'globals.c',
    'group_manager.c',
    'gtk23compat.c',
    'gw-mouseover.c',
    'gw-named-marker-dialog.c',
    'gw-time-display.c',
    'help_doc.c',
    'help_view.c',
    'gw-wave-view-traces.c',
    'gw-wave-view.c',
    'list_x_view.c',
    'logfile.c',
    'lx2.c',
    'main.c',
    'marker_panel.c',
    'markerbox.c',
    'menu.c',
    'mouseover.c',
    'netlist_query_ui.c',
    'pagebuttons.c',
    'pipeio.c',
    'pixmaps.c',
    'print.c',
    'ptranslate.c',
    'rc.c',
    'regex.c',
    'renderopt.c',
    'savefile.c',
    'gate_cone_select.c',
    'gate_schematic_view.c',
    'schematic_layout.c',
    'schematic_node_key.c',
    'schematic_svg_edit.c',
    'schematic_view.c',
    'search.c',
    'shiftbuttons.c',
    'showchange.c',
    'signal_event_report.c',
    'signal_list.c',
    'signal_popover.c',
    'signal_value_lookup.c',
    'signalwindow.c',
    'simplereq.c',
    'source_panel.c',
    'status.c',
    'strace.c',
    'struct_fields.c',
    'struct_group.c',
    'struct_scope.c',
    'symbol.c',
    'time_compress.c',
    'time_span_comments.c',
    'undo_stack.c',
    'tcl_helper.c',
    'timeentry.c',
    'translate.c',
    'tree.c',
    'treesearch.c',
    'ttranslate.c',
    'vcd_saver.c',
    'vcd.c',
    'wcp_gtkwave.c',
    'wavewindow.c',
    'zoombuttons.c',
    'cocoa/cocoa_misc.c',
]

gtkwave_dependencies = [
    glib_dep,
    gtk_dep,
    gtk_mac_integration_dep,
    gtk_unix_print_dep,
    m_dep,
    libfst_dep,
    libjrb_dep,
    bzip2_dep,
    judy_dep,
    gnu_regex_dep,
    libgtkwave_dep,
    libwcp_dep,
]

gtkwave_include_directories = [
    config_inc,
    'cocoa',
]

executable(
    'gtkwave',
    gtkwave_sources,
    gtkwave_resources,
    vcd_keywords_c,
    version_h,
    dependencies: gtkwave_dependencies,
    include_directories: gtkwave_include_directories,
    c_args: warning_args,
    link_args: link_args,
    install: true,
    install_rpath: install_rpath,
)

# event_sequence backend unit test (GTK-free: glib + libfst only)

event_sequence_test = executable(
    'event_sequence_test',
    ['event_sequence.c', 'event_sequence_test.c'],
    dependencies: [glib_dep, libfst_dep],
    c_args: warning_args,
)
test('event_sequence', event_sequence_test)

# netlist_graph unit test (GTK-free: glib + json-glib), against a netlist
# saved from lib/libgtkwave/test/files/gate_cone_design.sv

netlist_graph_test = executable(
    'netlist_graph_test',
    ['netlist_graph.c', 'netlist_graph_test.c'],
    dependencies: [glib_dep, json_glib_dep],
    c_args: warning_args,
)
test(
    'netlist_graph',
    netlist_graph_test,
    args: [files('../lib/libgtkwave/test/files/gate_cone_design.netlist.json')],
)

cause_trace_test = executable(
    'cause_trace_test',
    ['cause_trace.c', 'netlist_graph.c', 'cause_trace_test.c'],
    dependencies: [glib_dep, json_glib_dep],
    c_args: warning_args,
)
test(
    'cause_trace',
    cause_trace_test,
    args: [files('../lib/libgtkwave/test/files/gate_cone_design.netlist.json')],
)

# auto_update poll-decision unit test (GTK-free: glib + libfst only)

auto_update_poll_test = executable(
    'auto_update_poll_test',
    ['auto_update_poll.c', 'auto_update_poll_test.c'],
    dependencies: [glib_dep, libfst_dep],
    c_args: warning_args,
)
test('auto_update_poll', auto_update_poll_test)

# schematic layout store + SVG editor unit tests (GTK-free: glib only)

schematic_layout_test = executable(
    'schematic_layout_test',
    ['schematic_layout.c', 'schematic_svg_edit.c', 'schematic_layout_test.c'],
    dependencies: [glib_dep, m_dep],
    c_args: warning_args,
)
test('schematic_layout', schematic_layout_test)

schematic_node_key_test = executable(
    'schematic_node_key_test',
    ['schematic_node_key.c', 'schematic_node_key_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('schematic_node_key', schematic_node_key_test)

gate_cone_select_test = executable(
    'gate_cone_select_test',
    ['gate_cone_select.c', 'gate_cone_select_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('gate_cone_select', gate_cone_select_test,
     args: [meson.project_source_root() / 'lib/libgtkwave/test/files/gate_cone_design.sv'])

derived_expr_test = executable(
    'derived_expr_test',
    ['derived_expr.c', 'derived_expr_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('derived_expr', derived_expr_test)

condition_search_test = executable(
    'condition_search_test',
    ['condition_search.c', 'derived_expr.c', 'condition_search_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('condition_search', condition_search_test)

help_doc_test = executable(
    'help_doc_test',
    ['help_doc.c', 'help_doc_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('help_doc', help_doc_test,
     args: [meson.project_source_root() / 'share/help/features.md',
            meson.project_source_root() / 'src/menu.c'])

struct_scope_test = executable(
    'struct_scope_test',
    ['struct_scope.c', 'struct_scope_test.c'],
    dependencies: [glib_dep, libfst_dep],
    c_args: warning_args,
)
test('struct_scope', struct_scope_test)

struct_fields_test = executable(
    'struct_fields_test',
    ['struct_fields.c', 'struct_fields_test.c'],
    dependencies: [glib_dep],
    c_args: warning_args,
)
test('struct_fields', struct_fields_test)

# twinwave

twinwave_sources = [
    'twinwave.c',
]

twinwave_dependencies = [
    gtk_dep,
    libgtkwave_dep,
]

twinwave_include_directories = [
    config_inc,
    # 'cocoa',
]

executable(
    'twinwave',
    twinwave_sources,
    dependencies: twinwave_dependencies,
    include_directories: twinwave_include_directories,
    install: true,
    install_rpath: install_rpath,
)
