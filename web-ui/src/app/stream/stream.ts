import { Component, ElementRef, Injector, afterNextRender, computed, effect, inject, signal, viewChild } from '@angular/core';
import { ApiService } from '../core/api.service';
import { EMPTY_FILTER, PacketFilter, matchesFilter, normalizeHex } from '../core/filter';
import { formatTime, hexDump } from '../core/hexdump';
import { Dir, PacketRow } from '../core/models';
import { specFromRow } from '../core/spec';
import { Workbench } from '../core/workbench.service';

const KINDS = ['text', 'variant', 'game', 'hello', 'raw'];

@Component({
  selector: 'app-stream',
  templateUrl: './stream.html',
  styleUrl: './stream.css',
})
export class StreamComponent {
  protected readonly api = inject(ApiService);
  private readonly bench = inject(Workbench);
  private readonly injector = inject(Injector);
  protected readonly kinds = KINDS;
  protected readonly dir = signal<PacketFilter['dir']>('all');
  protected readonly activeKinds = signal<ReadonlySet<string>>(new Set());
  protected readonly text = signal('');
  protected readonly hex = signal('');
  protected readonly hideProxy = signal(false);
  protected readonly limit = signal(500);
  protected readonly selectedId = signal<number | null>(null);
  protected readonly follow = signal(true);

  protected readonly filter = computed<PacketFilter>(() => ({
    ...EMPTY_FILTER,
    dir: this.dir(),
    kinds: this.activeKinds(),
    text: this.text(),
    hex: this.hex(),
    hideProxy: this.hideProxy(),
  }));
  protected readonly hexInvalid = computed(() => this.hex() !== '' && normalizeHex(this.hex()) === null);

  protected readonly filtered = computed(() => {
    const f = this.filter();
    return this.api.rows().filter((r) => matchesFilter(r, f));
  });
  protected readonly visible = computed(() => this.filtered().slice(-this.limit()));
  protected readonly selected = computed<PacketRow | undefined>(() => {
    const id = this.selectedId();
    return id === null ? undefined : (this.api.rows().find((r) => r.id === id) ?? undefined);
  });
  protected readonly selectedDump = computed(() => hexDump(this.selected()?.hex ?? ''));
  protected readonly selectedJson = computed(() => JSON.stringify(this.selected()?.decoded ?? {}, null, 2));

  protected time = formatTime;
  private readonly listEl = viewChild<ElementRef<HTMLElement>>('list');

  constructor() {
    // "follow": keep the newest packet in view as rows arrive.
    effect(() => {
      this.visible();
      if (!this.follow()) return;
      afterNextRender(
        () => {
          const el = this.listEl()?.nativeElement;
          if (el) el.scrollTop = el.scrollHeight;
        },
        { injector: this.injector },
      );
    });
  }

  protected toggleKind(kind: string): void {
    const next = new Set(this.activeKinds());
    if (!next.delete(kind)) next.add(kind);
    this.activeKinds.set(next);
  }

  protected arrow(dir: Dir): string {
    return dir === 'server_bound' ? '→ server' : '← client';
  }

  protected resend(row: PacketRow): void {
    this.bench.sendToInjector(specFromRow(row), row.dir);
  }

  protected async copyHex(row: PacketRow): Promise<void> {
    try {
      await navigator.clipboard.writeText(row.hex);
    } catch {
      /* clipboard unavailable */
    }
  }
}
