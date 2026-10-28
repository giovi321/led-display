// Pure JSON path scanner (no ESP-IDF, no heap, no <math.h>).
//
// Pulls one string or number out of a JSON document without building a tree.
// Path syntax: dot-separated object keys and [n] array indices, optionally
// starting with an index: "data.price", "[0].v", "items[1][0]". Keys that
// contain dots, brackets or quotes are not supported. At most 16 steps.
//
// Strings are unescaped into `out`; \uXXXX is encoded as UTF-8, surrogate
// pairs are combined. A lone or invalid surrogate (or \u0000) is
// JSONPATH_BAD_JSON. Numbers are copied verbatim as written.
//
// Only the path to the target and the values skipped on the way are parsed;
// the rest of the document after the target is not inspected. Nesting
// deeper than 32 levels is JSONPATH_BAD_JSON.
#pragma once

#include <stddef.h>

typedef enum {
    JSONPATH_OK = 0,
    JSONPATH_BAD_PATH,     // malformed path, or path beyond the limits
    JSONPATH_BAD_JSON,     // document is not valid JSON where the scan looked
    JSONPATH_NOT_FOUND,    // key or index absent
    JSONPATH_NOT_SCALAR,   // target is an object, array, bool or null
    JSONPATH_TOO_LONG      // value does not fit into out (including the NUL)
} jsonpath_err_t;

#define JSONPATH_PATH_MAX 64

// Extract the string or number at `path` from `json` into `out` (strings
// unescaped, numbers copied verbatim as written in the document). `json`
// must be NUL terminated. `out` is NUL terminated when outsz > 0.
// An empty `path` is not handled here; the poller trims the whole body itself
// (an empty path returns JSONPATH_BAD_PATH).
jsonpath_err_t jsonpath_extract(const char *json, const char *path,
                                char *out, size_t outsz);

// Short human-readable text for an error ("path not found", ...).
const char *jsonpath_err_str(jsonpath_err_t e);
