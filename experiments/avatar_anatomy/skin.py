"""Offline procedural forearm twist distribution; requires a matching game shader change.
No source vertices, indices, weights, tracked targets or bone matrices are edited.
"""
import numpy as np,json
from pathlib import Path

def unit(v):return v/np.maximum(np.linalg.norm(v,axis=-1,keepdims=True),1e-12)
def rotate(v,axis,angle):
 angle=np.asarray(angle);c=np.cos(angle)[...,None];s=np.sin(angle)[...,None]
 return v*c+np.cross(axis,v)*s+axis*np.sum(v*axis,axis=-1,keepdims=True)*(1-c)
def transport(v,a,b):
 """Shortest-arc transport only where its bend plane is well defined."""
 a,b=unit(a),unit(b);axis=np.cross(a,b);c=float(np.clip(np.dot(a,b),-1,1))
 if c<=-.995:raise ValueError('near-antiparallel transport is anatomically ambiguous')
 return v+np.cross(axis,v)+np.cross(axis,np.cross(axis,v))/(1+c)

def bind_reference(axis):
 # App -Z is the documented authored body-front direction. Select its
 # projection once from the bind, not a changing posed/world direction.
 axis=unit(axis)
 for name,ref in [('body_front',np.array([0.,0.,-1.])),('bind_up_fallback',np.array([0.,1.,0.])),('bind_right_fallback',np.array([1.,0.,0.]))]:
  projected=ref-axis*np.dot(ref,axis)
  if np.linalg.norm(projected)>.1:return unit(projected),name
 raise ValueError('invalid bind axis')

def fore_info(dl,body,skin,diagnostics=None,joints_override=None):
 J=joints_override or json.loads(str(np.load(Path(__file__).parent/'base'/('body_'+('masc' if body==0 else 'fem')+'.npz'))['joints']))
 p=np.array(dl['joints']);info={};diagnostics={} if diagnostics is None else diagnostics
 for side,(ids,suffix) in enumerate([((2,3,4),'l'),((5,6,7),'r')]):
  bone=5+side;S,E,W=p[list(ids)];be=np.array(J['elbow_'+suffix]);bw=np.array(J['wrist_'+suffix]);bs=np.array(J['shoulder_'+suffix]);ua0=be-bs;fa0=bw-be
  if min(np.linalg.norm(E-S),np.linalg.norm(W-E),np.linalg.norm(ua0),np.linalg.norm(fa0))<1e-5:
   diagnostics[bone]={'status':'disabled_zero_length','confidence':0.};continue
  ua,fa=unit(E-S),unit(W-E);cosine=float(np.clip(np.dot(ua,fa),-1,1))
  if cosine<=-.995:
   diagnostics[bone]={'status':'disabled_antiparallel','confidence':0.};continue
  confidence=np.clip((cosine+.995)/.045,0,1);confidence=float(confidence*confidence*(3-2*confidence))
  try:
   ur,reference=bind_reference(ua0);fr=transport(ur,ua0,fa0)
  except ValueError:
   diagnostics[bone]={'status':'disabled_ambiguous_bind','confidence':0.};continue
  u=skin[3+side,:3,:3]@ur;f=skin[5+side,:3,:3]@fr;u-=ua*np.dot(u,ua);f-=fa*np.dot(f,fa)
  if min(np.linalg.norm(u),np.linalg.norm(f))<1e-5:
   diagnostics[bone]={'status':'disabled_invalid_frame','confidence':0.};continue
  u=unit(transport(unit(u),ua,fa));f=unit(f)
  roll=float(np.arctan2(np.dot(fa,np.cross(u,f)),np.dot(u,f)))
  diagnostics[bone]={'status':'active' if confidence==1 else 'fading_ambiguous_bend','confidence':confidence,'bind_reference':reference,'raw_roll':roll}
  info[bone]=(E,W,fa,be,bw,roll)
 return info

