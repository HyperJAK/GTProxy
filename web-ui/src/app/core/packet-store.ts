import { PacketRow } from './models';

/**
 * Applies the proxy's event stream (packet + verdict events) to an ordered,
 * bounded list of rows. Pure TypeScript so it is unit-testable without Angular.
 */
export class PacketStore {
  private byId = new Map<number, PacketRow>();
  private list: PacketRow[] = [];

  constructor(private readonly capacity = 20000) {}

  get length(): number {
    return this.list.length;
  }

  snapshot(): PacketRow[] {
    return this.list.slice();
  }

  get(id: number): PacketRow | undefined {
    return this.byId.get(id);
  }

  clear(): void {
    this.byId.clear();
    this.list = [];
  }

  /** Returns true if the event changed the store. */
  apply(msg: any): boolean {
    if (msg?.t === 'packet') {
      if (this.byId.has(msg.id)) return false; // backlog + live overlap
      const row: PacketRow = {
        id: msg.id,
        ts: msg.ts,
        dir: msg.dir,
        origin: msg.origin,
        state: msg.state,
        kind: msg.kind,
        name: msg.name,
        summary: msg.summary,
        malformed: !!msg.malformed,
        len: msg.len,
        hex: msg.hex,
        decoded: msg.decoded,
      };
      this.byId.set(row.id, row);
      this.list.push(row);
      if (this.list.length > this.capacity + 500) {
        for (const old of this.list.splice(0, this.list.length - this.capacity)) {
          this.byId.delete(old.id);
        }
      }
      return true;
    }
    if (msg?.t === 'verdict') {
      const row = this.byId.get(msg.id);
      if (!row) return false;
      // Immutable update so signal/OnPush consumers see the change. Verdicts
      // normally follow their packet closely, so scan from the end.
      let idx = -1;
      for (let i = this.list.length - 1; i >= 0; i--) {
        if (this.list[i].id === msg.id) {
          idx = i;
          break;
        }
      }
      if (idx < 0) return false;
      const next: PacketRow = { ...row, state: msg.state, by: msg.by || undefined };
      if (msg.hex !== undefined && msg.hex !== row.hex) {
        next.originalHex = row.originalHex ?? row.hex;
        next.hex = msg.hex;
        next.len = msg.len;
        next.name = msg.name ?? row.name;
        next.summary = msg.summary ?? row.summary;
        next.decoded = msg.decoded ?? row.decoded;
      }
      this.list[idx] = next;
      this.byId.set(next.id, next);
      return true;
    }
    return false;
  }
}
