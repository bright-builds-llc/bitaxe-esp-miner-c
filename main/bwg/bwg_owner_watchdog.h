#ifndef BWG_OWNER_WATCHDOG_H
#define BWG_OWNER_WATCHDOG_H

#include <stdbool.h>

/** Subscribes the calling task, the Gate's owner, to the task watchdog. */
void bwg_owner_watchdog_start(void);

/** Feeds the watchdog when the caller is the owner, and does nothing for another task. */
void bwg_owner_watchdog_feed(void);

/** Whether the owner is subscribed, every feed has worked, and the last was within a second. */
bool bwg_owner_watchdog_alive(void);

#endif // BWG_OWNER_WATCHDOG_H
