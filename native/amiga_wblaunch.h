#ifndef AMIGA_WBLAUNCH_H
#define AMIGA_WBLAUNCH_H

/* Started from the Workbench icon: run the drawer's `run` script, wait for
 * it, return 1 - the caller then exits. Otherwise (a Shell start, no script,
 * or the script could not be started) return 0 and play. See the .c file. */
int amiga_wb_relaunch(void);

#endif
