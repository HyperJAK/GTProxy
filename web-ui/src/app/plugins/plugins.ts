import { Component, OnDestroy, effect, inject, signal, untracked } from '@angular/core';
import { Subscription } from 'rxjs';
import { ApiService } from '../core/api.service';
import { PluginInfo } from '../core/models';
import { formatTime } from '../core/hexdump';

interface LogLine {
  ts: number;
  level: string;
  message: string;
}

@Component({
  selector: 'app-plugins',
  templateUrl: './plugins.html',
  styleUrl: './plugins.css',
})
export class PluginsComponent implements OnDestroy {
  private readonly api = inject(ApiService);
  protected readonly plugins = signal<PluginInfo[]>([]);
  protected readonly selected = signal<string | null>(null);
  protected readonly logs = signal<LogLine[]>([]);
  protected readonly error = signal('');
  protected readonly time = formatTime;
  private readonly sub: Subscription;
  private readonly poll = setInterval(() => {
    if (this.api.connected()) void this.api.call<PluginInfo[]>('plugins.list').then((p) => this.plugins.set(p)).catch(() => undefined);
  }, 3000);

  constructor() {
    this.sub = this.api.events.subscribe((ev) => {
      if (ev.t === 'plugin_state') void this.refresh();
      if (ev.t === 'plugin_log' && ev.plugin === this.selected()) {
        this.logs.update((l) => [...l, ev as LogLine].slice(-500));
      }
    });
    // (re)load when the connection comes up
    effect(() => {
      if (this.api.connected()) untracked(() => void this.refresh());
    });
  }

  ngOnDestroy(): void {
    this.sub.unsubscribe();
    clearInterval(this.poll);
  }

  protected async refresh(): Promise<void> {
    try {
      this.plugins.set(await this.api.call<PluginInfo[]>('plugins.list'));
      if (this.selected() === null && this.plugins().length > 0) await this.select(this.plugins()[0].name);
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  protected async select(name: string): Promise<void> {
    this.selected.set(name);
    try {
      this.logs.set(await this.api.call<LogLine[]>('plugins.logs', { name, limit: 500 }));
    } catch {
      this.logs.set([]);
    }
  }

  protected async act(op: 'enable' | 'disable' | 'reload' | 'rescan', name = ''): Promise<void> {
    this.error.set('');
    try {
      this.plugins.set(await this.api.call<PluginInfo[]>(`plugins.${op}`, { name }));
      if (name) await this.select(name);
    } catch (e) {
      this.error.set((e as Error).message);
      await this.refresh();
    }
  }
}
