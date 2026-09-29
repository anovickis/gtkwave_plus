#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include "globals.h"
#include "gw-vcd-loader.h"
#include "gw-ghw-loader.h"
#include "gw-fst-loader.h"
#include "lx2.h"

static void set_common_settings(GwLoader *loader)
{
    const Settings *global_settings = &GLOBALS->settings;

    gw_loader_set_preserve_glitches(loader, global_settings->preserve_glitches);
    gw_loader_set_preserve_glitches_real(loader, global_settings->preserve_glitches_real);

    if (!GLOBALS->hier_was_explicitly_set) {
        GLOBALS->hier_delimeter = '.';
    }
    gw_loader_set_hierarchy_delimiter(loader, GLOBALS->hier_delimeter);
}

static GwDumpFile *load(GwLoader *loader, const gchar *fname)
{
    GError *error = NULL;
    GwDumpFile *file = gw_loader_load(loader, fname, &error);
    if (file == NULL) {
        g_printerr("Error loading %s: ", fname);
        if (error != NULL) {
            g_printerr("%s\n", error->message);
            g_error_free(error);
        } else {
            g_printerr("Unknown error\n");
        }
        exit(EXIT_FAILURE);
    }

    return file;
}

// TODO: remove
GwDumpFile *vcd_recoder_main(char *fname)
{
    const Settings *global_settings = &GLOBALS->settings;

    GwLoader *loader = gw_vcd_loader_new();
    set_common_settings(loader);
    gw_vcd_loader_set_vlist_prepack(GW_VCD_LOADER(loader), global_settings->vlist_prepack);
    gw_vcd_loader_set_vlist_compression_level(GW_VCD_LOADER(loader),
                                              global_settings->vlist_compression_level);
    gw_vcd_loader_set_warning_filesize(GW_VCD_LOADER(loader),
                                       global_settings->vcd_warning_filesize);

    GwDumpFile *file = load(loader, fname);

    g_object_unref(loader);

    GLOBALS->is_lx2 = LXT2_IS_VLIST;

    return file;
}

// TODO: remove
GwDumpFile *ghw_main(char *fname)
{
    GwLoader *loader = gw_ghw_loader_new();
    set_common_settings(loader);

    GwDumpFile *file = load(loader, fname);

    g_object_unref(loader);

    return file;
}

// TODO: remove
GwDumpFile *fst_main(char *fname, char *skip_start, char *skip_end)
{
    GwLoader *loader = gw_fst_loader_new();
    set_common_settings(loader);

    gw_fst_loader_set_start_time(GW_FST_LOADER(loader), skip_start);
    gw_fst_loader_set_end_time(GW_FST_LOADER(loader), skip_end);

    GwDumpFile *file = load(loader, fname);

    g_object_unref(loader);

    GLOBALS->is_lx2 = LXT2_IS_FST;

    return file;
}

