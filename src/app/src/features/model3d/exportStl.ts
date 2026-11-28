type GltfComponentType = 5120 | 5121 | 5122 | 5123 | 5125 | 5126;
type PrimitiveMode = 4 | 5 | 6;
type Vec3 = [number, number, number];
type Mat4 = [
  number, number, number, number,
  number, number, number, number,
  number, number, number, number,
  number, number, number, number,
];

interface GltfBufferView {
  buffer?: number;
  byteOffset?: number;
  byteLength: number;
  byteStride?: number;
}

interface GltfSparseAccessor {
  count: number;
  indices: { bufferView: number; byteOffset?: number; componentType: 5121 | 5123 | 5125 };
  values: { bufferView: number; byteOffset?: number };
}

interface GltfAccessor {
  bufferView?: number;
  byteOffset?: number;
  componentType: GltfComponentType;
  normalized?: boolean;
  count: number;
  type: 'SCALAR' | 'VEC2' | 'VEC3' | 'VEC4' | 'MAT2' | 'MAT3' | 'MAT4';
  sparse?: GltfSparseAccessor;
}

interface GltfPrimitive {
  attributes?: Record<string, number>;
  indices?: number;
  mode?: number;
  extensions?: Record<string, unknown>;
}

interface GltfDocument {
  accessors?: GltfAccessor[];
  bufferViews?: GltfBufferView[];
  meshes?: Array<{ primitives?: GltfPrimitive[] }>;
  nodes?: Array<{
    mesh?: number;
    children?: number[];
    matrix?: number[];
    translation?: number[];
    rotation?: number[];
    scale?: number[];
  }>;
  scenes?: Array<{ nodes?: number[] }>;
  scene?: number;
  extensionsUsed?: string[];
}

interface ParsedGlb {
  json: GltfDocument;
  binary: DataView;
}

const GLB_MAGIC = 0x46546c67;
const JSON_CHUNK = 0x4e4f534a;
const BIN_CHUNK = 0x004e4942;
const COMPONENTS: Record<GltfAccessor['type'], number> = {
  SCALAR: 1,
  VEC2: 2,
  VEC3: 3,
  VEC4: 4,
  MAT2: 4,
  MAT3: 9,
  MAT4: 16,
};
const COMPONENT_BYTES: Record<GltfComponentType, number> = {
  5120: 1,
  5121: 1,
  5122: 2,
  5123: 2,
  5125: 4,
  5126: 4,
};

function parseGlb(data: ArrayBuffer): ParsedGlb {
  const view = new DataView(data);
  if (data.byteLength < 20 || view.getUint32(0, true) !== GLB_MAGIC) {
    throw new Error('The generated file is not a valid binary glTF (GLB) file.');
  }
  if (view.getUint32(4, true) !== 2) throw new Error('Only glTF 2.0 GLB files can be exported to STL.');
  const declaredLength = view.getUint32(8, true);
  if (declaredLength > data.byteLength) throw new Error('The generated GLB file is truncated.');

  let json: GltfDocument | null = null;
  let binary = new DataView(new ArrayBuffer(0));
  let offset = 12;
  while (offset + 8 <= declaredLength) {
    const chunkLength = view.getUint32(offset, true);
    const chunkType = view.getUint32(offset + 4, true);
    const chunkStart = offset + 8;
    const chunkEnd = chunkStart + chunkLength;
    if (chunkEnd > declaredLength) throw new Error('The generated GLB contains an invalid chunk length.');
    if (chunkType === JSON_CHUNK) {
      const text = new TextDecoder().decode(new Uint8Array(data, chunkStart, chunkLength)).replace(/\u0000+$/g, '').trim();
      json = JSON.parse(text) as GltfDocument;
    } else if (chunkType === BIN_CHUNK) {
      binary = new DataView(data, chunkStart, chunkLength);
    }
    offset = chunkEnd;
  }
  if (!json) throw new Error('The generated GLB contains no glTF JSON chunk.');
  return { json, binary };
}

