import { describe, it, expect } from 'vitest';
import { emptySpec, specFromRow, variantFromInput, variantToInput } from './spec';

describe('specFromRow', () => {
  it('maps text packets', () => {
    const s = specFromRow({ kind: 'text', malformed: false, hex: '00', decoded: { msg_type: 2, text: 'a|b' } });
    expect(s).toEqual({ kind: 'text', msg_type: 2, text: 'a|b' });
  });

  it('maps variant packets keeping only editable header fields and typed values', () => {
    const s = specFromRow({
      kind: 'variant', malformed: false, hex: '00',
      decoded: { header: { type: 1, net_id: 5, type_name: 'x', data_size: 9 }, variants: [{ index: 0, type: 'string', value: 'OnSpawn' }] },
    });
    expect(s.kind).toBe('variant');
    if (s.kind === 'variant') {
      expect(s.header['net_id']).toBe(5);
      expect('data_size' in s.header).toBe(false); // computed by the proxy
      expect(s.values).toEqual([{ type: 'string', value: 'OnSpawn' }]);
    }
  });

  it('falls back to raw hex for malformed or unknown packets', () => {
    expect(specFromRow({ kind: 'game', malformed: true, hex: 'aa', decoded: { header: {} } })).toEqual({ kind: 'raw', hex: 'aa' });
    expect(specFromRow({ kind: 'raw', malformed: false, hex: 'bb', decoded: {} })).toEqual({ kind: 'raw', hex: 'bb' });
  });

  it('provides blank specs for every kind', () => {
    for (const k of ['raw', 'text', 'variant', 'game'] as const) expect(emptySpec(k).kind).toBe(k);
  });

  it('converts vector inputs', () => {
    expect(variantFromInput('vec2', '1, 2.5')).toEqual([1, 2.5]);
    expect(variantToInput({ type: 'vec3', value: [1, 2, 3] })).toBe('1, 2, 3');
    expect(variantFromInput('signed', '7')).toBe('7');
  });
});
