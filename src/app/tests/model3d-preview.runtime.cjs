const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/features/model3d/exportStl.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  fileName: filename,
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
const mod = { exports: {} };
Function('exports', 'module', compiled.outputText)(mod.exports, mod);
const { createGlbPreview, glbUrlToStlBlob } = mod.exports;

function glb(document, binary) {
  const raw = Buffer.from(JSON.stringify(document));
  const json = Buffer.concat([raw, Buffer.alloc((4 - raw.length % 4) % 4, 0x20)]);
  const data = Buffer.concat([binary, Buffer.alloc((4 - binary.length % 4) % 4)]);
  const out = Buffer.alloc(28 + json.length + data.length);
  out.writeUInt32LE(0x46546c67, 0); out.writeUInt32LE(2, 4); out.writeUInt32LE(out.length, 8);
  out.writeUInt32LE(json.length, 12); out.writeUInt32LE(0x4e4f534a, 16); json.copy(out, 20);
  out.writeUInt32LE(data.length, 20 + json.length); out.writeUInt32LE(0x004e4942, 24 + json.length); data.copy(out, 28 + json.length);
  return out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength);
}

const positions = [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]];
const faces = [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3];
const binary = Buffer.alloc(48 + 2 * faces.length);
positions.flat().forEach((value, i) => binary.writeFloatLE(value, 4 * i));
faces.forEach((value, i) => binary.writeUInt16LE(value, 48 + 2 * i));
const doc = {
  asset: { version: '2.0' }, scene: 0, scenes: [{ nodes: [0] }],
  nodes: [{ mesh: 0, translation: [2, 3, 4] }],
  meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 1 }] }],
  buffers: [{ byteLength: binary.length }],
  bufferViews: [{ buffer: 0, byteLength: 48 }, { buffer: 0, byteOffset: 48, byteLength: 24 }],
  accessors: [{ bufferView: 0, componentType: 5126, count: 4, type: 'VEC3' }, { bufferView: 1, componentType: 5123, count: 12, type: 'SCALAR' }],
};
const model = glb(doc, binary);
const view = { width: 320, height: 320, yaw: 0, pitch: 0, zoom: 1 };
const preview = createGlbPreview(model);
assert.equal(preview.triangleCount, 4);
assert.ok(preview.render(view).some((value, i) => i % 4 === 3 && value === 255));
assert.throws(() => createGlbPreview(new ArrayBuffer(16)), /valid binary glTF/);
assert.throws(() => preview.render({ ...view, width: 961 }), /invalid GLB preview view/i);
const stripDoc = structuredClone(doc);
stripDoc.meshes[0].primitives[0] = { attributes: { POSITION: 0 }, mode: 5 };
assert.equal(createGlbPreview(glb(stripDoc, binary)).triangleCount, 2);
const fanDoc = structuredClone(stripDoc);
fanDoc.meshes[0].primitives[0].mode = 6;
assert.equal(createGlbPreview(glb(fanDoc, binary)).triangleCount, 2);
const compressedDoc = structuredClone(doc);
compressedDoc.meshes[0].primitives[0].extensions = { KHR_draco_mesh_compression: {} };
assert.throws(() => createGlbPreview(glb(compressedDoc, binary)), /uncompressed/);

