"""Downloads for the asset catalog: every card's models as .glb (several models: one .zip), textures as .png, and the
contributor template pack. Used by tools/gen_catalog.py and tools/catalog_avatar.py."""
import html
import os
import re
import shutil
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
PNG_DIRS = [os.path.join(ROOT, 'build', 'open_src', 'tex'), os.path.join(ROOT, 'build', 'open_src')]


def _models(text):
    seen = []
    for t in re.findall(r'[a-z][a-z0-9_]*', text):
        if t not in seen and os.path.exists(os.path.join(ASSETS, t + '.mdl')):
            seen.append(t)
    return seen


def _texture(text):
    m = re.search(r'tex/(?:open_)?([a-z0-9_]+)\.tex', text)
    if not m:
        return None
    for d in PNG_DIRS:
        for n in (m.group(1), 'open_' + m.group(1)):
            p = os.path.join(d, n + '.png')
            if os.path.exists(p):
                return p
    return None


def link(out_dir, id_, text):
    """Make the download file for one catalog card; returns the button HTML ('' when there is nothing to give)."""
    import export_glb
    dl = os.path.join(out_dir, 'dl')
    os.makedirs(dl, exist_ok=True)
    models = _models(text)
    if models:
        if len(models) == 1:
            rel = 'dl/%s_%s.glb' % (id_, models[0])
            export_glb.export(models[0], os.path.join(out_dir, rel))
        else:
            rel = 'dl/%s.zip' % id_
            tmp = os.path.join(dl, '_' + id_)
            os.makedirs(tmp, exist_ok=True)
            with zipfile.ZipFile(os.path.join(out_dir, rel), 'w', zipfile.ZIP_DEFLATED) as z:
                for m in models:
                    p = os.path.join(tmp, m + '.glb')
                    export_glb.export(m, p)
                    z.write(p, m + '.glb')
            shutil.rmtree(tmp)
        label = 'Download .glb' if len(models) == 1 else 'Download %d .glb' % len(models)
    else:
        png = _texture(text)
        if not png:
            return ''
        rel = 'dl/%s_%s' % (id_, os.path.basename(png))
        shutil.copy2(png, os.path.join(out_dir, rel))
        label = 'Download .png'
    return '<a class="copy dl" href="%s" download>%s</a>' % (html.escape(rel), label)


def templates(out_dir):
    """The contributor template pack (tools/make_templates.py) as dl/project-arena-templates.zip."""
    import make_templates
    src = os.path.join(ROOT, 'build', 'templates')
    import sys
    argv = sys.argv
    sys.argv = ['make_templates.py', src]
    try:
        make_templates.main()
    finally:
        sys.argv = argv
    rel = 'dl/project-arena-templates.zip'
    os.makedirs(os.path.join(out_dir, 'dl'), exist_ok=True)
    with zipfile.ZipFile(os.path.join(out_dir, rel), 'w', zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(src):
            for f in files:
                p = os.path.join(base, f)
                z.write(p, os.path.join('project-arena-templates', os.path.relpath(p, src)))
    return rel
