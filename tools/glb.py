"""Minimal glTF 2.0 binary (.glb) writer and reader for Project Arena's asset exchange (numpy only).

Writer: Glb().add_mesh(...), add_skin(...), save(path). Reader: read_glb(path) -> list of primitives with
positions, normals, uvs, joints / weights, indices, material name and the material's extras.
"""
import io
import json
import struct

import numpy as np

FLOAT, UBYTE, USHORT, UINT = 5126, 5121, 5123, 5125
ARRAY, ELEMENT = 34962, 34963
_NP = {FLOAT: np.float32, UBYTE: np.uint8, USHORT: np.uint16, UINT: np.uint32}
_TYPES = {1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4', 16: 'MAT4'}
_COUNT = {v: k for k, v in _TYPES.items()}


class Glb:
    def __init__(self, generator='Project Arena tools/export_glb.py'):
        self.j = {'asset': {'version': '2.0', 'generator': generator}, 'scene': 0, 'scenes': [{'nodes': []}],
                  'nodes': [], 'meshes': [], 'materials': [], 'accessors': [], 'bufferViews': [], 'buffers': []}
        self.bin = bytearray()
        self._mat = {}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin += b'\0'
        v = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.bin += data
        self.j['bufferViews'].append(v)
        return len(self.j['bufferViews']) - 1

    def accessor(self, arr, ctype, target=None, normalized=False, minmax=False):
        arr = np.ascontiguousarray(arr, dtype=_NP[ctype])
        comps = 1 if arr.ndim == 1 else arr.shape[1] * (arr.shape[2] if arr.ndim == 3 else 1)
        a = {'bufferView': self._view(arr.tobytes(), target), 'componentType': ctype, 'count': int(arr.shape[0]),
             'type': _TYPES[comps]}
        if normalized:
            a['normalized'] = True
        if minmax:
            flat = arr.reshape(arr.shape[0], -1)
            a['min'] = [float(x) for x in flat.min(0)]
            a['max'] = [float(x) for x in flat.max(0)]
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def image(self, rgba_u8):
        from PIL import Image
        buf = io.BytesIO()
        Image.fromarray(rgba_u8, 'RGBA').save(buf, 'PNG')
        self.j.setdefault('images', []).append({'bufferView': self._view(buf.getvalue()), 'mimeType': 'image/png'})
        self.j.setdefault('samplers', [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}])
        self.j.setdefault('textures', []).append({'sampler': 0, 'source': len(self.j['images']) - 1})
        return len(self.j['textures']) - 1

    def material(self, name, color=(0.8, 0.8, 0.8, 1), emissive=(0, 0, 0), roughness=0.6, metallic=0.0, blend=False,
                 double=False, texture=None, tex_coord=0, extras=None):
        key = (name, tuple(round(c, 4) for c in color), texture, tex_coord, double, blend, json.dumps(extras, sort_keys=True))
        if key in self._mat:
            return self._mat[key]
        pbr = {'baseColorFactor': [float(c) for c in color], 'metallicFactor': float(metallic), 'roughnessFactor': float(roughness)}
        if texture is not None:
            pbr['baseColorTexture'] = {'index': texture, 'texCoord': tex_coord}
        m = {'name': name, 'pbrMetallicRoughness': pbr}
        if any(emissive):
            m['emissiveFactor'] = [float(min(1.0, c)) for c in emissive]
        if blend:
            m['alphaMode'] = 'BLEND'
        if double:
            m['doubleSided'] = True
        if extras:
            m['extras'] = extras
        self.j['materials'].append(m)
        self._mat[key] = len(self.j['materials']) - 1
        return self._mat[key]

    def add_mesh(self, name, prims, skin=None, translation=None):
        """prims: dicts with pos, nrm, uv0, [uv1], [joints, weights], idx, material."""
        out = []
        for p in prims:
            attr = {'POSITION': self.accessor(p['pos'], FLOAT, ARRAY, minmax=True),
                    'NORMAL': self.accessor(p['nrm'], FLOAT, ARRAY),
                    'TEXCOORD_0': self.accessor(p['uv0'], FLOAT, ARRAY)}
            if p.get('uv1') is not None:
                attr['TEXCOORD_1'] = self.accessor(p['uv1'], FLOAT, ARRAY)
            if p.get('joints') is not None:
                attr['JOINTS_0'] = self.accessor(p['joints'], UBYTE, ARRAY)
                attr['WEIGHTS_0'] = self.accessor(p['weights'], UBYTE, ARRAY, normalized=True)
            out.append({'attributes': attr, 'indices': self.accessor(p['idx'], UINT, ELEMENT), 'material': p['material']})
        self.j['meshes'].append({'name': name, 'primitives': out})
        node = {'name': name, 'mesh': len(self.j['meshes']) - 1}
        if skin is not None:
            node['skin'] = skin
        if translation is not None:
            node['translation'] = [float(x) for x in translation]
        self.j['nodes'].append(node)
        self.j['scenes'][0]['nodes'].append(len(self.j['nodes']) - 1)

    def add_skin(self, names, positions, parents):
        """Joints with world positions (no rotation) and a parent index (-1 = root). Returns the skin index."""
        base = len(self.j['nodes'])
        for i, (n, p, par) in enumerate(zip(names, positions, parents)):
            local = np.asarray(p, float) - (np.asarray(positions[par], float) if par >= 0 else 0)
            self.j['nodes'].append({'name': n, 'translation': [float(x) for x in local]})
        for i, par in enumerate(parents):
            if par >= 0:
                self.j['nodes'][base + par].setdefault('children', []).append(base + i)
            else:
                self.j['scenes'][0]['nodes'].append(base + i)
        ibm = np.zeros((len(names), 16), np.float32)
        for i, p in enumerate(positions):
            m = np.eye(4, dtype=np.float32)
            m[:3, 3] = -np.asarray(p, np.float32)
            ibm[i] = m.T.reshape(-1)   # column-major
        self.j.setdefault('skins', []).append({'name': 'avatar_rig', 'joints': list(range(base, base + len(names))),
                                               'inverseBindMatrices': self.accessor(ibm, FLOAT),
                                               'skeleton': base + parents.index(-1)})
        return len(self.j['skins']) - 1

    def save(self, path):
        while len(self.bin) % 4:
            self.bin += b'\0'
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        for k in ('materials', 'meshes'):
            if not self.j[k]:
                del self.j[k]
        js = json.dumps(self.j, separators=(',', ':')).encode()
        js += b' ' * ((4 - len(js) % 4) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


def read_glb(path):
    """-> (json, list of primitives: pos, nrm, uv0, uv1, joints, weights, idx (all world-space for unskinned nodes
    with a translation), material name, material extras, node name, skinned)."""
    b = open(path, 'rb').read()
    magic, ver, total = struct.unpack_from('<III', b, 0)
    if magic != 0x46546C67:
        raise ValueError('not a .glb file')
    off, j, binb = 12, None, b''
    while off < len(b):
        n, typ = struct.unpack_from('<II', b, off)
        chunk = b[off + 8:off + 8 + n]
        if typ == 0x4E4F534A:
            j = json.loads(chunk)
        elif typ == 0x004E4942:
            binb = chunk
        off += 8 + n

    def acc(i):
        a = j['accessors'][i]
        v = j['bufferViews'][a['bufferView']]
        dt = _NP[a['componentType']]
        k = _COUNT[a['type']]
        start = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = v.get('byteStride', 0)
        item = np.dtype(dt).itemsize * k
        if stride and stride != item:
            raw = np.frombuffer(binb, np.uint8, a['count'] * stride, start).reshape(a['count'], stride)[:, :item]
            arr = np.frombuffer(raw.tobytes(), dt).reshape(a['count'], k)
        else:
            arr = np.frombuffer(binb, dt, a['count'] * k, start).reshape(a['count'], k)
        if a.get('normalized'):
            arr = arr.astype(np.float32) / np.iinfo(dt).max
        return arr

    def node_matrix(node):
        m = np.eye(4)
        if 'matrix' in node:
            return np.array(node['matrix'], float).reshape(4, 4).T
        t, r, s = node.get('translation', [0, 0, 0]), node.get('rotation', [0, 0, 0, 1]), node.get('scale', [1, 1, 1])
        x, y, z, w = r
        R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m[:3, :3] = R * np.array(s)
        m[:3, 3] = t
        return m

    parent = {}
    for i, n in enumerate(j.get('nodes', [])):
        for c in n.get('children', []):
            parent[c] = i

    def world(i):
        m = node_matrix(j['nodes'][i])
        while i in parent:
            i = parent[i]
            m = node_matrix(j['nodes'][i]) @ m
        return m

    prims = []
    for ni, node in enumerate(j.get('nodes', [])):
        if 'mesh' not in node:
            continue
        skinned = 'skin' in node
        M = np.eye(4) if skinned else world(ni)
        for p in j['meshes'][node['mesh']]['primitives']:
            at = p['attributes']
            pos = acc(at['POSITION']).astype(np.float64)
            nrm = acc(at['NORMAL']).astype(np.float64) if 'NORMAL' in at else np.zeros_like(pos)
            pos = pos @ M[:3, :3].T + M[:3, 3]
            nrm = nrm @ np.linalg.inv(M[:3, :3]).T
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
            n = len(pos)
            mat = j['materials'][p['material']] if 'material' in p else {'name': 'unnamed'}
            prims.append(dict(
                pos=pos, nrm=nrm,
                uv0=acc(at['TEXCOORD_0']).astype(np.float64) if 'TEXCOORD_0' in at else np.zeros((n, 2)),
                uv1=acc(at['TEXCOORD_1']).astype(np.float64) if 'TEXCOORD_1' in at else None,
                joints=acc(at['JOINTS_0']).astype(np.int64) if 'JOINTS_0' in at else None,
                weights=acc(at['WEIGHTS_0']).astype(np.float64) if 'WEIGHTS_0' in at else None,
                idx=acc(p['indices']).reshape(-1).astype(np.int64) if 'indices' in p else np.arange(n),
                material=mat.get('name', 'unnamed'), extras=mat.get('extras', {}), double=bool(mat.get('doubleSided')),
                node=node.get('name', ''),
                skinned=skinned))
    return j, prims
