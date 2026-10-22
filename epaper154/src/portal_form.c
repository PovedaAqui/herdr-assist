/* Parsing do formulário do portal — C puro, testado no host. */

#include <stdbool.h>
#include <string.h>

#include "portal.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void portal_url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)(hexval(r[1]) * 16 + hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Copia o valor (decodificado) de `key` em body "a=1&b=2". false se a chave
 * não existe ou o valor não cabe em out (nunca trunca uma senha calado). */
bool portal_form_get(const char *body, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            char tmp[256];
            if (n >= sizeof(tmp)) return false;
            memcpy(tmp, v, n);
            tmp[n] = '\0';
            portal_url_decode(tmp);
            if (strlen(tmp) >= len) return false;
            strcpy(out, tmp);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}
