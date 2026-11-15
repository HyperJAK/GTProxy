import { Dir, PacketRow } from './models';

export interface PacketFilter {
  dir: 'all' | Dir;
  kinds: ReadonlySet<string>; // empty = all kinds
  text: string; // matches name, summary, state, decoded text (case-insensitive)
  hex: string; // hex byte-sequence search, separators ignored
  hideProxy: boolean; // hide packets the proxy itself generated
}

export const EMPTY_FILTER: PacketFilter = {
  dir: 'all',
  kinds: new Set(),
  text: '',
  hex: '',
  hideProxy: false,
};

/** "DE ad:be" -> "deadbe"; returns null if the input is not hex. */
export function normalizeHex(input: string): string | null {
  const cleaned = input.replace(/0x/gi, '').replace(/[\s:,\-]/g, '').toLowerCase();
  if (cleaned === '') return '';
  return /^[0-9a-f]+$/.test(cleaned) ? cleaned : null;
}

/**
 * Hex search must align to byte boundaries: "ab" must not match inside
 * "1ab2" when that straddles two bytes.
 */
export function hexContains(haystackHex: string, needleHex: string): boolean {
  let from = 0;
  for (;;) {
    const i = haystackHex.indexOf(needleHex, from);
    if (i < 0) return false;
    if (i % 2 === 0) return true;
    from = i + 1;
  }
}

export function matchesFilter(row: PacketRow, f: PacketFilter): boolean {
  if (f.dir !== 'all' && row.dir !== f.dir) return false;
  if (f.kinds.size > 0 && !f.kinds.has(row.kind)) return false;
  if (f.hideProxy && row.origin !== 'wire') return false;
  if (f.text) {
    const needle = f.text.toLowerCase();
    const hay = `${row.name} ${row.summary} ${row.state} ${row.origin} ${row.decoded?.text ?? ''}`.toLowerCase();
    if (!hay.includes(needle)) return false;
  }
  if (f.hex) {
    const needle = normalizeHex(f.hex);
    if (needle === null) return false; // invalid hex matches nothing
    if (needle !== '' && !hexContains(row.hex, needle)) return false;
  }
  return true;
}
