import { PacketRow } from './models';

/** Edit spec understood by the proxy's `packet.encode` / `inject` / `intercept.forward`. */
export type PacketSpec =
  | { kind: 'raw'; hex: string }
  | { kind: 'text'; msg_type: number; text: string }
  | { kind: 'variant'; header: Record<string, number>; values: VariantValue[] }
  | { kind: 'game'; header: Record<string, number>; extra_hex: string };

export interface VariantValue {
  type: 'string' | 'float' | 'vec2' | 'vec3' | 'unsigned' | 'signed';
  value: string | number | number[];
}

export const HEADER_FIELDS = [
  'type', 'net_id', 'item_net_id', 'flags', 'float_var', 'int_data',
  'pos_x', 'pos_y', 'pos_x2', 'pos_y2', 'int_x', 'int_y',
] as const;

/**
 * Build an editable spec from a decoded packet. Anything we cannot edit
 * structurally (unknown or malformed packets) falls back to raw hex, so
 * the bytes are always editable.
 */
export function specFromRow(row: Pick<PacketRow, 'kind' | 'malformed' | 'hex' | 'decoded'>): PacketSpec {
  const d = row.decoded;
  if (!row.malformed && d) {
    if (row.kind === 'text' && typeof d.text === 'string') {
      return { kind: 'text', msg_type: d.msg_type ?? 3, text: d.text };
    }
    if (row.kind === 'variant' && d.header && Array.isArray(d.variants)) {
      return { kind: 'variant', header: pickHeader(d.header), values: d.variants.map(toVariantValue) };
    }
    if (row.kind === 'game' && d.header) {
      return { kind: 'game', header: pickHeader(d.header), extra_hex: d.extra_hex ?? '' };
    }
  }
  return { kind: 'raw', hex: row.hex };
}

function pickHeader(h: Record<string, any>): Record<string, number> {
  const out: Record<string, number> = {};
  for (const k of HEADER_FIELDS) out[k] = Number(h[k] ?? 0);
  return out;
}

function toVariantValue(v: any): VariantValue {
  return { type: v.type, value: v.value };
}

export function emptySpec(kind: PacketSpec['kind']): PacketSpec {
  switch (kind) {
    case 'raw':
      return { kind, hex: '' };
    case 'text':
      return { kind, msg_type: 3, text: 'action|log\n|msg|hello' };
    case 'variant':
      return {
        kind,
        header: pickHeader({ net_id: 0xffffffff }),
        values: [{ type: 'string', value: 'OnConsoleMessage' }, { type: 'string', value: 'hello' }],
      };
    case 'game':
      return { kind, header: pickHeader({}), extra_hex: '' };
  }
}

/** Display form of a variant value for a single-line input. */
export function variantToInput(v: VariantValue): string {
  return Array.isArray(v.value) ? v.value.join(', ') : String(v.value);
}

/** Parse the single-line input back to what the encoder expects. */
export function variantFromInput(type: VariantValue['type'], input: string): VariantValue['value'] {
  if (type === 'vec2' || type === 'vec3') {
    return input.split(',').map((s) => Number(s.trim()));
  }
  return input;
}
