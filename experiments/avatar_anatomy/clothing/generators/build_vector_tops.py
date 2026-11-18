import sys,pathlib,json,numpy as np,collections
from common import config,ANATOMY
args,SOURCE=config('Build approved vector_tops models; requires NumPy.', corrected=True)
import preview_avatar as a;import build_open_assets as B
ROOT=args.output;a.pr.ASSETS=str(SOURCE)
M=[(0,(.70,.76,.79,1),(0,0,0,.12),0,0,''),(0,(.035,.045,.065,1),(0,0,0,-.55),0,0,''),(3,(.08,.70,1.,1),(0,0,0,0),0,0,''),(3,(.44,.14,.86,1),(0,0,0,0),0,0,'')]
for sex,gender in [('m','masc'),('f','fem')]:
 J=json.loads(str(np.load(ANATOMY/'base'/('body_'+gender+'.npz'))['joints']))
 # Pattern coordinates follow body size, not a single sex-specific patch pasted on both.
 sx=.2120116/abs(J['shoulder_r'][0]);sy=.44/abs(a.mdl('av_top_0_'+sex)[0]['p'][:,1].min())
 for lower in [0]:
  name=('av_top_0_' if lower==0 else f'av_bottom_{lower}_')+sex;V,I,secs,mats=a.mdl(name)
  # Barycentric clipping makes crisp seams without moving the body silhouette or changing the bind.
  planes=[]
  def plane(x,y,z,d):planes.append(np.array([x*sx,y*sy,z,d]))
  if lower==0:
   for x in [0,.012,.03,.125,.19,.28,.40,.44,.507,.513,.617,.623,.66,.678]:
    plane(1,0,0,-x)
    if x:plane(1,0,0,x)
   for y in [-.035,-.085,-.30,-.367,-.373,-.393,-.399]:plane(0,1,0,-y)
   plane(0,0,1,0)
   for sign in [-1,1]:
    for off in [-.003,.003]:plane(-sign*.5,1,0,.22+off)
  else:
   for x in [0,.055,.095,.14]:
    plane(1,0,0,-x)
    if x:plane(1,0,0,x)
   for y in [-.45,-.465,-.472,-.52,-.58,-.64,-.70,-.76]:plane(0,1,0,-y)
   for sign in [-1,1]:
    for off in [-.003,.003]:plane(sign*.5,1,0,.64+off)
  def color(p):
   x,y,z=p; x=abs(x*sx);y*=sy
   if lower:
    if -.472<y<-.465:return 2
    if y>-.465:return 1
    line=-.64-.5*x
    if abs(y-line)<.003:return 2 if lower!=3 else 3
    if lower==2 and .055<x<.095 and -.58<y<-.52:return 3
    if lower==3:return 0 if x>.14 else 1
    return 0 if x>.095 else 1
   if x>.19:
    if .507<x<.513:return 3
    if .617<x<.623:return 2
    if x>.678 or .40<x<.507:return 1
    if x<.28:return 0 if y>-.085 else 1
    return 0 if x>.513 or z<0 else 1
   if -.399<y<-.393:return 3
   if -.373<y<-.367:return 2
   if y>-.035:return 1
   if z<0:
    line=-.22+.5*x
    if abs(y-line)<.003:return 2
    if y>line:return 0
    return 0 if x>.125 and y>-.367 else 1
   if x<.012 and -.30<y<-.085:return 2
   if x<.03:return 1
   return 0 if y>-.30 else 1
  bins=collections.defaultdict(list);preserve=[];extra=[]
  for st,count,mi in secs:
   mat=mats[mi];skin=((mat['flags']>>8)&15)==4
   # Preserve the original bare thighs and additive lower fade instead of inventing legs.
   basekeep=lower and (skin or mat['shader']==2)
   if basekeep or (lower==0 and skin):
    slot=len(M)+len(extra);extra.append((mat['shader'],tuple(mat['tint']),tuple(mat['em']),mat['uv'],mat['flags'], 'open_soft_strip' if mat['shader']==2 else ''))
   for ids in I[st:st+count].reshape(-1,3):
    P=V['p'][ids].astype(float);polys=[np.eye(3)]
    keep=basekeep or (lower==0 and skin and P[:,1].min()>.02 and abs(P[:,0]).max()<.12)
    if not keep:
     for pl in planes:
      vals=np.c_[P,np.ones(3)]@pl;out=[]
      for poly in polys:
       d=poly@vals
       if np.min(d)>=-1e-10 or np.max(d)<=1e-10:out.append(poly);continue
       for sg in [-1,1]:
        Q=[]
        for i in range(len(poly)):
         j=(i+1)%len(poly);di,dj=sg*d[i],sg*d[j]
         if di>=0:Q.append(poly[i])
         if (di>=0)!=(dj>=0):Q.append(poly[i]+(poly[j]-poly[i])*(di/(di-dj)))
        if len(Q)>=3:out.append(np.array(Q))
      polys=out
    for poly in polys:
     matid=slot if keep else color(poly.mean(0)@P)
     for j in range(1,len(poly)-1):
      bary=poly[[0,j,j+1]];q=bary@P
      if np.linalg.norm(np.cross(q[1]-q[0],q[2]-q[0]))<1e-12:continue
      v=np.zeros(3,dtype=V.dtype)
      for attr in ['p','n','u0','u1','c']:v[attr]=bary@V[attr][ids].astype(float)
      v['n']/=np.maximum(np.linalg.norm(v['n'],axis=1)[:,None],1e-12)
      dense=np.zeros((3,9))
      for k in range(4):np.add.at(dense,(np.arange(3),V['bi'][ids,k]),V['bw'][ids,k]/255.)
      w=bary@dense;order=np.argsort(-w,axis=1)[:,:4];ww=np.take_along_axis(w,order,1);ww/=ww.sum(1)[:,None];quant=np.floor(ww*255).astype(int);quant[:,0]+=255-quant.sum(1);v['bi']=order;v['bw']=quant
      if not keep:v['c']=255
      if np.linalg.norm(np.cross(v['p'][1]-v['p'][0],v['p'][2]-v['p'][0]))>=1e-10:bins[matid].append(v)
  vv=[];ii=[];ss=[];nv=ni=0
  for mid,lists in sorted(bins.items()):
   v=np.concatenate(lists);raw=v.view(np.dtype((np.void,v.dtype.itemsize))).ravel();_,first,inv=np.unique(raw,return_index=True,return_inverse=True);v=v[first];vv.append(v);ii.append(inv.astype(np.uint32)+nv);ss.append((ni,len(inv),mid));nv+=len(v);ni+=len(inv)
  v=np.concatenate(vv);idx=np.concatenate(ii);out=('av_vector_top_' if lower==0 else f'av_vector_bottom_{lower}_')+sex
  B.write_model(str(ROOT/'assets'/(out+'.mdl')),B.mdl_bytes(3,v,idx,ss,M+extra));print(out,len(v),len(idx)//3,'weights valid',np.all(v['bw'].sum(1)==255),flush=True)
