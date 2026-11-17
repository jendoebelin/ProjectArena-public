import sys,pathlib,json,numpy as np
from common import config
args,SOURCE=config('Build final natural '+'tennis'+' models; requires NumPy and SciPy.', variants=True)
import preview_avatar as a;import build_open_assets as B
R=args.output
from under import under
from fit import WaistFit
variant=args.variant
M=[(0,(.70,.76,.79,1),(0,0,0,-.15),0,0,''),(0,(.025,.036,.055,1),(0,0,0,-.6),0,0,''),(3,(.08,.65,.95,1),(0,0,0,0),0,0,''),(3,(.44,.14,.86,1),(0,0,0,0),0,0,''),(0,(.065,.095,.12,1),(0,0,0,-.75),0,0,''),(0,(.115,.17,.18,1),(0,0,0,-.65),0,0,''),(0,(.34,.25,.13,1),(0,0,0,.35),0,0,'')]
ss=lambda x: np.clip(x,0,1)**2*(3-2*np.clip(x,0,1))
for sex in 'mf':
 a.pr.ASSETS=str(SOURCE);source,si,ssource,sm=a.pr.load_mdl('av_bottom_3_'+sex);nonfade=np.concatenate([si[st:st+n] for st,n,mi in ssource if sm[mi]['shader']!=2]);P=source['p'][nonfade];waist=float(P[:,1].max())+.004;bottom=float(P[:,1].min());base=source
 profile=[]
 for y in np.linspace(bottom,waist,28):
  pp=P[abs(P[:,1]-y)<.025]
  if len(pp)<4:pp=P[np.argsort(abs(P[:,1]-y))[:30]]
  cz=(pp[:,2].min()+pp[:,2].max())/2;rx=max(abs(pp[:,0]))+.003;rz=(pp[:,2].max()-pp[:,2].min())/2+.003
  ratio=max(1,float(np.sqrt((pp[:,0]/rx)**2+((pp[:,2]-cz)/rz)**2).max()));profile.append([float(y),float(rx*ratio+.004),float(rz*ratio+.004),float(cz)])
 profile=np.array(profile)
 top=a.pr.load_mdl('av_top_0_'+sex);fit=WaistFit([top,(source,si,ssource,sm)],float(profile[-1,3]))
 radialY=np.linspace(bottom+.003,waist,85);radialAngles=np.linspace(0,2*np.pi,144,endpoint=False);radial=np.array([fit.convex_radii(y,radialAngles) for y in radialY]);drape=np.maximum.accumulate(radial[::-1],axis=0)[::-1]
 for kind,lower in [('tennis',5)]:
  hem=waist-.255 if kind=='tennis' else max(waist-.315,bottom+.100);base,bi,bs=under(source,si,ssource,sm,hem+.025);N=120 if kind=='tennis' else 144
  ts=np.unique(np.r_[np.linspace(0,1,17),np.linspace(0,.10,31),.025,.045,.075,.095,.96,.975])
  if kind=='kilt':ts=np.unique(np.r_[ts,.246,.254,.496,.504,.746,.754])
  vv=[];ii=[];secs=[];weights=[];types=[];fractions=[];angles=[];nv=ni=0
  def add(p,tri,mat,frac,theta,clothkind,weight=None,smooth=True):
   global nv,ni
   p=np.array(p,float);tri=np.array(tri,int).reshape(-1,3);norm=np.zeros_like(p);fn=np.cross(p[tri[:,1]]-p[tri[:,0]],p[tri[:,2]]-p[tri[:,0]])
   for j in range(3):np.add.at(norm,tri[:,j],fn)
   norm/=np.maximum(np.linalg.norm(norm,axis=1)[:,None],1e-12)
   v=np.zeros(len(p),dtype=base.dtype);v['p']=p;v['n']=norm;v['c']=255;v['bi'][:,0]=0;v['bw'][:,0]=255
   vv.append(v);ii.append(tri.ravel().astype(np.uint32)+nv);secs.append((ni,tri.size,mat));nv+=len(p);ni+=tri.size
   f=np.broadcast_to(frac,(len(p),));th=np.broadcast_to(theta,(len(p),));w=ss((f-.40)/.60) if weight is None else np.broadcast_to(weight,(len(p),));weights.extend(w.tolist());fractions.extend(f.tolist());angles.extend(th.tolist());types.extend([clothkind]*len(p))
  # Coverage stays fixed and fully opaque under all skirt motion.
  v=base.copy();v['c']=255;vv.append(v);ii.append(bi.copy());secs.extend(bs);nv=len(v);ni=len(bi);weights.extend([0]*len(v));fractions.extend([0]*len(v));angles.extend([0]*len(v));types.extend([0]*len(v))
  def point(th,t,inner=False,apron=False):
   y=waist+(hem-waist)*t
   rx=np.interp(y,profile[:,0],profile[:,1]);rz=np.interp(y,profile[:,0],profile[:,2]);cz=np.interp(y,profile[:,0],profile[:,3])
   # Outward flare independent of body width; actual coverage envelope is the lower bound.
   if kind=='tennis':
    rx=max(rx,.170+.068*ss(t));rz=max(rz,.125+.052*ss(t));fold=(.006+.010*t)*ss(t/.24)*(abs((th/(2*np.pi)*20)%1-.5)*2)
   else:
    rx=max(rx,.178+.062*ss(t));rz=max(rz,.132+.047*ss(t));front=abs((th+np.pi)%(2*np.pi)-np.pi)<.78
    fold=0 if front or apron else .016*ss(t/.22)*((th/(2*np.pi)*18)%1)
   extra=(.004 if apron else 0)+(.0005 if inner else .003)
   old=np.array([((rx+fold+extra)*np.sin(th)),y,cz-(rz+fold+extra)*np.cos(th)])
   if t<.40:
    clearance=.0015 if inner else .004
    radius=fit.envelope(y,th)+clearance+(.004 if apron else 0)
    close=np.array([radius*np.sin(th),y,fit.center[1]-radius*np.cos(th)])
    blend=ss((t-.095)/(.40-.095));old=close*(1-blend)+old*blend
   if kind=='tennis' and t>.095:
    k=int((th%(2*np.pi))/(2*np.pi)*144);u=(th%(2*np.pi))/(2*np.pi)*144-k
    radiusA=np.interp(y,radialY,drape[:,k]);radiusB=np.interp(y,radialY,drape[:,(k+1)%144]);radius=radiusA*(1-u)+radiusB*u
    late=ss((t-.55)/.45);fold=.005*ss((t-.12)/.65)*(abs((th/(2*np.pi)*20)%1-.5)*2)
    radius+=(.002 if inner else .0045)+.012*late+fold
    natural=np.array([radius*np.sin(th),y,fit.center[1]-radius*np.cos(th)])
    blend=ss((t-.095)/.18);old=old*(1-blend)+natural*blend
   return old.tolist()
  # Per-cell material groups share the exact same surface coordinates and motion metadata.
  chunks={}
  for inside in [False,True]:
   for j in range(len(ts)-1):
    ta,tb=ts[j:j+2];tm=(ta+tb)/2
    for k in range(N):
     aa=k*2*np.pi/N;ab=(k+1)*2*np.pi/N;am=(aa+ab)/2
     if inside:mat=1
     elif tm<.075:mat=1 if tm<.045 else (2 if kind=='tennis' else 6)
     elif kind=='tennis':mat=2 if .96<tm<.975 else (1 if tm>.975 else 0)
     else:
      stripe=k%18;mat=6 if any(abs(tm-c)<.0041 for c in [.25,.5,.75]) else (5 if stripe in [0,1,2] else 4)
     p=[point(aa,ta,inside),point(ab,ta,inside),point(aa,tb,inside),point(ab,tb,inside)];tri=[[0,1,2],[1,3,2]]
     if inside:tri=[list(reversed(f)) for f in tri]
     # Tile duplicated positions are intentional; viewer motion must displace identical coordinates identically.
     rec=chunks.setdefault(mat,{'p':[],'f':[],'t':[],'a':[]});off=len(rec['p']);rec['p']+=p;rec['f'] += [[off+i for i in f] for f in tri];rec['t'] += [ta,ta,tb,tb];rec['a'] += [aa,ab,aa,ab]
  for mat,rec in chunks.items():add(rec['p'],rec['f'],mat,rec['t'],rec['a'],1 if kind=='tennis' else 2)
  # Close top and bottom thickness rings.
  for t in [0,1]:
   p=[];f=[];ang=[]
   for k in range(N):
    th=k*2*np.pi/N;p.extend([point(th,t),point(th,t,True)]);ang.extend([th,th])
   for k in range(N):
    n=(k+1)%N;ff=[[2*k,2*n,2*k+1],[2*n,2*n+1,2*k+1]]
    if t==0:ff=[list(reversed(v)) for v in ff]
    f+=ff
   add(p,f,1,t,ang,1 if kind=='tennis' else 2)
  if kind=='kilt':
   # Smooth overlapping front apron, independent of the side/back knife pleats.
   anglesA=np.linspace(-.80,.80,25);tt=np.unique(np.r_[np.linspace(.02,1,20),.246,.254,.496,.504,.746,.754]);chunks={}
   for j in range(len(tt)-1):
    for k in range(len(anglesA)-1):
     ta,tb=tt[j:j+2];aa,ab=anglesA[k:k+2];tm=(ta+tb)/2;mat=6 if any(abs(tm-c)<.0041 for c in [.25,.5,.75]) else (5 if k in [5,6,17,18] else 4)
     p=[point(aa,ta,False,True),point(ab,ta,False,True),point(aa,tb,False,True),point(ab,tb,False,True)];rec=chunks.setdefault(mat,{'p':[],'f':[],'t':[],'a':[]});off=len(rec['p']);rec['p']+=p;rec['f'] += [[off,off+1,off+2],[off+1,off+3,off+2]];rec['t'] += [ta,ta,tb,tb];rec['a'] += [aa,ab,aa,ab]
   for mat,rec in chunks.items():add(rec['p'],rec['f'],mat,rec['t'],rec['a'],3,np.asarray(ss((np.array(rec['t'])-.4)/.6))*.65)
   # Two small bronze buckle frames at the apron fastening side, on the pinned upper band.
   for t in [.14,.23]:
    th=.88;c=np.array(point(th,t))+np.array([np.sin(th),0,-np.cos(th)])*.006;u=np.array([np.cos(th),0,np.sin(th)]);up=np.array([0.,1.,0.]);normal=np.array([np.sin(th),0,-np.cos(th)])
    for dx,dy,w,h in [(0,.007,.026,.003),(0,-.007,.026,.003),(-.013,0,.003,.014),(.013,0,.003,.014)]:
     cc=c+dx*u+dy*up;p=[cc+sx*w/2*u+sy*h/2*up+sz*.002*normal for sz in [-1,1] for sy in [-1,1] for sx in [-1,1]];f=[[0,1,2],[1,3,2],[4,6,5],[5,6,7],[0,4,1],[1,4,5],[2,3,6],[3,7,6],[0,2,4],[2,6,4],[1,5,3],[3,5,7]]
     add(p,f,6,t,th,0,0)
  V=np.concatenate(vv);I=np.concatenate(ii)
  # Smooth the fabric across tessellation/material seams while retaining its real pleated geometry.
  PP=V['p'].astype(float);_,first,inv=np.unique(np.round(PP,6),axis=0,return_index=True,return_inverse=True);Q=PP[first];T=inv[I].reshape(-1,3);NN=np.zeros_like(Q);FN=np.cross(Q[T[:,1]]-Q[T[:,0]],Q[T[:,2]]-Q[T[:,0]])
  for j in range(3):np.add.at(NN,T[:,j],FN)
  NN/=np.maximum(np.linalg.norm(NN,axis=1)[:,None],1e-12);cloth=np.array(types)>0;V['n'][cloth]=NN[inv][cloth]
  name=f'av_{kind}_lower_{sex}'+('_original' if variant=='original' else '');B.write_model(str(R/'assets'/(name+'.mdl')),B.mdl_bytes(3,V,I,secs,M+[(0,(1,1,1,1),(0,0,0,.05),0,1024,'')]))
  meta={'type':kind,'waistY':waist,'hemY':hem,'centerXZ':[0,float(profile[-1,3])],'collisionProfile':[{'y':float(y),'rx':float(rx),'rz':float(rz),'centerZ':float(cz)} for y,rx,rz,cz in profile],'clothWeight':weights,'weightsAreFinal':True,'centerZ':float(profile[-1,3]),'clothT':fractions,'clothKind':types,'clothAngle':angles,'maxDisplacement':.055 if kind=='tennis' else .045}
  meta['radialCollisionProfile']=[{'y':float(y),'radii':r.tolist()} for y,r in zip(radialY,radial)];meta['radialAngleConvention']='atan2(x-cx,-(z-cz))';meta['radialCenterXZ']=[0,float(fit.center[1])]
  (R/'assets'/(name+'.motion.json')).write_text(json.dumps(meta,separators=(',',':')));print(name,len(V),len(I)//3,'waist/hem',waist,hem,'cloth',sum(np.array(weights)>0),flush=True)
