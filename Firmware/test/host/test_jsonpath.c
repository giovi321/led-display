// Host-side unit tests for jsonpath.c (plain gcc + assert, no ESP-IDF).
// Covers: object/array paths, string unescaping (incl. \u and surrogate
// pairs), verbatim numbers, whitespace, every error code, and skipping of
// sibling values that contain structural characters inside strings.
#include <stdio.h>
#include <string.h>

#include "jsonpath.h"

static int s_failures;

static void ok(const char *json, const char *path, const char *expect,
               const char *what)
{
    char out[128];
    jsonpath_err_t e = jsonpath_extract(json, path, out, sizeof(out));
    if (e != JSONPATH_OK) {
        fprintf(stderr, "FAIL %s: expected OK got %s\n", what, jsonpath_err_str(e));
        s_failures++;
    } else if (strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %s: expected '%s' got '%s'\n", what, expect, out);
        s_failures++;
    }
}

static void err(const char *json, const char *path, jsonpath_err_t expect,
                const char *what)
{
    char out[128];
    jsonpath_err_t e = jsonpath_extract(json, path, out, sizeof(out));
    if (e != expect) {
        fprintf(stderr, "FAIL %s: expected err %d (%s) got %d (%s)\n", what,
                (int)expect, jsonpath_err_str(expect), (int)e, jsonpath_err_str(e));
        s_failures++;
    }
}

static void test_paths(void)
{
    ok("{\"a\":1}", "a", "1", "top key");
    ok("{\"data\":{\"price\":12.5}}", "data.price", "12.5", "nested key");
    ok("[{\"v\":\"x\"}]", "[0].v", "x", "leading index");
    ok("{\"items\":[1,[2,3]]}", "items[1][0]", "2", "nested index");
    ok("{\"items\":[1,[2,3]]}", "items[1][1]", "3", "nested index 2");
    ok("{\"a\":{\"b\":[{\"c\":\"deep\"}]}}", "a.b[0].c", "deep", "mixed path");
    ok("{\"a\":1,\"a\":2}", "a", "1", "first duplicate key wins");
    ok("{\"ab\":1,\"a\":2}", "a", "2", "key prefix is not a match");
    ok("{\"a\":\"\"}", "a", "", "empty string value");
}

static void test_strings(void)
{
    ok("{\"t\":\"caf\\u00e9 \\\"x\\\"\"}", "t", "caf\xc3\xa9 \"x\"", "u00e9 and quote");
    ok("{\"t\":\"a\\\\b\\/c\\n\\t\\r\\b\\f\"}", "t", "a\\b/c\n\t\r\b\f", "simple escapes");
    ok("{\"t\":\"\\u20ac\"}", "t", "\xe2\x82\xac", "3-byte utf-8");
    ok("{\"t\":\"\\ud83d\\ude00\"}", "t", "\xf0\x9f\x98\x80", "surrogate pair");
    ok("{\"t\":\"\\u0041\\u007a\"}", "t", "Az", "ascii escapes");
    ok("{\"t\":\"\\u00E9\"}", "t", "\xc3\xa9", "uppercase hex");
    ok("{\"t\":\"\xc3\xa9\"}", "t", "\xc3\xa9", "raw utf-8 passes through");
    // Lone or invalid surrogates and NUL escapes are BAD_JSON (documented).
    err("{\"t\":\"\\ud83d\"}", "t", JSONPATH_BAD_JSON, "lone high surrogate");
    err("{\"t\":\"\\ud83dx\"}", "t", JSONPATH_BAD_JSON, "high surrogate then char");
    err("{\"t\":\"\\ude00\"}", "t", JSONPATH_BAD_JSON, "lone low surrogate");
    err("{\"t\":\"\\ud83d\\u0041\"}", "t", JSONPATH_BAD_JSON, "high then non-low");
    err("{\"t\":\"\\u00zz\"}", "t", JSONPATH_BAD_JSON, "bad hex digit");
    err("{\"t\":\"\\u00\"}", "t", JSONPATH_BAD_JSON, "short unicode escape");
    err("{\"t\":\"\\u0000\"}", "t", JSONPATH_BAD_JSON, "NUL escape");
    err("{\"t\":\"\\q\"}", "t", JSONPATH_BAD_JSON, "unknown escape");
    err("{\"t\":\"abc", "t", JSONPATH_BAD_JSON, "unterminated string");
}

static void test_numbers(void)
{
    ok("{\"n\":-1.5e3}", "n", "-1.5e3", "exponent verbatim");
    ok("{\"n\":0}", "n", "0", "zero");
    ok("{\"n\":1234567.891}", "n", "1234567.891", "long decimal");
    ok("{\"n\":-0.0}", "n", "-0.0", "negative zero");
    ok("{\"n\":1E+2}", "n", "1E+2", "upper exponent");
    ok("{\"n\":7}", "n", "7", "number ends at brace");
    ok("[4.20,5]", "[0]", "4.20", "trailing zero kept");
    err("{\"n\":01}", "n", JSONPATH_BAD_JSON, "leading zero");
    err("{\"n\":-}", "n", JSONPATH_BAD_JSON, "bare minus");
    err("{\"n\":1.}", "n", JSONPATH_BAD_JSON, "dot without digits");
    err("{\"n\":1e}", "n", JSONPATH_BAD_JSON, "exponent without digits");
    err("{\"n\":+1}", "n", JSONPATH_BAD_JSON, "plus sign");
    err("{\"n\":.5}", "n", JSONPATH_BAD_JSON, "leading dot");
}

