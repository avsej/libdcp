/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "logging.h"
#include "internal.h"
#include <stdarg.h>

#ifdef _WIN32
#    define flockfile(x) (void)0
#    define funlockfile(x) (void)0
#endif

#if defined(unix) || defined(__unix__) || defined(__unix) || defined(_POSIX_VERSION)
#    include <unistd.h>
#    include <pthread.h>
#    include <sys/types.h>
#    if defined(__linux__)
#        include <sys/syscall.h>
#        define GET_THREAD_ID() (long)syscall(SYS_gettid)
#        define THREAD_ID_FMT "ld"
#    elif defined(__APPLE__)
#        define GET_THREAD_ID() getpid(), pthread_mach_thread_np(pthread_self())
#        define THREAD_ID_FMT "d/%x"
#    elif defined(__sun) && defined(__SVR4)
#        include <thread.h>
#        define GET_THREAD_ID() getpid(), thr_self()
#        define THREAD_ID_FMT "ld/%u"
#    elif defined(__FreeBSD__)
#        include <sys/thr.h>
static long ret_thr_self(void)
{
    long tmp;
    thr_self(&tmp);
    return tmp;
}
#        define GET_THREAD_ID() getpid(), ret_thr_self()
#        define THREAD_ID_FMT "d/%ld"
#    else
#        define GET_THREAD_ID() 0
#        define THREAD_ID_FMT "d"
#    endif
#elif defined(_WIN32)
#    define GET_THREAD_ID() GetCurrentThreadId()
#    define THREAD_ID_FMT "d"
#else
#    define GET_THREAD_ID() 0
#    define THREAD_ID_FMT "d"
#endif

static hrtime_t start_time = 0;
struct ldcp_CONSOLELOGGER {
    struct ldcp_LOGGER base;
    FILE *fp;
    int minlevel;
};

static void console_log(struct ldcp_LOGGER *procs, unsigned int iid, const char *subsys, int severity,
                        const char *srcfile, int srcline, const char *fmt, va_list ap);

static struct ldcp_CONSOLELOGGER console_logger = {{0 /* version */, {{console_log} /* v1 */} /*v*/},
                                                   NULL,
                                                   /** Minimum severity */
                                                   LDCP_LOG_INFO};

struct ldcp_LOGGER *ldcp_console_logger = &console_logger.base;

/**
 * Return a string representation of the severity level
 */
static const char *level_to_string(int severity)
{
    switch (severity) {
        case LDCP_LOG_TRACE:
            return "TRACE";
        case LDCP_LOG_DEBUG:
            return "DEBUG";
        case LDCP_LOG_INFO:
            return "INFO";
        case LDCP_LOG_WARN:
            return "WARN";
        case LDCP_LOG_ERROR:
            return "ERROR";
        case LDCP_LOG_FATAL:
            return "FATAL";
        default:
            return "";
    }
}

/**
 * Default logging callback for the verbose logger.
 */
static void console_log(struct ldcp_LOGGER *procs, unsigned int iid, const char *subsys, int severity,
                        const char *srcfile, int srcline, const char *fmt, va_list ap)
{
    FILE *fp;
    hrtime_t now;
    struct ldcp_CONSOLELOGGER *vprocs = (struct ldcp_CONSOLELOGGER *)procs;

    if (severity < vprocs->minlevel) {
        return;
    }

    if (!start_time) {
        start_time = gethrtime();
    }

    now = gethrtime();
    if (now == start_time) {
        now++;
    }

    fp = vprocs->fp ? vprocs->fp : stderr;

    flockfile(fp);
    fprintf(fp, "%lums ", (unsigned long)(now - start_time) / 1000000);

    fprintf(fp, "[I%d] {%" THREAD_ID_FMT "} [%s] (%s - L:%d) ", iid, GET_THREAD_ID(), level_to_string(severity), subsys,
            srcline);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    funlockfile(fp);

    (void)procs;
    (void)srcfile;
}

LDCP_INTERNAL_API
void ldcp_log(const ldcp_SETTINGS *settings, const char *subsys, int severity, const char *srcfile, int srcline,
              const char *fmt, ...)
{
    va_list ap;
    ldcp_LOG_CALLBACK callback;

    if (!settings->logger) {
        return;
    }

    if (settings->logger->version != 0) {
        return;
    }

    callback = settings->logger->v.v0.callback;

    va_start(ap, fmt);
    callback(settings->logger, settings->iid, subsys, severity, srcfile, srcline, fmt, ap);
    va_end(ap);
}

LDCP_INTERNAL_API
struct ldcp_LOGGER *ldcp_init_console_logger(void)
{
    char vbuf[1024];
    char namebuf[PATH_MAX] = {0};
    int lvl = 0;
    int has_file = 0;

    has_file = ldcp_getenv_nonempty("LDCP_LOGFILE", namebuf, sizeof(namebuf));
    if (has_file && console_logger.fp == NULL) {
        FILE *fp = fopen(namebuf, "a");
        if (!fp) {
            fprintf(stderr, "libcouchbase: could not open file '%s' for logging output. (%s)\n", namebuf,
                    strerror(errno));
        }
        console_logger.fp = fp;
    }

    if (!ldcp_getenv_nonempty("LDCP_LOGLEVEL", vbuf, sizeof(vbuf))) {
        return NULL;
    }

    if (sscanf(vbuf, "%d", &lvl) != 1) {
        return NULL;
    }

    if (!lvl) {
        /** "0" */
        return NULL;
    }

    /** The "lowest" level we can expose is WARN, e.g. ERROR-1 */
    lvl = LDCP_LOG_ERROR - lvl;
    console_logger.minlevel = lvl;
    return ldcp_console_logger;
}