function identity(): Mat4 {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

function multiply(a: Mat4, b: Mat4): Mat4 {
  const out = new Array<number>(16).fill(0) as Mat4;
  for (let column = 0; column < 4; column += 1) {
    for (let row = 0; row < 4; row += 1) {
      out[column * 4 + row] =
        a[row] * b[column * 4]
        + a[4 + row] * b[column * 4 + 1]
        + a[8 + row] * b[column * 4 + 2]
        + a[12 + row] * b[column * 4 + 3];
    }
  }
  return out;
}

function nodeMatrix(node: NonNullable<GltfDocument['nodes']>[number]): Mat4 {
  if (Array.isArray(node.matrix) && node.matrix.length === 16) return [...node.matrix] as Mat4;
  const [tx, ty, tz] = node.translation || [0, 0, 0];
  const [qx, qy, qz, qw] = node.rotation || [0, 0, 0, 1];
  const [sx, sy, sz] = node.scale || [1, 1, 1];
  const xx = qx * qx; const yy = qy * qy; const zz = qz * qz;
  const xy = qx * qy; const xz = qx * qz; const yz = qy * qz;
  const wx = qw * qx; const wy = qw * qy; const wz = qw * qz;
  return [
    (1 - 2 * (yy + zz)) * sx, (2 * (xy + wz)) * sx, (2 * (xz - wy)) * sx, 0,
    (2 * (xy - wz)) * sy, (1 - 2 * (xx + zz)) * sy, (2 * (yz + wx)) * sy, 0,
    (2 * (xz + wy)) * sz, (2 * (yz - wx)) * sz, (1 - 2 * (xx + yy)) * sz, 0,
    tx, ty, tz, 1,
  ];
}

function transformPoint(matrix: Mat4, point: Vec3): Vec3 {
  const [x, y, z] = point;
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
    matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
    matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14],
  ];
}

function componentValue(view: DataView, byteOffset: number, type: GltfComponentType, normalized = false): number {
  let value: number;
  switch (type) {
    case 5120: value = view.getInt8(byteOffset); return normalized ? Math.max(value / 127, -1) : value;
    case 5121: value = view.getUint8(byteOffset); return normalized ? value / 255 : value;
    case 5122: value = view.getInt16(byteOffset, true); return normalized ? Math.max(value / 32767, -1) : value;
    case 5123: value = view.getUint16(byteOffset, true); return normalized ? value / 65535 : value;
    case 5125: value = view.getUint32(byteOffset, true); return normalized ? value / 4294967295 : value;
    case 5126: return view.getFloat32(byteOffset, true);
    default: throw new Error(`Unsupported glTF component type: ${type}`);
  }
}

function accessorData(document: GltfDocument, binary: DataView, accessorIndex: number): number[][] {
  const accessor = document.accessors?.[accessorIndex];
  if (!accessor) throw new Error(`Missing glTF accessor ${accessorIndex}.`);
  const componentCount = COMPONENTS[accessor.type];
  const componentBytes = COMPONENT_BYTES[accessor.componentType];
  const values = Array.from({ length: accessor.count }, () => new Array<number>(componentCount).fill(0));

  if (accessor.bufferView !== undefined) {
    const bufferView = document.bufferViews?.[accessor.bufferView];
    if (!bufferView || (bufferView.buffer ?? 0) !== 0) throw new Error('STL export supports only the embedded GLB buffer.');
    const stride = bufferView.byteStride || componentCount * componentBytes;
    const start = (bufferView.byteOffset || 0) + (accessor.byteOffset || 0);
    const view = binary;
    for (let item = 0; item < accessor.count; item += 1) {
      for (let component = 0; component < componentCount; component += 1) {
        values[item][component] = componentValue(
          view,
          start + item * stride + component * componentBytes,
          accessor.componentType,
          accessor.normalized,
        );
      }
    }
  }

  const sparse = accessor.sparse;
  if (sparse) {
    const indexViewInfo = document.bufferViews?.[sparse.indices.bufferView];
    const valueViewInfo = document.bufferViews?.[sparse.values.bufferView];
    if (!indexViewInfo || !valueViewInfo) throw new Error('The GLB contains an invalid sparse accessor.');
    const indexView = binary;
    const valueView = binary;
    const indexBytes = COMPONENT_BYTES[sparse.indices.componentType];
    const indexStart = (indexViewInfo.byteOffset || 0) + (sparse.indices.byteOffset || 0);
    const valueStart = (valueViewInfo.byteOffset || 0) + (sparse.values.byteOffset || 0);
    for (let sparseItem = 0; sparseItem < sparse.count; sparseItem += 1) {
      const target = componentValue(indexView, indexStart + sparseItem * indexBytes, sparse.indices.componentType);
      if (!Number.isInteger(target) || target < 0 || target >= accessor.count) throw new Error('The GLB contains an invalid sparse accessor index.');
      for (let component = 0; component < componentCount; component += 1) {
        values[target][component] = componentValue(
          valueView,
          valueStart + (sparseItem * componentCount + component) * componentBytes,
          accessor.componentType,
          accessor.normalized,
        );
      }
    }
  }
  return values;
}

