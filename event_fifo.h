/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef EVENT_FIFO_H
#define EVENT_FIFO_H

#include "lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Outgoing session-event channel (-efifo): one "session-begin\n" or
 * "session-end\n" line per state transition, for a consumer that needs to
 * know when a mirroring session starts and stops without parsing the log. */

/* Delivered lines strictly alternate, but a dropped write swallows a later
 * transition, so the last line can read "session-end" mid-session (a begin
 * was dropped) or "session-begin" while idle (an end was dropped). */

/* The write fd is held from event_fifo_open() to event_fifo_close(), and
 * uxplay calls the pair once per run, so a FIFO replaced at that path in
 * between cannot be repaired from a consumer's side. */

/* Creates the FIFO if absent and opens it non-blocking for writing; logger
 * may be NULL. Returns 0, or -1 with errno set (ENOTSUP: the path exists
 * and is not a FIFO). */
int event_fifo_open(logger_t *logger, const char *path);

/* Emitted once per mirror connection, so a dropped write (no consumer, full
 * FIFO) is logged but never retried, and the unchanged state then swallows
 * that session's end: the last line can read "session-end" mid-session. */
void event_fifo_session_begin(void);

/* Prompt on TEARDOWN and on a mirror connection reset; a mirror socket
 * closed with FIN and no TEARDOWN waits for the missed-feedback timeout
 * ("-reset n" seconds, default 60). */
void event_fifo_session_end(void);

void event_fifo_close(void);

#ifdef __cplusplus
}
#endif

#endif //EVENT_FIFO_H
