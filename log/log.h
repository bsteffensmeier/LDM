/**
 * Copyright 2016 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file log.h
 * @author Steven R. Emmerson
 *
 * This file defines the API for LDM logging.
 */

#ifndef LOG_LOG_H_
#define LOG_LOG_H_

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <syslog.h>

/**
 * Macros for the `ulog` backend reproduced here with the "LOG_" prefix:
 */
#define LOG_LOCALTIME 0x100u ///< Use local time. Default is UTC.
#define LOG_NOTIME    0x200u ///< Don't add a timestamp
#define LOG_IDENT     0x400u ///< Add the facility identifier

/// Logging levels
typedef enum {
    LOG_LEVEL_DEBUG = 0,///< Debug messages
    LOG_LEVEL_INFO,     ///< Informational messages
    LOG_LEVEL_NOTICE,   ///< Notices
    LOG_LEVEL_WARNING,  ///< Warnings
    LOG_LEVEL_ERROR,    ///< Error messages
    LOG_LEVEL_ALERT,    ///< Unused
    LOG_LEVEL_CRIT,     ///< Unused
    LOG_LEVEL_EMERG,    ///< Unused
    LOG_LEVEL_COUNT     ///< Number of levels
} log_level_t;

/*
 * The declarations in the following header-file are package-private -- so don't
 * use them.
 */
#include "log_private.h"

#ifdef __cplusplus
    extern "C" {
#endif

/**
 * Returns the default destination for log messages if the process is a daemon
 * (i.e., doesn't have a controlling terminal).
 *
 * @retval ""   The system logging daemon
 * @return      The pathname of the standard LDM log file
 */
const char* log_get_default_daemon_destination(void);

/**
 * Returns the default destination for log messages, which depends on whether or
 * not log_avoid_stderr() has been called. If it hasn't been called, then the
 * default destination will be the standard error stream; otherwise, the default
 * destination will be that given by log_get_default_daemon_destination().
 *
 * @retval ""   The system logging daemon
 * @retval "-"  The standard error stream
 * @return      The pathname of the log file
 */
const char* log_get_default_destination(void);

/**
 * Indicates if the standard error file descriptor refers to a file that is not
 * `/dev/null`. This function may be called at any time.
 *
 * @retval true   Standard error file descriptor refers to a file that is not
 *               `/dev/null`
 * @retval false  Standard error file descriptor is closed or refers to
 *                `/dev/null`.
 */
bool log_is_stderr_useful(void);

/**
 * Initializes the logging module. Should be called before most other functions.
 * <dl>
 *     <dt>log_get_facility() <dd>will return `LOG_LDM`.
 *     <dt>log_get_level() <dd>will return `LOG_LEVEL_NOTICE`.
 * </dl>
 *
 * @param[in] id       The pathname of the program (e.g., `argv[0]`). Caller may
 *                     free.
 * @retval    0        Success.
 * @retval    -1       Error.
 */
int log_init(
        const char* const id);

/**
 * Tells this module to avoid using the standard error stream (because the
 * process has become a daemon, for example).
 */
void log_avoid_stderr(void);

/**
 * Refreshes the logging module. If logging is to the system logging daemon,
 * then it will continue to be. If logging is to a file, then the file is closed
 * and re-opened; thus enabling log file rotation. If logging is to the standard
 * error stream, then it will continue to be if log_avoid_stderr() hasn't been
 * called; otherwise, logging will be to the provider default.
 *
 * This function is async-signal-safe.
 */
void log_refresh(void);

/**
 * Finalizes the logging module. Should be called eventually after log_init(),
 * after which no more logging should occur.
 */
#define log_fini() do {\
    LOG_LOC_DECL(loc);\
    (void)log_fini_located(&loc);\
} while (false)

/**
 * Enables logging down to a given level. Should be called between
 * log_init() and log_fini().
 *
 * @param[in] level  The lowest level through which logging should occur. The
 *                   levels are ordered: LOG_LEVEL_DEBUG < LOG_LEVEL_INFO <
 *                   LOG_LEVEL_NOTICE  < LOG_LEVEL_WARNING <
 *                   LOG_LEVEL_ERROR.
 * @retval    0      Success.
 * @retval    -1     Failure.
 */
int log_set_level(
        const log_level_t level);

/**
 * Returns the current logging level. Should be called between log_init()
 * and log_fini().
 *
 * @return The lowest level through which logging will occur. The levels are
 *         ordered: LOG_LEVEL_ERROR > LOG_LEVEL_WARNING > LOG_LEVEL_NOTICE
 *         > LOG_LEVEL_INFO > LOG_LEVEL_DEBUG.
 */
