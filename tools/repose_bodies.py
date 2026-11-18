"""The PR #25 bodies (experiments/avatar_anatomy/base: Quaternius's CC0 bodies, retargeted by its retarget.py, bound
with the arms straight out) posed the way the game's bodies are bound: the arms down and out, the elbows bent
forward (the MPFB bodies' bind, app/src/avatar_bodies.inc before 2026-11-17). Everything downstream then holds as it
was: tools/build_body_parts.py (sleeves, bracers and drapes measured along arms that hang), the rig's tuning, the
torso's shape for the elbows (tools/measure_torso.py).

Each arm is turned with its own skin weights (linear blend, the game's own skinning): the upper arm about the shoulder
the least way onto the old bind's upper arm direction (it swings down, about the forward axis), the forearm about the
elbow the least way onto the old forearm direction (it bends forward); the hands (thumbs) with the forearms. Normals
turn the same way. The torso, neck, head and collarbones stay as they were. The joints are moved to match.

Writes build/open_src/body_q/body_<masc|fem>.npz (the same fields as tools/blender/make_body.py's) and copies the
eyes and brows (head bones only, unchanged).

Usage: python -I tools/repose_bodies.py
"""
import json
import os
import shutil

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'experiments', 'avatar_anatomy', 'base')
OUT = os.path.join(ROOT, 'build', 'open_src', 'body_q')
FACES = os.path.join(ROOT, 'tools', 'bodies_q')   # FACE 2-4's changes (tools/make_faces_q.py)
UPL, UPR, FOL, FOR = 3, 4, 5, 6

# the MPFB bodies' bind (right side; the left mirrored): shoulder -> elbow, elbow -> wrist
TARGET = {'masc': ((0.18010, -0.20300, 0.00040), (0.14050, -0.13470, -0.20100)),
          'fem': ((0.15790, -0.17140, -0.00370), (0.11060, -0.12190, -0.13740))}


def unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def from_to(a, b):
    """The least rotation (3x3) taking unit vector a onto unit vector b."""
    a, b = unit(a), unit(b)
    v = np.cross(a, b)
    c = float(a @ b)
    if c < -0.999999:
        raise ValueError('opposite directions')
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + c)


# The head's regions, as tools/blender/make_avatar.py --head reads them to fit the hair, face shields and masks (1 the
# lips, 2 the scalp, 3 the ears; tools/blender/make_body.py marks them on the MPFB heads): his heads come unmarked, so
# they are marked where the MPFB heads have them, by angle from the middle of the head (make_avatar's HEAD_C, in its
# head space: x right, y forward, z up, the eyes at the origin): the scalp from the MPFB hairline up (its lowest
# elevation every 10 degrees from the front round to the back), the lips at the front between their elevations
# (the outermost 6 mm), the ears out at the sides between theirs (the outermost 2 cm).
HAIRLINE = {'masc': [42.3, 41.2, 39.8, 31.8, 28.9, 19.1, 3.0, 2.9, 9.4, 11.9, 9.2, -16.5, -29.0, -40.2, -41.5, -39.9, -42.4, -41.4, -40.8],
            'fem': [47.5, 46.7, 45.0, 36.2, 32.8, 27.4, 3.5, 3.5, 3.7, 10.7, 7.8, -17.6, -25.7, -38.1, -37.4, -42.1, -46.5, -46.8, -46.9]}
LIPS_EL, LIPS_AZ = (-39.0, -26.5), 18.0
EARS_EL, EARS_AZ = (-30.0, 8.0), (78.0, 105.0)