// FSDB is a proprietary Synopsys format. Reading it directly requires linking
// their FsdbReader SDK (libnffr.so), which needs a separate "VC Apps Access
// Program Agreement" beyond a normal Verdi/VCS license -- so instead of
// linking that restricted-distribution library, we shell out to the
// `fsdb2vcd` conversion utility (installed alongside Verdi/VCS) and load the
// resulting VCD through the existing, battle-tested VCD loader. Slower than a
// native reader would be, but needs nothing beyond what a Verdi/VCS install
// already provides on $PATH.
static gchar *find_fsdb2vcd(void)
{
    gchar *path = g_find_program_in_path("fsdb2vcd");
    if (path != NULL) {
        return path;
    }

    static const gchar *env_homes[] = {"VERDI_HOME", "VCS_HOME"};
    for (guint i = 0; i < G_N_ELEMENTS(env_homes); i++) {
        const gchar *home = g_getenv(env_homes[i]);
        if (home == NULL) {
            continue;
        }

        gchar *candidate = g_build_filename(home, "bin", "fsdb2vcd", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

// Best-effort native FSDB reader. Reuses the same clean-room, reverse-engineered
// decoding logic developed and validated against a large self-generated test
// corpus (see FINDINGS.md in that research tree), via a small CLI wrapper
// script rather than a hand-ported C reimplementation -- the format still has
// real unsolved edges (certain multi-variable value encodings, >4-transition
// vectors, some bit-widths), so re-deriving all of that logic in C by hand
// would risk introducing new bugs while duplicating already-tested work.
//
// The wrapper's contract is all-or-nothing: exit 0 with a VCD on stdout only
// if it can decode every scope/variable in the file with full confidence;
// anything less prints nothing and exits non-zero, and we fall through to the
// fsdb2vcd path below. This needs nothing beyond a `python3` on $PATH -- no
// Verdi/VCS install required -- but only succeeds on a subset of real files.
static gboolean try_fsdb_native_decode(const char *fname, const char *out_vcd_path)
{
    gchar *python_path = g_find_program_in_path("python3");
    if (python_path == NULL) {
        return FALSE;
    }

    if (!g_file_test(GTKWAVE_FSDB_NATIVE_SCRIPT, G_FILE_TEST_IS_REGULAR)) {
        g_free(python_path);
        return FALSE;
    }

    gchar *argv[] = {python_path, (gchar *)GTKWAVE_FSDB_NATIVE_SCRIPT, (gchar *)fname, NULL};
    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    gboolean spawned = g_spawn_sync(NULL,
                                    argv,
                                    NULL,
                                    G_SPAWN_SEARCH_PATH,
                                    NULL,
                                    NULL,
                                    &out,
                                    &err,
                                    &exit_status,
                                    &error);

    g_free(python_path);

    gboolean ok =
        spawned && g_spawn_check_wait_status(exit_status, NULL) && out != NULL && out[0] != '\0';

    if (ok) {
        ok = g_file_set_contents(out_vcd_path, out, -1, NULL);
    }

    if (!ok && err != NULL && err[0] != '\0') {
        g_printerr("GTKWAVE | Native FSDB decode declined: %s\n", err);
    }

    g_clear_error(&error);
    g_free(out);
    g_free(err);

    return ok;
}

GwDumpFile *fsdb_main(char *fname)
{
    GError *error = NULL;
    gchar *temp_path = NULL;
    gint temp_fd = g_file_open_tmp("gtkwave-fsdb-XXXXXX.vcd", &temp_path, &error);
    if (temp_fd < 0) {
        g_printerr("GTKWAVE | Could not create a temporary file for FSDB conversion: %s\n",
                   error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }
    g_close(temp_fd, NULL);

    if (try_fsdb_native_decode(fname, temp_path)) {
        g_printerr("GTKWAVE | Decoded '%s' natively (no Verdi/VCS needed).\n", fname);

        GwDumpFile *file = vcd_recoder_main(temp_path);

        g_unlink(temp_path);
        g_free(temp_path);

        return file;
    }

    gchar *fsdb2vcd_path = find_fsdb2vcd();
    if (fsdb2vcd_path == NULL) {
        g_printerr("GTKWAVE | Could not find 'fsdb2vcd' (checked $PATH, $VERDI_HOME/bin, "
                   "$VCS_HOME/bin).\n"
                   "GTKWAVE | Reading FSDB requires a Synopsys Verdi or VCS installation "
                   "providing this\n"
                   "GTKWAVE | conversion utility -- gtkwave converts FSDB to VCD via fsdb2vcd "
                   "rather than\n"
                   "GTKWAVE | linking Synopsys's separately-licensed FsdbReader SDK directly.\n");
        g_unlink(temp_path);
        g_free(temp_path);
        return NULL;
    }

    g_printerr("GTKWAVE | No full native decode -- converting '%s' to VCD via fsdb2vcd...\n",
               fname);

    // The installed fsdb2vcd is a shell wrapper whose shebang (#!/bin/sh) doesn't
    // match the syntax it actually uses, so it must be invoked via bash explicitly
    // rather than executed directly.
    gchar *argv[] = {(gchar *)"bash", fsdb2vcd_path, (gchar *)"-o", temp_path, fname, NULL};
    gchar *conv_stdout = NULL;
    gchar *conv_stderr = NULL;
    gint exit_status = 0;
    gboolean spawned = g_spawn_sync(NULL,
                                    argv,
                                    NULL,
                                    G_SPAWN_SEARCH_PATH,
                                    NULL,
                                    NULL,
                                    &conv_stdout,
                                    &conv_stderr,
                                    &exit_status,
                                    &error);

    g_free(fsdb2vcd_path);

    if (!spawned || !g_spawn_check_wait_status(exit_status, NULL)) {
        g_printerr("GTKWAVE | fsdb2vcd failed to convert '%s':\n", fname);
        if (error != NULL) {
            g_printerr("GTKWAVE | %s\n", error->message);
        }
        if (conv_stderr != NULL && conv_stderr[0] != '\0') {
            g_printerr("%s\n", conv_stderr);
        }
        g_clear_error(&error);
        g_free(conv_stdout);
        g_free(conv_stderr);
        g_unlink(temp_path);
        g_free(temp_path);
        return NULL;
    }

    g_free(conv_stdout);
    g_free(conv_stderr);

    GwDumpFile *file = vcd_recoder_main(temp_path);

    g_unlink(temp_path);
    g_free(temp_path);

    return file;
}
