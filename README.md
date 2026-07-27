# dmnetbridge

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmnetbridge is a [DMOD](https://github.com/choco-technologies/dmod) library
module that sits between IP-address-level code (`dmip`) and the
netif/routing/ARP layer below it. It answers the two questions a
multi-interface, receive-without-naming-an-interface network stack needs
answered somewhere:

- **Sending**: which interface, and which MAC address, does "send to this
  IP" actually resolve to? `dmnetbridge_send()` looks up the route, resolves
  the next hop, builds the Ethernet header, and transmits - so `dmip` only
  ever has to think in IP addresses.
- **Receiving**: who reads a given interface's incoming frames, and how does
  a completed frame reach whichever code wants it, when that's a different
  thread than whichever one is doing the reading? `dmnetbridge_handle_netif_rx()`
  is a blocking per-interface pump loop; the `packet_received` DIF is how a
  completed frame reaches every interested protocol module.

```
┌──────────────────────────────────────────────┐
│               dmip (dmudp, ...)               │
├──────────────────────────────────────────────┤
│                DMNETBRIDGE                    │
│  send: route lookup + ARP + Ethernet framing  │
│  receive: pump loop + packet_received DIF     │
├──────────────┬───────────────┬────────────────┤
│   DMROUTE    │    DMNETIF    │     DMARP      │
│ route table  │  named ifaces │  MAC resolve   │
└──────────────┴───────────────┴────────────────┘
```

See [docs/dmnetbridge.md](docs/dmnetbridge.md) for the full rationale
(including why this had to be its own module, not more of `dmip`) and
[docs/api-reference.md](docs/api-reference.md) for the complete API.

## Key design points

- **`dmnetbridge_send()` vs `dmnetbridge_send_on_iface()`.** The former
  looks up the egress route via `dmroute_lookup()`; the latter is told the
  interface directly and treats the destination as on-link, skipping
  routing entirely. Use `_send_on_iface()` when a caller already knows which
  interface to use and there may be no route yet - e.g. a DHCP client's
  pre-lease broadcast.
- **`packet_received` is a DIF (1:N, discovered at runtime); `dmarp_note_frame()`
  is a plain Built-in API call.** dmnetbridge already hard-depends on
  `dmarp` for `dmarp_resolve()`, so routing every received frame to it too
  is just another ordinary call. `dmip`, by contrast, is exactly the case a
  DIF is for: dmnetbridge has no reason to hard-depend on every current or
  future protocol module that might want raw frames, so `packet_received`
  is declared once and implementors are discovered dynamically via
  `Dmod_GetNextDifModule()`/`Dmod_GetDifFunction()`.
- **One pump loop per interface, enforced.** `dmnetbridge_handle_netif_rx()`
  is the *only* code path that should call `dmnetif_receive()` on a given
  interface once `networkd` owns it - a second concurrent reader would race
  it and could steal frames. A second call for an interface already being
  pumped returns immediately instead of starting a competing loop.
- **Restart safety.** `dmnetbridge_reset()` clears dmnetbridge's own
  bookkeeping of "which interfaces currently have a pump loop running" (used
  to enforce the point above) - `networkd` calls it before spawning any pump
  threads of its own, so a previous instance's bookkeeping can't make a
  fresh start think an interface is already being pumped. It does not touch
  any other module's state (dmip's receive queue, dmarp's cache, dmroute's
  table).
- **Known limitation:** `dmnetif_is_present(iface)` is only re-checked
  between `dmnetif_receive()` calls, not while one is in progress - a real
  driver blocks indefinitely inside that call waiting for the next frame,
  so an interface that disappears while nothing is arriving on it won't be
  noticed until the next frame (if any) or never. Fixing this needs a
  bounded-timeout read, which `dmnetif_receive()` does not have today.

## Building

