/**
 * @file dmnetbridge_test.c
 * @brief Test steps for dmnetbridge
 *
 * Two fixture interfaces ("test0"/"test1") are registered against
 * "/null"/"/null2" (real enough for Dmod_FileOpen() to succeed without a
 * real driver - same pattern as lib/dmarp/tests/dmarp_test.c). No real
 * driver means:
 *  - dmnetif_get_mac_address()/dmarp_resolve() on a cache miss always
 *    fails (weak default Dmod_Ioctl() -> -ENOSYS, surfaced by dmarp_resolve()
 *    as -ENODEV) - dmnetbridge_send() maps *any* dmarp_resolve() failure to
 *    -EHOSTUNREACH, so this is exactly what exercises that path.
 *  - dmnetif_send() checks iface->up before ever touching the file (see
 *    dmnetif.c) and these fixtures can never be brought up (no real
 *    driver - dmnetif_up()'s DMDRVI_IOCTL_NET_START hits the same weak
 *    default), so even a cache-seeded dmnetbridge_send() call still ends
 *    in -EIO, never 0 - that's the deepest this pipeline (route lookup,
 *    ARP cache hit, Ethernet framing) can be exercised without a real
 *    driver.
 *
 * dmnetbridge_handle_netif_rx() itself is not exercised end-to-end here
 * (it would spin forever against these fixtures - dmnetif_is_present()
 * never goes false and dmnetif_receive() never returns data) beyond the
 * one-thread-per-interface guard (see handle_netif_rx_refuses_second_pump_for_same_interface).
 * Full receive-path coverage (a real frame flowing through
 * dmarp_note_frame() and the packet_received DIF to a real implementor)
 * belongs to dmip's own tests, once it implements that DIF - this binary
 * doesn't link dmip.
 */
#include "dmod_test.h"
#include "dmnetbridge.h"
#include "dmnetif.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

#define TEST_DEVICE_PATH_0 "/dev/null"
#define TEST_DEVICE_PATH_1 "/dev/null"

static dmnetif_iface_t g_iface0 = NULL;
static dmnetif_iface_t g_iface1 = NULL;

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t ip = { 0 };
    ip.family = dmroute_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

static dmnetif_mac_addr_t make_mac(uint8_t last_octet)
{
    dmnetif_mac_addr_t mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, last_octet } };
    return mac;
}

static bool ipv4_equal(const dmroute_addr_t* a, const dmroute_addr_t* b)
{
    if (a->family != b->family)
        return false;
    if (a->family != dmroute_family_v4)
        return true;
    for (int i = 0; i < DMROUTE_IPV4_ADDR_LEN; i++)
    {
        if (a->addr.v4[i] != b->addr.v4[i])
            return false;
    }
    return true;
}

void dmod_test_setup(void)
{
    g_iface0 = dmnetif_register("test0", TEST_DEVICE_PATH_0);
    g_iface1 = dmnetif_register("test1", TEST_DEVICE_PATH_1);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface0);
    dmnetif_unregister(g_iface1);
    g_iface0 = NULL;
    g_iface1 = NULL;
}

/* ---- get_source_address ---- */

DMOD_TEST_STEP(get_source_address_returns_enetunreach_without_route)
{
    dmroute_addr_t dst = make_v4(10, 1, 0, 1);
    dmroute_addr_t src = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_get_source_address(&dst, &src), -ENETUNREACH);
}

DMOD_TEST_STEP(get_source_address_returns_ifaces_ip)
{
    dmroute_addr_t iface_ip = make_v4(10, 1, 1, 1);
    dmnetif_set_ip_address(g_iface0, &iface_ip);

    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_addr_t net = make_v4(10, 1, 1, 0);
    dmroute_route_t route = dmroute_add(&net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    DMOD_TEST_EXPECT_NOT_NULL(route);

    dmroute_addr_t dst = make_v4(10, 1, 1, 42);
    dmroute_addr_t src = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_get_source_address(&dst, &src), 0);
    DMOD_TEST_EXPECT_TRUE(ipv4_equal(&src, &iface_ip));

    dmroute_remove(route);
}