function triangleIndices(indices: number[], mode: PrimitiveMode): Array<[number, number, number]> {
  const triangles: Array<[number, number, number]> = [];
  if (mode === 4) {
    for (let i = 0; i + 2 < indices.length; i += 3) triangles.push([indices[i], indices[i + 1], indices[i + 2]]);
  } else if (mode === 5) {
    for (let i = 0; i + 2 < indices.length; i += 1) {
      triangles.push(i % 2 === 0 ? [indices[i], indices[i + 1], indices[i + 2]] : [indices[i + 1], indices[i], indices[i + 2]]);
    }
  } else if (mode === 6) {
    for (let i = 1; i + 1 < indices.length; i += 1) triangles.push([indices[0], indices[i], indices[i + 1]]);
  }
  return triangles.filter(([a, b, c]) => a !== b && b !== c && a !== c);
}

function normal(a: Vec3, b: Vec3, c: Vec3): Vec3 {
  const ux = b[0] - a[0]; const uy = b[1] - a[1]; const uz = b[2] - a[2];
  const vx = c[0] - a[0]; const vy = c[1] - a[1]; const vz = c[2] - a[2];
  const nx = uy * vz - uz * vy;
  const ny = uz * vx - ux * vz;
  const nz = ux * vy - uy * vx;
  const length = Math.hypot(nx, ny, nz);
  return length > 1e-12 ? [nx / length, ny / length, nz / length] : [0, 0, 0];
}

function collectTriangles(document: GltfDocument, binary: DataView): Array<[Vec3, Vec3, Vec3]> {
  const nodes = document.nodes || [];
  const meshes = document.meshes || [];
  const triangles: Array<[Vec3, Vec3, Vec3]> = [];
  const accessorCache = new Map<number, number[][]>();
  const readAccessor = (index: number) => {
    const cached = accessorCache.get(index);
    if (cached) return cached;
    const value = accessorData(document, binary, index);
    accessorCache.set(index, value);
    return value;
  };

  const visit = (nodeIndex: number, parent: Mat4, path: Set<number>) => {
    if (path.has(nodeIndex)) throw new Error('The generated GLB contains a cyclic node hierarchy.');
    const node = nodes[nodeIndex];
    if (!node) return;
    const world = multiply(parent, nodeMatrix(node));
    const nextPath = new Set(path).add(nodeIndex);
    if (node.mesh !== undefined) {
      const mesh = meshes[node.mesh];
      for (const primitive of mesh?.primitives || []) {
        if (primitive.extensions?.KHR_draco_mesh_compression || primitive.extensions?.EXT_meshopt_compression) {
          throw new Error('STL export does not support compressed GLB geometry. Download the GLB to retain it.');
        }
        const positionAccessor = primitive.attributes?.POSITION;
        if (positionAccessor === undefined) continue;
        const rawPositions = readAccessor(positionAccessor);
        const positions = rawPositions.map(value => transformPoint(world, [value[0], value[1], value[2]]));
        const indices = primitive.indices === undefined
          ? positions.map((_, index) => index)
          : readAccessor(primitive.indices).map(value => value[0]);
        const mode = (primitive.mode ?? 4) as PrimitiveMode;
        if (![4, 5, 6].includes(mode)) continue;
        for (const [ia, ib, ic] of triangleIndices(indices, mode)) {
          const a = positions[ia]; const b = positions[ib]; const c = positions[ic];
          if (a && b && c) triangles.push([a, b, c]);
        }
      }
    }
    for (const child of node.children || []) visit(child, world, nextPath);
  };

  const scene = document.scenes?.[document.scene ?? 0];
  let roots = scene?.nodes || [];
  if (roots.length === 0 && nodes.length > 0) {
    const childNodes = new Set(nodes.flatMap(node => node.children || []));
    roots = nodes.map((_, index) => index).filter(index => !childNodes.has(index));
  }
  for (const root of roots) visit(root, identity(), new Set<number>());
  return triangles;
}

