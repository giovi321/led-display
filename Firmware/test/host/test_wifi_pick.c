// Host tests for wifi_pick.c: the order in which the boot/reconnect logic
// walks the saved networks (scan hits by signal strength, then blind attempts
// for whatever the scan missed).
//   gcc -I../../main test_wifi_pick.c ../../main/wifi_pick.c && ./a.out

#include <assert.h>
#include <stdio.h>

#include "wifi_pick.h"

static void test_no_saved(void)
{
    const char *saved[1] = {""};
    wifi_pick_scan_t scan[1] = {{"home", -40}};
    int order[4];

    assert(wifi_pick_order(saved, 0, scan, 1, order, 4) == 0);
    assert(wifi_pick_order(saved, 1, scan, 1, order, 4) == 0);   // empty SSID
    assert(wifi_pick_order(NULL, 3, scan, 1, order, 4) == 0);
    printf("test_no_saved ok\n");
}

static void test_no_scan_keeps_stored_order(void)
{
    const char *saved[3] = {"a", "b", "c"};
    int order[3];

    assert(wifi_pick_order(saved, 3, NULL, 0, order, 3) == 3);
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2);
    printf("test_no_scan_keeps_stored_order ok\n");
}

static void test_strongest_first(void)
{
    const char *saved[3] = {"weak", "strong", "middle"};
    wifi_pick_scan_t scan[3] = {
        {"weak", -85}, {"strong", -35}, {"middle", -60},
    };
    int order[3];

    assert(wifi_pick_order(saved, 3, scan, 3, order, 3) == 3);
    assert(order[0] == 1);   // strong
    assert(order[1] == 2);   // middle
    assert(order[2] == 0);   // weak
    printf("test_strongest_first ok\n");
}

// A saved SSID seen on several APs / bands counts as its strongest hit.
static void test_duplicate_scan_hits(void)
{
    const char *saved[2] = {"mesh", "other"};
    wifi_pick_scan_t scan[4] = {
        {"mesh", -88}, {"other", -55}, {"mesh", -41}, {"unknown", -30},
    };
    int order[2];

    assert(wifi_pick_order(saved, 2, scan, 4, order, 2) == 2);
    assert(order[0] == 0);   // mesh, best hit -41
    assert(order[1] == 1);
    printf("test_duplicate_scan_hits ok\n");
}

// Scanned networks come first; the ones the scan missed follow in stored
// order, so a hidden SSID still gets a blind attempt.
static void test_unscanned_appended(void)
{
    const char *saved[4] = {"hidden1", "visible-weak", "hidden2", "visible-strong"};
    wifi_pick_scan_t scan[2] = {{"visible-weak", -80}, {"visible-strong", -45}};
    int order[4];

    assert(wifi_pick_order(saved, 4, scan, 2, order, 4) == 4);
    assert(order[0] == 3);
    assert(order[1] == 1);
    assert(order[2] == 0);
    assert(order[3] == 2);
    printf("test_unscanned_appended ok\n");
}

static void test_ties_keep_stored_order(void)
{
    const char *saved[2] = {"first", "second"};
    wifi_pick_scan_t scan[2] = {{"second", -50}, {"first", -50}};
    int order[2];

    assert(wifi_pick_order(saved, 2, scan, 2, order, 2) == 2);
    assert(order[0] == 0);
    assert(order[1] == 1);
    printf("test_ties_keep_stored_order ok\n");
}

// max_out is the caller's buffer, not a hint: never write past it, and keep
// the best candidates when it is smaller than the saved list.
static void test_out_capacity(void)
{
    const char *saved[5] = {"a", "b", "c", "d", "e"};
    wifi_pick_scan_t scan[5] = {
        {"a", -70}, {"b", -60}, {"c", -50}, {"d", -40}, {"e", -30},
    };
    int order[2] = {-1, -1};

    assert(wifi_pick_order(saved, 5, scan, 5, order, 2) == 2);
    assert(order[0] == 4);
    assert(order[1] == 3);
    assert(wifi_pick_order(saved, 5, scan, 5, order, 0) == 0);
    printf("test_out_capacity ok\n");
}

// Gaps in the saved slots (a deleted middle entry) must not shift indices.
static void test_skips_empty_slots(void)
{
    const char *saved[5] = {"a", "", NULL, "d", ""};
    wifi_pick_scan_t scan[1] = {{"d", -40}};
    int order[5];

    assert(wifi_pick_order(saved, 5, scan, 1, order, 5) == 2);
    assert(order[0] == 3);
    assert(order[1] == 0);
    printf("test_skips_empty_slots ok\n");
}

int main(void)
{
    test_no_saved();
    test_no_scan_keeps_stored_order();
    test_strongest_first();
    test_duplicate_scan_hits();
    test_unscanned_appended();
    test_ties_keep_stored_order();
    test_out_capacity();
    test_skips_empty_slots();
    printf("wifi_pick: all tests passed\n");
    return 0;
}
