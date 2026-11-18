"""Reference-measured depth-only Quaternius-derived proportions; skeleton/UV/skin stay unchanged."""
import numpy as np,os,json,shutil
from pathlib import Path
HERE=Path(__file__).resolve().parent
SRC=str(HERE/'base_unshaped');OUT=str(HERE/'base')
os.makedirs(OUT,exist_ok=True)
def ss(t):t=np.clip(t,0,1);return t*t*(3-2*t)
for sex in ['masc','fem']:
 src=np.load(SRC+'/body_'+sex+'.npz');data={k:src[k] for k in src.files};P=data['pos'].astype(float);N=data['nrm'].astype(float);J=json.loads(str(data['joints']))
 def deform(Q):
  R=Q.copy();x,y,z=Q.T
  hip=float((J['hip_l'][1]+J['hip_r'][1])*.5)
  shoulder=float((J['shoulder_l'][1]+J['shoulder_r'][1])*.5)
  L=shoulder-hip;f=(y-hip)/L
  centre=float((J['hip_l'][2]+J['hip_r'][2])*.5)*(1-np.clip(f,0,1))
  # Match measured waist depth direction while keeping EVERY x coordinate.
  # Balance chest/waist and hip/waist ratios, not force one at the other's expense.
  fullness=.007 if sex=='masc' else .008
  band=np.exp(-((f-.30)/.18)**4)
  R[:,2]+=fullness*np.tanh((z-centre)/.03)*band
  if sex=='masc':
   # Localized depth-only reduction of superhero pectoral projection.
   mask=np.exp(-((abs(x)-.105)/.095)**4-((y+.175)/.11)**4)*ss((-z-.045)/.035)
   R[:,2]+=.012*mask
  return R
 Q=deform(P);h=1e-5;F=np.zeros((len(P),3,3))
 for k in range(3):
  delta=np.zeros(3);delta[k]=h;F[:,:,k]=(deform(P+delta)-deform(P-delta))/(2*h)
 assert np.all(np.linalg.det(F)>.5)
 NN=np.linalg.solve(np.transpose(F,(0,2,1)),N[:,:,None])[:,:,0];NN/=np.maximum(np.linalg.norm(NN,axis=1,keepdims=True),1e-12)
 T=data['tris'];a=P[T];b=Q[T];fa=np.cross(a[:,1]-a[:,0],a[:,2]-a[:,0]);fb=np.cross(b[:,1]-b[:,0],b[:,2]-b[:,0]);flips=((fa*fb).sum(1)<0).sum();assert flips==0
 data['pos']=Q.astype('f4');data['nrm']=NN.astype('f4');np.savez_compressed(OUT+'/body_'+sex+'.npz',**data)
 for part in ['eyes','brows']:shutil.copyfile(SRC+'/'+part+'_'+sex+'.npz',OUT+'/'+part+'_'+sex+'.npz')
 print(sex,'max displacement mm',np.linalg.norm(Q-P,axis=1).max()*1000,'minimum Jacobian determinant',np.linalg.det(F).min(),'flipped normals',flips)