function binaryStl(triangles: Array<[Vec3, Vec3, Vec3]>): ArrayBuffer {
  if (triangles.length === 0) throw new Error('The generated GLB contains no triangle mesh to export.');
  const output = new ArrayBuffer(84 + triangles.length * 50);
  const bytes = new Uint8Array(output);
  const header = new TextEncoder().encode('Lemonade App STL export');
  bytes.set(header.subarray(0, 80), 0);
  const view = new DataView(output);
  view.setUint32(80, triangles.length, true);
  let offset = 84;
  for (const [a, b, c] of triangles) {
    const n = normal(a, b, c);
    for (const value of [...n, ...a, ...b, ...c]) {
      view.setFloat32(offset, value, true);
      offset += 4;
    }
    view.setUint16(offset, 0, true);
    offset += 2;
  }
  return output;
}

export async function glbUrlToStlBlob(glbUrl: string): Promise<Blob> {
  const response = await fetch(glbUrl);
  if (!response.ok) throw new Error(`Unable to read generated GLB (${response.status}).`);
  const parsed = parseGlb(await response.arrayBuffer());
  return new Blob([binaryStl(collectTriangles(parsed.json, parsed.binary))], { type: 'model/stl' });
}

// Read directly from the GLB buffer, including sparse overrides. The software
// renderer needs every face, but does not expand it into per-triangle JS arrays.
function previewAccessor(document: GltfDocument, binary: DataView, index: number, type: 'VEC3' | 'SCALAR') {
  const accessor = document.accessors?.[index];
  if (!accessor || accessor.type !== type || !Number.isSafeInteger(accessor.count) || accessor.count < 0) {
    throw new Error('Invalid GLB preview accessor.');
  }
  const components = COMPONENTS[type];
  const bytes = COMPONENT_BYTES[accessor.componentType];
  if (!bytes) throw new Error('Unsupported GLB preview component type.');
  const getView = (index: number) => {
    const view = document.bufferViews?.[index];
    if (!view || (view.buffer ?? 0) !== 0 || (view as GltfBufferView & { extensions?: Record<string, unknown> }).extensions?.EXT_meshopt_compression) {
      throw new Error('Software preview requires uncompressed embedded geometry.');
    }
    return view;
  };
  const view = accessor.bufferView === undefined ? undefined : getView(accessor.bufferView);
  const stride = view?.byteStride || components * bytes;
  const start = (view?.byteOffset || 0) + (accessor.byteOffset || 0);
  const sparse = accessor.sparse;
  const indexStart = sparse ? (getView(sparse.indices.bufferView).byteOffset || 0) + (sparse.indices.byteOffset || 0) : 0;
  const valueStart = sparse ? (getView(sparse.values.bufferView).byteOffset || 0) + (sparse.values.byteOffset || 0) : 0;
  const indexBytes = sparse ? COMPONENT_BYTES[sparse.indices.componentType] : 0;
  return {
    count: accessor.count,
    offset(item: number): number | undefined {
      if (!Number.isInteger(item) || item < 0 || item >= accessor.count) throw new Error('Invalid GLB preview index.');
      let offset = view ? start + item * stride : undefined;
      if (sparse) {
        // glTF sparse indices are sorted. Find an override without allocating the accessor.
        let lo = 0; let hi = sparse.count;
        while (lo < hi) {
          const mid = Math.floor((lo + hi) / 2);
          const target = componentValue(binary, indexStart + mid * indexBytes, sparse.indices.componentType);
          if (target < item) lo = mid + 1; else hi = mid;
        }
        if (lo < sparse.count && componentValue(binary, indexStart + lo * indexBytes, sparse.indices.componentType) === item) {
          offset = valueStart + lo * components * bytes;
        }
      }
      return offset;
    },
    readScalar(item: number): number {
      const offset = this.offset(item);
      return offset === undefined ? 0 : componentValue(binary, offset, accessor.componentType, accessor.normalized);
    },
    readVec3(item: number, target: Vec3): void {
      const offset = this.offset(item);
      for (let c = 0; c < 3; c++) target[c] = offset === undefined ? 0
        : componentValue(binary, offset + c * bytes, accessor.componentType, accessor.normalized);
    },
  };
}