function indexedModel(vertices, indices) {
  const data = Buffer.alloc(vertices.length * 4 + indices.length * 4);
  vertices.forEach((v, i) => data.writeFloatLE(v, 4 * i));
  indices.forEach((v, i) => data.writeUInt32LE(v, vertices.length * 4 + 4 * i));
  const d = structuredClone(doc);
  d.nodes[0] = { mesh: 0 };
  d.buffers[0].byteLength = data.length;
  d.bufferViews[0].byteLength = vertices.length * 4;
  d.bufferViews[1].byteOffset = vertices.length * 4;
  d.bufferViews[1].byteLength = indices.length * 4;
  d.accessors[0].count = vertices.length / 3;
  d.accessors[1] = { bufferView: 1, componentType: 5125, count: indices.length, type: 'SCALAR' };
  return glb(d, data);
}
function grid(n) {
  const vertices = []; const indices = [];
  for (let y = 0; y <= n; y++) for (let x = 0; x <= n; x++) vertices.push(x / n, y / n, 0);
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
    const i = y * (n + 1) + x;
    indices.push(i, i + 1, i + n + 1, i + 1, i + n + 2, i + n + 1);
  }
  return createGlbPreview(indexedModel(vertices, indices));
}
// Identical planar surface, subdivided into 131072 faces. Face sampling would
// leave most of this surface transparent, while rasterizing it must not.
const coarse = grid(1).render(view);
const denseGrid = grid(256);
assert.equal(denseGrid.triangleCount, 131072);
const dense = denseGrid.render(view);
assert.deepEqual(dense, coarse, 'tessellation density must not change surface coverage');
for (let y = 80; y < 240; y++) for (let x = 80; x < 240; x++) assert.equal(dense[4 * (y * 320 + x) + 3], 255);

function torus(n, m) {
  const vertices = []; const indices = [];
  for (let i = 0; i < n; i++) for (let j = 0; j < m; j++) {
    const u = 2 * Math.PI * i / n; const v = 2 * Math.PI * j / m;
    const r = 1 + 0.4 * Math.cos(v);
    vertices.push(r * Math.cos(u), r * Math.sin(u), 0.4 * Math.sin(v));
    const a = i * m + j; const b = ((i + 1) % n) * m + j;
    const c = i * m + (j + 1) % m; const d = ((i + 1) % n) * m + (j + 1) % m;
    indices.push(a, b, c, b, d, c);
  }
  return createGlbPreview(indexedModel(vertices, indices));
}
const ring = torus(512, 128);
assert.equal(ring.triangleCount, 131072);
const ringPixels = ring.render(view);
assert.equal(ringPixels[4 * (160 * 320 + 160) + 3], 0, 'real through-hole must stay open');
// Expected projected radius ~40..94 px. Check interior annulus, away from silhouette.
for (let y = 65; y < 255; y++) for (let x = 65; x < 255; x++) {
  const r = Math.hypot(x + 0.5 - 160, y + 0.5 - 160);
  if (r > 47 && r < 87) assert.equal(ringPixels[4 * (y * 320 + x) + 3], 255, 'no invented holes in torus surface');
}
assert.notDeepEqual(ring.render({ ...view, yaw: 0.6, pitch: 0.2 }), ringPixels, 'rotation must update the view');

// The front sloped triangle must occlude the rear flat one, independent of order.
const overlapVertices = [-1, -1, 0, 1, -1, 0, 0, 1, 0, -1, -1, 2, 1, -1, 3, 0, 1, 2];
const frontFirst = createGlbPreview(indexedModel(overlapVertices, [3, 4, 5, 0, 1, 2])).render(view);
const backFirst = createGlbPreview(indexedModel(overlapVertices, [0, 1, 2, 3, 4, 5])).render(view);
assert.deepEqual(frontFirst, backFirst, 'occlusion must not depend on face ordering');
assert.notEqual(frontFirst[4 * (160 * 320 + 160)], 212, 'rear flat triangle must not overwrite the front');

(async () => {
  const url = URL.createObjectURL(new Blob([model]));
  try {
    const stl = new DataView(await (await glbUrlToStlBlob(url)).arrayBuffer());
    assert.equal(stl.getUint32(80, true), 4, 'STL export must retain all original triangles');
    assert.equal(stl.byteLength, 84 + 4 * 50);
    assert.equal(stl.getFloat32(96, true), 2);
  } finally { URL.revokeObjectURL(url); }
  console.log('Full-surface GLB preview, real hole, occlusion and unchanged STL export passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
