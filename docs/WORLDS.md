# World inspector

The *Worlds* tab shows what the proxy has seen of each world you visit: **vending
machines** (what they sell, at what raw price, where), **display blocks**, and
**dropped items** with quantities (per stack and totalled per item). It keeps
every world visited during this proxy run (newest 100, in memory only) and can
search them all for an item by name or id.

It is read-only. It builds on the world model upstream GTProxy already had
(`src/world/`, filled from the `SendMapData` packet when you join a world, and
updated by `ItemChangeObject` packets); the new part is `src/world/world_inspector.*`
plus the control-plane operations `worlds.list`, `worlds.get`, `worlds.search`,
`worlds.clear` and a `world` event.

## Item names
Names come from `items.dat`, which the proxy parses when the server sends it (and caches
in `resources/items.dat`). Until then, items show as `item #<id>`.

## Know the limits (not verified against a real server)
* **Vending machines come from the map snapshot taken when you join.** Upstream never applied
  `SendTileUpdateData` (there is a TODO in `WorldHandler`), so a machine restocked or
  re-priced while you are standing in the world is not updated until you re-join.
* **Vending `price` is the raw wire value.** Its unit (and whether negative means something) is not interpreted.
  Stock counts are not shown: the parsed vending extra only carries item id and price.
* **Dropped-item positions:** the object list is read as `item u16, x f32, y f32, amount u8, flags u8, id u32`.
  Upstream read the two coordinates as 32-bit *ints*, which would give garbage if they are floats; this fork
  changed it to floats. That is from memory of the format, not from a capture. Tile coordinates are `pos / 32`.
* **Live drop updates:** the existing `ItemChangeObject` handler had a bug (the change type was stored unsigned,
  so `-1` "new drop" and `-3` "update" never matched and every event fell into the "pickup" branch). The type is now
  signed so the intended branches run. The packet also reads *amount* and *item id* from the same header field
  (`int_data` and `item_id` share storage), so **amounts on live-dropped items are likely wrong**; amounts from the
  initial map are the reliable ones. I could not confirm the real field layout without captures.
* Other tile extras beyond vending/display blocks are parsed by upstream but not shown.

## API
* `worlds.list` – summaries, newest first (`current` marks the world you are in)
* `worlds.get {name}` – full detail: `vending[]`, `displays[]`, `drops[]`, `drop_totals[]`
* `worlds.search {query}` – item name substring or numeric id; hits from every remembered world (max 500)
* `worlds.clear`
* event `{"t":"world","name":...,"revision":n,"vending":n,"drops":n}` whenever a world's data changes
