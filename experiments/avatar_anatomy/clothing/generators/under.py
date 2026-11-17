import numpy as np
def under(V,I,S,M,cut):
 out=[];ids=[];secs=[];nv=ni=0
 for skin in [False,True]:
  chunks=[]
  for st,count,mi in S:
   if M[mi]['shader']==2:continue
   for tri in I[st:st+count].reshape(-1,3):
    P=V['p'][tri].astype(float);d=P[:,1]-cut;poly=[]
    for k in range(3):
     j=(k+1)%3;sg=-1 if skin else 1
     if sg*d[k]>=0:poly.append(np.eye(3)[k])
     if (sg*d[k]>=0)!=(sg*d[j]>=0):poly.append(np.eye(3)[k]+(np.eye(3)[j]-np.eye(3)[k])*d[k]/(d[k]-d[j]))
    for k in range(1,len(poly)-1):
     bary=np.array(poly)[[0,k,k+1]];v=np.zeros(3,dtype=V.dtype)
     for attr in ['p','n','u0','u1','c']:v[attr]=bary@V[attr][tri].astype(float)
     v['n']/=np.maximum(np.linalg.norm(v['n'],axis=1)[:,None],1e-12);v['c']=255
     # Native lower models attach rigidly to torso, verified at input.
     assert np.all(V['bi'][tri][V['bw'][tri]>0]==0)
     v['bi'][:,0]=0;v['bw'][:,0]=255
     if np.linalg.norm(np.cross(v['p'][1]-v['p'][0],v['p'][2]-v['p'][0]))>1e-10:chunks.append(v)
  v=np.concatenate(chunks);raw=v.view(np.dtype((np.void,v.dtype.itemsize))).ravel();_,first,inv=np.unique(raw,return_index=True,return_inverse=True);v=v[first];out.append(v);ids.append(inv.astype(np.uint32)+nv);secs.append((ni,len(inv),7 if skin else 1));nv+=len(v);ni+=len(inv)
 return np.concatenate(out),np.concatenate(ids),secs
