"""Model-space <-> attachment-frame transforms used by app/src/main.cpp (kept in sync by hand).

The app draws hand props as grip * [mirror] * L where L = ueLocalToGrip(loc_cm, pyr_deg, scale). New (open) props
are authored directly in the attachment frame (grip: +X right, +Y up/thumb, -Z forward, metres) and converted to
model space with inv(L), so the app code stays unchanged.
"""
import numpy as np


def ue_local_to_grip(loc_cm, pyr_deg, scale):
    p, y, r = np.radians(pyr_deg)
    sp, cp, sy, cy, sr, cr = np.sin(p), np.cos(p), np.sin(y), np.cos(y), np.sin(r), np.cos(r)
    R = np.array([[cp * cy, cp * sy, sp],
                  [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
                  [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]])
    T = np.eye(4)
    for c in range(3):
        T[:3, c] = R[c] * scale[c]          # column c = row c of R, scaled (matches the C++ column-major fill)
    T[:3, 3] = loc_cm
    S = np.zeros((4, 4)); S[0, 0] = 100; S[2, 1] = 100; S[1, 2] = 100; S[3, 3] = 1     # gltf m -> UE cm
    C = np.zeros((4, 4)); C[2, 0] = -0.01; C[0, 1] = 0.01; C[1, 2] = 0.01; C[3, 3] = 1  # UE cm -> grip m
    return C @ T @ S


LANCER = ue_local_to_grip((1, -0.5, 0), (5, 0, 0), (1, 1, 1))
KNUCKLE = ue_local_to_grip((1.5, 0, 0), (0, 0, 0), (1, 1, 1))
PLATE = ue_local_to_grip((0, 0, 0), (0, 0, 180), (10, 10, 10))   # shield model -> shield frame


def to_model(M, pos, nrm):
    """Frame-space points/normals -> model space for a draw of frame * M."""
    Mi = np.linalg.inv(M)
    p = (np.c_[pos, np.ones(len(pos))] @ Mi.T)[:, :3]
    n = nrm @ M[:3, :3]               # inverse-transpose of Mi's linear part = M^T
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    return p.astype(np.float32), n.astype(np.float32)


def head_to_model(pos, nrm):
    """Head frame (eyes at origin, -Z forward, +Y up) -> head model space (rotY(180), +1.66 m)."""
    p = pos * np.array([-1, 1, -1]) + np.array([0, 1.66, 0])
    return p.astype(np.float32), (nrm * np.array([-1, 1, -1])).astype(np.float32)


def body_to_model(pos, nrm):
    """Body frame (origin on the floor under the head, -Z forward, +Y up) -> torso model space (faces +Z)."""
    return (pos * np.array([-1, 1, -1])).astype(np.float32), (nrm * np.array([-1, 1, -1])).astype(np.float32)