def label_head(d, J, name):
    region = np.zeros(len(d['pos']), d['region'].dtype)
    eye = np.array(J['eye_mid'], float)
    Q = d['pos'].astype(float) - eye
    HP = np.stack([Q[:, 0], -Q[:, 2], Q[:, 1]], -1)
    T, dom = d['tris'], d['bone'][:, 0]
    headv = np.unique(T[np.isin(dom[T], [2]).all(1)].reshape(-1))
    cr = HP[headv][HP[headv][:, 2] > -0.01]
    C = np.array([0.0, 0.5 * (cr[:, 1].min() + cr[:, 1].max()), -0.006])
    v = HP[headv] - C
    el = np.degrees(np.arctan2(v[:, 2], np.hypot(v[:, 0], v[:, 1])))
    az = np.degrees(np.abs(((np.arctan2(v[:, 1], v[:, 0]) - np.pi / 2 + np.pi) % (2 * np.pi)) - np.pi))
    r = np.linalg.norm(v, axis=1)
    scalp = el >= np.interp(az, np.arange(0, 181, 10), HAIRLINE[name])
    lips = (az < LIPS_AZ) & (el > LIPS_EL[0]) & (el < LIPS_EL[1])
    lips &= r >= (r[lips].max() - 0.006) if lips.any() else lips
    ears = (az > EARS_AZ[0]) & (az < EARS_AZ[1]) & (el > EARS_EL[0]) & (el < EARS_EL[1])
    ears &= np.abs(HP[headv][:, 0]) >= (np.abs(HP[headv][ears, 0]).max() - 0.02) if ears.any() else ears
    region[headv[scalp]] = 2
    region[headv[lips]] = 1
    region[headv[ears]] = 3
    print('  %s head: scalp %d, lips %d, ears %d vertices' % (name, scalp.sum(), lips.sum(), ears.sum()))
    return region


def turns(name, quiet=False):
    """Per bone (the game's 9): the rotation R and the point it turns about, and where that point goes (x -> c' +
    R (x - c)); and the joints moved to match."""
    J = json.loads(str(np.load(os.path.join(SRC, 'body_%s.npz' % name))['joints']))
    R = [np.eye(3) for _ in range(9)]
    C = [np.zeros(3) for _ in range(9)]
    Cn = [np.zeros(3) for _ in range(9)]
    J2 = dict(J)
    for side, up, fo, sg in (('r', UPR, FOR, 1.0), ('l', UPL, FOL, -1.0)):
        S, E, Wr, Th = (np.array(J[k + '_' + side], float) for k in ('shoulder', 'elbow', 'wrist', 'thumb'))
        tu, tf = (np.array(t) * [sg, 1, 1] for t in TARGET[name])
        Ru = from_to(E - S, tu)
        E2 = S + Ru @ (E - S)
        Rf = from_to(Ru @ (Wr - E), tf) @ Ru
        R[up], C[up], Cn[up] = Ru, S, S
        R[fo], C[fo], Cn[fo] = Rf, E, E2
        J2['elbow_' + side] = E2.tolist()
        J2['wrist_' + side] = (E2 + Rf @ (Wr - E)).tolist()
        J2['thumb_' + side] = (E2 + Rf @ (Th - E)).tolist()
        if not quiet:
            bend = np.degrees(np.arccos(np.clip(unit(Ru @ (Wr - E)) @ unit(tf), -1, 1)))
            down = np.degrees(np.arccos(np.clip(unit(E - S) @ unit(tu), -1, 1)))
            print('  %s %s: upper arm down %.1f deg, elbow bent %.1f deg' % (name, side, down, bend))
    return R, C, Cn, J, J2


def skin(P, N, B, W, R, C, Cn):
    """Linear blend skinning by the turns (positions and normals)."""
    W = W / np.maximum(W.sum(1, keepdims=True), 1e-12)
    P2 = np.zeros_like(P)
    N2 = np.zeros_like(N)
    for k in range(4):
        w = W[:, k:k + 1]
        for bone in range(9):
            sel = B[:, k] == bone
            if not sel.any():
                continue
            P2[sel] += w[sel] * (Cn[bone] + (P[sel] - C[bone]) @ R[bone].T)
            N2[sel] += w[sel] * (N[sel] @ R[bone].T)
    N2 /= np.maximum(np.linalg.norm(N2, axis=1, keepdims=True), 1e-12)
    return P2, N2


