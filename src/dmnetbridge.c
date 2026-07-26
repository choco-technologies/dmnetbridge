/**
 * @file dmnetbridge.c
 * @brief DMOD Network Bridge - Implementation
 *
 * See dmnetbridge.h for the module's role. Two pieces of state:
 *
 *  - g_pumping_ifaces / g_pump_mutex: which interfaces currently have a
 *    dmnetbridge_handle_netif_rx() loop running, so a second call for the
 *    same interface is refused (see try_start_pumping()) and
 *    dmnetbridge_reset() has something to clear on a networkd restart.
 *    Stores dmnetif_iface_t handles directly (borrowed from dmnetif, not
 *    owned here) - dmod_deinit() destroys the list itself but never frees
 *    an element.
 *
 * Everything else (send path) is stateless per call, same as dmip's own
 * send path used to be.
 */
#define DMOD_ENABLE_REGISTRATION    ON
/* Needed (in addition to DMOD_ENABLE_REGISTRATION above) specifically
 * because this file calls Dmod_GetNextDifModule()/Dmod_GetDifFunction()
 * with dmod_dmnetbridge_packet_received_sig by name (see
 * broadcast_packet_received() below) - each independently-loaded module
 * needs its own real definition of that signature string compiled in,
 * not just the `extern` declaration dmnetbridge.h provides by default
 * (see dmod/examples/dif/vfs/vfs.c for the same pattern). */
#define ENABLE_DIF_REGISTRATIONS    ON
#include "dmod.h"
#include "dmnetbridge.h"
#include "dmroute.h"
#include "dmnetif.h"
#include "dmarp.h"
#include "dmlist.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/** @brief Length in bytes of an Ethernet II header (dest MAC + src MAC + ethertype) */
#define DMNETBRIDGE_ETH_HEADER_LEN 14u

/**
 * @brief Receive buffer size for dmnetbridge_handle_netif_rx()
 *
 * Generous headroom over a standard Ethernet MTU (DMNETIF_DEFAULT_MTU is
 * 1500) plus the 14-byte L2 header, so a full-size frame is never
 * truncated - same rationale dmip.c's own DMIP_RECEIVE_BUFFER_LEN uses.
 */
#define DMNETBRIDGE_RECEIVE_BUFFER_LEN 2048u

/**
 * @brief Registry of interfaces currently being pumped by
 *        dmnetbridge_handle_netif_rx() (dmnetif_iface_t values, stored
 *        directly - not heap-allocated wrapper entries)
 *
 * Created in dmod_init(), destroyed in dmod_deinit().
 */
static dmlist_context_t* g_pumping_ifaces = NULL;

/**
 * @brief Mutex guarding g_pumping_ifaces
 */
static dmosi_mutex_t g_pump_mutex = NULL;

/**
 * @brief dmlist_compare_func_t comparing two elements by pointer identity
 */
static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

/**
 * @brief Write a 16-bit value into a byte buffer in network (big-endian) order
 */
static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

/**
 * @brief Record `iface` as currently being pumped, unless it already is
 *
 * @return true if `iface` was not already marked pumping (and now is),
 *         false if it was already marked (caller should not start a
 *         second pump loop for the same interface)
 */
static bool try_start_pumping(dmnetif_iface_t iface)
{
    dmosi_mutex_lock(g_pump_mutex);
    bool already = dmlist_find(g_pumping_ifaces, iface, compare_pointer) != NULL;
    bool started = !already && dmlist_push_back(g_pumping_ifaces, iface);
    dmosi_mutex_unlock(g_pump_mutex);
    return started;
}

/**
 * @brief Undo try_start_pumping() once a pump loop is done
 */
static void stop_pumping(dmnetif_iface_t iface)
{
    dmosi_mutex_lock(g_pump_mutex);
    dmlist_remove(g_pumping_ifaces, iface, compare_pointer);
    dmosi_mutex_unlock(g_pump_mutex);
}

/**
 * @brief Call every module implementing the packet_received DIF, in
 *        registration order, for one received frame
 */
static void broadcast_packet_received(dmnetif_iface_t iface, const uint8_t* frame, size_t frame_len)
{
    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);

        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }
}

/* ---- Send path ---- */

/**
 * @brief Look up the egress interface and next-hop address for `dst`
 *
 * Shared by dmnetbridge_get_source_address() and dmnetbridge_send() - both
 * need exactly this answer, just for different reasons (one to learn the
 * source address, the other to actually resolve/transmit through it).
 * Moved here unchanged from dmip.c's own resolve_egress().
 */
static int resolve_egress(const dmroute_addr_t* dst, dmnetif_iface_t* out_iface, dmroute_addr_t* out_next_hop)
{
    dmroute_route_t route = dmroute_lookup(dst);
    if (route == NULL)
        return -ENETUNREACH;

    dmnetif_iface_t iface = dmnetif_find_by_name(dmroute_get_iface_name(route));
    if (iface == NULL)
        return -ENODEV;

    dmroute_addr_t gateway = { 0 };
    dmroute_get_gateway(route, &gateway);

    *out_iface = iface;
    *out_next_hop = (gateway.family != dmroute_family_none) ? gateway : *dst;
    return 0;
}

/**
 * @brief Implementation of dmnetbridge_get_source_address() - see dmnetbridge.h
 */
dmod_dmnetbridge_api_declaration(1.0, int, _get_source_address, ( const dmroute_addr_t* dst, dmroute_addr_t* out_src ))
{
    if (dst == NULL || out_src == NULL || dst->family != dmroute_family_v4)
        return -EINVAL;

    dmnetif_iface_t iface = NULL;
    dmroute_addr_t next_hop = { 0 };
    int result = resolve_egress(dst, &iface, &next_hop);
    if (result != 0)
        return result;

    dmnetif_get_ip_address(iface, out_src);
    return 0;
}

