import numpy as np
class WaistFit:
 def __init__(self, parts, centerZ):
  self.center=np.array([0.,centerZ]);self.cache={};self.tri=[]
  for v,i,s,m in parts:
   idx=np.concatenate([i[st:st+n] for st,n,mi in s if m[mi]['shader']!=2]);tri=v['p'][idx.reshape(-1,3)].astype(float)
   # Only central torso/hip triangles, never hanging arms.
   tri=tri[np.max(abs(tri[:,:,0]),axis=1)<.28];self.tri.append(tri)
  self.tri=np.concatenate(self.tri)
 def segments(self,y):
  key=round(y,8)
  if key in self.cache:return self.cache[key]
  tri=self.tri[(self.tri[:,:,1].min(1)<=y)&(self.tri[:,:,1].max(1)>=y)];lines=[]
  for p in tri:
   pts=[]
   for i,j in [(0,1),(1,2),(2,0)]:
    if (p[i,1]>y)!=(p[j,1]>y):pts.append((p[i]+(p[j]-p[i])*(y-p[i,1])/(p[j,1]-p[i,1]))[[0,2]])
   if len(pts)==2:lines.append(pts)
  self.cache[key]=np.array(lines);return self.cache[key]
 def radius(self,y,theta):
  lines=self.segments(y);d=np.array([np.sin(theta),-np.cos(theta)]);a=lines[:,0]-self.center;b=lines[:,1]-lines[:,0];cross=lambda u,v:u[...,0]*v[...,1]-u[...,1]*v[...,0]
  den=cross(d,b);ok=abs(den)>1e-10;t=np.divide(cross(a,b),den,out=np.zeros(len(a)),where=ok);s=np.divide(cross(a,d),den,out=np.zeros(len(a)),where=ok);hit=t[ok&(s>=-1e-8)&(s<=1+1e-8)&(t>0)]
  if not len(hit):raise ValueError(f'No torso contour hit at {y},{theta}')
  return max(hit)

 def envelope(self,y,theta):
  hits=[]
  for dy in [-.0015,0,.0015]:
   try:hits.append(self.radius(y+dy,theta))
   except ValueError:pass
  if not hits:
   for dy in [-.004,.004]:
    try:hits.append(self.radius(y+dy,theta))
    except ValueError:pass
  if not hits:raise ValueError('No surrounding torso contour')
  return max(hits)

 def convex_radii(self,y,angles):
  from scipy.spatial import ConvexHull
  points=[]
  for dy in [-.0025,0,.0025]:
   lines=self.segments(y+dy)
   if len(lines):points.extend(lines.reshape(-1,2))
  points=np.unique(np.round(points,7),axis=0);h=ConvexHull(points);poly=points[h.vertices];a=poly-self.center;b=np.roll(poly,-1,axis=0)-poly
  out=[]
  for th in angles:
   d=np.array([np.sin(th),-np.cos(th)]);cross=lambda u,v:u[...,0]*v[...,1]-u[...,1]*v[...,0];den=cross(d,b);ok=abs(den)>1e-10;t=np.divide(cross(a,b),den,out=np.zeros(len(a)),where=ok);ss=np.divide(cross(a,d),den,out=np.zeros(len(a)),where=ok);hit=t[ok&(ss>=-1e-8)&(ss<=1+1e-8)&(t>0)]
   if not len(hit):raise ValueError('No convex hip hit')
   out.append(max(hit)+.0015)
  return np.array(out)
