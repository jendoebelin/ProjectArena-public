"""Portable paths and explicit output selection for experimental clothing builds."""
import argparse, pathlib, sys, tempfile, zipfile, shutil
HERE = pathlib.Path(__file__).resolve().parent
ANATOMY = HERE.parents[1]
REPO = ANATOMY.parents[1]
sys.path.insert(0, str(REPO / 'tools'))
_TEMP = []
def baseline_assets():
    archive = ANATOMY / 'assets.zip'
    if archive.exists():
        temp = tempfile.TemporaryDirectory(prefix='arena-clothing-source-')
        _TEMP.append(temp)
        with zipfile.ZipFile(archive) as z:
            z.extractall(temp.name)
        return pathlib.Path(temp.name)
    source = ANATOMY / 'assets'
    if not source.exists():
        raise FileNotFoundError('Build the experimental anatomy assets first, or supply --source-assets.')
    return source

def config(description, variants=False, corrected=False, source_required=True):
    parser = argparse.ArgumentParser(description=description)
    if variants:
        parser.add_argument('variant', choices=['liked', 'original'])
    parser.add_argument('--output', required=True, type=pathlib.Path,
                        help='New build directory; native output is written in its assets/ subdirectory.')
    parser.add_argument('--source-assets', type=pathlib.Path,
                        help='Override native body source directory (otherwise anatomy assets.zip or app/assets_open).')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'assets').mkdir(exist_ok=True)
    if not source_required:
        source = None
    elif args.source_assets:
        source = args.source_assets.resolve()
    elif variants and args.variant == 'original':
        source = REPO / 'app/assets_open'
    else:
        source = baseline_assets()
    if corrected:
        # Corrected experimental top0 is the source for the final outfit tops.
        # The anatomy archive is updated with that same approved collar geometry.
        for sex in 'mf':
            if not (source / f'av_top_0_{sex}.mdl').exists():
                raise FileNotFoundError('Corrected experimental top0 models are required.')
    # Lower fade materials use the game's existing strip texture. Keep each output portable.
    candidates = ([source / 'tex/open_soft_strip.tex'] if source is not None else [])
    candidates.append(REPO / 'app/assets_open/tex/open_soft_strip.tex')
    for texture in candidates:
        if texture.exists():
            destination = args.output / 'assets/tex/open_soft_strip.tex'
            destination.parent.mkdir(parents=True, exist_ok=True)
            if texture.resolve() != destination.resolve():
                shutil.copyfile(texture, destination)
            break
    else:
        raise FileNotFoundError('Required source texture app/assets_open/tex/open_soft_strip.tex is missing.')
    return args, source