/**
 * @brief Implementation of dmnetbridge_get_mtu() - see dmnetbridge.h
 */
dmod_dmnetbridge_api_declaration(1.0, int, _get_mtu, ( const dmroute_addr_t* dst, uint16_t* out_mtu ))
{
    if (dst == NULL || out_mtu == NULL || dst->family != dmroute_family_v4)
        return -EINVAL;

    dmnetif_iface_t iface = NULL;
    dmroute_addr_t next_hop = { 0 };
    int result = resolve_egress(dst, &iface, &next_hop);
    if (result != 0)
        return result;

    dmnetif_get_mtu(iface, out_mtu);
    return 0;
}

/**
 * @brief Implementation of dmnetbridge_send() - see dmnetbridge.h
 */
dmod_dmnetbridge_api_declaration(1.0, int, _send, ( const dmroute_addr_t* dst_ip, uint16_t ethertype, const void* payload, size_t payload_len, uint32_t arp_timeout_ms, dmnetif_iface_t* out_iface ))
{
    if (dst_ip == NULL || (payload == NULL && payload_len > 0) || dst_ip->family != dmroute_family_v4)
        return -EINVAL;

    dmnetif_iface_t iface = NULL;
    dmroute_addr_t next_hop = { 0 };
    int result = resolve_egress(dst_ip, &iface, &next_hop);
    if (result != 0)
        return result;

    dmnetif_mac_addr_t dst_mac = { 0 };
    if (dmarp_resolve(iface, &next_hop, &dst_mac, arp_timeout_ms) != 0)
        return -EHOSTUNREACH;

    dmnetif_mac_addr_t local_mac = { 0 };
    dmnetif_get_mac_address(iface, &local_mac);

    size_t frame_len = DMNETBRIDGE_ETH_HEADER_LEN + payload_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    if (frame == NULL)
        return -ENOMEM;

    memcpy(&frame[0], dst_mac.addr, DMNETIF_MAC_ADDR_LEN);
    memcpy(&frame[6], local_mac.addr, DMNETIF_MAC_ADDR_LEN);
    write_u16_be(&frame[12], ethertype);
    if (payload_len > 0)
    {
        memcpy(&frame[DMNETBRIDGE_ETH_HEADER_LEN], payload, payload_len);
    }

    bool sent = dmnetif_send(iface, frame, frame_len) != 0;
    Dmod_Free(frame);

    if (!sent)
        return -EIO;

    if (out_iface != NULL)
    {
        *out_iface = iface;
    }

    return 0;
}

/* ---- Receive path ---- */

/**
 * @brief Implementation of dmnetbridge_handle_netif_rx() - see dmnetbridge.h
 *
 * Known limitation: dmnetif_is_present(iface) is only re-checked between
 * dmnetif_receive() calls, not while one is in progress - a real driver
 * (dmeth) blocks indefinitely inside that call waiting for the next frame
 * (see dmeth_port.h), so an interface that disappears while nothing is
 * arriving on it won't be noticed until the next frame (if any) or never.
 * Fixing this would need a bounded-timeout read, which dmnetif_receive()
 * does not have today - out of scope here.
 */
dmod_dmnetbridge_api_declaration(1.0, void, _handle_netif_rx, ( dmnetif_iface_t iface ))
{
    if (iface == NULL || !try_start_pumping(iface))
        return;

    uint8_t* frame = Dmod_Malloc(DMNETBRIDGE_RECEIVE_BUFFER_LEN);
    if (frame != NULL)
    {
        while (dmnetif_is_present(iface))
        {
            size_t frame_len = dmnetif_receive(iface, frame, DMNETBRIDGE_RECEIVE_BUFFER_LEN);
            if (frame_len == 0)
                continue;

            dmarp_note_frame(iface, frame, frame_len);
            broadcast_packet_received(iface, frame, frame_len);
        }

        Dmod_Free(frame);
    }

    stop_pumping(iface);
}

/**
 * @brief Implementation of dmnetbridge_reset() - see dmnetbridge.h
 */
dmod_dmnetbridge_api_declaration(1.0, void, _reset, ( void ))
{
    dmosi_mutex_lock(g_pump_mutex);
    while (dmlist_size(g_pumping_ifaces) > 0)
    {
        dmlist_pop_front(g_pumping_ifaces);
    }
    dmosi_mutex_unlock(g_pump_mutex);
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the pump registry and its
 *        guarding mutex
 */
int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    g_pumping_ifaces = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_pump_mutex = dmosi_mutex_create(false);
    if (g_pumping_ifaces == NULL || g_pump_mutex == NULL)
    {
        DMOD_LOG_ERROR("Failed to allocate dmnetbridge state\n");
        return -1;
    }

    DMOD_LOG_INFO("dmnetbridge initialized\n");
    return 0;
}

/**
 * @brief Module deinitialization - frees the pump registry and its mutex
 *
 * Does not free the interfaces it references - they're borrowed from
 * dmnetif, not owned here (see g_pumping_ifaces's own doc comment).
 */
int dmod_deinit(void)
{
    dmlist_destroy(g_pumping_ifaces);
    g_pumping_ifaces = NULL;

    dmosi_mutex_destroy(g_pump_mutex);
    g_pump_mutex = NULL;

    DMOD_LOG_INFO("dmnetbridge deinitialized\n");
    return 0;
}
