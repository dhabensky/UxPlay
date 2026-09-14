/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "audio_renderer.h"
#define SECOND_IN_NSECS 1000000000UL

/* -threadtest diagnostic timing (see uxplay.cpp's threadtest_driver and
 * docs/threadtest.md) -- gated by env var since this file doesn't see
 * uxplay.cpp's do_threadtest global; zero cost/output unless
 * UX_THREADTEST_DIAG is set (-threadtest sets it automatically). */
static double tt_diag_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}
#define TT_DIAG(...) do { if (g_getenv("UX_THREADTEST_DIAG")) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* -threadtest diagnostic only: how many audio_renderer_render_buffer() /
 * decoded-buffer callbacks since the last (re)start -- reset to 0 by every
 * branch of audio_renderer_start() that actually did something. Declared
 * here (not next to audio_renderer_start() below) so the decode-probe
 * callback above it in this file can also see it. */
static int tt_calls_since_start = 0;

/* Lightweight decode-output counter (log-only, no buffer-content
 * inspection at all -- deliberately NOT av_sync_probe/UX_PROBE, whose
 * per-buffer content mapping requires real concurrent video+audio
 * threading to have ever been exercised safely; see docs/threadtest.md).
 * Attached once, at pipeline-construction time, to the decoder element's
 * SRC pad
 * (avdec_aac/avdec_alac -- found by factory name, not element name, since
 * GStreamer's auto-numbering isn't guaranteed) -- answers "did decoded PCM
 * ever leave the decoder after a restart" without touching sample data,
 * so it carries none of av_sync_probe's per-buffer gst_buffer_map risk. */
static GstPadProbeReturn tt_decode_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void) pad; (void) info; (void) user_data;
    if (tt_calls_since_start < 6) {
        TT_DIAG("threadtest: DECODED-BUFFER-OUT t=%.6f\n", tt_diag_now());
    }
    return GST_PAD_PROBE_OK;
}
static void install_decode_probe(GstElement *pipeline) {
    GstIterator *it = gst_bin_iterate_elements(GST_BIN(pipeline));
    GValue v = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &v)) {
        case GST_ITERATOR_OK: {
            GstElement *el = GST_ELEMENT(g_value_get_object(&v));
            GstElementFactory *f = gst_element_get_factory(el);
            const gchar *fname = f ? GST_OBJECT_NAME(f) : "";
            if (fname && g_str_has_prefix(fname, "avdec_")) {
                GstPad *p = gst_element_get_static_pad(el, "src");
                if (p) {
                    gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, tt_decode_probe, NULL, NULL);
                    gst_object_unref(p);
                }
            }
            g_value_reset(&v);
            break;
        }
        case GST_ITERATOR_RESYNC: gst_iterator_resync(it); break;
        default: done = TRUE; break;
        }
    }
    g_value_unset(&v);
    gst_iterator_free(it);
}

/* A/V sync self-measurement probe installer, defined in video_renderer.c */
void install_av_sync_probe(GstElement *pipeline);

#define NFORMATS 2     /* set to 4 to enable AAC_LD and PCM:  allowed, but  never seen in real-world use */

static GstClockTime gst_audio_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
const char * format[NFORMATS];

static const gchar *avdec_aac = "avdec_aac";
static const gchar *avdec_alac = "avdec_alac";
static gboolean aac = FALSE;
static gboolean alac = FALSE;
static gboolean render_audio = FALSE;
static gboolean async = FALSE;
static gboolean vsync = FALSE;
static gboolean sync = FALSE;
static gboolean audio_rtp = FALSE;

typedef struct audio_renderer_s {
    GstElement *appsrc; 
    GstElement *pipeline;
    GstElement *volume;
    GstBus *bus;
    unsigned char ct;
} audio_renderer_t ;
static audio_renderer_t *renderer_type[NFORMATS];
static audio_renderer_t *renderer = NULL;

/* GStreamer Caps strings for Airplay-defined audio compression types (ct) */

/* ct = 1; linear PCM (uncompressed): 44100/16/2, S16LE */
static const char lpcm_caps[]="audio/x-raw,rate=(int)44100,channels=(int)2,format=S16LE,layout=interleaved";

/* ct = 2; codec_data is ALAC magic cookie:  44100/16/2 spf = 352 */    
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                           "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

/* ct = 4; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1:  AAC-LC 44100/2 spf = 1024 */
static const char aac_lc_caps[] ="audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)1210";

