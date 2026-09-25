/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "event_fifo.h"

#include <errno.h>

#ifndef _WIN32

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "lib/threads.h"

#define EVENT_SESSION_BEGIN "session-begin\n"
#define EVENT_SESSION_END   "session-end\n"

/* Emitters run on the main, httpd and RAOP mirror threads; the mutex covers
 * the state test and the write together, so delivered lines cannot invert. */
static mutex_handle_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static int event_fd = -1;
static int session_active = 0;
/* Non-NULL only between a successful open and close, so emits stay silent
 * when no -efifo was requested. */
static logger_t *event_logger = NULL;

int event_fifo_open(logger_t *logger, const char *path) {
    event_fifo_close();
    if (mkfifo(path, 0600) < 0 && errno != EEXIST) {
        return -1;
    }
    /* O_RDWR holds our own reader reference: no ENXIO when no consumer is
     * attached and no EPIPE/SIGPIPE when one leaves. A full FIFO then
     * gives EAGAIN, which drops the event. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    /* A pre-existing regular file would accept every event forever and lose
     * the <= PIPE_BUF write atomicity the line protocol relies on. */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    if (!S_ISFIFO(st.st_mode)) {
        if (logger) {
            logger_log(logger, LOGGER_ERR, "event fifo %s is not a fifo", path);
        }
        close(fd);
        errno = ENOTSUP;
        return -1;
    }
    MUTEX_LOCK(event_mutex);
    event_fd = fd;
    event_logger = logger;
    session_active = 0;
    MUTEX_UNLOCK(event_mutex);
    return 0;
}

static void emit(int active, const char *line) {
    MUTEX_LOCK(event_mutex);
    if (session_active != active) {
        size_t len = strlen(line);
        ssize_t written = (event_fd < 0 ? -1 : write(event_fd, line, len));
        if (written == (ssize_t) len) {
            session_active = active;
        } else if (event_logger) {
            logger_log(event_logger, LOGGER_ERR, "event fifo: dropped %.*s (%s)",
                       (int) len - 1, line, (event_fd < 0 ? "not open" : strerror(errno)));
        }
    }
    MUTEX_UNLOCK(event_mutex);
}

void event_fifo_session_begin(void) {
    emit(1, EVENT_SESSION_BEGIN);
}

void event_fifo_session_end(void) {
    emit(0, EVENT_SESSION_END);
}

void event_fifo_close(void) {
    MUTEX_LOCK(event_mutex);
    if (event_fd >= 0) {
        close(event_fd);
        event_fd = -1;
    }
    event_logger = NULL;
    MUTEX_UNLOCK(event_mutex);
}

#else /* _WIN32: no FIFOs, events are not emitted */

int event_fifo_open(logger_t *logger, const char *path) {
    (void) logger;
    (void) path;
    errno = ENOSYS;
    return -1;
}

void event_fifo_session_begin(void) {
}

void event_fifo_session_end(void) {
}

void event_fifo_close(void) {
}

#endif