export interface SoftwarePreviewView {
  width: number;
  height: number;
  yaw: number;
  pitch: number;
  zoom: number;
}

// A depth buffer, rather than a painter's sort or a face sample, preserves
// occlusion and actual openings in the mesh at any tessellation density.
function rasterizeTriangle(projected: Float32Array, ia: number, ib: number, ic: number,
                           width: number, height: number, depth: Float32Array, pixels: Uint8ClampedArray) {
  const a = 3 * ia; const b = 3 * ib; const c = 3 * ic;
  const ax = projected[a]; const ay = projected[a + 1]; const az = projected[a + 2];
  const ux = projected[b] - ax; const uy = projected[b + 1] - ay; const uz = projected[b + 2] - az;
  const vx = projected[c] - ax; const vy = projected[c + 1] - ay; const vz = projected[c + 2] - az;
  const nx = uy * vz - uz * vy; const ny = uz * vx - ux * vz; const area = ux * vy - uy * vx;
  if (!Number.isFinite(area) || area === 0) return;
  const left = Math.max(0, Math.ceil(Math.min(ax, ax + ux, ax + vx) - 0.5));
  const right = Math.min(width - 1, Math.floor(Math.max(ax, ax + ux, ax + vx) - 0.5));
  const top = Math.max(0, Math.ceil(Math.min(ay, ay + uy, ay + vy) - 0.5));
  const bottom = Math.min(height - 1, Math.floor(Math.max(ay, ay + uy, ay + vy) - 0.5));
  if (left > right || top > bottom) return;
  const inverse = 1 / area;
  const length = Math.hypot(nx, ny, area);
  const shade = Math.round(90 + 150 * Math.min(1, Math.abs(0.3 * nx - 0.5 * ny + 0.81 * area) / length));
  // Pixel centers and inclusive shared edges avoid cracks between adjacent faces.
  for (let y = top; y <= bottom; y++) {
    const dy = y + 0.5 - ay;
    for (let x = left; x <= right; x++) {
      const dx = x + 0.5 - ax;
      const u = (dx * vy - dy * vx) * inverse;
      const v = (ux * dy - uy * dx) * inverse;
      if (u < -1e-7 || v < -1e-7 || u + v > 1 + 1e-7) continue;
      const z = az + u * uz + v * vz;
      const i = y * width + x;
      if (z <= depth[i]) continue;
      depth[i] = z;
      pixels[4 * i] = pixels[4 * i + 1] = pixels[4 * i + 2] = shade;
      pixels[4 * i + 3] = 255;
    }
  }
}

