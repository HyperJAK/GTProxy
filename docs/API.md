# Control-plane API

The proxy listens on **127.0.0.1 only** (configurable under `control` in
`config.json`; non-loopback binds are refused unless `allow_remote` is set).
Requests with a browser `Origin` other than `localhost` / `127.0.0.1` / `[::1]`,
or a `Host` header that is not loopback, are rejected with 403 (this stops
other websites and DNS-rebinding pages from driving the proxy).

| Port (default) | Protocol | Purpose |
|---|---|---|
| 7301 | HTTP | REST API under `/api`, and the built UI at `/` if `ui_dir` exists |
| 7302 | WebSocket | live event stream + the same operations as RPC |

The existing HTTPS server on `:443` is the *game client's* `server_data.php`
endpoint and is unchanged; the control plane deliberately does not share it.

## WebSocket

Server -> client messages are JSON objects with a `t` field:

| `t` | Meaning |
|---|---|
| `packet` | a packet was seen. Fields: `id, ts (µs since epoch), dir, origin, state, kind, name, summary, malformed, len, hex, decoded` |
| `verdict` | what happened to packet `id`: `state` is `forwarded`, `dropped`, `held`, `edited` (with new `hex`/`decoded`), `consumed` (handled inside the proxy), `failed`. `by` says who decided. |
| `connection` | `peer` (`game_client` / `upstream`), `state` |
| `status` | once a second: peers, latency, counters, intercept, recording, plugins |
| `intercept` | the held-packet queue changed |
| `plugin_log`, `plugin_state` | plugin activity |
| `reply` | answer to a request (below) |

`dir` is `server_bound` (heading to the server) or `client_bound` (heading to the game client).
`origin` is `wire` (received from a peer), `proxy` (written by built-in code), `ui`, or `plugin:<name>`.

Requests: `{"id": 1, "op": "inject", "params": {...}}` ->
`{"t":"reply","id":1,"ok":true,"result":...}` or `{"ok":false,"error":"..."}`.
Operations run on the proxy's main thread; a request that is not answered within 5 s fails with a 504-style error.

## REST

* `GET /api/status`, `/api/packets?limit=N` (recent history), `/api/profiles`, `/api/plugins`, `/api/templates`, `/api/sessions`, `/api/intercept`, `/api/ops`
* `POST /api/call/<op>` with a JSON object body = the `params` of the WebSocket form

All responses are `{"ok":true,"result":...}` or `{"ok":false,"error":"..."}` with a matching HTTP status.

## Operations

| op | params | result / notes |
|---|---|---|
| `status` | | same object as the `status` event |
| `packets.backlog` | `limit?` | recent `packet`/`verdict`/`connection` events (up to 5000 kept) |
| `packets.clear` | | |
| `packet.decode` | `hex` | `{kind,name,summary,malformed,decoded}` |
| `packet.encode` | `packet` or `hex` | `{hex,len,name,decoded}`; validation without sending |
| `inject` | `dir`, and `packet` (edit spec) / `hex` / `template` | `{id,len}`; 409 if the target peer is not connected |
| `intercept.get` | | `{enabled,rules,held:[{id,dir,name,len,hex,...}]}` |
| `intercept.set` | `enabled?`, `rules?` `[{direction,match,kind}]`, `flush?` (`"forward"`/`"drop"` when turning off) | rules empty = hold everything |
| `intercept.forward` | `id`, `packet?`/`hex?` (replacement) | |
| `intercept.drop` | `id` | |
| `intercept.release_all` | `drop?` | |
| `templates.list` / `templates.save` / `templates.delete` | `name`, `dir`, `description`, `packet` | persisted to `templates.json` |
| `profiles.list` / `profiles.save` / `profiles.set_active` / `profiles.delete` | profile fields / `name` | persisted to `config.json` |
| `plugins.list` / `enable` / `disable` / `reload` / `rescan` / `logs` | `name`, `limit?` | |
| `sessions.list` / `start` / `stop` / `replay` | `name` | replay is offline (see README) |

### Edit specs

The shape produced by `packet.decode` is also what `packet.encode`, `inject` and
`intercept.forward` accept; see [PLUGINS.md](PLUGINS.md#edit-specs).

### Intercept semantics

With intercept on, a packet matching a rule is **held** instead of forwarded. To
preserve ordering, later packets in the **same direction** queue behind it
(shown as `queued (order)`) and follow automatically as soon as nothing that
matched a rule is in front of them. Packets in the other direction are
unaffected. Turning intercept off forwards (or drops, with `flush:"drop"`)
everything held. At most 1000 packets are held; beyond that packets are forwarded.
Held packets have already been seen by the proxy's internal handlers; edits
affect only what is forwarded.

### Session files

`sessions/<name>.jsonl`: first line `{"t":"session","version":1,...}`, then every
`packet`, `verdict` and `connection` event exactly as streamed. Corrupt lines
are skipped on load.