DMOD_TEST_STEP(get_source_address_null_arguments_return_einval)
{
    dmroute_addr_t dst = make_v4(10, 1, 0, 2);
    dmroute_addr_t src = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_get_source_address(NULL, &src), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmnetbridge_get_source_address(&dst, NULL), -EINVAL);
}

DMOD_TEST_STEP(get_source_address_non_v4_family_returns_einval)
{
    dmroute_addr_t dst = make_v4(10, 1, 0, 3);
    dst.family = dmroute_family_v6;
    dmroute_addr_t src = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_get_source_address(&dst, &src), -EINVAL);
}

/* ---- send ---- */

DMOD_TEST_STEP(send_returns_enetunreach_without_route)
{
    dmroute_addr_t dst = make_v4(10, 2, 0, 1);
    uint8_t payload[4] = { 1, 2, 3, 4 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS, NULL), -ENETUNREACH);
}

DMOD_TEST_STEP(send_returns_ehostunreach_on_arp_cache_miss)
{
    /* No real driver behind "test0" means dmarp_resolve() can never
     * actually succeed on a cache miss (see this file's top comment) -
     * dmnetbridge_send() maps that to -EHOSTUNREACH. */
    dmroute_addr_t net = make_v4(10, 2, 1, 0);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    DMOD_TEST_EXPECT_NOT_NULL(route);

    dmroute_addr_t dst = make_v4(10, 2, 1, 55);
    uint8_t payload[4] = { 1, 2, 3, 4 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), 10, NULL), -EHOSTUNREACH);

    dmroute_remove(route);
}

DMOD_TEST_STEP(send_full_path_without_real_driver_returns_eio)
{
    /* Pre-seeding the ARP cache skips dmarp_resolve()'s (otherwise
     * unavoidable, no-real-driver) failure entirely, reaching
     * dmnetif_send() itself - which checks iface->up (see dmnetif.c) and
     * fails, since these fixtures can never actually be brought up (no
     * real driver behind "test0"/"test1" - dmnetif_up()'s
     * DMDRVI_IOCTL_NET_START would itself hit dmod's weak default
     * Dmod_Ioctl(), -ENOSYS). This is as far as this pipeline (route
     * lookup, ARP cache hit, Ethernet framing) can be exercised without a
     * real driver - same reasoning lib/dmip/tests/dmip_test.c's own
     * v4_send_full_path_without_real_driver_returns_eio documents. */
    dmroute_addr_t net = make_v4(10, 2, 2, 0);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    DMOD_TEST_EXPECT_NOT_NULL(route);

    dmroute_addr_t dst = make_v4(10, 2, 2, 77);
    dmnetif_mac_addr_t dst_mac = make_mac(0x30);
    dmarp_cache_insert(g_iface0, &dst, &dst_mac);

    uint8_t payload[4] = { 5, 6, 7, 8 };
    dmnetif_iface_t out_iface = NULL;
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS, &out_iface), -EIO);
    DMOD_TEST_EXPECT_NULL(out_iface); /* left untouched - only ever set on success */

    dmarp_cache_remove(g_iface0, &dst);
    dmroute_remove(route);
}

DMOD_TEST_STEP(send_out_iface_is_optional)
{
    /* NULL out_iface must not crash even on the -EIO path above. */
    dmroute_addr_t net = make_v4(10, 2, 3, 0);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    DMOD_TEST_EXPECT_NOT_NULL(route);

    dmroute_addr_t dst = make_v4(10, 2, 3, 88);
    dmnetif_mac_addr_t dst_mac = make_mac(0x31);
    dmarp_cache_insert(g_iface0, &dst, &dst_mac);

    uint8_t payload[2] = { 9, 9 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS, NULL), -EIO);

    dmarp_cache_remove(g_iface0, &dst);
    dmroute_remove(route);
}

DMOD_TEST_STEP(send_null_arguments_return_einval)
{
    dmroute_addr_t dst = make_v4(10, 2, 4, 1);
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(NULL, 0x0800, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS, NULL), -EINVAL);

    uint8_t payload[1] = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, NULL, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS, NULL), -EINVAL);
}

