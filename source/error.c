/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include "error.h"

static int status_active;

void startup_status_update(const char *message) {
  if (!status_active) return;
  printf("\x1b[2J\x1b[H\n\n  Angry Birds 2\n\n  %s\n\n  Please wait...", message);
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) {
    consoleInit(NULL);
    status_active = 1;
  }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
}

void fatal_error(const char *fmt, ...) {
  (void)fmt;
  startup_status_end();
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(1);
}
