-- Auto responder (example plugin)
--
-- Shows the read/modify/drop/inject side of the API. When the game client
-- sends a chat line that is one of the commands below, the plugin:
--   1. drops the packet so the server never sees it, and
--   2. injects a console message back to the game client.
-- Everything else passes through untouched.

local replies = {
  ["!ping"] = "pong (answered by the proxy)",
  ["!help"] = "proxy commands: !ping, !help, !echo <text>",
}

local function reply_to_client(text)
  gt.inject(gt.CLIENT_BOUND, {
    kind = "variant",
    header = { net_id = 4294967295 },
    values = {
      { type = "string", value = "OnConsoleMessage" },
      { type = "string", value = text },
    },
  })
end

function onPacketOut(pkt)
  -- chat input looks like:  action|input \n |text|<what the player typed>
  if pkt.kind ~= "text" or pkt:get("action") ~= "input" then return end

  local typed = pkt:get("text")
  if not typed then return end

  local answer = replies[typed]
  if not answer then
    local rest = typed:match("^!echo%s+(.+)$")
    if rest then answer = rest end
  end
  if not answer then return end   -- not for us: forward unchanged

  gt.info("answering " .. typed)
  reply_to_client(answer)
  pkt:drop()
end
