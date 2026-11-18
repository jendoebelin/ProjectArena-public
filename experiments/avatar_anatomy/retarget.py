"""Experimental CC0 Quaternius-to-game9 retarget, preserving source surface/UVs."""
import bpy,os,json,numpy as np,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
args=sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else []
if len(args)!=1:raise SystemExit("Pass the Quaternius Godot - UE folder after --")
PACK=Path(args[0]).resolve()
OUT=str(HERE/'base_unshaped')
os.makedirs(OUT,exist_ok=True)
def app(P):return np.asarray(P)[...,[0,2,1]]*[-1,1,1]
def mapped(name):
 if name=='neck_01':return 1
 if name=='Head':return 2
 for side,k in [('_l',3),('_r',4)]:
  if name.startswith('clavicle') and name.endswith(side):return k+4
  if name.startswith('upperarm') and name.endswith(side):return k
  if name.endswith(side) and name.startswith(('lowerarm','hand','thumb','index','middle','ring','pinky')):return k+2
 return 0
for sex,key in [('Male','masc'),('Female','fem')]:
 bpy.ops.wm.read_factory_settings(use_empty=True)
 bpy.ops.import_scene.gltf(filepath=str(PACK/('Superhero_'+sex+'_FullBody.gltf')))
 rig=next(o for o in bpy.context.scene.objects if o.type=='ARMATURE')
 meshes=[o for o in bpy.context.scene.objects if o.type=='MESH' and len(o.data.vertices)>100]
 body=max(meshes,key=lambda o:len(o.data.vertices));eyes=next(o for o in meshes if len(o.data.vertices)==556);brow=next(o for o in meshes if o not in (body,eyes))
 def coords(o):return np.array([tuple(o.matrix_world@v.co) for v in o.data.vertices])
 def joint(n):return np.array(rig.matrix_world@rig.data.bones[n].head_local)
 eyeP=coords(eyes);J={'neck':joint('neck_01'),'head':joint('Head'),'pelvis':joint('pelvis')}
 for side in ['l','r']:
  for k,n in [('shoulder','upperarm'),('elbow','lowerarm'),('wrist','hand'),('hip','thigh'),('knee','calf'),('thumb','thumb_02')]:J[k+'_'+side]=joint(n+'_'+side)
  J['eye_'+side]=eyeP[eyeP[:,0]*(1 if side=='l' else -1)>0].mean(0)
 origin=J['neck'];cut_z=.7*(J['knee_l'][2]+J['knee_r'][2])*.5+.3*(J['hip_l'][2]+J['hip_r'][2])*.5
 def extract(o,isbody):
  me=o.data;me.calc_loop_triangles();P=coords(o);norm=np.array([tuple(o.matrix_world.to_3x3()@v.normal) for v in me.vertices]);norm/=np.maximum(np.linalg.norm(norm,axis=1,keepdims=True),1e-12)
  keep=P[:,2]>cut_z-.15
  if isbody:
   for side in ['l','r']:
    e,w=J['elbow_'+side],J['wrist_'+side];axis=(w-e)/np.linalg.norm(w-e);t=(P-e)@axis/np.linalg.norm(w-e);rad=np.linalg.norm(P-e-np.outer((P-e)@axis,axis),axis=1)
    keep &= ~((t>1.015)&(rad<.13)&(np.linalg.norm(P-w,axis=1)<.32)&((P[:,0]>0)==(w[0]>0)))
  else:keep[:]=True
  dense=np.zeros((len(P),9));groups={g.index:g.name for g in o.vertex_groups}
  for v in me.vertices:
   for g in v.groups:dense[v.index,mapped(groups[g.group])]+=g.weight
  dense[dense.sum(1)<1e-8,2 if not isbody else 0]=1
  dense/=dense.sum(1,keepdims=True)
  uv=me.uv_layers.active;verts=[];uvs=[];normals=[];tris=[];lut={}
  for tr in me.loop_triangles:
   if not all(keep[i] for i in tr.vertices):continue
   tri=[]
   for vi,li in zip(tr.vertices,tr.loops):
    u=tuple(uv.data[li].uv) if uv else (0.,0.)
    cn=o.matrix_world.to_3x3().inverted().transposed()@me.corner_normals[li].vector;cn.normalize()
    kk=(vi,round(u[0],7),round(u[1],7),*tuple(round(x,6) for x in cn))
    if kk not in lut:lut[kk]=len(verts);verts.append(vi);uvs.append(u);normals.append(tuple(cn))
    tri.append(lut[kk])
   tris.append(tri)
  ids=np.array(verts);order=np.argsort(-dense[ids],axis=1)[:,:4];W=np.take_along_axis(dense[ids],order,axis=1);W/=W.sum(1,keepdims=True)
  return dict(pos=app(P[ids]-origin).astype('f4'),nrm=app(np.array(normals)).astype('f4'),uv=np.array(uvs,'f4'),tris=np.array(tris,'i4'),bone=order.astype('u1'),weight=W.astype('f4'),region=np.zeros(len(ids),'u1'),ao=np.ones(len(ids),'f4'))
 joints={k:app(v-origin).tolist() for k,v in J.items()};joints['eye_mid']=app((J['eye_l']+J['eye_r'])/2-origin).tolist();joints['eye_radius']=float(np.linalg.norm(eyeP[eyeP[:,0]>0]-J['eye_l'],axis=1).mean());joints['thigh_cut']=float(cut_z-origin[2])
 data=extract(body,True);data['joints']=json.dumps(joints);np.savez_compressed(OUT+'/body_'+key+'.npz',**data)
 for name,o in [('eyes',eyes),('brows',brow)]:np.savez_compressed(OUT+'/'+name+'_'+key+'.npz',**extract(o,False))
 print('EXTRACT',key,len(data['pos']),len(data['tris']),joints,flush=True)
