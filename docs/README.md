# dmnetbridge Documentation

Welcome to the dmnetbridge module documentation.

## Contents

- **[dmnetbridge.md](dmnetbridge.md)** - Architecture and rationale
- **[api-reference.md](api-reference.md)** - Complete API documentation

## Quick Reference

```c
#include "dmnetbridge.h"

// Send an IPv4 packet - routes, resolves ARP, frames, and transmits
dmnetbridge_send(&dst_ip, 0x0800, payload, payload_len, DMARP_DEFAULT_TIMEOUT_MS, NULL);

// Send on a known interface, bypassing routing (e.g. DHCP pre-lease)
dmnetbridge_send_on_iface(iface, &dst_ip, 0x0800, payload, payload_len, DMARP_DEFAULT_TIMEOUT_MS);

// Query the route without sending anything
dmnetbridge_get_source_address(&dst_ip, &src_ip);
dmnetbridge_get_mtu(&dst_ip, &mtu);

// Pump one interface's received frames (run one per interface, own thread)
dmnetbridge_handle_netif_rx(iface);

// Clear pump bookkeeping before a service (re)start
dmnetbridge_reset();
```

See [../README.md](../README.md) for full usage examples, including how to
implement the `packet_received` DIF.

View documentation using `dmf-man`:

```bash
dmf-man dmnetbridge          # Main documentation
dmf-man dmnetbridge api      # API reference
```
