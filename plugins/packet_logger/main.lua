-- Packet logger (example plugin)
--
-- Shows the read-only side of the API: every hook receives a packet object,
-- and gt.info() writes to this plugin's log (visible in the UI's Plugins tab).
-- It never modifies or drops anything.

local totals = { client_bound = 0, server_bound = 0 }

local function summarise(pkt)
  totals[pkt.dir] = totals[pkt.dir] + 1
  local arrow = pkt.dir == gt.SERVER_BOUND and "->" or "<-"
  gt.info(string.format("#%d %s %-7s %s (%d bytes)", pkt.id, arrow, pkt.kind, pkt.name, pkt.length))
end

-- onPacketIn : packets heading to the game client (from the server)
-- onPacketOut: packets heading to the server (from the game client)
function onPacketIn(pkt)  summarise(pkt) end
function onPacketOut(pkt) summarise(pkt) end

function onConnect(info)
  gt.info(info.peer .. " connected")
end

function onDisconnect(info)
  gt.info(string.format("%s disconnected (so far: %d to client, %d to server)",
    info.peer, totals.client_bound, totals.server_bound))
end
