#ifndef BWG_LINK_REPORT_H
#define BWG_LINK_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "bwg_writer.h"

/** Formats a receive failure still waiting to be reported, once. Returns false when there is none. */
bool bwg_link_report_format_rx_failure(char *line, size_t capacity);

/** Reports the receiver's first failure on the device log, with labels only. */
void bwg_link_report_note_rx_failure(const char *label, size_t observed_bytes);

/** Starts a fresh first-failure report for a new session. */
void bwg_link_report_clear_failures(void);

/** Writer task: keeps a failed record's report for the next session, once. A clean revoke between records is none. */
void bwg_link_report_note_tx_failure(const bwg_writer_result_t *result);
bool bwg_link_report_format_tx_failure(char *line, size_t capacity);

#endif // BWG_LINK_REPORT_H