/* ct = 8; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1: AAC_ELD 44100/2  spf = 480 */
static const char aac_eld_caps[] ="audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";

static gboolean check_plugins (void)
{
    GstRegistry *registry = NULL;
    const gchar *needed[] = { "app", "libav", "playback", "autodetect", "videoparsersbad",  NULL};
    const gchar *gst[] = {"plugins-base", "libav", "plugins-base", "plugins-good", "plugins-bad", NULL};
    registry = gst_registry_get ();
    gboolean ret = TRUE;
    for (int i = 0; i < g_strv_length ((gchar **) needed); i++) {
        GstPlugin *plugin = NULL;
        plugin = gst_registry_find_plugin (registry, needed[i]);
        if (!plugin) {
            g_print ("Required gstreamer plugin '%s' not found\n"
                     "Missing plugin is contained in  '[GStreamer 1.x]-%s'\n",needed[i], gst[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref (plugin);
        plugin = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

static gboolean check_plugin_feature (const gchar *needed_feature)
{
    GstPluginFeature *plugin_feature = NULL;
    GstRegistry *registry = gst_registry_get ();
    gboolean ret = TRUE;

    plugin_feature = gst_registry_find_feature (registry, needed_feature, GST_TYPE_ELEMENT_FACTORY);
    if (!plugin_feature) {
        g_print ("Required gstreamer libav plugin feature '%s' not found:\n\n"
	         "This may be missing because the FFmpeg package used by GStreamer-1.x-libav is incomplete.\n"
	         "(Some distributions provide an incomplete FFmpeg due to License or Patent issues:\n"
	         "in such cases a complete version for that distribution is usually made available elsewhere)\n",
	         needed_feature);
        ret = FALSE;
    } else {
        gst_object_unref (plugin_feature);
        plugin_feature = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin feature is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

bool gstreamer_init(){
    gst_init(NULL,NULL);    
    return (bool) check_plugins ();
}

void audio_renderer_init(logger_t *render_logger, const char* audiosink, const bool* audio_sync, const bool* video_sync, const char *artp_pipeline) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    GstClock *clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);

    audio_rtp = (bool) strlen(artp_pipeline);
    if (audio_rtp) {
        g_print("*** Audio RTP mode enabled: sending to %s\n", artp_pipeline);
    }

    logger = render_logger;
    
    aac = check_plugin_feature (avdec_aac);
    alac = check_plugin_feature (avdec_alac);

    for (int i = 0; i < NFORMATS ; i++) {
        renderer_type[i] = (audio_renderer_t *)  calloc(1,sizeof(audio_renderer_t));
        g_assert(renderer_type[i]);
        GString *launch = g_string_new("appsrc name=audio_source ! ");
        /* Cap the audio queue at 300ms (default is 1s): keeps the post-pause
         * drain tail and seek/restart resync transients short. Non-leaky (no
         * drops) so continuous audio stays glitch-free; it only bounds how much
         * gets buffered on a burst. */
        g_string_append(launch, "queue max-size-buffers=0 max-size-bytes=0 max-size-time=300000000 ! ");
        switch (i) {
        case 0:    /* AAC-ELD */
        case 2:    /* AAC-LC */
            if (aac) g_string_append(launch, "avdec_aac ! ");
            break;
        case 1:    /* ALAC */
            if (alac) g_string_append(launch, "avdec_alac ! ");
            break;
        case 3:   /*PCM*/
            break;
        default:
            break;
        }
        g_string_append (launch, "audioconvert ! ");
        g_string_append (launch, "audioresample quality=10 ! ");    /* maximum resampling quality for 44.1kHz -> 48kHz audio */
        g_string_append (launch, "volume name=volume ! ");

        if (!audio_rtp) {
            /* Normal path: local audio output */
            g_string_append (launch, "level ! ");
            g_string_append (launch, audiosink);
            switch(i) {
            case 1:  /*ALAC*/
                if (*audio_sync) {
                    g_string_append (launch, " sync=true");
                    async = TRUE;
                } else {
                    g_string_append (launch, " sync=false");
                    async = FALSE;
                }
                break;
            default:
                if (*video_sync) {
                    g_string_append (launch, " sync=true");
                    vsync = TRUE;
                } else {
                    g_string_append (launch, " sync=false");
                    vsync = FALSE;
                }
                break;
            }
        } else {
            /* RTP path: send decoded PCM over RTP */
            /* rtpL16pay requires S16BE (big-endian) format */
            g_string_append (launch, "audioconvert ! audio/x-raw,format=S16BE,rate=44100,channels=2 ! ");
            g_string_append (launch, "rtpL16pay ");
            g_string_append (launch, artp_pipeline);
        }
        renderer_type[i]->pipeline  = gst_parse_launch(launch->str, &error);
	if (error) {
          g_error ("gst_parse_launch error (audio %d):\n %s\n", i+1, error->message);
          g_clear_error (&error);
        }

        g_assert (renderer_type[i]->pipeline);
        gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
        install_av_sync_probe(renderer_type[i]->pipeline);
        install_decode_probe(renderer_type[i]->pipeline);
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
        renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "audio_source");
        renderer_type[i]->volume = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "volume");
        switch (i) {
        case 0:
            caps =  gst_caps_from_string(aac_eld_caps);
            renderer_type[i]->ct = 8;
            format[i] = "AAC-ELD 44100/2";
            break;
        case 1:
            caps =  gst_caps_from_string(alac_caps);
            renderer_type[i]->ct = 2;
            format[i] = "ALAC 44100/16/2";
            break;
        case 2:
            caps =  gst_caps_from_string(aac_lc_caps);
            renderer_type[i]->ct = 4;
            format[i] = "AAC-LC 44100/2";
            break;
        case 3:
            caps =  gst_caps_from_string(lpcm_caps);
            renderer_type[i]->ct = 1;
            format[i] = "PCM 44100/16/2 S16LE";
            break;
        default:
            break;
        }
        logger_log(logger, LOGGER_DEBUG, "Audio format %d: %s",i+1,format[i]);
        logger_log(logger, LOGGER_DEBUG, "GStreamer audio pipeline %d: \"%s\"", i+1, launch->str);
        g_string_free(launch, TRUE);
        g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
        gst_caps_unref(caps);
        g_object_unref(clock);
    }
}

void audio_renderer_stop() {
    if (renderer) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        renderer = NULL;
    }
}

static void get_renderer_type(unsigned char *ct, int *id) {
    render_audio = FALSE;
    *id = -1;
    for (int i = 0; i < NFORMATS; i++) {
        if (renderer_type[i]->ct == *ct) {
	    *id = i;
            break;
        }
    }
    switch (*id) {
    case 2:
    case 0:
        if (aac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_aac is missing, cannot decode AAC audio");
        }
        sync = vsync;
        break;
    case 1:
        if (alac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_alac is missing, cannot decode ALAC audio");
        }
        sync = async;
        break;
    case 3:
        render_audio = TRUE;
	sync = FALSE;
        break;
    default:
        break;
    }
}

void  audio_renderer_start(unsigned char *ct) {
    int id = -1;
    get_renderer_type(ct, &id);
    if (id >= 0 && renderer) {
        if (*ct != renderer->ct) {
            /* Genuine codec change: tear down the old pipeline and bring up
             * the new one -- gst_app_src_end_of_stream() permanently marks
             * an appsrc EOS'd (cycling the pipeline's state afterward does
             * NOT clear it; every push after that fails with GST_FLOW_EOS),
             * so this sequence is only safe when abandoning this renderer
             * for a different one, never when reusing the same one. */
            gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
            gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_INFO, "changed audio connection, format %s", format[id]);
            renderer = renderer_type[id];
            gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
            gst_audio_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
            tt_calls_since_start = 0;
        } else {
            /* Same-codec restart (a client tearing down and re-SETUPing
             * the same AAC-ELD/ALAC stream, e.g. on a track switch): only
             * the clock reference needs refreshing for the new session. Do
             * NOT send EOS or cycle pipeline state here (see the comment
             * above -- that permanently breaks the appsrc). */
            logger_log(logger, LOGGER_INFO, "restarting audio connection, format %s", format[id]);
            gst_audio_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
            tt_calls_since_start = 0;
            TT_DIAG("threadtest: main-thread BASE-TIME-REFRESHED t=%.6f\n", tt_diag_now());
        }
    } else if (id >= 0) {
        logger_log(logger, LOGGER_INFO, "start audio connection, format %s", format[id]);
        renderer = renderer_type[id];
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
        gst_audio_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
        tt_calls_since_start = 0;
    } else {
        logger_log(logger, LOGGER_ERR, "unknown audio compression type ct = %d", *ct);
    }
}

/* audio_renderer_start()/_stop() are only invoked from these two deferred
 * entry points (see docs/audio-pipeline.md): g_idle_add() onto the main
 * thread's GMainLoop serializes the httpd thread's SETUP-triggered start
 * and the RAOP audio thread's self-heal restart without a lock, so both
 * only ever run on the main thread. Every other existing caller of the
 * synchronous audio_renderer_start()/_stop() is unaffected
 * (audio_renderer_destroy() in particular relies on audio_renderer_stop()
 * completing synchronously before it frees the structures `renderer`
 * points into). Packs the single `ct` byte directly into the gpointer via
 * GUINT_TO_POINTER -- no heap allocation needed for something this small. */
static gboolean audio_renderer_deferred_start_cb(gpointer data) {
    unsigned char ct = (unsigned char) GPOINTER_TO_UINT(data);
    TT_DIAG("threadtest: main-thread DEFERRED-START-RUNNING t=%.6f\n", tt_diag_now());
    audio_renderer_start(&ct);
    return G_SOURCE_REMOVE;
}

void audio_renderer_start_deferred(unsigned char compression_type) {
    g_idle_add(audio_renderer_deferred_start_cb, GUINT_TO_POINTER((guint) compression_type));
}

/* The self-heal path needs stop()+start() to run as one atomic pair on
 * the main thread (not two separate idle callbacks, which another thread
 * could interleave between) -- and needs stop() to actually run (not be
 * skipped) so audio_renderer_start()'s own "same format as current
 * renderer -> no-op" branch doesn't swallow the restart, since the ct is
 * unchanged across a self-heal. */
static gboolean audio_renderer_deferred_self_heal_cb(gpointer data) {
    unsigned char ct = (unsigned char) GPOINTER_TO_UINT(data);
    audio_renderer_stop();
    audio_renderer_start(&ct);
    return G_SOURCE_REMOVE;
}

void audio_renderer_self_heal_deferred(unsigned char compression_type) {
    g_idle_add(audio_renderer_deferred_self_heal_cb, GUINT_TO_POINTER((guint) compression_type));
}

void audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;

    if (!render_audio) return;    /* do nothing unless render_audio == TRUE */

    if (tt_calls_since_start < 3) {
        TT_DIAG("threadtest: RAOP-audio-thread RENDER-BUFFER-CALL t=%.6f seqnum=%u ntp_time=%llu"
                " base_time=%llu (pts_would_be_negative=%d)\n", tt_diag_now(), (unsigned) *seqnum,
                (unsigned long long) *ntp_time, (unsigned long long) gst_audio_pipeline_base_time,
                (int) (*ntp_time < (uint64_t) gst_audio_pipeline_base_time));
        tt_calls_since_start++;
    }

    GstClockTime pts = (GstClockTime) *ntp_time ;    /* now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_audio_pipeline_base_time) {
            pts -= gst_audio_pipeline_base_time;
        } else {
            /* ntp < base_time: a clock jump from a seek/reconnect. Don't drop all
             * audio (that made the sound "fly off") — re-base to this frame and
             * keep playing; subsequent frames get valid PTS relative to it. */
            logger_log(logger, LOGGER_DEBUG, "audio ntp < base_time; re-basing audio clock (seek/reconnect)");
            gst_audio_pipeline_base_time = pts;
            pts = 0;
        }
    }
    if (data_len == 0 || renderer == NULL) return;

    /* all audio received seems to be either ct = 8 (AAC_ELD 44100/2 spf 460 ) AirPlay Mirror protocol *
     * or ct = 2 (ALAC 44100/16/2 spf 352) AirPlay protocol.                                           *
     * first byte data[0] of ALAC frame is 0x20,                                                       *
     * first byte of AAC_ELD is 0x8c, 0x8d or 0x8e: 0x100011(00,01,10) in modern devices               *
     *                   but is 0x80, 0x81 or 0x82: 0x100000(00,01,10) in ios9, ios10 devices          *
     * first byte of AAC_LC should be 0xff (ADTS) (but has never been  seen).                          */
    
    buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
    g_assert(buffer != NULL);
    //g_print("audio latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
    if (sync) {
        GST_BUFFER_PTS(buffer) = pts;
    }
    gst_buffer_fill(buffer, 0, data, *data_len);
    bool valid = false;
    switch (renderer->ct){
    case 8: /*AAC-ELD*/
        switch (data[0]){
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x80:
        case 0x81:
        case 0x82:
            valid = true;
            break;          
        default:
            valid = false;
            break;
        }
        break;
    case 2: /*ALAC*/
        valid = (data[0] == 0x20);
        break;
    case 4:  /*AAC_LC */
        valid = (data[0] == 0xff );
 	break;
    default:
        valid = true;
        break;
    }
    if (valid) {
        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            /* appsrc has stopped accepting data -- most likely GST_FLOW_EOS,
             * if gst_app_src_end_of_stream() was called on this appsrc (via
             * audio_renderer_stop(), on every RAOP disconnect/reconnect
             * cycle) and something about the following state transition
             * didn't fully clear that internally. Previously this failure
             * was completely silent: audio would just stop forever, with
             * zero log trace, recoverable only by the client tearing down
             * and re-establishing the whole AirPlay session from scratch
             * (which goes through the exact same audio_renderer_stop()/
             * audio_renderer_start() pair below, just triggered
             * externally). Self-heal the same way here instead of waiting
             * on the user to notice and manually reconnect. */
            logger_log(logger, LOGGER_ERR, "*** ERROR gst_app_src_push_buffer failed, GstFlowReturn = %d (%s); restarting audio renderer",
                       ret, gst_flow_get_name(ret));
            /* Deferred (see audio_renderer_self_heal_deferred()'s own
             * comment above): this runs on the RAOP audio thread, and the
             * httpd thread can be calling audio_renderer_start() at the
             * same moment for a concurrently-arriving SETUP request. */
            audio_renderer_self_heal_deferred(renderer->ct);
        }
    } else {
        logger_log(logger, LOGGER_ERR, "*** ERROR invalid  audio frame (compression_type %d) skipped ", renderer->ct);
        logger_log(logger, LOGGER_ERR, "***       first byte of invalid frame was  0x%2.2x ", (unsigned int) data[0]);
    }
}

