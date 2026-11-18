"""Extract approved native candidate clothing/headwear into an explicit output directory.
Example: python experiments/avatar_anatomy/clothing/extract_assets.py /tmp/arena-clothing
These are review assets, not game menu/renderer integration. Original and liked-body
fits are separate; garment motion requires the associated .motion.json and inspector code.
"""
import argparse,zipfile
from pathlib import Path
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 with zipfile.ZipFile(Path(__file__).with_name('assets.zip')) as z:
  for n in z.namelist():
   if not (out/n).resolve().is_relative_to(out):raise ValueError('Unsafe archive path')
  z.extractall(out)
 print('Extracted approved native candidates to',out)
