/*
 * log.h
 *
 * $Id$
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>

#define logerror(msg) log(-1, "error: %s failed: %s", msg, strerror(errno))


/* Set the log level */
void set_log_level(int level);

/* get the log level */
int get_log_level();

/* Will log whatever if loglevel >= level */
void debuglog(int level, const char *format, ...);

void hexdump(char *buf, unsigned int len);

#endif