log_level_t log_get_level(void);

/**
 * Lowers the logging threshold by one. Wraps at the bottom. Should be called
 * between log_init() and log_fini().
 */
void log_roll_level(void);

/**
 * Sets the logging identifier. Should be called between log_init() and
 * log_fini().
 *
 * @param[in] id        The new identifier. Caller may free.
 * @retval    0         Success.
 * @retval    -1        Failure.
 */
int log_set_id(
        const char* const id);

/**
 * Modifies the logging identifier. Should be called between log_init() and
 * log_fini().
 *
 * @param[in] hostId    The identifier of the remote host. Caller may free.
 * @param[in] isFeeder  Whether or not the process is sending data-products or
 *                      just notifications.
 * @retval    0         Success.
 */
int log_set_upstream_id(
        const char* const hostId,
        const bool        isFeeder);

/**
 * Returns the logging identifier. Should be called between log_init() and
 * log_fini().
 *
 * @return The logging identifier.
 */
const char* log_get_id(void);

/**
 * Sets the implementation-defined logging options. Should be called between
 * log_init() and log_fini().
 *
 * @param[in] options  The implementation-defined logging options.
 */
void log_set_options(
        const unsigned options);

/**
 * Returns the implementation-defined logging options. Should be called between
 * log_init() and log_fini().
 *
 * @return The implementation-defined logging options.
 */
unsigned log_get_options(void);

/**
 * Sets the facility that might be used (e.g., `LOG_LOCAL0`) when logging to the
 * system logging daemon. Should be called between log_init() and
 * log_fini(). May do nothing.
 *
 * @param[in] facility  The facility that will be used when logging to the
 *                      system logging daemon.
 * @retval    0         Success.
 * @retval    -1        Error.
 */
int log_set_facility(
        const int facility);

/**
 * Returns the facility that will be used (e.g., `LOG_LOCAL0`) when logging to
 * the system logging daemon. Should be called between log_init() and
 * log_fini().
 */
int log_get_facility(void);

/**
 * Sets the logging destination. Should be called between log_init() and
 * log_fini().
 *
 * @param[in] dest     The logging destination. Caller may free. One of <dl>
 *                         <dt>""   <dd>The system logging daemon.
 *                         <dt>"-"  <dd>The standard error stream.
 *                         <dt>else <dd>The file whose pathname is `dest`.
 *                     </dl>
 * @retval    0        Success.
 * @retval    -1       Failure.
 */
int log_set_destination(
        const char* const dest);

/**
 * Returns the logging destination. Should be called between log_init() and
 * log_fini().
 *
 * @return       The logging destination. One of <dl>
 *                   <dt>""      <dd>The system logging daemon.
 *                   <dt>"-"     <dd>The standard error stream.
 *                   <dt>else    <dd>The pathname of the log file.
 *               </dl>
 */
const char* log_get_destination(void);

/**
 * Clears the message-queue of the current thread.
 */
void log_clear(void);

/**
 * Frees the log-message resources of the current thread. Should only be called
 * when no more logging by the current thread will occur.
 */
#define log_free() do {\
    LOG_LOC_DECL(loc);\
    log_free_located(&loc);\
} while (false)

/**
 * Indicates if a message of the given level would be logged.
 *
 * @param[in] level  Logging level
 * @retval    true   iff a message of level `level` would be logged
 */
bool log_is_level_enabled(
        const log_level_t level);

/**
 * Indicates if a log message of WARNING level will be written. Useful if a
 * format argument of a message is expensive to evaluate.
 */
#define log_is_enabled_warning  log_is_level_enabled(LOG_LEVEL_WARNING)
/**
 * Indicates if a log message of NOTICE level will be written. Useful if a
 * format argument of a message is expensive to evaluate.
 */
#define log_is_enabled_notice   log_is_level_enabled(LOG_LEVEL_NOTICE)
/**
 * Indicates if a log message of INFO level will be written. Useful if a
 * format argument of a message is expensive to evaluate.
 */
#define log_is_enabled_info     log_is_level_enabled(LOG_LEVEL_INFO)
/**
 * Indicates if a log message of DEBUG level will be written. Useful if a
 * format argument of a message is expensive to evaluate.
 */
#define log_is_enabled_debug    log_is_level_enabled(LOG_LEVEL_DEBUG)

