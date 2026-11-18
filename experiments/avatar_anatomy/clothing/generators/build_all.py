"""Build clothing source closure into a new explicit output tree, without editing shipped assets."""
import argparse, pathlib, subprocess, sys, tempfile, shutil
HERE = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=pathlib.Path,
                    help='Build tree containing base/, vector/, nightforge/ and lowers/ directories, each with assets/.')
args = parser.parse_args()
out = args.output.resolve()
def run(script, folder, *extra, source=None):
    command = [sys.executable, str(HERE / script), *extra, '--output', str(out / folder)]
    if source is not None:
        command += ['--source-assets', str(source)]
    subprocess.run(command, check=True)
with tempfile.TemporaryDirectory(prefix='arena-clothing-body-') as temporary:
    # Build the common body source first without adding duplicate heads/lowers to clothing output.
    source_output = pathlib.Path(temporary)
    run('build_collar.py', source_output)
    source = source_output / 'assets'
    base_output = out / 'base/assets'
    base_output.mkdir(parents=True, exist_ok=True)
    for sex in 'mf':
        shutil.copyfile(source / f'av_top_0_{sex}.mdl', base_output / f'av_top_0_{sex}.mdl')
    shutil.copytree(source / 'tex', base_output / 'tex', dirs_exist_ok=True)
    for script in ['build_vector_tops.py', 'build_nightforge_tops.py']:
        run(script, 'base', source=source)
    run('build_vector_shorts.py', 'vector', source=source)
    run('build_opaque_shorts.py', 'vector', source=source)
    run('build_nightforge_shorts.py', 'nightforge', source=source)
    for variant in ['liked', 'original']:
        for script in ['build_tennis.py', 'build_kilt.py', 'build_general_opaque.py']:
            run(script, 'lowers', variant, source=source if variant == 'liked' else None)
