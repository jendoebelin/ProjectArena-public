"""Quaternius-derived experimental alternative, game9 weights and existing garment system."""
import os,sys,numpy as np,types,tempfile,zipfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
from PIL import Image
sys.path.insert(0,str(REPO/'tools/blender'))
source=open(str(REPO/'tools/build_body_parts.py')).read()
for old,new in [('np.zeros((F, 7))','np.zeros((F, 9))'),('np.zeros((len(self.P), 7))','np.zeros((len(self.P), 9))'),('np.zeros((0, 7))','np.zeros((0, 9))'),('reshape(-1, 3, 7)','reshape(-1, 3, 9)'),('np.zeros((len(P), 7))','np.zeros((len(P), 9))'),('hw = np.zeros(7)','hw = np.zeros(9)')]:source=source.replace(old,new)
b=types.ModuleType('qparts');b.__file__=os.path.abspath(str(REPO/'tools/build_body_parts.py'));exec(compile(source,b.__file__,'exec'),b.__dict__)
ROOT=str(HERE);b.SRC=ROOT+'/base';b.OUT=ROOT+'/assets'
b.Body.round_bust=lambda self:None;b.Body.lift_chest=lambda self:None;b.Body.smooth_bust=lambda self,**kw:None
b.clavicle_weights=lambda P,W:W # retain authored clavicle weights, don't fabricate them
orig_write=b.Mesh.write
def audited_write(self,name,mats):
 worst=0.;count=0
 for soups in self.groups.values():
  W=b.Soup.cat(soups).Wd
  lost=W.sum(1)-np.sort(W,axis=1)[:,-4:].sum(1)
  worst=max(worst,float(lost.max(initial=0)));count+=int(np.count_nonzero(lost>1e-6))
 print('WEIGHT_REDUCTION',name,'max discarded mass',worst,'corners >1e-6',count)
 return orig_write(self,name,mats)
b.Mesh.write=audited_write
b.TOPS={0:b.TOPS[0]};base_bottoms=dict(b.BOTTOMS)
def extras(mesh,body,head_tris):
 for src,mat in [('eyes','eye_white'),('brows','brow')]:
  d=np.load(b.SRC+'/'+src+'_'+('masc' if body.key=='m' else 'fem')+'.npz');ids=d['tris'].reshape(-1);W=np.zeros((len(d['pos']),9))
  for k in range(4):np.add.at(W,(np.arange(len(W)),d['bone'][:,k]),d['weight'][:,k])
  mesh.add(mat,b.Soup(d['pos'][ids],d['nrm'][ids],d['uv'][ids],d['ao'][ids],W[ids]))
b.add_head_extras=extras
origm=b.materials
def mats():
 m=origm();m['eye_white']=dict(m['eye_white'],diffuse='quaternius_eye_brown');return m
b.materials=mats
b.write_tex('quaternius_eye_brown',np.array(Image.open(str(HERE/'T_Eye_Brown.png')).convert('RGBA')))
bodies=[]
for key in ('m','f'):
 body=b.Body(key);bodies.append(body)
 raster=b.UVRaster(body);grid='grid_q'+key;shorts='shorts_q'+key
 b.write_tex('open_design_'+grid,raster.image(b.design('grid',body,raster.X,raster.dom,raster.at,raster.phi)))

 top=list(b.TOPS[0]);top[4]=grid;b.TOPS[0]=tuple(top)
 for n,original in base_bottoms.items():
  bottom=list(original);name=original[4]+'_q'+key
  b.write_tex('open_design_'+name,raster.image(b.design(original[4],body,raster.X,raster.dom,raster.at,raster.phi)))
  bottom[4]=name;b.BOTTOMS[n]=tuple(bottom)
 b.build_body(body,raster,None)
with tempfile.TemporaryDirectory() as tmp:
 b.ROOT=tmp;os.makedirs(tmp+'/app/src',exist_ok=True);b.write_bind(bodies)
 text=open(tmp+'/app/src/avatar_bodies.inc').read().replace('from the MPFB2 bodies','from the CC0 Quaternius source bodies (experimental alternative)')
 open(ROOT+'/avatar_bodies.inc','w').write(text)

with zipfile.ZipFile(HERE/'assets.zip','w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
 for asset in sorted((HERE/'assets').rglob('*')):
  if asset.is_file():archive.write(asset,str(asset.relative_to(HERE/'assets')))
