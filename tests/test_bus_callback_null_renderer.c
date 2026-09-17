/* Regression test: the audio bus callback must survive renderer == NULL
 * (a real crash before this fix). Links audio_renderer.c directly and
 * calls it with a synthetic GST_MESSAGE_ERROR. */
#include <gst/gst.h>
#include <stdarg.h>
#include <stdio.h>
#include "../lib/logger.h"

/* Minimal stub: the real implementation lives in lib/logger.c, which pulls
 * in a lot we don't need for this narrowly-scoped test; we only need the
 * symbol to exist and not crash. */
void logger_log(logger_t *logger, int level, const char *fmt, ...) {
    (void) logger; (void) level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Stub: audio_renderer.c now references this extern (video_renderer.c's
 * A/V sync probe installer), which this test has no reason to link. */
void install_av_sync_probe(GstElement *pipeline) {
    (void) pipeline;
}

/* audio_renderer.c's `renderer` and gstreamer_audio_pipeline_bus_callback
 * are file-static; pull the translation unit in directly so we can call it. */
#include "../renderers/audio_renderer.c"

int main(void) {
    gst_init(NULL, NULL);

    g_assert(renderer == NULL); /* precondition this test exists to exercise */

    GstElement *fake_src = gst_element_factory_make("fakesrc", "audio_source");
    g_assert(fake_src != NULL);

    GError *err = g_error_new(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "synthetic test error");
    GstMessage *msg = gst_message_new_error(GST_OBJECT(fake_src), err, "synthetic debug info");
    GstBus *bus = gst_bus_new();
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    /* renderer is still NULL here: this is exactly the crash scenario. */
    gboolean ret = gstreamer_audio_pipeline_bus_callback(bus, msg, loop);

    g_assert(ret == TRUE);
    g_print("PASS: gstreamer_audio_pipeline_bus_callback survived GST_MESSAGE_ERROR with renderer == NULL\n");

    gst_message_unref(msg);
    g_error_free(err);
    gst_object_unref(bus);
    gst_object_unref(fake_src);
    g_main_loop_unref(loop);
    return 0;
}