DMOD_TEST_STEP(send_non_v4_family_returns_einval)
{
    dmroute_addr_t dst = make_v4(10, 2, 4, 2);
    dst.family = dmroute_family_v6;
    uint8_t payload[1] = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS, NULL), -EINVAL);
}

DMOD_TEST_STEP(send_zero_length_payload_is_allowed)
{
    /* NULL/zero-length payload must reach the same -EIO wall as every
     * other step here, not -EINVAL - a zero-length payload is a valid
     * "just the IP header, no data" packet. */
    dmroute_addr_t net = make_v4(10, 2, 5, 0);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    DMOD_TEST_EXPECT_NOT_NULL(route);

    dmroute_addr_t dst = make_v4(10, 2, 5, 9);
    dmnetif_mac_addr_t dst_mac = make_mac(0x32);
    dmarp_cache_insert(g_iface0, &dst, &dst_mac);

    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS, NULL), -EIO);

    dmarp_cache_remove(g_iface0, &dst);
    dmroute_remove(route);
}

DMOD_TEST_STEP(send_on_iface_bypasses_missing_route)
{
    /* No route entry exists for this destination at all - dmnetbridge_send()
     * cannot get past resolve_egress() and returns -ENETUNREACH, exactly
     * what a DHCP client would see if it tried to use dmnetbridge_send()
     * before it has a lease (no route can exist yet). */
    dmroute_addr_t dst = make_v4(10, 2, 6, 5);
    uint8_t payload[4] = { 1, 2, 3, 4 };
    DMOD_TEST_EXPECT_EQ(dmnetbridge_send(&dst, 0x0800, payload, sizeof(payload), 10, NULL), -ENETUNREACH);

    /* dmnetbridge_send_on_iface() is told the interface explicitly and never
     * calls dmroute_lookup(), so - despite the same missing route - it
     * reaches exactly as far as dmnetbridge_send() would with a real route
     * in place: pre-seeding the ARP cache gets it all the way to
     * dmnetif_send() itself, which fails only because this fixture's
     * interface can never be brought up (see this file's top comment), the
     * same -EIO every other full-path test here ends at. */
    dmnetif_mac_addr_t dst_mac = make_mac(0x40);
    dmarp_cache_insert(g_iface0, &dst, &dst_mac);

    DMOD_TEST_EXPECT_EQ(dmnetbridge_send_on_iface(g_iface0, &dst, 0x0800, payload, sizeof(payload), 10), -EIO);

    dmarp_cache_remove(g_iface0, &dst);
}

/* ---- reset ---- */

DMOD_TEST_STEP(reset_is_safe_with_nothing_pumping)
{
    dmnetbridge_reset();
    dmnetbridge_reset();
}

/* ---- handle_netif_rx: one-thread-per-interface guard ---- */

static void pump_thread_entry(void* arg)
{
    dmnetbridge_handle_netif_rx((dmnetif_iface_t)arg);
}

DMOD_TEST_STEP(handle_netif_rx_refuses_second_pump_for_same_interface)
{
    /* g_iface1 (not g_iface0 - keeps this independent of every other step's
     * use of g_iface0) is deliberately never touched by anything else in
     * this file, so it starts out not-yet-pumped for this step. */
    dmosi_thread_t background = dmosi_thread_create(pump_thread_entry, g_iface1, 0, 4096, "test-pump", NULL);
    DMOD_TEST_EXPECT_NOT_NULL(background);

    /* Give the background thread a moment to register itself as pumping
     * before this (synchronous, on the test's own thread) second call -
     * which must return immediately rather than starting a second
     * concurrent pump loop on the same interface. */
    dmosi_thread_sleep(50);
    dmnetbridge_handle_netif_rx(g_iface1);

    /* The background thread itself spins forever against this no-driver
     * fixture (see this file's top comment) - forcefully stop it rather
     * than leaking a busy loop for the rest of this test binary's life. */
    dmosi_thread_kill(background, 0);
    dmosi_thread_destroy(background);

    dmnetbridge_reset();
}
