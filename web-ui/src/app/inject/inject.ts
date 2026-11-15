import { Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { ApiService } from '../core/api.service';
import { Dir, PacketTemplate } from '../core/models';
import { PacketSpec, emptySpec } from '../core/spec';
import { Workbench } from '../core/workbench.service';
import { PacketEditorComponent } from '../editor/packet-editor';

@Component({
  selector: 'app-inject',
  imports: [PacketEditorComponent],
  templateUrl: './inject.html',
  styleUrl: './inject.css',
})
export class InjectComponent {
  private readonly api = inject(ApiService);
  private readonly bench = inject(Workbench);

  protected readonly initial = signal<PacketSpec>(emptySpec('text'));
  protected readonly spec = signal<PacketSpec>(emptySpec('text'));
  protected readonly dir = signal<Dir>('server_bound');
  protected readonly templates = signal<PacketTemplate[]>([]);
  protected readonly templateName = signal('');
  protected readonly templateDesc = signal('');
  protected readonly message = signal<{ ok: boolean; text: string } | null>(null);
  protected readonly canSave = computed(() => this.templateName().trim().length > 0);

  constructor() {
    effect(() => {
      const draft = this.bench.draft();
      if (!draft) return;
      untracked(() => {
        this.initial.set(draft.spec);
        this.spec.set(draft.spec);
        this.dir.set(draft.dir);
        this.bench.draft.set(null);
      });
    });
    void this.refresh();
  }

  private async refresh(): Promise<void> {
    try {
      this.templates.set((await this.api.call<{ templates: PacketTemplate[] }>('templates.list')).templates);
    } catch {
      /* not connected yet */
    }
  }

  protected async send(): Promise<void> {
    try {
      const r = await this.api.call<{ id: number; len: number }>('inject', { dir: this.dir(), packet: this.spec() });
      this.message.set({ ok: true, text: `Sent ${r.len} bytes (packet #${r.id}) ${this.dir() === 'server_bound' ? 'to the server' : 'to the client'}.` });
    } catch (e) {
      this.message.set({ ok: false, text: (e as Error).message });
    }
  }

  protected load(t: PacketTemplate): void {
    this.initial.set(t.packet);
    this.spec.set(t.packet);
    this.dir.set(t.dir);
    this.templateName.set(t.name);
    this.templateDesc.set(t.description);
  }

  protected async save(): Promise<void> {
    try {
      const r = await this.api.call<{ templates: PacketTemplate[] }>('templates.save', {
        name: this.templateName().trim(),
        description: this.templateDesc(),
        dir: this.dir(),
        packet: this.spec(),
      });
      this.templates.set(r.templates);
      this.message.set({ ok: true, text: `Saved template "${this.templateName().trim()}".` });
    } catch (e) {
      this.message.set({ ok: false, text: (e as Error).message });
    }
  }

  protected async remove(t: PacketTemplate): Promise<void> {
    try {
      this.templates.set((await this.api.call<{ templates: PacketTemplate[] }>('templates.delete', { name: t.name })).templates);
    } catch (e) {
      this.message.set({ ok: false, text: (e as Error).message });
    }
  }
}
