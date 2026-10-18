#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "auto_update_poll.h"
#include "fstapi.h"

static void emit_step(fstWriterContext *w, fstHandle sig, int t)
{
    fstWriterEmitTimeChange(w, (uint64_t)t * 10);
    fstWriterEmitValueChange(w, sig, (t & 1) ? "1" : "0");
}

// Drives a real, still-open FST writer through the states a live
// simulation produces and checks each poll decision.
static void test_live_writer(void)
{
    gchar *dir = g_dir_make_tmp("autoupd-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "live.fst", NULL);

    AutoUpdatePoll poll = {0};

    // File doesn't exist yet.
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);
    g_assert_false(poll.primed);

    fstWriterContext *w = fstWriterCreate(path, 1);
    g_assert_nonnull(w);
    fstWriterSetTimescale(w, -9);
    fstWriterSetScope(w, FST_ST_VCD_MODULE, "tb", NULL);
    fstHandle clk = fstWriterCreateVar(w, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterSetUpscope(w);
    fstWriterEmitValueChange(w, clk, "0");

    // Created but nothing flushed: not readable, must not prime or reload.
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);
    g_assert_false(poll.primed);
    g_assert_false(auto_update_poll_prime(&poll, path));

    // Flush until the file becomes readable. The writer commits data in
    // bursts and lags the last flush, so how many flushes that takes isn't
    // fixed -- and until then every poll must stay IDLE, never reload.
    int t = 1;
    while (t <= 40 && !auto_update_poll_prime(&poll, path)) {
        g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);
        emit_step(w, clk, t++);
        fstWriterFlushContext(w);
    }
    g_assert_true(poll.primed);
    guint64 primed_end = poll.end_time;
    g_assert_cmpuint(primed_end, >, 0);

    // Unchanged file: idle, repeatedly.
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);

    // The writer keeps going: new flushed blocks => reload, exactly once.
    // (mtime granularity can be coarse, so make sure time passes.)
    g_usleep(20 * 1000);
    for (int more = 0; more < 12; more++) {
        emit_step(w, clk, t++);
        fstWriterFlushContext(w);
    }
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_RELOAD);
    g_assert_cmpuint(poll.end_time, >, primed_end);
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);

    // Simulation finishes: closing flushes the tail and finalizes the file.
    g_usleep(20 * 1000);
    emit_step(w, clk, t++);
    fstWriterClose(w);
    AutoUpdateAction final = auto_update_poll_check(&poll, path);
    g_assert_cmpint(final, ==, AUTO_UPDATE_RELOAD);
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_IDLE);

    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

// A new run overwriting the same path ends EARLIER than the old one; that
// still has to trigger a reload.
static void test_rewritten_shorter(void)
{
    gchar *dir = g_dir_make_tmp("autoupd-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "run.fst", NULL);

    for (int pass = 0; pass < 2; pass++) {
        fstWriterContext *w = fstWriterCreate(path, 1);
        fstWriterSetTimescale(w, -9);
        fstWriterSetScope(w, FST_ST_VCD_MODULE, "tb", NULL);
        fstHandle clk = fstWriterCreateVar(w, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
        fstWriterSetUpscope(w);
        fstWriterEmitValueChange(w, clk, "0");
        int steps = pass == 0 ? 20 : 4;
        for (int t = 1; t <= steps; t++) {
            emit_step(w, clk, t);
        }
        fstWriterClose(w);
        g_usleep(20 * 1000);
    }
    // (second pass has now replaced the file; check against a baseline taken
    // from a longer run)
    AutoUpdatePoll poll = {0};
    poll.primed = TRUE;
    poll.size = 1;
    poll.mtime_usec = 1;
    poll.end_time = 200;
    g_assert_cmpint(auto_update_poll_check(&poll, path), ==, AUTO_UPDATE_RELOAD);
    g_assert_cmpuint(poll.end_time, ==, 40);

    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/auto-update/live-writer", test_live_writer);
    g_test_add_func("/auto-update/rewritten-shorter", test_rewritten_shorter);
    return g_test_run();
}
