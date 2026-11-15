export type Dir = 'client_bound' | 'server_bound';

export interface PacketRow {
  id: number;
  ts: number; // microseconds since epoch
  dir: Dir;
  origin: string; // wire | proxy | ui | inject | plugin:<name>
  state: string; // received | forwarded | held | edited | dropped | consumed | injected | sent | failed
  by?: string;
  kind: string; // hello | text | variant | game | raw
  name: string;
  summary: string;
  malformed: boolean;
  len: number;
  hex: string;
  /** Original bytes if a later verdict replaced them (edit). */
  originalHex?: string;
  decoded: any;
}

export interface Peer {
  connected: boolean;
  address: string;
  rtt_ms: number;
}

export interface Status {
  version: string;
  uptime_s: number;
  profile: string;
  game_client: Peer;
  upstream: Peer;
  counters: {
    client_bound_packets: number;
    client_bound_bytes: number;
    server_bound_packets: number;
    server_bound_bytes: number;
    dropped: number;
    edited: number;
    injected: number;
    consumed: number;
    held_total: number;
    write_failures: number;
  };
  intercept: { enabled: boolean; held: number };
  recording: { active: boolean; name: string; events: number };
  plugins: { total: number; enabled: number; errors: number };
  ws_clients: number;
}

export interface ConnectionEvent {
  ts: number;
  peer: 'game_client' | 'upstream';
  state: 'connected' | 'disconnected';
}

export interface HeldPacket {
  id: number;
  dir: Dir;
  name: string;
  summary: string;
  len: number;
  since: number;
  by_rule: boolean;
  hex: string;
}

export interface InterceptRule {
  direction: 'any' | Dir;
  match: string;
  kind: string;
}

export interface InterceptState {
  enabled: boolean;
  rules: InterceptRule[];
  held: HeldPacket[];
}

export interface PacketTemplate {
  name: string;
  description: string;
  dir: Dir;
  packet: any;
}

export interface PluginInfo {
  name: string;
  version: string;
  description: string;
  path: string;
  enabled: boolean;
  loaded: boolean;
  error: string;
  errors: number;
  packets: number;
}

export interface Profile {
  name: string;
  address: string;
  port: number;
  tls: boolean;
  direct: boolean;
  enet_address: string;
  enet_port: number;
}
