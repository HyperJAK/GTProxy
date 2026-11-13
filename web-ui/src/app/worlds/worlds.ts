import { Component, OnDestroy, effect, inject, signal, untracked } from '@angular/core';
import { Subscription } from 'rxjs';
import { ApiService } from '../core/api.service';
import { formatTime } from '../core/hexdump';

interface WorldSummary {
  name: string;
  current: boolean;
  width: number;
  height: number;
  players: number;
  vending: number;
  displays: number;
  drops: number;
  last_seen: number;
}

interface Hit {
  world: string;
  kind: 'vending' | 'display' | 'drop';
  x: number;
  y: number;
  item: string;
  item_id: number;
  price?: number;
  amount?: number;
  machine?: string;
}

@Component({
  selector: 'app-worlds',
  templateUrl: './worlds.html',
  styleUrl: './worlds.css',
})
export class WorldsComponent implements OnDestroy {
  private readonly api = inject(ApiService);
  protected readonly worlds = signal<WorldSummary[]>([]);
  protected readonly selected = signal<string | null>(null);
  protected readonly detail = signal<any | null>(null);
  protected readonly query = signal('');
  protected readonly hits = signal<Hit[] | null>(null);
  protected readonly error = signal('');
  protected readonly time = formatTime;
  private readonly sub: Subscription;
  private timer?: ReturnType<typeof setTimeout>;

  constructor() {
    this.sub = this.api.events.subscribe((ev) => {
      if (ev.t !== 'world') return;
      // debounce: drops can change many times per second
      clearTimeout(this.timer);
      this.timer = setTimeout(() => void this.refresh(), 400);
    });
    effect(() => {
      if (this.api.connected()) untracked(() => void this.refresh());
    });
  }

  ngOnDestroy(): void {
    this.sub.unsubscribe();
    clearTimeout(this.timer);
  }

  protected async refresh(): Promise<void> {
    try {
      const list = await this.api.call<WorldSummary[]>('worlds.list');
      this.worlds.set(list);
      const sel = this.selected() ?? list.find((w) => w.current)?.name ?? list[0]?.name ?? null;
      if (sel) await this.select(sel);
      if (this.query().trim()) await this.search();
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  protected async select(name: string): Promise<void> {
    this.selected.set(name);
    try {
      this.detail.set(await this.api.call<any>('worlds.get', { name }));
    } catch {
      this.detail.set(null);
    }
  }

  protected async search(): Promise<void> {
    const q = this.query().trim();
    if (!q) {
      this.hits.set(null);
      return;
    }
    try {
      this.hits.set(await this.api.call<Hit[]>('worlds.search', { query: q }));
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  protected async open(world: string): Promise<void> {
    this.hits.set(null);
    this.query.set('');
    await this.select(world);
  }

  protected async clearHistory(): Promise<void> {
    await this.api.call('worlds.clear');
    this.selected.set(null);
    this.detail.set(null);
    await this.refresh();
  }
}
