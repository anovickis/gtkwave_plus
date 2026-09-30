#include "signal_value_lookup.h"
#include "globals.h"
#include "gw-mouseover.h"
#include <string.h>

gchar *signal_value_at_time(const gchar *signal_path, GwTime time)
{
    if (GLOBALS->dump_file == NULL) {
        return NULL;
    }

    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, signal_path);
    if (sym == NULL || sym->n == NULL) {
        return NULL;
    }

    // A throwaway, stack-only trace borrowing the symbol's node -- mirrors
    // the pattern AddNodeTraceReturn() uses for a plain (non-vector,
    // non-aliased) symbol lookup, without registering a real, visible
    // trace in GLOBALS->traces.
    GwTrace temp;
    memset(&temp, 0, sizeof(GwTrace));
    temp.name = sym->name;
    temp.name_full = sym->name;
    temp.n.nd = sym->n;

    return local_trace_asciival(&temp, time);
}
