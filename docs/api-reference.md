# dmnetbridge API Reference

See [dmnetbridge.md](dmnetbridge.md) for the architecture and rationale
behind this API's shape.

## Send

| Function                                                                          | Description                                                        |
|------------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `dmnetbridge_send(dst_ip, ethertype, payload, payload_len, arp_timeout_ms, out_iface)` | Route, resolve, frame and transmit `payload` to `dst_ip`. Returns `0`, `-EINVAL`, `-ENETUNREACH`, `-ENODEV`, `-EHOSTUNREACH`, `-ENOMEM`, or `-EIO`. `out_iface` is optional. |
| `dmnetbridge_send_on_iface(iface, dst_ip, ethertype, payload, payload_len, arp_timeout_ms)` | Like `dmnetbridge_send()`, but treats `dst_ip` as on-link on `iface` (next-hop = `dst_ip`) instead of calling `dmroute_lookup()` - use when the caller already knows the interface and there may be no route yet (e.g. a DHCP client's pre-lease broadcast). Returns `0` or a negative errno. |
| `dmnetbridge_get_source_address(dst, out_src)`                                     | The source address `dmnetbridge_send()` would use to reach `dst` (egress interface's own IP). |
| `dmnetbridge_get_mtu(dst, out_mtu)`                                                | The MTU `dmnetbridge_send()` would transmit through to reach `dst`. `*out_mtu` is left untouched on failure. |

## Receive

| Function                                    | Description                                                        |
|------------------------------------------------|-----------------------------------------------------------------------|
| `dmnetbridge_handle_netif_rx(iface)`            | Blocking pump loop for one interface - reads frames and dispatches them (`dmarp_note_frame()` + every `packet_received` DIF implementor) until the interface is gone. Run one per interface, in its own thread. |
| `packet_received(iface, frame, frame_len)` (DIF) | Declared here, implemented by `dmip`. Delivers one raw received frame to an interested protocol module. |

## Lifecycle

| Function                | Description                                                        |
|----------------------------|-----------------------------------------------------------------------|
| `dmnetbridge_reset(void)`  | Clears dmnetbridge's own "which interfaces are being pumped" bookkeeping - call before spawning pump threads on a (re)start. |

All addresses use `dmroute_addr_t` from dmroute;
interfaces use `dmnetif_iface_t` from [dmnetif](../../dmnetif).
