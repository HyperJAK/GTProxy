import { Component, effect, inject, input, output, signal, untracked } from '@angular/core';
import { ApiService } from '../core/api.service';
import { HEADER_FIELDS, PacketSpec, VariantValue, emptySpec, variantFromInput, variantToInput } from '../core/spec';

/**
 * Structured packet editor: edit fields for text / variant / game packets or
 * the raw hex. Emits the edit spec; shows the encoded bytes (or the error)
 * live, using the proxy's own encoder so what you see is what gets sent.
 */
@Component({
  selector: 'app-packet-editor',
  templateUrl: './packet-editor.html',
  styleUrl: './packet-editor.css',
})
export class PacketEditorComponent {
  private readonly api = inject(ApiService);

  /** Replaces the editor content whenever the reference changes. */
  readonly initial = input.required<PacketSpec>();
  readonly specChange = output<PacketSpec>();
  /** Emits true once the user has changed anything since `initial` was set. */
  readonly dirty = output<boolean>();

  protected readonly spec = signal<PacketSpec>({ kind: 'raw', hex: '' });
  protected readonly encodedHex = signal('');
  protected readonly encodeError = signal('');
  protected readonly headerFields = HEADER_FIELDS;
  protected readonly kinds = ['text', 'variant', 'game', 'raw'] as const;
  protected readonly variantTypes = ['string', 'float', 'vec2', 'vec3', 'unsigned', 'signed'] as const;
  protected readonly variantToInput = variantToInput;

  private timer?: ReturnType<typeof setTimeout>;

  constructor() {
    effect(() => {
      const init = this.initial();
      untracked(() => {
        this.spec.set(structuredClone(init));
        this.dirty.emit(false);
        this.preview();
      });
    });
  }

  protected setKind(kind: PacketSpec['kind']): void {
    this.update(emptySpec(kind));
  }

  protected update(next: PacketSpec): void {
    this.spec.set(next);
    this.specChange.emit(next);
    this.dirty.emit(true);
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this.preview(), 200);
  }

  protected patch(p: object): void {
    this.update({ ...this.spec(), ...p } as PacketSpec);
  }

  protected setHeader(field: string, raw: string): void {
    const s = this.spec();
    if (s.kind !== 'variant' && s.kind !== 'game') return;
    this.update({ ...s, header: { ...s.header, [field]: Number(raw) } });
  }

  protected setValue(i: number, p: Partial<VariantValue>): void {
    const s = this.spec();
    if (s.kind !== 'variant') return;
    const values = s.values.map((v, idx) => (idx === i ? { ...v, ...p } : v));
    this.update({ ...s, values });
  }

  protected setValueInput(i: number, input: string): void {
    const s = this.spec();
    if (s.kind !== 'variant') return;
    this.setValue(i, { value: variantFromInput(s.values[i].type, input) });
  }

  protected addValue(): void {
    const s = this.spec();
    if (s.kind !== 'variant') return;
    this.update({ ...s, values: [...s.values, { type: 'string', value: '' }] });
  }

  protected removeValue(i: number): void {
    const s = this.spec();
    if (s.kind !== 'variant') return;
    this.update({ ...s, values: s.values.filter((_, idx) => idx !== i) });
  }

  private async preview(): Promise<void> {
    try {
      const r = await this.api.call<{ hex: string }>('packet.encode', { packet: this.spec() });
      this.encodedHex.set(r.hex);
      this.encodeError.set('');
    } catch (e) {
      this.encodedHex.set('');
      this.encodeError.set((e as Error).message);
    }
  }
}
