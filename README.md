# GTProxy — private-server protocol debugger

A fork of [ZTzTopia/GTProxy](https://github.com/ZTzTopia/GTProxy) turned into a
**UI-driven packet debugging proxy for Growtopia-compatible servers that you run
yourself** (for example a self-hosted GrowServer). It sits between a client and
your server, shows every packet in a web dashboard, and lets you hold, edit,
drop, inject, script and replay traffic so you can learn and test the protocol.

> ### Intended use
> This tool is for **servers you control** (127.0.0.1 or your LAN) and for
> learning the packet protocol. It is not built for, tuned for, or documented
> against any official or third-party live service, and it contains no
> ban-evasion, identity-spoofing, credential-capture, remote-control or
> mass-action features. Use it only against infrastructure you are allowed to test.
> The control plane only listens on loopback and refuses requests from other web origins.

## What you get

| | |
|---|---|
| **Live packet stream** | Every packet in both directions, timestamped, colour-coded by kind (text / variant / game / hello / raw), with direction, size and what happened to it. |
| **Filter & search** | Direction, kind, free text (name, summary, decoded text) and byte-aligned hex search. Pause / resume / clear. |
| **Decoder** | Text packets (`key\|value` lines), variant lists (function calls), game/tank packet headers + extra data; a raw hex dump for anything else or anything malformed. |
| **Intercept (Burp-style)** | Hold packets that match rules, edit fields or raw hex, then forward or drop. Ordering is preserved. |
| **Inject** | Send hand-built packets to the server or the client; save them as templates. |
| **Dashboard** | Peer connection state, round-trip latency, packet/byte counters and rates, connection log. |
| **World inspector** | Vending machines (item, raw price, tile), display blocks and dropped items with quantities per world, remembered across worlds and searchable by item. See [docs/WORLDS.md](docs/WORLDS.md) for what is and is not reliable. |
| **Plugins** | Sandboxed Lua plugins with `onPacketIn/onPacketOut/onConnect/onDisconnect`; manage them (enable / disable / reload / logs) in the UI. Two examples included. |
| **Profiles** | Several server profiles in `config.json`, switchable from the UI. |
| **Record & replay** | Record sessions to `sessions/*.jsonl`; replay them offline through your plugins. |
| **Structured logging** | `proxy.jsonl` (one JSON object per line) next to the human-readable `proxy.log`. |

Everything the upstream proxy already did is kept: ENet transport, the HTTPS
`server_data.php` endpoint, sub-server (`OnSendToServer`) redirection, in-game
`/commands` and the original `scripts/*.lua` engine.

## How it fits together

```
 game client ──ENet──▶ [ proxy: Server ]──▶ PacketFlow ──▶ [ proxy: Client ]──ENet──▶ your server
                                              │  ▲
                       hooks (plugins) ◀──────┤  │ inject / edit / drop / release
                       event stream ──────────┘  │
                                              Control plane (127.0.0.1)
                                              WebSocket :7302 + REST :7301
                                                   ▲
                                              Angular UI (web-ui/)
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the packet flow and every hook point,
[docs/API.md](docs/API.md) for the control-plane protocol and
[docs/PLUGINS.md](docs/PLUGINS.md) for the plugin guide.

## Build

### Core (C++23, CMake 3.24+)

**With Conan (Windows, macOS, Linux — the upstream way):**

```bash
pip install "conan>2.0"
git clone --recurse-submodules <this repository>
cd GTProxy
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build --build-config Debug --output-on-failure
```

**Without Conan (Ubuntu 24.04; this is what the fork was developed and tested on):**

```bash
scripts/dev/linux-deps.sh "$HOME/.gtproxy-deps"          # apt packages + a few header-only libs
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGTPROXY_USE_CONAN=OFF -DCMAKE_PREFIX_PATH="$HOME/.gtproxy-deps"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Targets: `Proxy` (the proxy), `GTProxy_tests`, `GTProxy_fake_peers` (dev tool, below).
The post-build step copies `resources/`, the example `plugins/` and — if you built it — the UI next to the executable.

### UI (Angular)

```bash
cd web-ui
npm ci
npm run build             # -> web-ui/dist/web-ui/browser
npx ng test --watch=false # unit tests (vitest)
```

Re-run the CMake build afterwards to copy it to `<build>/src/ui`, or point
`control.ui_dir` in `config.json` at `web-ui/dist/web-ui/browser`. For UI
development run the proxy and `npm start` (port 4200); the dev page talks to the
proxy on the same host (`ws://localhost:7302`), which the origin check allows.

## Run it against your own server

1. Start the proxy from the directory containing `resources/` (the build puts the exe there). On first start it writes `config.json`.
2. Open **http://127.0.0.1:7301/** for the dashboard.
3. Choose a **server profile** (UI: *Profiles & sessions*, or edit `config.json`):

   ```jsonc
   "profiles": [
     // your server runs on this machine, you know its ENet port:
     { "name": "localhost", "direct": true, "enet_address": "127.0.0.1", "enet_port": 17091 },
     // your server is on the LAN and serves /growtopia/server_data.php over HTTPS:
     { "name": "lan-server", "address": "192.168.1.100", "port": 443, "tls": true }
   ],
   "active_profile": "localhost"
   ```
   * **non-direct** profile: the proxy answers the client's `POST /growtopia/server_data.php` (it listens on `0.0.0.0:443`, certificate in `resources/`) by forwarding it to `address:port`, then rewrites the reply to point the client at the proxy (`127.0.0.1:<server.port>`, default 16999) and remembers the real ENet endpoint.
   * **direct** profile: no upstream request is made; the proxy replies to `server_data.php` itself and connects ENet to `enet_address:enet_port`. Use it when your server does not serve that endpoint, or when it would clash with the proxy's own port 443 on the same machine.
   * `client.dns_server` defaults to `"system"` (no outside lookups). `"cloudflare"` / `"google"` use DNS-over-HTTPS.
4. Point your client at the proxy (hosts-file entry for the name the client resolves, or a patched client — whichever you already use for your server). **How you redirect your client is outside this repository.**
5. The default ports `17091` (ENet) and `443` (HTTPS) in the examples are assumptions, not values verified against any particular server: set whatever yours uses.

### Try it with no client and no server

`GTProxy_fake_peers` plays both ends: it starts a stand-in server, asks the proxy's `server_data.php`, connects a stand-in client and exchanges a scripted conversation every second. With the default `localhost` direct profile:

```bash
./build/src/Proxy            # terminal 1
./build/GTProxy_fake_peers   # terminal 2
```

then watch the *Packets* tab, try intercept, enable the example plugins, record and replay a session.

### Plugins in one minute

```lua
-- plugins/hello/main.lua
function onPacketOut(pkt)
  if pkt.kind == "text" and pkt:get("text") == "!ping" then
    gt.inject(gt.CLIENT_BOUND, { kind = "variant", header = { net_id = 4294967295 },
      values = { { type = "string", value = "OnConsoleMessage" }, { type = "string", value = "pong" } } })
    pkt:drop()
  end
end
```

Press *Rescan folder* in the Plugins tab and tick the checkbox. Full guide, API and sandbox limits: [docs/PLUGINS.md](docs/PLUGINS.md). Examples: `plugins/packet_logger`, `plugins/auto_responder`.

## Configuration reference (new keys)

```jsonc
"control": {
  "enabled": true,
  "bind": "127.0.0.1",   // anything else is ignored unless allow_remote is true
  "allow_remote": false,
  "http_port": 7301, "ws_port": 7302,
  "ui_dir": "ui", "plugins_dir": "plugins", "sessions_dir": "sessions", "templates_file": "templates.json"
},
"profiles": [ ... ], "active_profile": "localhost"
```

## Testing

Everything below runs on Linux without a Growtopia client or any server:

* **C++ (`GTProxy_tests`, 80 tests):** packet codec incl. fuzzing, hand-assembled packet fixtures, the packet pipeline with fake connections, the control API, WebSocket/REST integration on loopback (including origin/host rejection), **real ENet loopback** (fake client → proxy → fake upstream, with intercept), session record/replay, config/profiles, structured logging, and the plugin runtime (sandbox, infinite loop, memory bomb, isolation, circuit breaker, example plugins). Plugin/flow/codec tests also ran clean under valgrind.
* **UI (`ng test`, 15 tests):** packet store, filters, hex search, hex dump, edit-spec conversion.

### Untested in sandbox

* **Windows builds.** The code has Winsock paths (`src/control/socket.hpp`) and uses upstream's MSVC flags, but nothing was compiled or run on Windows or macOS. CI for Windows (`cmake_ci.yml`, Conan) is upstream's and was not run for this fork.
* **Conan dependency resolution.** Conan Center was unreachable from the build sandbox, so the fork was built with system packages (`-DGTPROXY_USE_CONAN=OFF`). The Conan path is unchanged except for the compile fixes below, but it was not exercised. Versions used here: fmt 9.1 and spdlog 1.12 (Ubuntu) vs. fmt 12 / spdlog 1.16 pinned in `conanfile.py`; eventpp 0.1.3, glaze 6.2.0, sol2 3.5.0, magic_enum 0.9.7 match.
* **A real Growtopia-compatible client and a real private server.** No packet captured from a real session was used; all fixtures are synthetic, built from the wire layout read out of upstream's decoder (see ARCHITECTURE.md → Assumptions). The `direct` profile's minimal `server_data.php` reply (`server|…\nport|…\ntype|1\ntype2|1`) in particular has not been checked against a real client.
* **The HTTPS `server_data.php` path against a non-direct profile.** Only the direct path ran end to end (via `GTProxy_fake_peers`).
* The browser UI was exercised with headless Chromium against the real proxy binary, not on other browsers.

### Upstream fixes needed to build (and found while adding the world inspector)

Upstream `HEAD` (`f51c5a3`) did not compile against eventpp 0.1.3: the dispatcher policy used `get_event`/`can_continue_invoking` (eventpp 0.1.x looks for `getEvent`/`canContinueInvoking`), and three call sites used removed `appendListener`/`removeListener`. Also fixed: IPv4 octets equal to `0` (e.g. `10.0.0.5`) were treated as hostnames and sent to DNS-over-HTTPS; a `fmt::format!` typo in `main.cpp`; a null dereference when parsing a map before `items.dat` was loaded; the unsigned `ItemChangeObject` change type (drops were never added); dropped-item coordinates read as ints (details and caveats in docs/WORLDS.md).

## Roadmap

[ROADMAP.md](ROADMAP.md) lists ideas that are intentionally not implemented.

## Credits and license

Based on [ZTzTopia/GTProxy](https://github.com/ZTzTopia/GTProxy). MIT License — see [LICENSE](LICENSE).
