#include "devname.h"

#include <string.h>

static bool alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static bool allowed(char c)
{
    return alnum(c) || c == '-';
}

bool devname_valid(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n > DEVNAME_MAX) return false;
    // Hostname labels may not start or end with a hyphen; some DHCP servers
    // drop the whole option when they do.
    if (!alnum(s[0]) || !alnum(s[n - 1])) return false;
    for (size_t i = 0; i < n; i++) {
        if (!allowed(s[i])) return false;
    }
    return true;
}

bool devname_clean(const char *in, char *out, size_t outsz)
{
    if (!out || outsz == 0) return false;
    out[0] = '\0';
    if (!in) return false;

    size_t n = 0;
    for (const char *p = in; *p != '\0' && n + 1 < outsz && n < DEVNAME_MAX; p++) {
        char c = allowed(*p) ? *p : '-';
        if (c == '-' && (n == 0 || out[n - 1] == '-')) continue;
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '-') n--;
    out[n] = '\0';
    return n > 0;
}

const char *devname_hostname(const char *stored)
{
    return (stored && stored[0] != '\0') ? stored : DEVNAME_DEFAULT_HOST;
}

const char *devname_ap_ssid(const char *stored)
{
    // Verbatim: the setup network is "the device", so its SSID is the name the
    // user gave it. Only an unset name falls back to the historical SSID.
    return (stored && stored[0] != '\0') ? stored : DEVNAME_DEFAULT_AP;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool devname_host_matches(const char *host, const char *name)
{
    if (!host || !name || name[0] == '\0') return false;

    size_t i = 0;
    for (; name[i] != '\0'; i++) {
        if (lower(host[i]) != lower(name[i])) return false;
    }
    const char *rest = host + i;
    if (*rest == '.') {
        rest++;
        if (*rest == '\0' || *rest == ':') return false;   // trailing dot only
        while (*rest != '\0' && *rest != ':') {
            if (*rest == '.') return false;                // second suffix label
            rest++;
        }
    }
    return *rest == '\0' || *rest == ':';
}
