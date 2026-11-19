/* SPDX-License-Identifier: MPL-2.0 */
/* The simulator's FM-1 on USB-A (tests): see fm1sim.c. Only in the simulator's host (RFM14_PLAT_SIM). */
#ifndef RFM14_FM1SIM_H
#define RFM14_FM1SIM_H

int fm1sim_start(void);                            /* after engine_start, before Move Remote's platform runs */
void fm1sim_stop(void);
int fm1sim_cmd(const char *op, long a, long b);    /* 0 done; -1 unknown, no unit plugged in, or not started */
int fm1sim_json(char *out, int cap);

#endif
