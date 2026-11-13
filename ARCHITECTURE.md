# Architecture

This document describes how packets move through GTProxy and where this fork
hooks in. It was written by reading the source at the fork point
(`f51c5a3`); anything I could not verify from the code is listed under
[Assumptions](#assumptions-and-unverified-points).

## Process and threads

| Thread | What runs there |
|---|---|
| **main** | `core::Core::run()`: a ~5 ms loop calling `Server::process()`, `Client::process()` (ENet `enet_host_service`) and the Lua scheduler. **All packet handling, every dispatcher listener, and every ENet write happens here.** |
| HTTPS thread | `core::WebServer` (cpp-httplib `SSLServer`, `0.0.0.0:443`): answers the game client's `POST /growtopia/server_data.php`. |
| control-plane threads (this fork) | REST (cpp-httplib, `127.0.0.1`) and the WebSocket acceptor/reader threads. They never touch ENet; they enqueue commands for the main thread. |

ENet is not thread-safe, so the rule for this fork is: **other threads may
only enqueue work for the main loop** (`control::CommandQueue`).

## Connection flow

```
 Growtopia-compatible client                 GTProxy                          your server
 ───────────────────────────                 ───────                          ───────────
 POST /growtopia/server_data.php ──────────▶ WebServer (TLS :443)
                                              │ POST same path upstream ───────────▶ server_data.php
                                              │ ◀───────────────────────────────────  server|<ip> port|<enet port> …
                                              │ remembers pending_address_/pending_port_
 ◀──────────────────────────────────────────  rewrites to server|127.0.0.1 port|<proxy port> type2|1
 ENet connect  :16999 ─────────────────────▶ network::Server  (listens, 1 peer, 2 channels)
                                              │ ClientConnect event
                                              ▼
                                            WebServer::on_client_connect / ConnectionHandler
                                              │ network::Client::connect(pending_address_, pending_port_)
                                              └────────────────────────────────▶ ENet connect
                                                                                  ServerConnect event
```

Sub-server redirects (`OnSendToServer`) are handled by
`ConnectionHandler::setup_on_send_to_server_handler`: it stores the real
address/port as pending, rewrites the packet to `127.0.0.1:<proxy port>` and
cancels the original, so the client reconnects to the proxy instead of leaving.

Both ENet hosts use the range-coder compressor, `enet_crc32` checksums and the
"new packet" header flags (`usingNewPacket` / `usingNewPacketForServer`). These
are properties of the patched ENet in `lib/enet`.

## Packet flow (the important part)

```
 client ──▶ Server::on_receive ─┐                       ┌─ Client::on_receive ◀── upstream server
   (ServerBound)                │                       │      (ClientBound)
                                ▼                       ▼
                       PacketDecoder::decode  (reads NetMessageType, text / game packet / variant)
                                │
                  decoded?  ────┤──── no ──▶ RawPacketEvent only
                                │
                                ▼
                 PacketEventRegistry::emit  ──▶  TypedPacketEvent<PacketId>   (only for registered ids)
                                │                 listeners run in priority order; any may cancel()
                      canceled? ├── yes ─▶ STOP: packet is swallowed, raw event never fires
                                ▼
              Dispatcher.dispatch(RawPacketEvent{ClientBoundPacket | ServerBoundPacket, bytes})
                                │
                                ▼
                 ForwardingHandler listener  ──▶ opposite_connection.write(bytes)
```

Source locations:

* `src/network/server.cpp` `Server::on_receive` / `src/network/client.cpp` `Client::on_receive`: entry points. `Server` rejects packets `< 4` or `> 16384` bytes.
* `src/packet/packet_decoder.cpp`: wire format. First 4 bytes are a little-endian `NetMessageType`. Types 2/3 carry NUL-terminated `key|value` text. Type 4 carries a packed 56-byte `GameUpdatePacket`, then `data_size` extra bytes; `PACKET_CALL_FUNCTION` extras are a *variant list* (count byte, then `index, type, value` triples).
* `src/packet/packet_event_registry.hpp` + `register_packet_events.hpp`: maps `PacketId` to typed events.
* `src/core/handlers/forwarding_handler.cpp`: the single place where unmodified packets are forwarded. **A packet that no typed listener cancels is forwarded verbatim from the raw bytes** (not re-serialised), so unknown packet types pass through untouched.
* `src/core/handlers/connection_handler.cpp`: connection lifecycle, `OnSendToServer` rewrite, `items.dat` caching.
* `src/command/command_handler.cpp`: listens (priority "highest") on the `Input` text packet; `/commands` typed in-game are executed and the packet is cancelled.
* Packets the proxy *originates* (commands, scripts, rewrites) go through `PacketHelper::write(packet, connection)` → `IConnection::write` and therefore **do not pass through the raw forward path**.

Consequences that shaped the design:

1. A pure "tap on the raw event" would miss packets that were cancelled (e.g. the original `OnSendToServer`) and packets the proxy itself writes.
2. Because handlers are synchronous on the main thread, the raw event for a packet fires *inside* `on_receive`. A held (intercepted) packet therefore cannot block: it is parked in a queue and written later from a control-plane command.

## Hook points used by this fork

| Need | Hook | Where |
|---|---|---|
| See every packet received (even ones later cancelled) | `WireTap` called first thing in `Server::on_receive` / `Client::on_receive` | `src/control/packet_flow.*`, `src/network/*` |
| Modify / drop / hold before forwarding | `ForwardingHandler` raw listeners call `PacketFlow::forward()` instead of writing directly | `src/core/handlers/forwarding_handler.cpp` |
| See packets the proxy originates | `write observer` on `network::Server` / `network::Client` | `src/network/*` |
| Connect / disconnect | existing `ClientConnect`, `ServerConnect`, `ClientDisconnect`, `ServerDisconnect` events | `src/event/event.hpp` |
| Run commands from other threads | `CommandQueue::drain()` called each `Core::run` iteration | `src/core/core.cpp` |

Pipeline order inside `PacketFlow::forward` (see `src/control/packet_flow.hpp`):

```
record+publish  →  plugin hooks (onPacketIn/Out; may edit/drop)  →  intercept (hold?)  →  write to peer
```

## Existing scripting (`src/scripting`, `scripts/`)

Upstream already embeds Lua through sol2: a single shared `sol::state`, safe
libraries only (no `io`/`os`/`package`), and bindings for events, commands,
packets, world and items. It auto-loads every `scripts/*.lua` at start-up. That
is kept untouched. The fork's **plugin system** (`src/plugin`) is separate: one
isolated Lua state per plugin, with enable/disable/reload, instruction limits
and per-plugin logs, so a bad plugin cannot take down the others. See
`docs/PLUGINS.md`.

## Assumptions and unverified points

* **Upstream HEAD did not compile against eventpp 0.1.3** (the dispatcher policy used `get_event` / `can_continue_invoking`, which eventpp 0.1.x does not look for; and three call sites still used the removed `appendListener`/`removeListener` names). Fixed in the first commit of this fork. I could not check which eventpp version upstream's Conan resolves to in their CI.
* **Wire formats come from reading the decoder only.** I had no live client or server, so no capture was compared against a real session. Structure sizes (56-byte `GameUpdatePacket`) are computed from the `#pragma pack(1)` struct.
* **`config.server.address` is the upstream HTTP host** used for `server_data.php`, while **`config.server.port` is the proxy's own ENet listen port** (default 16999). Easy to confuse; the fork's profiles keep that distinction.
* The upstream web server binds `0.0.0.0:443` with the bundled `resources/cert.pem`. A private server on the same machine also wanting `:443` will conflict. The fork adds a "direct" profile mode that skips the upstream `server_data.php` fetch.
* ENet writes use `ENET_PACKET_FLAG_RELIABLE` always and channel 0 unless the packet type says otherwise; unreliable/unsequenced semantics of the original packets are **not** preserved on forward. That is existing behaviour.
* Windows could not be built in the sandbox that produced this fork (see README "Untested in sandbox").