def deform(v,dl,part,body,mode='twist',roll_overrides=None):
 P=v['p'].astype(float);N=v['n'].astype(float);B=np.array(part['skin']).reshape(-1,4,4).transpose(0,2,1);status={};I=fore_info(dl,body,B,status)
 if roll_overrides:
  I={bone:(*values[:-1],roll_overrides.get(bone,values[-1])) for bone,values in I.items()}
 I={bone:(*values[:-1],values[-1]*status[bone]['confidence']) for bone,values in I.items()}
 Q=np.zeros_like(P);norm=np.zeros_like(N);records=[]
 for k in range(4):
  ids=v['bi'][:,k];weights=v['bw'][:,k].astype(float)/255
  for bone in np.unique(ids):
   sel=(ids==bone)&(weights>0);R=B[bone,:3,:3];T=P[sel]@R.T+B[bone,:3,3];n=N[sel]@np.linalg.inv(R)
   if bone in I and mode!='baseline':
    E,W,axis,be,bw,roll=I[bone];bind_delta=bw-be;length2=bind_delta@bind_delta
    sraw=(P[sel]-be)@bind_delta/length2;s=np.clip(sraw/.96,0,1);f=s*s*(3-2*s)
    if mode=='zero-roll':f[:]=0
    theta=-roll*(1-f);before=T.copy();T=E+rotate(T-E,axis,theta);n=rotate(n,axis,theta)
    # Differential correction for the spatially varying twist. This term vanishes
    # at both endpoint planes, preserving the original distal wrist normals too.
    if mode=='twist':
     grad=(roll*6*s*(1-s))[:,None]*bind_delta[None,:]/(length2*.96)
     ag=rotate(grad@np.linalg.inv(R),axis,theta);n-=ag*np.sum(np.cross(axis,T-E)*n,axis=1,keepdims=True)
    radius0=np.linalg.norm((before-E)-((before-E)@axis)[:,None]*axis,axis=1);radius1=np.linalg.norm((T-E)-((T-E)@axis)[:,None]*axis,axis=1)
    assert np.max(abs(radius0-radius1),initial=0)<1e-8
    if mode=='twist' and np.any(sraw>=1):assert np.max(np.abs(T[sraw>=1]-before[sraw>=1]))<1e-8
    records.append((int(bone),float(roll),int(sel.sum()),int((sraw>=1).sum())))
   Q[sel]+=T*weights[sel,None];norm[sel]+=n*weights[sel,None]
 assert np.isfinite(Q).all() and np.isfinite(norm).all()
 out=v.copy();out['p']=Q;out['n']=norm
 return out,{'status':status,'influences':records}

def unwrap_angle(previous,current):
 """Caller-owned per-avatar state is required across the +/-pi branch."""
 return previous+np.arctan2(np.sin(current-previous),np.cos(current-previous))


class TwistState:
 """One instance per avatar; loss/reacquisition resets phase and fades correction.
 Correction is explicitly capped at180 degrees: repeated full turns cannot
 wind up skin indefinitely, and are flagged as outside this anatomical model.
 """
 def __init__(self):self.arms={}
 def update(self,diagnostics,tracked,dt):
  result={};events={}
  for side,bone in enumerate((5,6)):
   info=diagnostics.get(bone,{})
   if not tracked[side] or info.get('confidence',0)<=0 or not np.isfinite(dt) or dt<=0 or dt>.1:
    self.arms.pop(bone,None);result[bone]=0.;events[bone]='reset_or_untracked';continue
   raw=info['raw_roll'];state=self.arms.get(bone)
   if state is None:
    state={'angle':raw,'gain':0.};self.arms[bone]=state;events[bone]='reacquired_fade'
   else:
    state['angle']=unwrap_angle(state['angle'],raw);state['gain']=1-(1-state['gain'])*np.exp(-dt/.12);events[bone]='tracking'
   bounded=float(np.clip(state['angle'],-np.pi,np.pi))
   if bounded!=state['angle']:events[bone]='excess_rotation_capped_not_anatomically_valid'
   result[bone]=bounded*state['gain']
  return result,events
