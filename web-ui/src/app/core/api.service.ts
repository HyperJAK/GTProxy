import { Injectable, computed, signal } from '@angular/core';
import { Subject } from 'rxjs';
import { ConnectionEvent, InterceptState, PacketRow, Status } from './models';
import { PacketStore } from './packet-store';

interface Pending {
  resolve: (v: any) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

/** Connection to the proxy control plane (WebSocket, auto-reconnecting). */
@Injectable({ providedIn: 'root' })
export class ApiService {
  readonly connected = signal(false);
  readonly status = signal<Status | null>(null);
  readonly statusRate = signal({ in: 0, out: 0 }); // packets per second, derived from status deltas
  readonly intercept = signal<InterceptState | null>(null);
  readonly connections = signal<ConnectionEvent[]>([]);
  readonly rows = signal<PacketRow[]>([]);
  readonly paused = signal(false);
  readonly pendingWhilePaused = signal(0);
  readonly url = signal(this.defaultUrl());
  /** Raw non-packet events (plugin logs etc.) for components that care. */
  readonly events = new Subject<any>();
  readonly totalRows = computed(() => this.rows().length);

  private readonly store = new PacketStore();
  private ws?: WebSocket;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private queue: any[] = [];
  private held: any[] = [];
  private flushTimer?: ReturnType<typeof setTimeout>;
  private retry = 500;
  private lastStatus?: { at: number; in: number; out: number };
  private closed = false;

  private defaultUrl(): string {
    try {
      const saved = localStorage.getItem('gtproxy.ws');
      if (saved) return saved;
    } catch {
      /* storage unavailable */
    }
    const host = typeof location !== 'undefined' && location.hostname ? location.hostname : '127.0.0.1';
    return `ws://${host}:7302`;
  }

  setUrl(url: string): void {
    try {
      localStorage.setItem('gtproxy.ws', url);
    } catch {
      /* ignore */
    }
    this.url.set(url);
    this.ws?.close();
  }

  connect(): void {
    this.closed = false;
    const ws = new WebSocket(this.url());
    this.ws = ws;
    ws.onopen = () => {
      this.retry = 500;
      this.connected.set(true);
      void this.bootstrap();
    };
    ws.onmessage = (e) => this.onMessage(e.data);
    ws.onclose = () => {
      this.connected.set(false);
      for (const [, p] of this.pending) {
        clearTimeout(p.timer);
        p.reject(new Error('connection closed'));
      }
      this.pending.clear();
      if (!this.closed) {
        setTimeout(() => this.connect(), this.retry);
        this.retry = Math.min(this.retry * 2, 8000);
      }
    };
    ws.onerror = () => ws.close();
  }

  disconnect(): void {
    this.closed = true;
    this.ws?.close();
  }

  /** Invoke a control-plane operation. Rejects with the server's error message. */
  call<T = any>(op: string, params: object = {}, timeoutMs = 8000): Promise<T> {
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new Error('not connected to the proxy'));
        return;
      }
      const id = this.nextId++;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${op} timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.ws.send(JSON.stringify({ id, op, params }));
    });
  }

  setPaused(paused: boolean): void {
    this.paused.set(paused);
    if (!paused) {
      this.queue.push(...this.held);
      this.held = [];
      this.pendingWhilePaused.set(0);
      this.scheduleFlush();
    }
  }

  clear(): void {
    this.store.clear();
    this.queue = [];
    this.held = [];
    this.pendingWhilePaused.set(0);
    this.rows.set([]);
    void this.call('packets.clear').catch(() => undefined);
  }

  getRow(id: number): PacketRow | undefined {
    return this.store.get(id);
  }

  private async bootstrap(): Promise<void> {
    try {
      const backlog = await this.call<any[]>('packets.backlog');
      for (const ev of backlog) this.onEvent(ev);
      this.intercept.set(await this.call<InterceptState>('intercept.get'));
      this.status.set(await this.call<Status>('status'));
    } catch {
      /* the live stream still works */
    }
  }

  private onMessage(data: string): void {
    let msg: any;
    try {
      msg = JSON.parse(data);
    } catch {
      return;
    }
    if (msg.t === 'reply') {
      const p = this.pending.get(msg.id);
      if (!p) return;
      this.pending.delete(msg.id);
      clearTimeout(p.timer);
      if (msg.ok) p.resolve(msg.result);
      else p.reject(new Error(msg.error ?? 'request failed'));
      return;
    }
    this.onEvent(msg);
  }

  private onEvent(msg: any): void {
    switch (msg.t) {
      case 'packet':
      case 'verdict':
        if (this.paused()) {
          this.held.push(msg);
          this.pendingWhilePaused.set(this.held.length);
        } else {
          this.queue.push(msg);
          this.scheduleFlush();
        }
        break;
      case 'status':
        this.onStatus(msg);
        break;
      case 'intercept':
        this.intercept.set(msg);
        break;
      case 'connection':
        this.connections.update((c) => [msg as ConnectionEvent, ...c].slice(0, 100));
        break;
      default:
        this.events.next(msg);
    }
  }

  private onStatus(s: Status): void {
    const now = Date.now();
    const inCount = s.counters.client_bound_packets;
    const outCount = s.counters.server_bound_packets;
    if (this.lastStatus && now > this.lastStatus.at) {
      const dt = (now - this.lastStatus.at) / 1000;
      this.statusRate.set({
        in: Math.max(0, (inCount - this.lastStatus.in) / dt),
        out: Math.max(0, (outCount - this.lastStatus.out) / dt),
      });
    }
    this.lastStatus = { at: now, in: inCount, out: outCount };
    this.status.set(s);
  }

  private scheduleFlush(): void {
    if (this.flushTimer) return;
    this.flushTimer = setTimeout(() => {
      this.flushTimer = undefined;
      let changed = false;
      for (const ev of this.queue) changed = this.store.apply(ev) || changed;
      this.queue = [];
      if (changed) this.rows.set(this.store.snapshot());
    }, 100);
  }
}
