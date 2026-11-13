import { Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { ApiService } from '../core/api.service';
import { PacketEditorComponent } from '../editor/packet-editor';
import { formatTime } from '../core/hexdump';
import { HeldPacket, InterceptRule } from '../core/models';
import { PacketSpec, specFromRow } from '../core/spec';

@Component({
  selector: 'app-intercept',
  imports: [PacketEditorComponent],
  templateUrl: './intercept.html',
  styleUrl: './intercept.css',
})
export class InterceptComponent {
  protected readonly api = inject(ApiService);
  protected readonly state = this.api.intercept;
  protected readonly selectedId = signal<number | null>(null);
  protected readonly edited = signal<PacketSpec | null>(null);
  protected readonly dirty = signal(false);
  protected readonly error = signal('');
  protected readonly rules = signal<InterceptRule[] | null>(null);
  protected readonly time = formatTime;

  protected readonly held = computed(() => this.state()?.held ?? []);
  protected readonly shownRules = computed(() => this.rules() ?? this.state()?.rules ?? []);
  protected readonly selected = computed<HeldPacket | undefined>(() => this.held().find((h) => h.id === this.selectedId()) ?? this.held()[0]);
  /** Decode the held bytes with the same codec the proxy uses, so edits start from the real packet. */
  protected readonly initialSpec = signal<PacketSpec>({ kind: 'raw', hex: '' });
  protected loadedFor = -1;

  constructor() {
    // Load the editor whenever the selected held packet changes.
    effect(() => {
      const h = this.selected();
      if (h && h.id !== this.loadedFor) untracked(() => void this.loadSpec(h));
    });
  }

  protected async select(h: HeldPacket): Promise<void> {
    this.selectedId.set(h.id);
    await this.loadSpec(h);
  }

  private async loadSpec(h: HeldPacket): Promise<void> {
    this.loadedFor = h.id;
    this.dirty.set(false);
    this.edited.set(null);
    try {
      const d = await this.api.call<any>('packet.decode', { hex: h.hex });
      this.initialSpec.set(specFromRow({ kind: d.kind, malformed: d.malformed, hex: h.hex, decoded: d.decoded }));
    } catch {
      this.initialSpec.set({ kind: 'raw', hex: h.hex });
    }
  }

  protected async toggle(enabled: boolean): Promise<void> {
    await this.run(() => this.api.call('intercept.set', { enabled, rules: this.shownRules() }));
  }

  protected addRule(): void {
    this.rules.set([...this.shownRules(), { direction: 'any', match: '', kind: '' }]);
  }

  protected setRule(i: number, p: Partial<InterceptRule>): void {
    this.rules.set(this.shownRules().map((r, idx) => (idx === i ? { ...r, ...p } : r)));
  }

  protected removeRule(i: number): void {
    this.rules.set(this.shownRules().filter((_, idx) => idx !== i));
  }

  protected async applyRules(): Promise<void> {
    await this.run(() => this.api.call('intercept.set', { enabled: this.state()?.enabled ?? false, rules: this.shownRules() }));
    this.rules.set(null);
  }

  protected async forward(h: HeldPacket): Promise<void> {
    const params: any = { id: h.id };
    if (this.dirty() && this.edited() && h.id === this.loadedFor) params.packet = this.edited();
    await this.run(() => this.api.call('intercept.forward', params));
  }

  protected async drop(h: HeldPacket): Promise<void> {
    await this.run(() => this.api.call('intercept.drop', { id: h.id }));
  }

  protected async releaseAll(drop: boolean): Promise<void> {
    await this.run(() => this.api.call('intercept.release_all', { drop }));
  }

  private async run(fn: () => Promise<unknown>): Promise<void> {
    this.error.set('');
    try {
      await fn();
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }
}
