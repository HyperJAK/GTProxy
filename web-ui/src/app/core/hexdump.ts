/** Classic 16-bytes-per-line dump: offset, hex columns, printable ASCII. */
export function hexDump(hex: string, width = 16): string {
  const lines: string[] = [];
  for (let off = 0; off < hex.length / 2; off += width) {
    const bytes: number[] = [];
    for (let i = 0; i < width && (off + i) * 2 < hex.length; i++) {
      bytes.push(parseInt(hex.substr((off + i) * 2, 2), 16));
    }
    const cols = bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ');
    const ascii = bytes.map((b) => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '.')).join('');
    lines.push(`${off.toString(16).padStart(6, '0')}  ${cols.padEnd(width * 3 - 1, ' ')}  ${ascii}`);
  }
  return lines.join('\n');
}

export function formatTime(tsMicros: number): string {
  const d = new Date(tsMicros / 1000);
  const p = (n: number, w = 2) => n.toString().padStart(w, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}
