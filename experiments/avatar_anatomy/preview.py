"""Opt-in desktop clay preview, not the game's live VR renderer.
Example: python experiments/avatar_anatomy/preview.py --body feminine --pose rest --lower 1 --output /tmp/avatar.png
Requires Python + numpy + Pillow and a GNU/Clang-compatible C++17 compiler (CXX, default g++; not MSVC cl).
"""
from pathlib import Path
import argparse,copy,json,os,subprocess,sys,zipfile
import numpy as np
HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
sys.path.insert(0,str(REPO/'tools'))
import preview_avatar as renderer
from skin import deform

def helper():
    exe=REPO/'build/experimental_avatar'/('pose.exe' if os.name=='nt' else 'pose')
    exe.parent.mkdir(parents=True,exist_ok=True)
    dependencies=[HERE/'pose.cpp',HERE/'avatar_experimental.cpp',HERE/'avatar_bodies.inc',HERE/'torso_follow.h',REPO/'app/src/avatar.h']
    if not exe.exists() or any(p.stat().st_mtime>exe.stat().st_mtime for p in dependencies):
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-O2','-I'+str(REPO/'app/src'),str(HERE/'pose.cpp'),str(HERE/'avatar_experimental.cpp'),'-o',str(exe)],check=True)
    return exe

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--body',choices=['masculine','feminine'],default='feminine')
    parser.add_argument('--pose',choices=['rest','ready','guard','throw','reach','reachfit'],default='rest')
    parser.add_argument('--lower',type=int,choices=range(4),default=1)
    parser.add_argument('--view',choices=['front','oblique','side'],default='oblique')
    parser.add_argument('--output',required=True)
    args=parser.parse_args();body=int(args.body=='feminine')
    look=f'body={body},suit=0,hair=0,headgear=0,visor=0,mask=0,lower={args.lower}'
    dl=json.loads(subprocess.check_output([str(helper()),look,args.pose,'0','0']))
    asset_dir=REPO/'build/experimental_avatar/assets'
    asset_dir.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(HERE/'assets.zip') as archive:
        for name in archive.namelist():
            if not (asset_dir/name).resolve().is_relative_to(asset_dir.resolve()):raise ValueError('Unsafe asset archive path')
        archive.extractall(asset_dir)
    renderer.pr.ASSETS=str(asset_dir);renderer.pr.W=700;renderer.pr.H=850;renderer._mdl.clear();entries=[]
    for part in dl['parts']:
        item=renderer.mdl(part['model']);v,_=deform(item[0],dl,part,body);v['c'][:]=255;mats=copy.deepcopy(item[3])
        # Match the inspected neutral-clay comparison: omit additive fade strips,
        # material textures and accessories. This is not a finished game render.
        sections=[s for s in item[2] if mats[s[2]]['shader']!=2]
        for mat in mats:
            mat['tint'][:3]=(.52,.52,.52);mat['diffuse']=None;mat['mask']=None;mat['em'][:]=0;mat['flags']=0
        entries.append(((v,item[1],sections,mats),np.array(part['m']).reshape(4,4).T,1.,np.ones(3),np.zeros(3),dict(bones=None,pattern=None,rim=(0,0,0,0),channels=np.ones((16,3)))))
    eyes={'front':[0,1.35,2.4],'oblique':[-1,1.4,2.2],'side':[-2.4,1.4,0]}
    renderer.pr.render(entries,eyes[args.view],[0,1.17,0],args.output,fov=44 if args.pose=='reach' else 32,bg=(.06,.07,.09))
    for k,ids in enumerate([(2,3,4),(5,6,7)]):
        s,e,w=np.array(dl['joints'])[list(ids)]
        print(('left','right')[k],'upper/fore mm',round(np.linalg.norm(e-s)*1000,2),round(np.linalg.norm(w-e)*1000,2),'raw-target gap mm',round(np.linalg.norm(w-dl['targets'][k])*1000,2))
    print('Experimental static clay preview. Optional lower meshes stop at thighs. No game shader or tracking-loss integration.')
if __name__=='__main__':main()
