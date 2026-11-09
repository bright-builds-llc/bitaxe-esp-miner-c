#ifndef BWG_MARK_VALID_H
#define BWG_MARK_VALID_H

#include "global_state.h"

/**
 * Replaces upstream's unconditional mark-valid. An image on trial is marked valid only after its startup
 * check (settings read back cleanly and the Gate link is up where the Gate runs), and rolls back if the
 * check hasn't passed by the deadline. Call once, after NVS is initialized.
 */
void bwg_mark_valid_start(const GlobalState *GLOBAL_STATE);

/** The Gate link is up. */
void bwg_mark_valid_note_link_ready(void);

/** This image has no Gate provisioning for the board, so the check doesn't wait for a link. */
void bwg_mark_valid_note_no_link(void);

#endif // BWG_MARK_VALID_H
