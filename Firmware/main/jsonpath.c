// Pure JSON path scanner: see jsonpath.h. No heap, no ESP-IDF, no libm.
#include "jsonpath.h"

#include <stdint.h>
#include <string.h>

#define MAX_STEPS 16
#define MAX_DEPTH 32

typedef struct {
    int      is_index;
    uint32_t index;
    char     key[JSONPATH_PATH_MAX + 1];
} step_t;

typedef struct {
    const char *p;
    int         depth;     // current container nesting
} scan_t;

// Result of reading one string into an optional destination.
typedef struct {
    char  *dst;            // NULL: compare only / skip
    size_t cap;            // bytes available in dst including the NUL
    size_t len;            // bytes produced so far (counted even on overflow)
    int    overflow;
} sink_t;

static void skip_ws(scan_t *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static void sink_put(sink_t *k, char c)
{
    if (k->dst && k->len + 1 < k->cap) {
        k->dst[k->len] = c;
    } else if (k->dst) {
        k->overflow = 1;
    }
    k->len++;
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;      // also catches the NUL terminator
    }
    *out = v;
    return 1;
}

static void put_utf8(sink_t *k, uint32_t cp)
{
    if (cp < 0x80) {
        sink_put(k, (char)cp);
    } else if (cp < 0x800) {
        sink_put(k, (char)(0xC0 | (cp >> 6)));
        sink_put(k, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sink_put(k, (char)(0xE0 | (cp >> 12)));
        sink_put(k, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sink_put(k, (char)(0x80 | (cp & 0x3F)));
    } else {
        sink_put(k, (char)(0xF0 | (cp >> 18)));
        sink_put(k, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sink_put(k, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sink_put(k, (char)(0x80 | (cp & 0x3F)));
    }
}

// s->p is at the opening quote. Unescape into the sink (dst may be NULL to
// count only). Leaves s->p after the closing quote. Returns OK or BAD_JSON.
static jsonpath_err_t read_string(scan_t *s, sink_t *k)
{
    const char *p = s->p + 1;
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (c == '\0') return JSONPATH_BAD_JSON;
        if (c == '"') { p++; break; }
        if (c < 0x20) return JSONPATH_BAD_JSON;       // raw control character
        if (c != '\\') { sink_put(k, (char)c); p++; continue; }
        p++;
        switch (*p) {
        case '"':  sink_put(k, '"');  p++; break;
        case '\\': sink_put(k, '\\'); p++; break;
        case '/':  sink_put(k, '/');  p++; break;
        case 'b':  sink_put(k, '\b'); p++; break;
        case 'f':  sink_put(k, '\f'); p++; break;
        case 'n':  sink_put(k, '\n'); p++; break;
        case 'r':  sink_put(k, '\r'); p++; break;
        case 't':  sink_put(k, '\t'); p++; break;
        case 'u': {
            uint32_t cp, lo;
            if (!hex4(p + 1, &cp)) return JSONPATH_BAD_JSON;
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &lo)
                    || lo < 0xDC00 || lo > 0xDFFF) {
                    return JSONPATH_BAD_JSON;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return JSONPATH_BAD_JSON;
            } else if (cp == 0) {
                return JSONPATH_BAD_JSON;
            }
            put_utf8(k, cp);
            break;
        }
        default:
            return JSONPATH_BAD_JSON;
        }
    }
    s->p = p;
    return JSONPATH_OK;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// s->p at the first character of a number. Validates the JSON grammar and
// reports the span [start, s->p).
static jsonpath_err_t scan_number(scan_t *s)
{
    const char *p = s->p;
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (is_digit(*p)) p++;
    } else {
        return JSONPATH_BAD_JSON;
    }
    if (*p == '.') {
        p++;
        if (!is_digit(*p)) return JSONPATH_BAD_JSON;
        while (is_digit(*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!is_digit(*p)) return JSONPATH_BAD_JSON;
        while (is_digit(*p)) p++;
    }
    // A number must be followed by a delimiter, not more value characters.
    if (*p && !strchr(" \t\r\n,]}", *p)) return JSONPATH_BAD_JSON;
    s->p = p;
    return JSONPATH_OK;
}

static jsonpath_err_t scan_literal(scan_t *s, const char *lit)
{
    size_t n = strlen(lit);
    if (strncmp(s->p, lit, n) != 0) return JSONPATH_BAD_JSON;
    if (s->p[n] && !strchr(" \t\r\n,]}", s->p[n])) return JSONPATH_BAD_JSON;
    s->p += n;
    return JSONPATH_OK;
}

static jsonpath_err_t skip_value(scan_t *s);

// s->p at '{' or '['. Skips the whole container.
static jsonpath_err_t skip_container(scan_t *s)
{
    char open = *s->p, close = (open == '{') ? '}' : ']';
    jsonpath_err_t e;

    if (++s->depth > MAX_DEPTH) return JSONPATH_BAD_JSON;
    s->p++;
    skip_ws(s);
    if (*s->p == close) {
        s->p++;
        s->depth--;
        return JSONPATH_OK;
    }
    for (;;) {
        skip_ws(s);
        if (open == '{') {
            sink_t none = { NULL, 0, 0, 0 };
            if (*s->p != '"') return JSONPATH_BAD_JSON;
            if ((e = read_string(s, &none)) != JSONPATH_OK) return e;
            skip_ws(s);
            if (*s->p != ':') return JSONPATH_BAD_JSON;
            s->p++;
            skip_ws(s);
        }
        if ((e = skip_value(s)) != JSONPATH_OK) return e;
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == close) { s->p++; break; }
        return JSONPATH_BAD_JSON;
    }
    s->depth--;
    return JSONPATH_OK;
}

static jsonpath_err_t skip_value(scan_t *s)
{
    switch (*s->p) {
    case '{':
    case '[': return skip_container(s);
    case '"': { sink_t none = { NULL, 0, 0, 0 }; return read_string(s, &none); }
    case 't': return scan_literal(s, "true");
    case 'f': return scan_literal(s, "false");
    case 'n': return scan_literal(s, "null");
    default:  return scan_number(s);
    }
}

// Copy the scalar at s->p into out. NOT_SCALAR for object/array/bool/null.
static jsonpath_err_t take_scalar(scan_t *s, char *out, size_t outsz)
{
    jsonpath_err_t e;
    char c = *s->p;

    if (c == '{' || c == '[' || c == 't' || c == 'f' || c == 'n') {
        // Validate literals so "tru" is BAD_JSON, not NOT_SCALAR.
        if (c == 't' || c == 'f' || c == 'n') {
            e = skip_value(s);
            return e == JSONPATH_OK ? JSONPATH_NOT_SCALAR : e;
        }
        return JSONPATH_NOT_SCALAR;
    }
    if (c == '"') {
        sink_t k = { out, outsz, 0, 0 };
        if ((e = read_string(s, &k)) != JSONPATH_OK) return e;
        if (k.overflow || k.len + 1 > outsz) return JSONPATH_TOO_LONG;
        out[k.len] = '\0';
        return JSONPATH_OK;
    }
    {
        const char *start = s->p;
        size_t n;
        if ((e = scan_number(s)) != JSONPATH_OK) return e;
        n = (size_t)(s->p - start);
        if (n + 1 > outsz) return JSONPATH_TOO_LONG;
        memcpy(out, start, n);
        out[n] = '\0';
        return JSONPATH_OK;
    }
}

// Does the string at s->p (a quote) equal key? Consumes the string.
static jsonpath_err_t key_equals(scan_t *s, const char *key, int *match)
{
    char buf[JSONPATH_PATH_MAX + 1];
    sink_t k = { buf, sizeof(buf), 0, 0 };
    jsonpath_err_t e = read_string(s, &k);
    if (e != JSONPATH_OK) return e;
    *match = !k.overflow && k.len == strlen(key) && memcmp(buf, key, k.len) == 0;
    return JSONPATH_OK;
}

// Walk into the container at s->p to find step st; leave s->p at the value.
static jsonpath_err_t descend(scan_t *s, const step_t *st)
{
    jsonpath_err_t e;
    char open = *s->p;
    char close;
    uint32_t idx = 0;

    if (st->is_index ? open != '[' : open != '{') {
        if (open != '{' && open != '[') {
            // Scalar (or garbage) where a container was expected.
            scan_t probe = *s;
            e = skip_value(&probe);
            if (e != JSONPATH_OK) return e;
        }
        return JSONPATH_NOT_FOUND;
    }
    close = (open == '{') ? '}' : ']';
    if (++s->depth > MAX_DEPTH) return JSONPATH_BAD_JSON;
    s->p++;
    skip_ws(s);
    if (*s->p == close) return JSONPATH_NOT_FOUND;   // empty container
    for (;;) {
        int match;
        skip_ws(s);
        if (open == '{') {
            if (*s->p != '"') return JSONPATH_BAD_JSON;
            if ((e = key_equals(s, st->key, &match)) != JSONPATH_OK) return e;
            skip_ws(s);
            if (*s->p != ':') return JSONPATH_BAD_JSON;
            s->p++;
            skip_ws(s);
        } else {
            match = (idx == st->index);
        }
        if (match) {
            if (*s->p == '\0') return JSONPATH_BAD_JSON;
            return JSONPATH_OK;
        }
        if ((e = skip_value(s)) != JSONPATH_OK) return e;
        skip_ws(s);
        if (*s->p == ',') { s->p++; idx++; continue; }
        if (*s->p == close) return JSONPATH_NOT_FOUND;
        return JSONPATH_BAD_JSON;
    }
}

static int parse_path(const char *path, step_t *steps, int *nsteps)
{
    size_t len = strlen(path);
    const char *p = path;
    int n = 0;
    int first = 1;

    if (len == 0 || len > JSONPATH_PATH_MAX) return 0;
    while (*p) {
        if (n >= MAX_STEPS) return 0;
        if (*p == '[') {
            uint32_t v = 0;
            int digits = 0;
            p++;
            while (is_digit(*p)) {
                if (v > (UINT32_MAX - 9) / 10) return 0;
                v = v * 10 + (uint32_t)(*p - '0');
                digits++;
                p++;
            }
            if (!digits || *p != ']') return 0;
            p++;
            steps[n].is_index = 1;
            steps[n].index = v;
            steps[n].key[0] = '\0';
            n++;
        } else {
            size_t klen = 0;
            if (!first) {
                // A key after the first step needs a dot unless it follows
                // an index ("a[0].b"); a bare key right after "]" is invalid.
                return 0;
            }
            while (*p && *p != '.' && *p != '[' && *p != ']') klen++, p++;
            if (klen == 0 || klen > JSONPATH_PATH_MAX) return 0;
            steps[n].is_index = 0;
            steps[n].index = 0;
            memcpy(steps[n].key, p - klen, klen);
            steps[n].key[klen] = '\0';
            n++;
        }
        first = 0;
        if (*p == '.') {
            p++;
            if (*p == '\0' || *p == '[' || *p == '.' || *p == ']') return 0;
            first = 1;          // a key must follow a dot
        } else if (*p == ']') {
            return 0;
        }
    }
    *nsteps = n;
    return n > 0;
}

static jsonpath_err_t extract_impl(const char *json, const char *path,
                                   char *out, size_t outsz)
{
    step_t steps[MAX_STEPS];
    int nsteps = 0;
    scan_t s;
    jsonpath_err_t e;

    if (out && outsz) out[0] = '\0';
    if (!json || !path || !out) return JSONPATH_BAD_PATH;
    if (!parse_path(path, steps, &nsteps)) return JSONPATH_BAD_PATH;

    s.p = json;
    s.depth = 0;
    skip_ws(&s);
    if (*s.p == '\0') return JSONPATH_BAD_JSON;

    for (int i = 0; i < nsteps; i++) {
        if ((e = descend(&s, &steps[i])) != JSONPATH_OK) return e;
        skip_ws(&s);
    }
    if (outsz == 0) return JSONPATH_TOO_LONG;
    return take_scalar(&s, out, outsz);
}

jsonpath_err_t jsonpath_extract(const char *json, const char *path,
                                char *out, size_t outsz)
{
    jsonpath_err_t e = extract_impl(json, path, out, outsz);
    // A failed scan may have written part of a string: never leave out
    // unterminated or holding a partial value.
    if (e != JSONPATH_OK && out && outsz) out[0] = '\0';
    return e;
}

const char *jsonpath_err_str(jsonpath_err_t e)
{
    switch (e) {
    case JSONPATH_OK:         return "ok";
    case JSONPATH_BAD_PATH:   return "bad path";
    case JSONPATH_BAD_JSON:   return "invalid JSON";
    case JSONPATH_NOT_FOUND:  return "path not found";
    case JSONPATH_NOT_SCALAR: return "value is not a string or number";
    case JSONPATH_TOO_LONG:   return "value too long";
    }
    return "unknown error";
}
