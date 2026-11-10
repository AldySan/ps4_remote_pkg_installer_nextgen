/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Based on "Remote Package Installer" by flatz.
 *
 * Local controller input, polled straight from the pad service.
 */

#ifndef PAD_H
#define PAD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Opens the user's controller for per-frame polling. Never fatal: when the
 * pad service is unavailable the caller keeps running without local input
 * (control still works through the web dashboard) and pad_poll() reports
 * "nothing pressed".
 */
bool pad_init(int user_id);

/* Releases the controller handle. Safe to call when pad_init failed. */
void pad_fini(void);

/*
 * Reads the pad once and returns the buttons newly pressed since the
 * previous call: an edge-detected bitmask of ORBIS_PAD_BUTTON_* values with
 * the d-pad directions also synthesized from the left stick. Returns 0 when
 * there are no new presses or no pad is available.
 */
uint32_t pad_poll(void);

#endif /* PAD_H */
