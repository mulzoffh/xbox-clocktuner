#include "settings.h"
#include "platform.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lower_(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int str_ieq(const char *a, const char *b)
{
    while (*a && *b && lower_(*a) == lower_(*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

void kv_load(kv_t *kv, const char *path)
{
    static char buf[16384];
    kv->n = 0;
    if (file_read(path, buf, sizeof(buf)) < 0) return;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char tmp[256];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        trim(tmp);
        char *eq = strchr(tmp, '=');
        if (tmp[0] && tmp[0] != ';' && tmp[0] != '#' && eq) {
            *eq = 0;
            char k[64], v[256];
            strncpy(k, tmp, sizeof(k) - 1); k[sizeof(k) - 1] = 0; trim(k);
            strncpy(v, eq + 1, sizeof(v) - 1); v[sizeof(v) - 1] = 0; trim(v);
            kv_set(kv, k, v);
        }
        line = nl ? nl + 1 : NULL;
    }
}

int kv_save(const kv_t *kv, const char *path, const char *header)
{
    static char buf[16384];
    size_t n = 0;
    if (header) n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s", header);
    for (int i = 0; i < kv->n && n < sizeof(buf) - 300; i++)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s = %s\r\n", kv->it[i].key, kv->it[i].val);
    return file_write(path, buf, n);
}

const char *kv_get(const kv_t *kv, const char *key, const char *def)
{
    for (int i = 0; i < kv->n; i++)
        if (str_ieq(kv->it[i].key, key)) return kv->it[i].val;
    return def;
}

long kv_int(const kv_t *kv, const char *key, long def)
{
    const char *v = kv_get(kv, key, NULL);
    return (v && *v) ? strtol(v, NULL, 10) : def;
}

unsigned long kv_hex(const kv_t *kv, const char *key, unsigned long def)
{
    const char *v = kv_get(kv, key, NULL);
    return (v && *v) ? strtoul(v, NULL, 16) : def;
}

void kv_set(kv_t *kv, const char *key, const char *val)
{
    for (int i = 0; i < kv->n; i++)
        if (str_ieq(kv->it[i].key, key)) {
            strncpy(kv->it[i].val, val, sizeof(kv->it[i].val) - 1);
            kv->it[i].val[sizeof(kv->it[i].val) - 1] = 0;
            return;
        }
    if (kv->n >= (int)(sizeof(kv->it) / sizeof(kv->it[0]))) return;
    kv_item_t *it = &kv->it[kv->n++];
    strncpy(it->key, key, sizeof(it->key) - 1); it->key[sizeof(it->key) - 1] = 0;
    strncpy(it->val, val, sizeof(it->val) - 1); it->val[sizeof(it->val) - 1] = 0;
}

void kv_seti(kv_t *kv, const char *key, long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); kv_set(kv, key, b); }
void kv_setx(kv_t *kv, const char *key, unsigned long v) { char b[24]; snprintf(b, sizeof(b), "0x%06lX", v); kv_set(kv, key, b); }

/* ---------------- cerbios.ini ---------------- */
static char cini[65536];
static char cini_file[128];
static int cini_len = -1;

const char *cini_path(void) { return cini_file; }

int cini_load(const char *path)
{
    strncpy(cini_file, path, sizeof(cini_file) - 1);
    cini_len = file_read(path, cini, sizeof(cini) - 512);
    return cini_len >= 0;
}

/* find the line "key = value" (not commented); returns start of line or NULL */
static char *find_key(const char *key, char **line_end)
{
    if (cini_len < 0) return NULL;
    char *p = cini;
    size_t kl = strlen(key);
    while (*p) {
        char *e = strchr(p, '\n');
        char *end = e ? e : p + strlen(p);
        char *s = p;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s + kl <= end && *s != ';') {
            int match = 1;
            for (size_t i = 0; i < kl; i++) if (lower_(s[i]) != lower_(key[i])) { match = 0; break; }
            if (match) {
                char *q = s + kl;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                if (q < end && *q == '=') { *line_end = end; return p; }
            }
        }
        p = e ? e + 1 : end;
    }
    return NULL;
}

int cini_get(const char *key, char *out, size_t max)
{
    char *end;
    char *l = find_key(key, &end);
    if (!l) return 0;
    char *eq = memchr(l, '=', (size_t)(end - l));
    size_t n = (size_t)(end - eq - 1);
    if (n >= max) n = max - 1;
    memcpy(out, eq + 1, n);
    out[n] = 0;
    trim(out);
    return 1;
}

void cini_set(const char *key, const char *val)
{
    if (cini_len < 0) return;
    char repl[256];
    int rl = snprintf(repl, sizeof(repl), "%s = %s", key, val);
    char *end;
    char *l = find_key(key, &end);
    if (l) {
        if (end > l && end[-1] == '\r') end--;           /* keep the CR */
        size_t old = (size_t)(end - l);
        size_t tail = (size_t)(cini_len - (end - cini));
        if ((size_t)cini_len - old + (size_t)rl >= sizeof(cini) - 1) return;
        memmove(l + rl, end, tail + 1);
        memcpy(l, repl, (size_t)rl);
        cini_len = cini_len - (int)old + rl;
    } else {
        if ((size_t)cini_len + (size_t)rl + 4 >= sizeof(cini)) return;
        if (cini_len && cini[cini_len - 1] != '\n') { cini[cini_len++] = '\r'; cini[cini_len++] = '\n'; }
        memcpy(cini + cini_len, repl, (size_t)rl);
        cini_len += rl;
        cini[cini_len++] = '\r';
        cini[cini_len++] = '\n';
        cini[cini_len] = 0;
    }
}

int cini_save(void)
{
    if (cini_len < 0) return 0;
    char bak[140];
    snprintf(bak, sizeof(bak), "%s.bak", cini_file);
    if (!file_exists(bak)) CopyFileA(cini_file, bak, TRUE);
    return file_write(cini_file, cini, (size_t)cini_len);
}