dmnetbridge is a standalone DMOD module: its `CMakeLists.txt` fetches `dmod`
itself via CMake's `FetchContent` (defaulting to the `develop` branch), so
no other repository needs to be checked out first.

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This builds both the `dmnetbridge` library module and its `test_dmnetbridge`
test binary (see [Testing](#testing) below). Pass
`-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching it from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

The Makefile requires an existing `dmod` checkout (there is no
FetchContent equivalent for Make) and builds the `dmnetbridge` module
itself, not the test suite.

## Testing

Tests are a plain DMOD test module built with `dmod_add_test()`:
`tests/dmnetbridge_test.c` registers each test case with `DMOD_TEST_STEP()`
and runs against two fixture interfaces (`"test0"`/`"test1"`) registered
against `/dev/null` - real enough for `dmnetif_register()`'s underlying file
open to succeed without needing an actual driver behind it (same pattern
[dmarp](https://github.com/choco-technologies/dmarp)'s own tests use).

Despite being built as a native ELF binary, `test_dmnetbridge` is a DMOD
module like any other and must be run through `dmod_loader`, not invoked
directly. `dmf-get install` first resolves `test_dmnetbridge`'s own
dependency closure (`dmarp`, `dmnetif`, `dmroute`, `dmosi`, ...) into
`DMOD_DMF_DIR` so the loader can find every module `test_dmnetbridge.dmf`
needs at load time - exactly what CI does:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmnetbridge-local.dmd -y
dmod_loader build/dmf/test_dmnetbridge.dmf
```

It discovers and runs every `DMOD_TEST_STEP()` automatically and prints a
`Results: X/Y passed` summary.

`tests/dmnetbridge_test.c` covers:

- **`dmnetbridge_get_source_address()`** - `-ENETUNREACH` without a route,
  returning the egress interface's own IP once a route exists, and
  `NULL`/wrong-family argument rejection.
- **`dmnetbridge_send()`** - `-ENETUNREACH` without a route, `-EHOSTUNREACH`
  on an ARP cache miss (no real driver means `dmarp_resolve()` can never
  actually succeed on a miss), reaching the `-EIO` wall at `dmnetif_send()`
  once the ARP cache is pre-seeded (the deepest this pipeline can be
  exercised without a real driver), the optional `out_iface` parameter
  (including that it's left untouched on failure), `NULL`/wrong-family
  argument rejection, and that a zero-length payload is allowed (a valid
  "just the IP header, no data" packet), not rejected as `-EINVAL`.
- **`dmnetbridge_send_on_iface()`** - bypasses a missing route entirely,
  reaching the same `-EIO` wall as `dmnetbridge_send()` with a real route in
  place.
- **`dmnetbridge_reset()`** - safe to call repeatedly with nothing pumping.
- **`dmnetbridge_handle_netif_rx()`** - the one-thread-per-interface guard: a
  second call for an interface already being pumped by a background thread
  returns immediately instead of starting a competing loop.

Since the fixture interfaces have no real driver behind them, they can
never actually come "up" or send/receive a real frame -
`dmnetbridge_handle_netif_rx()` itself is not exercised end-to-end beyond
that guard (it would spin forever against these fixtures). Full
receive-path coverage (a real frame flowing through `dmarp_note_frame()`
and the `packet_received` DIF to a real implementor) belongs to `dmip`'s own
tests, since this test binary doesn't link `dmip`.

## Usage

### Sending a packet to an IP address

The common case: hand `dmnetbridge_send()` a destination IP, an ethertype,
and an already-built payload (e.g. a complete IP packet or fragment) - it
takes care of routing, ARP resolution, and framing.

```c
#include "dmnetbridge.h"
#include "dmroute.h"

int send_ipv4_packet(const dmroute_addr_t* dst_ip, const void* ip_packet, size_t len)
{
    dmnetif_iface_t out_iface = NULL;
    int ret = dmnetbridge_send(dst_ip, 0x0800 /* IPv4 */, ip_packet, len,
                                DMARP_DEFAULT_TIMEOUT_MS, &out_iface);
    /* 0 on success; -EINVAL / -ENETUNREACH / -ENODEV / -EHOSTUNREACH /
     * -ENOMEM / -EIO on failure - see docs/api-reference.md */
    return ret;
}
```

`out_iface` is optional - pass `NULL` if the caller doesn't need to know
which interface the packet actually went out on.

### Sending before a route exists

`dmnetbridge_send_on_iface()` is for a caller that already knows which
interface to use and can't rely on a route being there yet - e.g. a DHCP
client's pre-lease broadcast, where no route to the destination can exist
until a lease is obtained:

```c
#include "dmnetbridge.h"

int send_dhcp_discover(dmnetif_iface_t iface, const void* payload, size_t len)
{
    dmroute_addr_t broadcast = { .family = dmroute_family_v4,
                                  .addr.v4 = { 255, 255, 255, 255 } };

    return dmnetbridge_send_on_iface(iface, &broadcast, 0x0800, payload, len,
                                      DMARP_DEFAULT_TIMEOUT_MS);
}
```

### Finding the source address and MTU before a payload is ready

`dmip_v4_send()`-style callers often need to know the egress interface's own
IP address (to fill in an unset source address) or its MTU (to size
fragments) *before* they have a payload built - these expose exactly the
routing step `dmnetbridge_send()` does internally, without sending anything:

```c
#include "dmnetbridge.h"

dmroute_addr_t src = { 0 };
if (dmnetbridge_get_source_address(&dst_ip, &src) == 0)
{
    /* src.family is dmroute_family_none if the egress interface has no
     * address assigned yet - not treated as an error. */
}

uint16_t mtu = DMNETIF_DEFAULT_MTU; /* seed with a default first */
dmnetbridge_get_mtu(&dst_ip, &mtu); /* left untouched on failure */
```

### Implementing the `packet_received` DIF

Any module that wants raw received frames implements dmnetbridge's
`packet_received` DIF rather than dmnetbridge hard-depending on it - this is
exactly how `dmip` receives frames today:

```c
#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod.h"
#include "dmnetbridge.h"

dmod_dmnetbridge_dif_api_declaration(1.0, mymodule, void, _packet_received,
    ( dmnetif_iface_t iface, const uint8_t* frame, size_t frame_len ))
{
    if (frame == NULL || frame_len < 14u /* Ethernet header */)
        return;

    /* Check the ethertype at frame[12..13] and ignore what isn't yours -
     * dmnetbridge_handle_netif_rx() calls every implementor for every
     * frame, regardless of ethertype. */
}
```

The implementing module's `CMakeLists.txt` needs `dmod_link_modules(...
dmnetbridge)` and `set(DMOD_DIF_IMPLS dmnetbridge)` next to its
`dmod_add_library()` call.

### Running the receive pump

`dmnetbridge_handle_netif_rx()` blocks, so it's meant to be run one call per
interface, each from its own thread - the `networkd` service spawns one per
interface already registered with `dmnetif` at startup:

```c
#include "dmnetbridge.h"
#include "dmosi.h"

static void pump_thread_entry(void* arg)
{
    dmnetbridge_handle_netif_rx((dmnetif_iface_t)arg);
}

void start_pump_for(dmnetif_iface_t iface)
{
    dmosi_thread_create(pump_thread_entry, iface, 0, 4096, "netif-pump", NULL);
}
```

It returns once `dmnetif_is_present(iface)` goes false (e.g. the interface
was hot-unplugged or unregistered) - it does not loop forever.

### Restart safety

A restartable service (`networkd`) calls `dmnetbridge_reset()` before
spawning any pump threads of its own, so a previous instance's bookkeeping
can't make a fresh start think an interface is already being pumped:

```c
#include "dmnetbridge.h"

dmnetbridge_reset();
/* ... now safe to spawn dmnetbridge_handle_netif_rx() threads ... */
```

## API Overview

### Send

| Function                                                                                | Description                                                                                                                        |
|-------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|
| `dmnetbridge_send(dst_ip, ethertype, payload, payload_len, arp_timeout_ms, out_iface)`      | Route, resolve, frame and transmit `payload` to `dst_ip`. `out_iface` is optional.                                                     |
| `dmnetbridge_send_on_iface(iface, dst_ip, ethertype, payload, payload_len, arp_timeout_ms)` | Like `_send()`, but treats `dst_ip` as on-link on `iface`, skipping routing.                                                          |
| `dmnetbridge_get_source_address(dst, out_src)`                                             | The source address `dmnetbridge_send()` would use to reach `dst` (egress interface's own IP).                                        |
| `dmnetbridge_get_mtu(dst, out_mtu)`                                                        | The MTU `dmnetbridge_send()` would transmit through to reach `dst`. `*out_mtu` is left untouched on failure.                          |

### Receive

| Function                                          | Description                                                                                                              |
|------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `dmnetbridge_handle_netif_rx(iface)`                  | Blocking pump loop for one interface - reads frames and dispatches them (`dmarp_note_frame()` + every `packet_received` DIF implementor) until the interface is gone. Run one per interface, in its own thread. |
| `packet_received(iface, frame, frame_len)` (DIF)      | Declared here, implemented by `dmip`. Delivers one raw received frame to an interested protocol module.                  |

### Lifecycle

| Function                | Description                                                                                                    |
|----------------------------|----------------------------------------------------------------------------------------------------------------------|
| `dmnetbridge_reset()`      | Clears dmnetbridge's own "which interfaces are being pumped" bookkeeping - call before spawning pump threads on a (re)start. |

All addresses use `dmroute_addr_t` from
[dmroute](https://github.com/choco-technologies/dmroute); interfaces use
`dmnetif_iface_t` from
[dmnetif](https://github.com/choco-technologies/dmnetif). Full
parameter/return documentation (including every error code) lives in
[docs/api-reference.md](docs/api-reference.md).

## Documentation

See the `docs/` directory:

- **[dmnetbridge.md](docs/dmnetbridge.md)** - Architecture and rationale
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmnetbridge`.

## Dependencies

- [`dmroute`](https://github.com/choco-technologies/dmroute) - route lookup
  for the send path (`dmroute_lookup()`,
  `dmroute_get_iface_name()`/`_get_gateway()`), plus the shared
  `dmroute_addr_t` type every address field uses.
- [`dmnetif`](https://github.com/choco-technologies/dmnetif) - the interface
  registry and frame I/O (`dmnetif_find_by_name()`, `dmnetif_send()`/
  `_receive()`, `dmnetif_get_mac_address()`/`_get_ip_address()`/`_get_mtu()`,
  `dmnetif_is_present()`).
- [`dmarp`](https://github.com/choco-technologies/dmarp) -
  `dmarp_resolve()` before sending, `dmarp_note_frame()` for every received
  frame (opportunistic ARP learning).
- `dmlist` - backs the "which interfaces are being pumped" bookkeeping.
- `dmosi` - the mutex guarding that bookkeeping.

## Project Structure

```
dmnetbridge/
├── docs/              # Documentation (markdown format)
│   ├── README.md
│   ├── dmnetbridge.md
│   └── api-reference.md
├── include/           # Public headers
│   └── dmnetbridge.h
├── src/
│   └── dmnetbridge.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmnetbridge_test.c
├── CMakeLists.txt
├── Makefile
├── manifest.dmm
└── dmnetbridge.dmr
```

## Author

Patryk Kubiak

## License

MIT License (see [LICENSE](LICENSE) file for details)
