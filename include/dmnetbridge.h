#ifndef DMNETBRIDGE_H
#define DMNETBRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmroute.h"
#include "dmnetif.h"
#include "dmnetbridge_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmnetbridge.h
 * @brief DMOD Network Bridge - Public API
 *
 * dmnetbridge is the layer between IP-address-level code (dmip) and
 * netif/routing/ARP. It owns everything that needs to know about a
 * specific network interface on both directions of traffic:
 *
 *  - Sending: dmnetbridge_send() looks up the egress route (dmroute),
 *    resolves the next hop's MAC (dmarp), builds the Ethernet header, and
 *    transmits (dmnetif_send()). dmip no longer does any of this itself -
 *    it only ever deals with IP addresses, and calls dmnetbridge_send()
 *    once it has a payload ready to go.
 *
 *  - Receiving: dmnetbridge_handle_netif_rx() is a blocking pump, one call
 *    per interface, meant to be run from its own thread (by the
 *    `networkd` service - see services/networkd). It is the *only* code
 *    path allowed to call dmnetif_receive() on a given interface once
 *    networkd owns it: for every frame it reads, it feeds it to
 *    dmarp_note_frame() (opportunistic ARP learning - a plain Built-in
 *    API call, since dmnetbridge already hard-depends on dmarp for
 *    sending) and then broadcasts it to every module implementing the
 *    packet_received DIF below (dmip, today).
 *
 * packet_received is a DIF (1:N, discovered at runtime via
 * Dmod_GetNextDifModule()/Dmod_GetDifFunction()) rather than a Built-in
 * API, specifically so dmnetbridge does not need a hard compile-time
 * dependency on every protocol module that might want raw frames - dmip
 * is the only implementor today, but a future module could add its own
 * implementation without dmnetbridge ever needing to change. Contrast
 * this with dmarp_note_frame(): dmnetbridge already hard-depends on dmarp
 * (for dmarp_resolve()), so that one plain Built-in API call, not a DIF.
 *
 * dmnetbridge depends on dmroute (route lookup, dmroute_addr_t), dmnetif
 * (frame I/O, the interface registry), and dmarp (MAC resolution and
 * opportunistic learning).
 */

/**
 * @brief DIF interface: deliver one received frame to an interested
 *        protocol module
 *
 * Called by dmnetbridge_handle_netif_rx() for every frame it reads,
 * regardless of ethertype - each implementor is expected to check the
 * ethertype itself (offset 12-13 of `frame`) and ignore what isn't
 * theirs, the same way dmip.c's own reassembly code already does.
 *
 * @param iface     Interface the frame was received on
 * @param frame     Full raw Ethernet frame (dest MAC/src MAC/ethertype/
 *                   payload), exactly as dmnetif_receive() produced it.
 *                   Only valid for the duration of the call.
 * @param frame_len Number of valid bytes in `frame`
 */
dmod_dmnetbridge_dif(1.0, void, _packet_received, ( dmnetif_iface_t iface, const uint8_t* frame, size_t frame_len ));

/**
 * @brief Build and transmit one IP packet to `dst_ip`
 *
 * Replaces what dmip_v4_send() used to do inline: looks up the egress
 * route for `dst_ip` (dmroute_lookup()), resolves the next hop's MAC
 * (dmarp_resolve(), given `arp_timeout_ms` - the next hop is the route's
 * gateway if it has one, otherwise `dst_ip` itself, i.e. a
 * directly-connected destination), builds the 14-byte Ethernet header
 * (destination MAC just resolved, `iface`'s own MAC, `ethertype`) in
 * front of `payload`, and transmits it (dmnetif_send()).
 *
 * @param dst_ip         Destination IP address (family must be
 *                        dmroute_family_v4 - IPv6 has no route-to-MAC
 *                        resolution path yet, same gap dmarp.h documents
 *                        for itself)
 * @param ethertype      Ethertype to write into the Ethernet header (e.g.
 *                        DMIP_ETHERTYPE_IPV4)
 * @param payload        Data to send (already a complete IP packet/
 *                        fragment - dmnetbridge only adds the Ethernet
 *                        header, it has no opinion on what's inside)
 * @param payload_len    Length of `payload` in bytes
 * @param arp_timeout_ms Forwarded to dmarp_resolve() on a cache miss - use
 *                        DMARP_DEFAULT_TIMEOUT_MS if you don't have a
 *                        strong opinion
 * @param out_iface      Output: the interface the packet was actually
 *                        sent through. Optional - pass NULL if not needed.
 *
 * @return 0 on success, -EINVAL (bad argument/family), -ENETUNREACH (no
 *         route to `dst_ip`), -ENODEV (the route's interface is no
 *         longer registered), -EHOSTUNREACH (ARP resolution failed or
 *         timed out), -EIO (the driver rejected the frame)
 */
