# Writing plugins

Plugins are small Lua scripts that run **inside the proxy**, so you can react
to, change, drop or add packets without recompiling anything. They are meant
for protocol experiments against a server you run.

## Layout

```
plugins/
  my_plugin/
    main.lua        required: loaded when the plugin is enabled
    plugin.json     optional: {"name": "...", "version": "1.0.0", "description": "..."}
  state.json        written by the proxy: which plugins are enabled
```

* Plugins are discovered when the proxy starts and when you press **Rescan folder** in the UI's *Plugins* tab.
* **New plugins start disabled.** Nothing runs until you tick the checkbox.
* **Reload** re-reads `main.lua` (state inside the plugin is reset).
* Hooks run in alphabetical plugin order. Each plugin sees the bytes the previous one produced. If a plugin drops a packet, later plugins never see it.

## Hooks

Define any of these as global functions in `main.lua`:

| Hook | When | Argument |
|---|---|---|
| `onPacketIn(pkt)` | a packet is heading **to the game client** (it came from the server) | `Packet` |
| `onPacketOut(pkt)` | a packet is heading **to the server** (it came from the game client) | `Packet` |
| `onConnect(info)` | a peer connected; `info.peer` is `"game_client"` or `"upstream"` | table |
| `onDisconnect(info)` | a peer disconnected | table |
| `onLoad()` / `onUnload()` | once after load / before unload | none |

Packet hooks run for packets the proxy is about to forward. Packets the proxy
itself consumes (for example the built-in `/proxy` commands) never reach them,
and packets injected by the UI or by `gt.inject` do **not** re-enter the hooks
(so a responder cannot trigger itself).

To drop a packet either call `pkt:drop()` or `return false` from the hook.

## The `Packet` object

Only valid **inside the hook that received it**; using a stashed reference
later raises an error.

| Member | Meaning |
|---|---|
| `pkt.id` | sequence number shown in the UI |
| `pkt.dir` | `"server_bound"` or `"client_bound"` (`gt.SERVER_BOUND` / `gt.CLIENT_BOUND`) |
| `pkt.kind` | `"text"`, `"variant"`, `"game"`, `"hello"` or `"raw"` |
| `pkt.name` | short label: `"action\|input"`, `"OnSpawn"`, `"PACKET_STATE"` … |
| `pkt.length` | size in bytes |
| `pkt.hex` | the raw bytes as lowercase hex. **Assignable** to replace the packet. |
| `pkt.text` | text packets: the text. **Assignable.** `nil` for other kinds. |
| `pkt.decoded` | read-only table with the same structure the UI shows (see below) |
| `pkt:get(key)` | text packets: first value for `key` in the `key\|value` lines (`"|key|value"` lines work too), else `nil` |
| `pkt:edit(spec)` | replace the packet from an edit spec. Returns `true`, or `false, error` |
| `pkt:drop()` | do not forward this packet |

`pkt.decoded` examples:

```lua
-- text packet
pkt.decoded.text                       --> "action|input\n|text|hello"
pkt.decoded.lines[2].key               --> "text"
-- variant (function call) packet
pkt.decoded.variants[1].value          --> "OnSpawn"     (Lua arrays are 1-based)
pkt.decoded.variants[2].type           --> "signed"
-- game (tank) packet
pkt.decoded.header.pos_x, pkt.decoded.header.net_id, pkt.decoded.extra_hex
```

### Edit specs

`pkt:edit(spec)` and `gt.inject` use the same shapes as the UI's editor:

```lua
{ kind = "text",    msg_type = 2, text = "action|input\n|text|hi" }
{ kind = "variant", header = { net_id = 4294967295 },
  values = { { type = "string", value = "OnConsoleMessage" },
             { type = "string", value = "hello" } } }
{ kind = "game",    header = { type = 0, net_id = 7, pos_x = 10.5, pos_y = 20 }, extra_hex = "" }
{ kind = "raw",     hex = "0300000041" }
```