/**
 * Logs a single message at the DEBUG level, bypassing the message-queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_debug(...) do {\
    if (LOG_LEVEL_DEBUG >= log_level) {\
        LOG_LOC_DECL(loc);\
        logl_log_1(&loc, LOG_LEVEL_DEBUG, __VA_ARGS__);\
    }\
} while (0)

/**
 * Logs a single message at the INFO level, bypassing the message-queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_info(...) do {\
    if (LOG_LEVEL_INFO >= log_level) {\
        LOG_LOC_DECL(loc);\
        logl_log_1(&loc, LOG_LEVEL_INFO, __VA_ARGS__);\
    }\
} while (0)

/**
 * Logs a single message at the NOTICE level, bypassing the message-queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_notice(...) do {\
    if (LOG_LEVEL_NOTICE >= log_level) {\
        LOG_LOC_DECL(loc);\
        logl_log_1(&loc, LOG_LEVEL_NOTICE, __VA_ARGS__);\
    }\
} while (0)

/**
 * Logs a single message at the WARNING level, bypassing the message-queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_warning(...) do {\
    if (LOG_LEVEL_WARNING >= log_level) {\
        LOG_LOC_DECL(loc);\
        logl_log_1(&loc, LOG_LEVEL_WARNING, __VA_ARGS__);\
    }\
} while (0)

/**
 * Logs a single message at the ERROR level, bypassing the message-queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_error(...) do {\
    if (LOG_LEVEL_ERROR >= log_level) {\
        LOG_LOC_DECL(loc);\
        logl_log_1(&loc, LOG_LEVEL_ERROR, __VA_ARGS__);\
    }\
} while (0)

/**
 * Logs a single message at the ERROR level based on a system error code (i.e.,
 * `errno`), bypassing the message queue.
 *
 * @param[in] ...  Optional arguments of the message.
 */
#define log_syserr(...) do {\
    LOG_LOC_DECL(loc);\
    logl_errno_1(&loc, errno, __VA_ARGS__);\
} while (0)

/**
 * Adds a message to the current thread's queue of messages:
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_add(...) do { \
    LOG_LOC_DECL(loc); \
    logl_add(&loc, __VA_ARGS__);\
} while (false)

/**
 * Adds a variadic message to the current thread's queue of messages:
 *
 * @param[in] fmt  The format of the message.
 * @param[in] args The arguments of the format.
 */
#define log_vadd(fmt, args) do { \
    LOG_LOC_DECL(loc); \
    logl_vadd(&loc, fmt, args); \
} while (false)

/**
 * Adds a message based on a system error number (e.g., `errno`) to the current
 * thread's queue of messages:
 *
 * @param[in] n    The system error number (e.g., `errno`).
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_add_errno(n, ...) do {\
    LOG_LOC_DECL(loc); \
    logl_add_errno(&loc, n, __VA_ARGS__); \
} while (false)

/**
 * Adds a message based on the system error code (i.e., `errno`) to the current
 * thread's queue of error messages:
 *
 * @param[in] ...  Optional arguments of the message.
 */
#define log_add_syserr(...)  log_add_errno(errno, __VA_ARGS__)

/**
 * The following macros add a message to the current thread's queue of
 * messages, logs the queue, and then clear the queue:
 *
 * The argument-list of the variadic macros must comprise at least a
 * `NULL` argument in order to avoid a syntax error.
 */

/**
 * Adds a variadic message to the message-queue of the current thread, logs the
 * queue at a given level, and then clears the queue.
 *
 * @param[in] level  The level at which to log the queue.
 * @param[in] fmt    Format of the message.
 * @param[in] args   The arguments of the format.
 */
#define log_vlog_q(level, fmt, args) do { \
    LOG_LOC_DECL(loc); \
    logl_vlog_q(&loc, level, fmt, args); \
} while (false)

/**
 * Adds a message to the current thread's queue of messages based on a system
 * error number (e.g., `errno`), logs the queue at the error level, and then
 * clears the queue.
 *
 * @param[in] n    The system error number (e.g., `errno`).
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_errno_q(n, ...) do {\
    LOG_LOC_DECL(loc); \
    logl_errno_q(&loc, n, __VA_ARGS__); \
} while (false)

/**
 * Adds a message to the current thread's queue of messages based on the system
 * error number (i.e., `errno`), logs the queue at the error level, and then
 * clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_syserr_q(...)      log_errno_q(errno, __VA_ARGS__)

/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the ERROR level, and then clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_error_q(...)       LOG_LOG(LOG_LEVEL_ERROR,   __VA_ARGS__)
/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the WARNING level, and then clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_warning_q(...)     LOG_LOG(LOG_LEVEL_WARNING, __VA_ARGS__)
/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the NOTICE level, and then clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_notice_q(...)      LOG_LOG(LOG_LEVEL_NOTICE,  __VA_ARGS__)
/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the INFO level, and then clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_info_q(...)        LOG_LOG(LOG_LEVEL_INFO,    __VA_ARGS__)
/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the DEBUG level, and then clears the queue.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the format
 *                 of the message.
 */
