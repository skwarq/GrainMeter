// Geometry of the silo illustration and of the measuring points on the probe.
// Height 0 cm = probe tip (bottom end of the probe). Point 1 is assumed to be
// the lowest one when heights are unknown.

export const SILO = {
  width: 330,
  height: 420,
  cx: 112,
  roofApexY: 16,
  eaveY: 76,
  wallLeft: 20,
  wallRight: 204,
  wallBottomY: 316,
  hopperBottomY: 380,
  outletHalf: 16,
  groundY: 410,
  probeTopY: 34,
  // marker range on the probe
  tipY: 340,
  topY: 96,
};

export const POINT_COUNT = 8;

function knownHeight(p) {
  return p && typeof p.height_cm === 'number' && Number.isFinite(p.height_cm) && p.height_cm >= 0 ? p.height_cm : null;
}

/**
 * Vertical marker positions for measuring points.
 *
 * Height mode: when every point that has a sensor (rom) also has a known
 * height_cm, markers are placed to scale (0 cm = probe tip); points without a
 * sensor are then left out of the drawing. Otherwise all points are evenly
 * spaced by point number (point 1 lowest) - mixing scaled and guessed
 * positions would put sensors in a misleading order.
 * @param {Array<{point:number,rom?:string|null,height_cm:number|null}>} points
 * @param {{tipY:number, topY:number, count?:number}} geom
 * @returns {Array<{point:number, y:number, heightKnown:boolean, height:number|null}>}
 */
export function markerLayout(points, geom = SILO) {
  const count = geom.count || POINT_COUNT;
  const span = geom.tipY - geom.topY;
  const list = (points || []).filter((p) => p && p.point >= 1).sort((a, b) => a.point - b.point);
  const withSensor = list.filter((p) => p.rom);
  const heightMode = withSensor.length > 0 && withSensor.every((p) => knownHeight(p) !== null);
  if (heightMode) {
    const maxH = Math.max(...withSensor.map(knownHeight));
    const scale = maxH > 0 ? maxH * 1.08 : 1;
    return withSensor.map((p) => {
      const h = knownHeight(p);
      return { point: p.point, y: geom.tipY - (h / scale) * span, heightKnown: true, height: h };
    });
  }
  return list.map((p) => {
    const h = knownHeight(p);
    const frac = (Math.min(p.point, count) - 0.5) / count;
    return { point: p.point, y: geom.tipY - frac * span, heightKnown: h !== null, height: h };
  });
}

/**
 * Spread label positions so that labels at least `gap` apart do not overlap,
 * keeping them as close as possible to their markers and inside [minY,maxY].
 * @param {number[]} ys marker positions (any order)
 * @returns {number[]} label positions in the same order
 */
export function spreadLabels(ys, gap, minY, maxY) {
  const idx = ys.map((y, i) => ({ y, i })).sort((a, b) => a.y - b.y);
  const out = idx.map((o) => o.y);
  for (let k = 0; k < out.length; k++) {
    out[k] = Math.max(out[k], minY + k * gap);
    if (k > 0 && out[k] - out[k - 1] < gap) out[k] = out[k - 1] + gap;
  }
  // push back up if we ran past the bottom
  for (let k = out.length - 1; k >= 0; k--) {
    const limit = maxY - (out.length - 1 - k) * gap;
    if (out[k] > limit) out[k] = limit;
    if (k < out.length - 1 && out[k + 1] - out[k] < gap) out[k] = out[k + 1] - gap;
  }
  const res = new Array(ys.length);
  idx.forEach((o, k) => (res[o.i] = out[k]));
  return res;
}

/** Outline path of the silo body (roof + cylinder + hopper). */
export function siloPath(g = SILO) {
  const { cx, roofApexY, eaveY, wallLeft, wallRight, wallBottomY, hopperBottomY, outletHalf } = g;
  return [
    `M${cx} ${roofApexY}`,
    `L${wallRight + 8} ${eaveY}`,
    `L${wallRight} ${eaveY}`,
    `L${wallRight} ${wallBottomY}`,
    `L${cx + outletHalf} ${hopperBottomY}`,
    `L${cx - outletHalf} ${hopperBottomY}`,
    `L${wallLeft} ${wallBottomY}`,
    `L${wallLeft} ${eaveY}`,
    `L${wallLeft - 8} ${eaveY}`,
    'Z',
  ].join(' ');
}

/** Path of the grain inside the silo up to fillY (cylinder + hopper). */
export function grainPath(fillY, g = SILO) {
  const { cx, wallLeft, wallRight, wallBottomY, hopperBottomY, outletHalf } = g;
  const l = wallLeft + 3;
  const r = wallRight - 3;
  // slightly heaped surface (angle of repose)
  return [
    `M${l} ${fillY + 10}`,
    `Q${cx} ${fillY - 14} ${r} ${fillY + 10}`,
    `L${r} ${wallBottomY}`,
    `L${cx + outletHalf - 2} ${hopperBottomY - 2}`,
    `L${cx - outletHalf + 2} ${hopperBottomY - 2}`,
    `L${l} ${wallBottomY}`,
    'Z',
  ].join(' ');
}
