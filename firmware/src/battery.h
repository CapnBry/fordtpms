#pragma once

void battery_init(void);
/* Battery voltage in mV (after the on-board divider), or -1 if unavailable. */
int battery_read_mv(void);
