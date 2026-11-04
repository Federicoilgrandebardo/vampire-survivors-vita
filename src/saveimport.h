#ifndef SAVEIMPORT_H
#define SAVEIMPORT_H

int saveimport_start(void);           // 0 without network
void saveimport_stop(void);           // closes the server and drops anything received
void saveimport_poll(void);           // once per frame, never blocks
const char *saveimport_address(void);
char *saveimport_take(void);          // the uploaded file (text), or NULL; free with saveimport_free
void saveimport_free(char *s);

#endif
