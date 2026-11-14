#ifndef TROPHIES_H
#define TROPHIES_H

void trophies_init(void (*frame)(void));  // after the window exists: may show the system's setup dialog; frame draws behind it
void trophies_unlock(const char *key);    // achievement key; does nothing without NoTrpDrm
int trophies_active(void);                // the system shows its own notifications

#endif
