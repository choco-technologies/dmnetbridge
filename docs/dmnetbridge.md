# DMNETBRIDGE - DMOD Network Bridge

## Overview

dmnetbridge is the layer between IP-address-level code (`dmip`) and
netif/routing/ARP. Before it existed, `dmip` did its own route lookup,
ARP resolution, and frame I/O, and every receive call took an explicit
`dmnetif_iface_t` - meaning a caller had to know, and poll, one specific
interface at a time. dmnetbridge exists to answer two design questions
that come with a multi-interface, receive-without-naming-an-interface
model:

- **Sending**: which interface, and which MAC address, does "send to this
  IP" actually resolve to? `dmnetbridge_send()` answers that once, so
  `dmip` only ever has to think in IP addresses.
- **Receiving**: who reads a given interface's incoming frames, and how
  does a completed packet reach whichever code actually wants it, when
  that's a different thread than whichever one is doing the reading?
  `dmnetbridge_handle_netif_rx()` and the `packet_received` DIF answer
  that.

```
┌──────────────────────────────────────────────┐
│               dmip (dmudp, ...)               │
├──────────────────────────────────────────────┤
│                DMNETBRIDGE                    │
│  send: route lookup + ARP + Ethernet framing  │
│  receive: pump loop + packet_received DIF      │
├──────────────┬───────────────┬────────────────┤
│   DMROUTE    │    DMNETIF    │     DMARP      │
│ route table  │  named ifaces │  MAC resolve   │
└──────────────┴───────────────┴────────────────┘
```

## Why this had to be a separate module, not more of dmip

Before dmnetbridge, `dmip_v4_send()` called `dmroute_lookup()`/
`dmarp_resolve()`/`dmnetif_send()` inline, and `dmip_v4_receive()`/
`_v6_receive()`/`_receive()` each took an `iface` parameter and made their
own `dmnetif_receive()` call. Removing the `iface` parameter (so a caller
could receive "the next packet on any interface" instead of polling one
at a time) meant *something* had to own reading every interface - and once
something (`networkd`) does that in a dedicated thread per interface,
`dmip` calling `dmnetif_receive()` itself would be a second, racing reader
of the same interface. Pulling routing/ARP/frame-I/O out into their own
module, with `dmip` reduced to pure IP-address-level logic, was the
natural place to draw that line - see `docs/dmip.md` in `lib/dmip` for
the receiving side of this from dmip's perspective.

## Sending

`dmnetbridge_send(dst_ip, ethertype, payload, payload_len, arp_timeout_ms, out_iface)`:

1. `dmroute_lookup(dst_ip)` - the egress route. `-ENETUNREACH` if none.
2. `dmnetif_find_by_name(dmroute_get_iface_name(route))` - the egress
   interface. `-ENODEV` if it's no longer registered.
3. Next hop = the route's gateway if it has one, else `dst_ip` itself
   (a directly-connected destination).
4. `dmarp_resolve(iface, next_hop, ..., arp_timeout_ms)` - the next hop's
   MAC address. `-EHOSTUNREACH` on failure (whatever the underlying
   reason - cache miss with no reply, a down interface, ...).
5. Build a 14-byte Ethernet header (resolved destination MAC, the
   interface's own MAC, `ethertype`) in front of `payload`, and
   `dmnetif_send()` it. `-EIO` if the driver rejects the frame (including
   simply being down - see `dmnetif_send()`'s own contract).

`dmnetbridge_get_source_address(dst, out_src)` and
`dmnetbridge_get_mtu(dst, out_mtu)` expose steps 1-3 (and the resulting
interface's IP address / MTU) on their own, for a caller (`dmip_v4_send()`)
that needs to know the answer *before* it has a payload ready to actually
send - filling in an unset source address, and sizing fragments to the
real egress interface's MTU, respectively.

## Receiving

`dmnetbridge_handle_netif_rx(iface)` is a blocking pump loop, one call per
interface, meant to be run from its own dedicated thread (the `networkd`
service spawns one per interface already registered with `dmnetif` at
startup - see `services/networkd`). For as long as
`dmnetif_is_present(iface)` stays true, it calls `dmnetif_receive(iface, ...)`
and, for every frame actually received:

1. Calls `dmarp_note_frame(iface, frame, frame_len)` - a plain Built-in
   API call, not a DIF, since dmnetbridge already hard-depends on dmarp
   for `dmarp_resolve()` (see `lib/dmarp/docs/dmarp.md`'s "Opportunistic
   learning" section).
2. Broadcasts the frame to every module implementing the
   `packet_received` DIF (`dmip`, today), via
   `Dmod_GetNextDifModule()`/`Dmod_GetDifFunction()`.

This is the *only* code path that should call `dmnetif_receive()` on a
given interface once `networkd` owns it - a second concurrent reader
(e.g. `dmarp_resolve()` polling on its own, the way it used to) would race
it and could steal frames meant for the other reader.

### Why `packet_received` is a DIF, but `dmarp_note_frame()` isn't

dmnetbridge already has a hard compile-time dependency on `dmarp` (for
`dmarp_resolve()`), so routing every received frame to it too is just
another ordinary Built-in API call - no indirection needed, and it also
sidesteps a real problem: `dmarp` would need dmnetbridge's header (for a
DIF signature) while dmnetbridge's implementation already needs dmarp's
header, a genuine circular in-tree dependency. `dmip`, on the other hand,
is exactly the case a DIF is for: dmnetbridge has no reason to hard-depend
on every current or future protocol module that might want raw frames, so
`packet_received` is declared once and discovered at runtime instead.

## Restart safety

`networkd` is a restartable service. `dmnetbridge_reset()` clears
dmnetbridge's own bookkeeping of "which interfaces currently have a pump
loop running" (used internally to refuse a second concurrent
`dmnetbridge_handle_netif_rx()` call for the same interface) - `networkd`
calls it before spawning any pump threads of its own, so a previous
instance's bookkeeping can't make a fresh start think an interface is
already being pumped when nothing is actually reading it anymore. It does
not touch any other module's state (dmip's receive queue, dmarp's cache,
dmroute's table) - each of those is that module's own lifecycle to manage.

## Dependencies

- `dmroute` - route lookup for the send path (`dmroute_lookup()`,
  `dmroute_get_iface_name()`/`_get_gateway()`)
- `dmnetif` - the interface registry and frame I/O
  (`dmnetif_find_by_name()`, `dmnetif_send()`/`_receive()`,
  `dmnetif_get_mac_address()`/`_get_ip_address()`/`_get_mtu()`,
  `dmnetif_is_present()`)
- `dmarp` - `dmarp_resolve()` before sending, `dmarp_note_frame()` for
  every received frame
- `dmlist` - backs the "which interfaces are being pumped" bookkeeping
- `dmosi` - mutex guarding that bookkeeping
