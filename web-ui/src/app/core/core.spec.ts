import { describe, it, expect } from 'vitest';
import { EMPTY_FILTER, hexContains, matchesFilter, normalizeHex } from './filter';
import { formatTime, hexDump } from './hexdump';
import { PacketRow } from './models';
import { PacketStore } from './packet-store';

const pkt = (id: number, extra: object = {}) => ({
  t: 'packet', id, ts: 1_700_000_000_000_000 + id, dir: 'server_bound', origin: 'wire', state: 'received',
  kind: 'text', name: 'action|input', summary: 'action|input |text|hello', malformed: false, len: 3, hex: '0200ff',
  decoded: { text: 'action|input\n|text|hello' }, ...extra,
});

describe('PacketStore', () => {
  it('adds packets and ignores duplicates (backlog overlapping live stream)', () => {
    const s = new PacketStore();
    expect(s.apply(pkt(1))).toBe(true);
    expect(s.apply(pkt(1))).toBe(false);
    expect(s.length).toBe(1);
  });

  it('applies verdicts immutably and records edits', () => {
    const s = new PacketStore();
    s.apply(pkt(1));
    const before = s.snapshot()[0];
    s.apply({ t: 'verdict', id: 1, state: 'edited', by: 'user', hex: 'aabb', len: 2, name: 'x', summary: 's', decoded: {} });
    s.apply({ t: 'verdict', id: 1, state: 'forwarded', by: 'user' });
    const after = s.snapshot()[0];
    expect(after).not.toBe(before);
    expect(after.state).toBe('forwarded');
    expect(after.hex).toBe('aabb');
    expect(after.originalHex).toBe('0200ff');
    expect(before.state).toBe('received');
  });

  it('ignores verdicts for unknown packets and bounds memory', () => {
    const s = new PacketStore(10);
    expect(s.apply({ t: 'verdict', id: 99, state: 'dropped' })).toBe(false);
    for (let i = 1; i <= 600; i++) s.apply(pkt(i));
    expect(s.length).toBeLessThanOrEqual(510);
    expect(s.get(1)).toBeUndefined();
    expect(s.get(600)).toBeDefined();
  });

  it('ignores unrelated events', () => {
    expect(new PacketStore().apply({ t: 'status' })).toBe(false);
  });
});

describe('filter', () => {
  const row = (over: Partial<PacketRow> = {}) => ({ ...(pkt(1) as any), ...over }) as PacketRow;

  it('matches by direction, kind and origin', () => {
    expect(matchesFilter(row(), { ...EMPTY_FILTER, dir: 'client_bound' })).toBe(false);
    expect(matchesFilter(row(), { ...EMPTY_FILTER, dir: 'server_bound' })).toBe(true);
    expect(matchesFilter(row(), { ...EMPTY_FILTER, kinds: new Set(['variant']) })).toBe(false);
    expect(matchesFilter(row({ origin: 'ui' }), { ...EMPTY_FILTER, hideProxy: true })).toBe(false);
  });

  it('full-text search covers name, summary and decoded text, case-insensitively', () => {
    expect(matchesFilter(row(), { ...EMPTY_FILTER, text: 'HELLO' })).toBe(true);
    expect(matchesFilter(row(), { ...EMPTY_FILTER, text: 'nomatch' })).toBe(false);
  });

  it('hex search is byte-aligned and tolerant of separators', () => {
    expect(normalizeHex('0x02 00:FF')).toBe('0200ff');
    expect(normalizeHex('xyz')).toBeNull();
    expect(hexContains('0200ff', '00ff')).toBe(true);
    expect(hexContains('0200ff', '20')).toBe(false); // straddles two bytes
    expect(matchesFilter(row(), { ...EMPTY_FILTER, hex: '00 ff' })).toBe(true);
    expect(matchesFilter(row(), { ...EMPTY_FILTER, hex: 'zz' })).toBe(false);
  });
});

describe('hexdump', () => {
  it('formats offsets, bytes and ASCII', () => {
    const d = hexDump('414243ff');
    expect(d.startsWith('000000  41 42 43 ff')).toBe(true);
    expect(d.endsWith('ABC.')).toBe(true);
  });
  it('wraps at 16 bytes', () => {
    expect(hexDump('00'.repeat(20)).split('\n').length).toBe(2);
  });
  it('formats timestamps', () => {
    expect(formatTime(0)).toMatch(/^\d\d:\d\d:\d\d\.\d{3}$/);
  });
});
