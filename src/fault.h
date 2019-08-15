/*
 * Copyright (c) 2003 Viliam Holub
 * All rights reserved.
 *
 * Distributed under the terms of GPL.
 *
 *
 *  Fault handlers
 *
 */

#ifndef FAULT_H_
#define FAULT_H_

#define ERR_OK      0  /**< OK */
#define ERR_IO      1  /**< I/O error occures */
#define ERR_MEM     2  /**< Not enough memory */
#define ERR_INIT    3  /**< Initial script fails */
#define ERR_PARM    4  /**< Invalid parameter */
#define ERR_INTERN  5  /**< Internal error */

/** Print error message to stderr */
extern void error(const char *fmt, ...);

/** Print message to stderr and exits */
extern void die(int ex, const char *fmt, ...);

/** Dump error description of I/O error */
extern void io_error(const char *filename);

/** Like io_error() but call exit */
extern void io_die(int ex, const char *filename);

#endif