Variant value types: `string`, `float`, `signed`, `unsigned`, `vec2` (`{x, y}`), `vec3`.
The proxy computes `data_size` and sets the "extended" flag when there is extra data.
Text packets get a trailing NUL appended.

## The `gt` API

| Function | Notes |
|---|---|
| `gt.info(...)`, `gt.warn`, `gt.error`, `gt.debug` | write to the plugin's log (UI + proxy log). `print` is an alias for `gt.info`. |
| `gt.inject(dir, spec_or_hex)` | send a new packet to the client or server. Returns the packet id, or `nil, error`. Only inside hooks, at most 32 per hook call. Injected packets are sent *before* the packet being handled. |
| `gt.json_encode(table)` / `gt.json_decode(str)` | JSON helpers. `json_decode` returns `nil, err` on bad input. |
| `gt.now_ms()` | wall-clock milliseconds |
| `gt.plugin` | the plugin's folder name |
| `gt.CLIENT_BOUND`, `gt.SERVER_BOUND` | direction constants |

## Sandbox and limits

Each plugin has its **own Lua state**, so plugins cannot see each other's globals.

* Available: `base` (minus the items below), `table`, `string` (minus `string.dump`), `math`, `utf8`, `coroutine`.
* **Not available:** `io`, `os`, `package`, `debug`, `require`, `dofile`, `loadfile`, `load`, `collectgarbage`, `getmetatable`, `rawget`, `rawset`. A plugin cannot read files, run programs, open sockets or load other code.
* **Memory:** 32 MiB per plugin; allocations beyond that fail with a Lua error.
* **Time:** each hook call may use at most 2,000,000 VM instructions **and** 50 ms of wall-clock time; loading `main.lua` may use 500 ms. A longer call is aborted with an error.
* **Failures never block traffic.** If a hook errors, loops forever or runs out of memory, the packet is forwarded unchanged, the error is logged, and the plugin keeps running. After **10 consecutive errors** the plugin is switched off automatically (and stays off after a restart until you enable it again).

Limits are compile-time defaults (`plugin::Limits` in `src/plugin/plugin.hpp`).

What the sandbox does **not** promise: it is not a security boundary against a
determined attacker (Lua's C stack and some library functions can still be
slow), so only run plugins you trust. Its job is to keep honest mistakes from
crashing or freezing the proxy.

## Walkthrough: the examples

* `plugins/packet_logger` – read-only; logs one line per packet and totals on disconnect.
* `plugins/auto_responder` – drops `!ping` / `!help` / `!echo <text>` typed in chat and injects an `OnConsoleMessage` back to the client.

```lua
function onPacketOut(pkt)
  if pkt.kind ~= "text" or pkt:get("action") ~= "input" then return end
  if pkt:get("text") == "!ping" then
    gt.inject(gt.CLIENT_BOUND, {
      kind = "variant", header = { net_id = 4294967295 },
      values = { { type = "string", value = "OnConsoleMessage" },
                 { type = "string", value = "pong" } } })
    pkt:drop()
  end
end
```

## Testing a plugin without a client

* Run `GTProxy_fake_peers` (see README) to get a stream of scripted traffic.
* Record a session in the UI, then press **Replay offline**: recorded packets are re-run through your current plugins and the result reports how many were changed or dropped. Nothing is sent to any peer.
* The repository's own tests exercise plugins with `tests/test_plugins.cpp`; copy a test there to pin down your plugin's behaviour with synthetic packets.

## Relationship to `scripts/`

Upstream GTProxy's `scripts/*.lua` engine (shared state, event bridge,
`/commands`, world/item bindings) is unchanged and still loads at start-up. The
plugin system above is separate on purpose: isolated, switchable, limited and
with per-plugin logs. Use `scripts/` for the upstream APIs, `plugins/` for
packet-level work.