def renormal(P, T, N, sel):
    """N with the vertices in sel given area-weighted normals of the mesh welded by position."""
    key = np.round(P * 1e5).astype(np.int64)
    _, cid = np.unique(key, axis=0, return_inverse=True)
    cid = cid.reshape(-1)
    Pc = np.zeros((cid.max() + 1, 3))
    Pc[cid] = P
    Tc = cid[T]
    fn = np.cross(Pc[Tc[:, 1]] - Pc[Tc[:, 0]], Pc[Tc[:, 2]] - Pc[Tc[:, 0]])
    Nc = np.zeros_like(Pc)
    for c in range(3):
        np.add.at(Nc, Tc[:, c], fn)
    Nc /= np.maximum(np.linalg.norm(Nc, axis=1, keepdims=True), 1e-12)
    out = N.copy()
    out[sel] = Nc[cid][sel]
    return out


def repose(name):
    d = np.load(os.path.join(SRC, 'body_%s.npz' % name))
    R, C, Cn, J, J2 = turns(name)
    P = d['pos'].astype(np.float64)
    P2, N2 = skin(P, d['nrm'].astype(np.float64), d['bone'].astype(np.int64), d['weight'].astype(np.float64), R, C, Cn)
    out = {k: d[k] for k in d.files}
    out['region'] = label_head(d, J, name)
    out['pos'] = P2.astype(d['pos'].dtype)
    out['nrm'] = N2.astype(d['nrm'].dtype)
    out['joints'] = np.array(json.dumps(J2))
    np.savez(os.path.join(OUT, 'body_%s.npz' % name), **out)
    # FACE 2-4 (tools/make_faces_q.py: the MPFB faces' changes carried to his heads): the same body, its head changed
    faces = os.path.join(FACES, 'faces_%s.npz' % name)
    if os.path.exists(faces):
        F = np.load(faces)
        brows = np.load(os.path.join(SRC, 'brows_%s.npz' % name))
        for f in (1, 2, 3):
            D = F['face%d' % f].astype(np.float64)
            Pf = P2 + D
            out_f = dict(out)
            out_f['pos'] = Pf.astype(d['pos'].dtype)
            out_f['nrm'] = renormal(Pf, d['tris'], N2, np.linalg.norm(D, axis=1) > 1e-6).astype(d['nrm'].dtype)
            np.savez(os.path.join(OUT, 'body_%s_face%d.npz' % (name, f)), **out_f)
            b = {k: brows[k] for k in brows.files}
            b['pos'] = (brows['pos'] + F['brows%d' % f]).astype(brows['pos'].dtype)
            np.savez(os.path.join(OUT, 'brows_%s_face%d.npz' % (name, f)), **b)
            shutil.copyfile(os.path.join(SRC, 'eyes_%s.npz' % name), os.path.join(OUT, 'eyes_%s_face%d.npz' % (name, f)))
        print('  %s: FACE 2-4 written' % name)
    moved = np.linalg.norm(P2 - P, axis=1)
    print('  %s: %d vertices, %d moved (most %.0f cm)' % (name, len(P), int((moved > 1e-6).sum()), moved.max() * 100))


def repose_mdl(src, dst, body):
    """A skinned model (.mdl v3) made on these bodies as they come (PR #25's outfits), posed the same way."""
    import struct
    b = bytearray(open(src, 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    if ver < 3:
        open(dst, 'wb').write(bytes(b))
        return 0
    dt = np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4), ('bi', 'u1', 4), ('bw', 'u1', 4)])
    v = np.frombuffer(bytes(b[24:24 + nv * 52]), dtype=dt).copy()
    R, C, Cn, _, _ = turns({'m': 'masc', 'f': 'fem'}[body], quiet=True)
    P2, N2 = skin(v['p'].astype(np.float64), v['n'].astype(np.float64), v['bi'].astype(np.int64), v['bw'].astype(np.float64), R, C, Cn)
    moved = int((np.linalg.norm(P2 - v['p'], axis=1) > 1e-6).sum())
    v['p'], v['n'] = P2, N2
    b[24:24 + nv * 52] = v.tobytes()
    open(dst, 'wb').write(bytes(b))
    return moved


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in ('masc', 'fem'):
        repose(name)
        for part in ('eyes', 'brows'):
            shutil.copyfile(os.path.join(SRC, '%s_%s.npz' % (part, name)), os.path.join(OUT, '%s_%s.npz' % (part, name)))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
