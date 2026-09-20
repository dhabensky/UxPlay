/* Regression test for video_renderer_release_display()'s epoch guard: a
 * deferred hide scheduled before a reconnect must no-op if choose_codec()
 * (re)confirms PLAYING (bumping video_connect_epoch) before the g_idle_add
 * callback runs -- otherwise it would re-hide the picture the reconnect
 * just restored. Links video_renderer.c directly to call the real guard
 * logic and the real g_idle_add()/GMainContext dispatch, not a reimplementation. */
#include <gst/gst.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../lib/logger.h"

static char last_log[256] = "";

/* Minimal stub: records the message so the test can tell which branch of
 * video_renderer_release_display_cb() ran, without needing a real kmssink. */
void logger_log(logger_t *logger, int level, const char *fmt, ...) {
    (void) logger; (void) level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", last_log);
}

/* Stub: video_renderer_init() (not exercised here) calls this; only the
 * symbol needs to exist. */
int logger_get_level(logger_t *logger) {
    (void) logger;
    return 0;
}

/* video_renderer.c's video_connect_epoch, release_display_cb, etc are
 * file-static; pull the translation unit in directly so we can call them. */
#include "../renderers/video_renderer.c"

static void drain_default_context(void) {
    while (g_main_context_iteration(NULL, FALSE)) { }
}

int main(void) {
    gst_init(NULL, NULL);

    /* Case 1: no reconnect -- epoch unchanged when the deferred callback
     * runs, the hide must actually happen. */
    g_atomic_int_set(&video_connect_epoch, 10);
    last_log[0] = '\0';
    video_renderer_release_display_cb(GUINT_TO_POINTER((guint) 10));
    g_assert(strstr(last_log, "hid video") != NULL);
    g_print("PASS: unchanged epoch -> hide runs\n");

    /* Case 2: choose_codec()'s g_atomic_int_inc() (a reconnect) raced ahead
     * of the deferred callback -- must no-op instead of re-hiding. */
    g_atomic_int_set(&video_connect_epoch, 20);
    g_atomic_int_inc(&video_connect_epoch); /* simulates choose_codec() */
    last_log[0] = '\0';
    video_renderer_release_display_cb(GUINT_TO_POINTER((guint) 20));
    g_assert(strstr(last_log, "skipping hide") != NULL);
    g_assert(strstr(last_log, "hid video") == NULL);
    g_print("PASS: reconnect-raced epoch -> hide suppressed\n");

    /* Case 3: the real public entry point + a real default-GMainContext
     * idle dispatch (same context main_loop()/-replay's replay_loop use),
     * reconnect happens between scheduling and dispatch. */
    g_atomic_int_set(&video_connect_epoch, 30);
    video_renderer_release_display();
    g_atomic_int_inc(&video_connect_epoch);
    last_log[0] = '\0';
    drain_default_context();
    g_assert(strstr(last_log, "skipping hide") != NULL);
    g_print("PASS: release_display() then reconnect before idle dispatch -> suppressed\n");

    /* Case 4: same real entry point, no reconnect -- hide must run. */
    g_atomic_int_set(&video_connect_epoch, 40);
    video_renderer_release_display();
    last_log[0] = '\0';
    drain_default_context();
    g_assert(strstr(last_log, "hid video") != NULL);
    g_print("PASS: release_display() with no reconnect -> hide runs\n");

    return 0;
}