static void test_whitespace(void)
{
    ok(" \n\t{ \"a\" \r\n : \n [ 1 , { \"b\" : \"x\" } ] } ", "a[1].b", "x", "whitespace");
    ok("{\"a\"\n:\n12}", "a", "12", "newlines around colon");
}

static void test_skipping(void)
{
    ok("{\"s\":\"{[\\\"}]\",\"v\":5}", "v", "5", "skip string with braces and escaped quote");
    ok("{\"s\":\"\\\\\",\"v\":6}", "v", "6", "skip string ending in escaped backslash");
    ok("{\"x\":{\"y\":[1,{\"z\":\"}\"}],\"w\":null},\"v\":7}", "v", "7", "skip nested value");
    ok("{\"x\":[true,false,null,-1.5e3,\"a\"],\"v\":8}", "v", "8", "skip literals");
    ok("[{\"a\":1},[2],\"s\",3]", "[3]", "3", "skip array elements");
    ok("{\"x\":\"a,b:c\",\"v\":9}", "v", "9", "skip string with comma and colon");
    // Non-matching keys also containing structural characters.
    ok("{\"k{[\\\"\":1,\"v\":2}", "v", "2", "skip key with structural chars");
    // A key written with an escape still matches its unescaped form.
    ok("{\"\\u0061\":3}", "a", "3", "escaped key matches");
}

static void test_errors(void)
{
    err("{\"a\":{}}", "a", JSONPATH_NOT_SCALAR, "object");
    err("{\"a\":[1]}", "a", JSONPATH_NOT_SCALAR, "array");
    err("{\"a\":true}", "a", JSONPATH_NOT_SCALAR, "true");
    err("{\"a\":false}", "a", JSONPATH_NOT_SCALAR, "false");
    err("{\"a\":null}", "a", JSONPATH_NOT_SCALAR, "null");
    err("{\"a\":1}", "b", JSONPATH_NOT_FOUND, "missing key");
    err("[1]", "[3]", JSONPATH_NOT_FOUND, "index out of range");
    err("[]", "[0]", JSONPATH_NOT_FOUND, "empty array");
    err("{}", "a", JSONPATH_NOT_FOUND, "empty object");
    err("{\"a\":1}", "a.b", JSONPATH_NOT_FOUND, "key into scalar");
    err("{\"a\":1}", "[0]", JSONPATH_NOT_FOUND, "index into object");
    err("[1]", "a", JSONPATH_NOT_FOUND, "key into array");
    err("{\"a\":", "a", JSONPATH_BAD_JSON, "truncated after colon");
    err("{\"a\"", "a", JSONPATH_BAD_JSON, "truncated after key");
    err("", "a", JSONPATH_BAD_JSON, "empty document");
    err("   ", "a", JSONPATH_BAD_JSON, "blank document");
    err("{\"a\" 1}", "a", JSONPATH_BAD_JSON, "missing colon");
    err("{a:1}", "a", JSONPATH_BAD_JSON, "unquoted key");
    err("{\"x\":[1,2,\"v\":3}", "v", JSONPATH_BAD_JSON, "bad separator while skipping");
    err("{\"x\":tru,\"v\":1}", "v", JSONPATH_BAD_JSON, "bad literal while skipping");
    err("{\"x\":[1,2", "v", JSONPATH_BAD_JSON, "unterminated skipped array");
    err("{\"a\":1 \"b\":2}", "b", JSONPATH_BAD_JSON, "missing comma");
    err("[1 2]", "[1]", JSONPATH_BAD_JSON, "missing comma in array");
    err("[1,", "[1]", JSONPATH_BAD_JSON, "truncated array");
    err("{\"a\":x}", "a", JSONPATH_BAD_JSON, "bad scalar token");
    err("{\"a\":-}", "a", JSONPATH_BAD_JSON, "bad number token");
}