void audio_renderer_set_volume(double volume) {
    if (!renderer) {
       return;
    }
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_object_set(renderer->volume, "volume", volume, NULL);
}

void audio_renderer_flush() {
    /* AirPlay FLUSH = seek/pause: drop all audio buffered in the pipeline so the
     * stale pre-seek audio doesn't keep playing and drag A/V out of sync. With
     * sync=false the sink plays buffers as they arrive, so resetting the segment
     * (flush_stop reset_time=TRUE) is harmless for timing; appsrc emits a fresh
     * segment on its next buffer. Flush the whole pipeline so appsrc's internal
     * queue, the queue element, decoder and alsasink are all cleared. */
    if (renderer && renderer->pipeline) {
        gst_element_send_event(renderer->pipeline, gst_event_new_flush_start());
        gst_element_send_event(renderer->pipeline, gst_event_new_flush_stop(TRUE));
    }
}

void audio_renderer_destroy() {
    audio_renderer_stop();
    for (int i = 0; i < NFORMATS ; i++ ) {
        gst_object_unref (renderer_type[i]->bus);
        renderer_type[i]->bus = NULL;
        gst_object_unref (renderer_type[i]->volume);
        renderer_type[i]->volume = NULL;
        gst_object_unref (renderer_type[i]->appsrc);
        renderer_type[i]->appsrc = NULL;
        gst_object_unref (renderer_type[i]->pipeline);
        renderer_type[i]->pipeline = NULL;
        free(renderer_type[i]);
    }
}

static gboolean gstreamer_audio_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (audio): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        g_error_free(err);
        g_free(debug);
        /* renderer can be NULL here: e.g. this bus watch can fire for a pipeline
         * whose renderer_type[] slot was never made the active "renderer" (audio
         * mode picks one of several format-specific pipelines), or during teardown. */
        if (renderer && renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_bus_set_flushing(bus, TRUE);
        if (renderer) {
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        }
        g_main_loop_quit( (GMainLoop *) loop);
	break;
    }
    case GST_MESSAGE_EOS:
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (audio)");
        break;
    case GST_MESSAGE_ELEMENT:
      // many "level" messages may be sent
        break;
    default:
        /* unhandled message */
        logger_log(logger, LOGGER_DEBUG,"GStreamer unhandled audio bus message: src = %s type = %s",
                   GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message));
        break;
    }
    return TRUE;
}

unsigned int audio_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < NFORMATS);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_audio_pipeline_bus_callback, (gpointer) loop); 
}