export function createGlbPreview(data: ArrayBuffer) {
  const { json: document, binary } = parseGlb(data);
  const nodes = document.nodes || [];
  type Accessor = ReturnType<typeof previewAccessor>;
  const parts: Array<{ positions: Accessor; indices: Accessor | null; world: Mat4; mode: number; count: number }> = [];
  const visit = (nodeIndex: number, parent: Mat4, path: Set<number>) => {
    if (path.has(nodeIndex)) throw new Error('The generated GLB contains a cyclic node hierarchy.');
    const node = nodes[nodeIndex];
    if (!node) return;
    const world = multiply(parent, nodeMatrix(node));
    const nextPath = new Set(path).add(nodeIndex);
    for (const primitive of (node.mesh === undefined ? [] : document.meshes?.[node.mesh]?.primitives || [])) {
      if (primitive.extensions?.KHR_draco_mesh_compression || primitive.extensions?.EXT_meshopt_compression) {
        throw new Error('Software preview requires uncompressed embedded geometry.');
      }
      const mode = primitive.mode ?? 4;
      if (![4, 5, 6].includes(mode) || primitive.attributes?.POSITION === undefined) continue;
      const positions = previewAccessor(document, binary, primitive.attributes.POSITION, 'VEC3');
      const indices = primitive.indices === undefined ? null : previewAccessor(document, binary, primitive.indices, 'SCALAR');
      const n = indices?.count ?? positions.count;
      const count = mode === 4 ? Math.floor(n / 3) : Math.max(0, n - 2);
      if (count) parts.push({ positions, indices, world, mode, count });
    }
    for (const child of node.children || []) visit(child, world, nextPath);
  };
  const children = new Set(nodes.flatMap(node => node.children || []));
  const roots = document.scenes?.[document.scene ?? 0]?.nodes || nodes.map((_, i) => i).filter(i => !children.has(i));
  for (const root of roots) visit(root, identity(), new Set<number>());
  const triangleCount = parts.reduce((sum, part) => sum + part.count, 0);
  if (!Number.isSafeInteger(triangleCount) || triangleCount < 1) throw new Error('The generated GLB contains no triangle mesh to preview.');

  const min = [Infinity, Infinity, Infinity]; const max = [-Infinity, -Infinity, -Infinity];
  const point: Vec3 = [0, 0, 0];
  let maxVertices = 0;
  for (const { positions, world } of parts) {
    maxVertices = Math.max(maxVertices, positions.count);
    for (let i = 0; i < positions.count; i++) {
      positions.readVec3(i, point);
      const p = transformPoint(world, point);
      if (!p.every(Number.isFinite)) throw new Error('Invalid GLB preview vertex.');
      for (let j = 0; j < 3; j++) { min[j] = Math.min(min[j], p[j]); max[j] = Math.max(max[j], p[j]); }
    }
  }
  const center = min.map((v, i) => v + (max[i] - v) / 2);
  const diameter = Math.hypot(...max.map((v, i) => v - min[i]));
  if (!Number.isFinite(diameter) || diameter <= 0) throw new Error('Invalid GLB preview bounds.');
  // Reuse a single compact projection buffer. The original GLB is retained in
  // the worker; no full triangle soup, sort, or duplicate index buffer is needed.
  const projected = new Float32Array(3 * maxVertices);

  return { triangleCount, render({ width, height, yaw, pitch, zoom }: SoftwarePreviewView): Uint8ClampedArray {
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 960 || height > 960
        || ![yaw, pitch, zoom].every(Number.isFinite) || zoom <= 0 || zoom > 4) throw new Error('Invalid GLB preview view.');
    const pixels = new Uint8ClampedArray(width * height * 4);
    const depth = new Float32Array(width * height).fill(-Infinity);
    const scale = 0.85 * Math.min(width, height) * zoom / diameter;
    const cy = Math.cos(yaw); const sy = Math.sin(yaw); const cp = Math.cos(pitch); const sp = Math.sin(pitch);
    for (const { positions, indices, world, mode, count } of parts) {
      for (let i = 0; i < positions.count; i++) {
        positions.readVec3(i, point);
        const p = transformPoint(world, point);
        const x = p[0] - center[0]; const y = p[1] - center[1]; const z = p[2] - center[2];
        const u = cy * x + sy * z; const w = -sy * x + cy * z;
        projected[3 * i] = width / 2 + scale * u;
        projected[3 * i + 1] = height / 2 - scale * (cp * y - sp * w);
        projected[3 * i + 2] = scale * (sp * y + cp * w);
      }
      const index = (i: number) => indices ? indices.readScalar(i) : i;
      for (let face = 0; face < count; face++) {
        let a: number; let b: number; let c: number;
        if (mode === 4) { a = index(3 * face); b = index(3 * face + 1); c = index(3 * face + 2); }
        else if (mode === 5) { a = index(face); b = index(face + 1); c = index(face + 2); }
        else { a = index(0); b = index(face + 1); c = index(face + 2); }
        if (![a, b, c].every(i => Number.isInteger(i) && i >= 0 && i < positions.count)) throw new Error('Invalid GLB preview index.');
        if (a !== b && b !== c && a !== c) rasterizeTriangle(projected, a, b, c, width, height, depth, pixels);
      }
    }
    return pixels;
  } };
}