#define log_debug_q(...) do {\
    LOG_LOC_DECL(loc);\
    logl_log_q(&loc, LOG_LEVEL_DEBUG, __VA_ARGS__);\
} while (0)
/**
 * Adds a message to the current thread's queue of messages, logs the queue at
 * the given level, and then clears the queue.
 *
 * @param[in] level  `log_level_t` logging level.
 * @param[in] ...    Optional arguments of the message -- starting with the
 *                   format of the message.
 */
#define log_log_q(level, ...)  LOG_LOG(level,             __VA_ARGS__)

/**
 * Logs the currently-accumulated log-messages of the current thread and resets
 * the message-queue for the current thread.
 *
 * @param[in] level  The level at which to log the messages. One of
 *                   LOG_LEVEL_ERROR, LOG_LEVEL_WARNING, LOG_LEVEL_NOTICE,
 *                   LOG_LEVEL_INFO, or LOG_LEVEL_DEBUG; otherwise, the
 *                   behavior is undefined.
 */
#define log_flush(level) do { \
    LOG_LOC_DECL(loc); \
    logl_flush(&loc, level); \
} while (false)

/**
 * Logs the message-queue of the current thread at the ERROR level and then
 * clears the queue.
 */
#define log_flush_error()    log_flush(LOG_LEVEL_ERROR)
/**
 * Logs the message-queue of the current thread at the WARNING level and then
 * clears the queue.
 */
#define log_flush_warning()  log_flush(LOG_LEVEL_WARNING)
/**
 * Logs the message-queue of the current thread at the NOTICE level and then
 * clears the queue.
 */
#define log_flush_notice()   log_flush(LOG_LEVEL_NOTICE)
/**
 * Logs the message-queue of the current thread at the INFO level and then
 * clears the queue.
 */
#define log_flush_info()     log_flush(LOG_LEVEL_INFO)
/**
 * Logs the message-queue of the current thread at the DEBUG level and then
 * clears the queue.
 */
#define log_flush_debug()    log_flush(LOG_LEVEL_DEBUG)

/**
 * Allocates memory. Adds an message to the current thread's queue of messages
 * if an error occurs.
 *
 * @param[in] nbytes    Number of bytes to allocate.
 * @param[in] msg       Message to print on error. Should complete the sentence
 *                      "Couldn't allocate <n> bytes for ...".
 * @retval    NULL      Out of memory. `log_add()` called.
 * @return              Pointer to the allocated memory.
 */
#define log_malloc(nbytes, msg) logl_malloc(__FILE__, __func__, __LINE__, \
        nbytes, msg)

/**
 * Re-allocates memory. Adds an message the current thread's queue of messages
 * if an error occurs.
 *
 * @param[in] buf       Previously-allocated buffer
 * @param[in] nbytes    Number of bytes to allocate.
 * @param[in] msg       Message to print on error. Should complete the sentence
 *                      "Couldn't allocate <n> bytes for ...".
 * @retval    NULL      Out of memory. Log message added.
 * @return              Pointer to the allocated memory.
 */
#define log_realloc(buf, nbytes, msg) logl_realloc(__FILE__, __func__, __LINE__, \
        buf, nbytes, msg)

/**
 * Logs an error message and then aborts the current process.
 *
 * @param[in] ...  Optional arguments of the message -- starting with the
 *                 format of the message.
 */
#define log_abort(...) do { \
    log_error_q(__VA_ARGS__); \
    abort(); \
} while (false)

#ifdef NDEBUG
    #define log_assert(expr)
#else
    /**
     * Tests an assertion. Logs an error-message and then aborts the process
     * if the assertion is false.
     *
     * @param[in] expr  The assertion to be tested.
     */
    #define log_assert(expr) do { \
        if (!(expr)) \
            log_abort("Assertion failure: %s", #expr); \
    } while (false)
#endif

#ifdef __cplusplus
    }
#endif

#endif /* LOG_LOG_H_ */
