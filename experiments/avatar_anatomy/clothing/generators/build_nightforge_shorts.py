import sys,pathlib,json,numpy as np,collections
from common import config,ANATOMY
args,SOURCE=config('Build approved nightforge_shorts models; requires NumPy.', corrected=False)
import preview_avatar as a;import build_open_assets as B
ROOT=args.output;a.pr.ASSETS=str(SOURCE)
M=[(0,(.38,.28,.14,1),(0,0,0,.30),0,0,''),(0,(.035,.042,.055,1),(0,0,0,-.55),0,0,''),(3,(.08,.65,.95,1),(0,0,0,0),0,0,''),(0,(.105,.135,.175,1),(0,0,0,.24),0,0,'')]
for sex,gender in [('m','masc'),('f','fem')]:
 J=json.loads(str(np.load(ANATOMY/'base'/('body_'+gender+'.npz'))['joints']))
 # Pattern coordinates follow body size, not a single sex-specific patch pasted on both.
 sx=.2120116/abs(J['shoulder_r'][0]);sy=.44/abs(a.mdl('av_top_0_'+sex)[0]['p'][:,1].min())
 for lower in [1,2,3,4]:
  name=('av_top_0_' if lower==0 else f'av_bottom_{min(lower,3)}_')+sex;V,I,secs,mats=a.mdl(name)
  # Barycentric clipping makes crisp seams without moving the body silhouette or changing the bind.
  planes=[]
  def plane(x,y,z,d):planes.append(np.array([x*sx,y*sy,z,d]))
  if lower==0:
   for x in [0,.006,.014,.03,.09,.14,.19,.28,.285,.289,.40,.44,.51,.66,.675]:
    plane(1,0,0,-x)
    if x:plane(1,0,0,x)
   for y in [-.035,-.055,-.06,-.08,-.10,-.16,-.20,-.26,-.28,-.38,-.395]:plane(0,1,0,-y)
   for z in [-.040,-.020,.010]:plane(0,0,1,-z)
   for sign in [-1,1]:
    for off in [-.0025,.0025]:plane(-sign*.55,1,0,.27+off)
    for off in [-.010,.010]:plane(-sign*.25,1,0,.13+off)
    for level in [.30,.35]:
     for off in [-.002,.002]:plane(-sign*.25,1,0,level+off)
    for off in [-.0025,.0025]:plane(sign,0,.8,-.60+off)
    for cy in [-.13,-.23]:
     for direction in [-1,1]:plane(sign, direction*.5,0,-.014-direction*.5*cy)
  else:
   for x in [0,.055,.095,.14]:
    plane(1,0,0,-x)
    if x:plane(1,0,0,x)
   for y in [-.45,-.46,-.49,-.515,-.58,-.70,-.76,-.812,-.818]:plane(0,1,0,-y)
   for sign in [-1,1]:
    for off in [-.0025,.0025]:plane(sign*.65,1,0,.64+off)
    for off in [-.003,.003]:plane(sign, .25,0,-.012+off)
  def color(p):
   x,y,z=p; x=abs(x*sx);y*=sy
   if lower:
    if -.49<y<-.45:return 0 if x<.055 else 1
    if lower==4 and -.818<y<-.812:return 2
    if y<-.76:return 1
    line=-.64-.65*x
    if abs(y-line)<.0025:return 2
    if lower==2 and abs(x+.25*y-.012)<.003 and -.70<y<-.515:return 0
    return 0 if x>.14 else 3
   if x>.19:
    if x>.675 or .40<x<.51:return 1
    if .285<x<.289:return 2
    if x<.28:return 0 if y>-.055 else 3
    if x>.51:
     if abs(x+.8*z-.60)<.0025:return 2
     return 0 if -.040<z<-.020 else 3
    return 3
   if y>-.035 or y<-.38:return 1
   if z<.010:
    line=-.27+.55*x
    if abs(y-line)<.0025:return 2
    if y>line:
     if x<.014:return 1
     if abs(y-(-.13+.25*x))<.010:return 0
     return 0 if x>.14 and y<-.08 else 3
    if x<.14 and any(abs(y-(-level+.25*x))<.002 for level in [.30,.35]):return 0
    return 1
   if any(x+.5*abs(y-cy)<.014 for cy in [-.13,-.23]):return 2
   if x<.03:return 1
   return (0 if x>.14 else 3) if y>-.28 else 1
  def depth(p):
   # Millimetre-scale flexible protection, continuous across painted seams.
   x,y,z=np.moveaxis(np.asarray(p),-1,0);xx=abs(x*sx);yy=y*sy
   ss=lambda t:np.clip(t,0,1)**2*(3-2*np.clip(t,0,1))
   edge=ss((yy+.44)/.04)*ss((.06-yy)/.04)*ss((.706-xx)/.035)
   chest=ss((yy+.29)/.06)*ss((-.045-yy)/.04)*ss((.19-xx)/.04)*ss((-z+.02)/.055)
   shoulder=np.exp(-((xx-.235)/.055)**2)*ss((yy+.13)/.07)
   fore=ss((xx-.51)/.025)*ss((.665-xx)/.035)*ss((-z+.025)/.06)
   return edge*(.0015+.0045*chest+.0065*shoulder+.004*fore)
  bins=collections.defaultdict(list);preserve=[];extra=[]
  for st,count,mi in secs:
   mat=mats[mi];skin=((mat['flags']>>8)&15)==4
   if lower==4 and mat['shader']==2:continue
   # Preserve the original bare thighs and additive lower fade instead of inventing legs.
   keep=lower in [1,2,3] and (skin or mat['shader']==2)
   if keep:
    slot=len(M)+len(extra);extra.append((mat['shader'],tuple(mat['tint']),tuple(mat['em']),mat['uv'],mat['flags'], 'open_soft_strip' if mat['shader']==2 else ''))
   for ids in I[st:st+count].reshape(-1,3):
    P=V['p'][ids].astype(float);polys=[np.eye(3)]
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
      if lower==0:
       p0=v['p'].astype(float);n0=v['n'].astype(float);e=1e-4
       grad=np.stack([(depth(p0+axis*e)-depth(p0-axis*e))/(2*e) for axis in np.eye(3)],axis=1)
       nn=n0-grad/(1+np.sum(n0*grad,axis=1))[:,None];nn/=np.maximum(np.linalg.norm(nn,axis=1)[:,None],1e-12)
       v['p']=p0+n0*depth(p0)[:,None];v['n']=nn
      if np.linalg.norm(np.cross(v['p'][1]-v['p'][0],v['p'][2]-v['p'][0]))>=1e-10:bins[matid].append(v)
  vv=[];ii=[];ss=[];nv=ni=0
  for mid,lists in sorted(bins.items()):
   v=np.concatenate(lists);raw=v.view(np.dtype((np.void,v.dtype.itemsize))).ravel();_,first,inv=np.unique(raw,return_index=True,return_inverse=True);v=v[first];vv.append(v);ii.append(inv.astype(np.uint32)+nv);ss.append((ni,len(inv),mid));nv+=len(v);ni+=len(inv)
  v=np.concatenate(vv);idx=np.concatenate(ii)
  if lower==4:
   cut=float(v['p'][:,1].min());caps=[]
   for side in [-1,1]:
    ids=np.where((abs(v['p'][:,1]-cut)<2e-5)&(v['p'][:,0]*side>0))[0];_,first=np.unique(np.round(v['p'][ids],6),axis=0,return_index=True);ids=ids[first];centre=v['p'][ids].mean(0);ang=np.arctan2(v['p'][ids,2]-centre[2],v['p'][ids,0]-centre[0]);ids=ids[np.argsort(ang)]
    ring=v[ids].copy();ring['n']=[0,-1,0];ring['c']=255;c=ring[:1].copy();c['p']=centre;c['n']=[0,-1,0];base=len(v);v=np.concatenate([v,ring,c]);ci=base+len(ring)
    for j in range(len(ring)):
     tri=[ci,base+j,base+(j+1)%len(ring)];p=v['p'][tri]
     if np.cross(p[1]-p[0],p[2]-p[0])[1]>0:tri.reverse()
     caps.extend(tri)
   ss.append((len(idx),len(caps),1));idx=np.r_[idx,np.array(caps,dtype=np.uint32)]
  out=('av_nightforge_top_' if lower==0 else f'av_nightforge_bottom_{lower}_')+sex
  B.write_model(str(ROOT/'assets'/(out+'.mdl')),B.mdl_bytes(3,v,idx,ss,M+extra));print(out,len(v),len(idx)//3,'weights valid',np.all(v['bw'].sum(1)==255),flush=True)