static void test_bad_path(void)
{
    err("{\"a\":1}", "a..b", JSONPATH_BAD_PATH, "double dot");
    err("{\"a\":1}", "a[", JSONPATH_BAD_PATH, "open bracket");
    err("{\"a\":1}", "[x]", JSONPATH_BAD_PATH, "non-numeric index");
    err("{\"a\":1}", "[]", JSONPATH_BAD_PATH, "empty index");
    err("{\"a\":1}", ".a", JSONPATH_BAD_PATH, "leading dot");
    err("{\"a\":1}", "a.", JSONPATH_BAD_PATH, "trailing dot");
    err("{\"a\":1}", "a]", JSONPATH_BAD_PATH, "stray close bracket");
    err("{\"a\":1}", "a[0", JSONPATH_BAD_PATH, "unclosed index");
    err("{\"a\":1}", "a[-1]", JSONPATH_BAD_PATH, "negative index");
    err("{\"a\":1}", "", JSONPATH_BAD_PATH, "empty path");
    err("{\"a\":1}", "a[99999999999999999999]", JSONPATH_BAD_PATH, "index overflow");
    err("{\"a\":1}", "[0]b", JSONPATH_BAD_PATH, "key glued to index");
    // Path length limit: 64 chars is the maximum.
    {
        char p[JSONPATH_PATH_MAX + 8];
        memset(p, 'k', sizeof(p));
        p[JSONPATH_PATH_MAX + 1] = '\0';
        err("{\"a\":1}", p, JSONPATH_BAD_PATH, "path too long");
        p[JSONPATH_PATH_MAX] = '\0';
        err("{\"a\":1}", p, JSONPATH_NOT_FOUND, "path of exactly 64 chars");
    }
    // 17 steps exceed the 16-step limit; 16 steps parse.
    err("{}", "a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a", JSONPATH_BAD_PATH, "17 steps");
    err("{}", "a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a", JSONPATH_NOT_FOUND, "16 steps");
}

static void test_too_long(void)
{
    char json[256], big[201], out[64], j2[128], s63[64], s64[65];

    memset(big, 'x', 200);
    big[200] = '\0';
    snprintf(json, sizeof(json), "{\"a\":\"%s\"}", big);
    if (jsonpath_extract(json, "a", out, sizeof(out)) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL 200-char string into 64-byte out\n");
        s_failures++;
    }
    // Exact fit: 63 chars + NUL fit, 64 do not.
    memset(s63, 'y', 63); s63[63] = '\0';
    memset(s64, 'y', 64); s64[64] = '\0';
    snprintf(j2, sizeof(j2), "{\"a\":\"%s\"}", s63);
    if (jsonpath_extract(j2, "a", out, sizeof(out)) != JSONPATH_OK
        || strcmp(out, s63) != 0) {
        fprintf(stderr, "FAIL 63 chars should fit in 64\n");
        s_failures++;
    }
    snprintf(j2, sizeof(j2), "{\"a\":\"%s\"}", s64);
    if (jsonpath_extract(j2, "a", out, sizeof(out)) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL 64 chars should not fit in 64\n");
        s_failures++;
    }
    if (jsonpath_extract("{\"a\":123456789}", "a", out, 5) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL long number into 5-byte out\n");
        s_failures++;
    }
    if (jsonpath_extract("{\"a\":\"\\u20ac\"}", "a", out, 3) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL 3-byte char into 3-byte out\n");
        s_failures++;
    }
    if (jsonpath_extract("{\"a\":1}", "a", out, 0) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL outsz 0\n");
        s_failures++;
    }
    if (jsonpath_extract("{\"a\":1}", "a", out, 1) != JSONPATH_TOO_LONG) {
        fprintf(stderr, "FAIL outsz 1\n");
        s_failures++;
    }
}

static void test_depth(void)
{
    // The root object is level 1, so a skipped value nested 31 arrays deep
    // reaches level 32 (allowed); 32 arrays deep reaches 33 (BAD_JSON).
    char json[256], out[16];
    for (int depth = 31; depth <= 32; depth++) {
        int n = 0, i;
        n += snprintf(json + n, sizeof(json) - (size_t)n, "{\"x\":");
        for (i = 0; i < depth; i++) json[n++] = '[';
        for (i = 0; i < depth; i++) json[n++] = ']';
        n += snprintf(json + n, sizeof(json) - (size_t)n, ",\"v\":1}");
        jsonpath_err_t e = jsonpath_extract(json, "v", out, sizeof(out));
        jsonpath_err_t want = depth <= 31 ? JSONPATH_OK : JSONPATH_BAD_JSON;
        if (e != want) {
            fprintf(stderr, "FAIL depth %d: got %s\n", depth, jsonpath_err_str(e));
            s_failures++;
        }
    }
}

static void test_err_str(void)
{
    if (strcmp(jsonpath_err_str(JSONPATH_NOT_FOUND), "path not found") != 0) {
        fprintf(stderr, "FAIL err_str not found\n");
        s_failures++;
    }
    for (int e = JSONPATH_OK; e <= JSONPATH_TOO_LONG; e++) {
        const char *s = jsonpath_err_str((jsonpath_err_t)e);
        if (!s || !*s) {
            fprintf(stderr, "FAIL err_str %d empty\n", e);
            s_failures++;
        }
    }
    if (!jsonpath_err_str((jsonpath_err_t)99)) {
        fprintf(stderr, "FAIL err_str unknown returns NULL\n");
        s_failures++;
    }
}

int main(void)
{
    test_paths();
    test_strings();
    test_numbers();
    test_whitespace();
    test_skipping();
    test_errors();
    test_bad_path();
    test_too_long();
    test_depth();
    test_err_str();

    if (s_failures) {
        fprintf(stderr, "test_jsonpath: %d FAILURE(S)\n", s_failures);
        return 1;
    }
    printf("test_jsonpath: all tests passed\n");
    return 0;
}
