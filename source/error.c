/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include "error.h"

void fatal_error(const char *fmt, ...) {
  (void)fmt;
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(1);
}
