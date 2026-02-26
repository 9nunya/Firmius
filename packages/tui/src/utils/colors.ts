/**
 * Interpolates between multiple hex colors.
 * @param colors Array of hex colors (e.g. ['#FF0000', '#0000FF'])
 * @param factor Value between 0 and 1
 */
export function interpolateColors(colors: string[], factor: number): string {
  if (colors.length === 0) return '#FFFFFF';
  if (colors.length === 1) return colors[0]!;

  const segments = colors.length - 1;
  const scaledFactor = factor * segments;
  const index = Math.min(Math.floor(scaledFactor), segments - 1);
  const localFactor = scaledFactor - index;

  return interpolateTwoColors(colors[index]!, colors[index + 1]!, localFactor);
}

function interpolateTwoColors(color1: string, color2: string, factor: number): string {
  const r1 = parseInt(color1.slice(1, 3), 16);
  const g1 = parseInt(color1.slice(3, 5), 16);
  const b1 = parseInt(color1.slice(5, 7), 16);

  const r2 = parseInt(color2.slice(1, 3), 16);
  const g2 = parseInt(color2.slice(3, 5), 16);
  const b2 = parseInt(color2.slice(5, 7), 16);

  const r = Math.round(r1 + factor * (r2 - r1));
  const g = Math.round(g1 + factor * (g2 - g1));
  const b = Math.round(b1 + factor * (b2 - b1));

  return `#${((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1).toUpperCase()}`;
}
