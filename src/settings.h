#ifndef SETTINGS_H
#define SETTINGS_H
#include <stddef.h>

/* simple key = value files (tuner.ini, state.ini) */
typedef struct { char key[32]; char val[200]; } kv_item_t;
typedef struct { kv_item_t it[64]; int n; } kv_t;

void kv_load(kv_t *kv, const char *path);
int kv_save(const kv_t *kv, const char *path, const char *header);
const char *kv_get(const kv_t *kv, const char *key, const char *def);
long kv_int(const kv_t *kv, const char *key, long def);
unsigned long kv_hex(const kv_t *kv, const char *key, unsigned long def);
void kv_set(kv_t *kv, const char *key, const char *val);
void kv_seti(kv_t *kv, const char *key, long v);
void kv_setx(kv_t *kv, const char *key, unsigned long v);

/* cerbios.ini editing: keeps every other line and comment as-is */
int cini_load(const char *path);
const char *cini_path(void);
int cini_get(const char *key, char *out, size_t max);
void cini_set(const char *key, const char *val);
int cini_save(void);              /* makes cerbios.ini.bak once, then writes through */

int str_ieq(const char *a, const char *b);
#endif