dmod_dmnetbridge_api(1.0, int, _send, ( const dmroute_addr_t* dst_ip, uint16_t ethertype, const void* payload, size_t payload_len, uint32_t arp_timeout_ms, dmnetif_iface_t* out_iface ));

/**
 * @brief Find the source address dmnetbridge_send() would use to reach `dst`
 *
 * Replaces what dmip_v4_get_source_address() used to do inline: looks up
 * the egress route for `dst` (dmroute_lookup()) and reads the resulting
 * interface's own IP address (dmnetif_get_ip_address()).
 *
 * @param dst     Destination address (family must be dmroute_family_v4)
 * @param out_src Output: the source address to use. `family` is
 *                dmroute_family_none if the egress interface has none
 *                assigned yet - not treated as an error, mirroring
 *                dmnetif_get_ip_address()'s own contract
 *
 * @return 0 on success, -EINVAL (bad argument/family), -ENETUNREACH (no
 *         route to `dst`), -ENODEV (the route's interface is no longer
 *         registered)
 */
dmod_dmnetbridge_api(1.0, int, _get_source_address, ( const dmroute_addr_t* dst, dmroute_addr_t* out_src ));

/**
 * @brief Find the MTU dmnetbridge_send() would transmit through to reach `dst`
 *
 * Lets dmip_v4_send() size its fragments to the real egress interface's
 * MTU without dmip ever calling dmroute_lookup()/dmnetif_get_mtu()
 * directly - same reasoning as dmnetbridge_get_source_address().
 * `*out_mtu` is left untouched on failure, so a caller that seeds it with
 * DMNETIF_DEFAULT_MTU first keeps that default rather than needing its
 * own fallback logic.
 *
 * @param dst     Destination address (family must be dmroute_family_v4)
 * @param out_mtu Output: the egress interface's MTU in bytes. Only
 *                written on success (return 0)
 *
 * @return 0 on success, -EINVAL (bad argument/family), -ENETUNREACH (no
 *         route to `dst`), -ENODEV (the route's interface is no longer
 *         registered)
 */
dmod_dmnetbridge_api(1.0, int, _get_mtu, ( const dmroute_addr_t* dst, uint16_t* out_mtu ));

/**
 * @brief Pump one interface: read and dispatch frames until it's gone
 *
 * Blocking, one call per interface - meant to be run from its own thread
 * (the `networkd` service spawns one per currently-registered interface,
 * via dmnetif_for_each() - see services/networkd). Loops calling
 * dmnetif_receive() on `iface` for as long as dmnetif_is_present(iface)
 * stays true; for every frame actually received, calls dmarp_note_frame()
 * and then every packet_received DIF implementor, in that order.
 *
 * This is the only code path that should call dmnetif_receive() on a
 * given interface once networkd owns it - see dmarp_note_frame()'s doc
 * comment (in dmarp.h) for why a second concurrent reader would starve
 * dmarp_resolve()'s own wait.
 *
 * Returns (does not loop forever) once dmnetif_is_present(iface) goes
 * false - e.g. the interface's backing device was hot-unplugged, or it
 * was unregistered.
 *
 * @param iface Interface to pump. If NULL or invalid, returns immediately.
 */
dmod_dmnetbridge_api(1.0, void, _handle_netif_rx, ( dmnetif_iface_t iface ));

/**
 * @brief Clear dmnetbridge's own bookkeeping of in-progress pump loops
 *
 * `networkd` is a restartable service - on startup, before spawning any
 * dmnetbridge_handle_netif_rx() threads of its own, it calls this so a
 * previous instance's bookkeeping (which interfaces already have a pump
 * loop associated with them) can't make a fresh start think an interface
 * is already being pumped when nothing is actually reading it anymore.
 *
 * Does not touch any other module's state (dmip's receive queue, dmarp's
 * cache, dmroute's table) - each of those is that module's own lifecycle
 * to manage, out of scope for this call.
 */
dmod_dmnetbridge_api(1.0, void, _reset, ( void ));

#ifdef __cplusplus
}
#endif

#endif // DMNETBRIDGE_H
