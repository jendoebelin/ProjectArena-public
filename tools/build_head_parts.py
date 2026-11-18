"""The head parts fitted to the game's heads, on their own (without the rest of tools/build_open_assets.py): the hair (with
its .swing rigs), the face shields (with their .tuck maps) and the masks, by tools/blender/make_avatar.py --head on
each body's head (build/open_src/body_q: tools/repose_bodies.py), and the face shields and masks on each of FACE 2-4,
then made into models as build_open_assets does.

The helmets are left out (--with-gear builds ours too): the game's are PR #25's fits to these heads
(tools/import_pr25.py).

Usage: python -I tools/build_head_parts.py [--out DIR] [--with-gear] [--skip-blender]
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import build_open_assets as B  # noqa: E402


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else B.OUT
    src = os.path.join(ROOT, 'build', 'open_src', 'head_q')
    os.makedirs(src, exist_ok=True)
    os.makedirs(out, exist_ok=True)
    faces = [k for k in B.FACE_KEYS if os.path.exists(os.path.join(ROOT, 'build', 'open_src', 'body_q', 'body_%s_face%s.npz' % (
        {'m': 'masc', 'f': 'fem'}[k[0]], k[1:])))]   # FACE 2-4 (tools/make_faces_q.py): their own face shields and masks
    if '--skip-blender' not in sys.argv:
        for key in ('m', 'f'):
            print('blender: head parts on the %s head' % key)
            B.run_blender('make_avatar.py', [os.path.join(src, 'avatar_head_%s.npz' % key), '--head', key])
        for key in faces:
            print('blender: face shields and masks on face', key)
            B.run_blender('make_avatar.py', [os.path.join(src, 'avatar_head_%s.npz' % key), '--head', key, 'av_visor', 'av_mask'])
    B.SRC, B.OUT = src, out
    built = set()
    for key in ('m', 'f') + tuple(faces):
        built |= B.build_props('avatar_head_%s.npz' % key)
    if '--with-gear' not in sys.argv:   # (the helmets: PR #25's fits, not ours)
        for f in os.listdir(out):
            if f.startswith('av_gear_') and f.endswith(('.mdl', '.tuck')) and f.rsplit('.', 1)[0] in built:
                os.remove(os.path.join(out, f))
        built = {b for b in built if not b.startswith('av_gear_')}
    print('head parts:', len(built), 'models ->', out)


if __name__ == '__main__':
    main()
