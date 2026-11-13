# Roadmap

Ideas that are **not** implemented. The fork is deliberately limited to a local
protocol-debugging tool for servers you run; see the scope list in the README.

## Likely next

* World inspector: apply `SendTileUpdateData` so vending machines stay current, show stock, persist visited worlds to disk, expose worlds to plugins.

* Packet-level search across a recorded session file from the UI.
* Export the stream as HAR-like JSON / pcap-style hex dump files.
* Per-plugin persistent key/value storage (`gt.store`).
* Plugin timers (`gt.after(ms, fn)`) running on the proxy loop.
* Structured editors for specific well-known variants (e.g. `OnSpawn` fields) instead of the generic list.
* Virtual scrolling in the packet table (it currently renders the newest 500 matches).
* Replay "live" against a server you run, with pacing and a diff of the replies.
* Packet diff view between an original and an edited-in-flight packet.
* UI log viewer for the structured `proxy.jsonl`.
* Preserve per-packet ENet channel and flags when forwarding (today: channel 0, reliable, as upstream).
* Pause (freeze) on the proxy side, not just in the UI.
* macOS CI job.

## Needs verification before building

* Item database (`items.dat`) decoding in the UI (upstream parses it; the UI does not show it).
* Tile/world packet decoders beyond the generic game-packet header + extra hex.
