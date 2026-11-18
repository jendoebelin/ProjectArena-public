// Render-only secondary motion. Raw tracking/rig inputs are never modified.
// Advance one state once per avatar/frame, not once per eye/pass.
const clamp=(x,a,b)=>Math.max(a,Math.min(b,x));
const angle=x=>Math.atan2(Math.sin(x),Math.cos(x));
const smooth=x=>{x=clamp(x,0,1);return x*x*(3-2*x)};
export const SWISH_STYLES={tennis:{omega:11,damping:.61,maxOffset:.065,maxTwist:.23,drag:.025,inertia:.007,turnLag:.10},kilt:{omega:8,damping:.72,maxOffset:.05,maxTwist:.17,drag:.020,inertia:.006,turnLag:.12}};
export class SwishState {
 constructor(style='tennis'){this.style=style;this.clear()}
 clear(){this.ready=false;this.x=this.z=this.twist=this.vx=this.vz=this.vt=0;this.filteredVX=this.filteredVZ=this.filteredTurn=0;this.output={x:0,z:0,twist:0,visible:false,resetReason:'new'};}
 reset(input,reason='explicit'){
  this.clear();this.ready=!!input&&input.valid!==false&&[input.time,input.position?.[0],input.position?.[2],input.yaw].every(Number.isFinite);
  if(this.ready){this.time=input.time;this.px=input.position[0];this.pz=input.position[2];this.yaw=input.yaw;this.epoch=input.epoch??0;this.identity=input.identity??'';}
  this.output={x:0,z:0,twist:0,visible:this.ready,resetReason:reason};return this.output;
 }
 update(input){
  if(!input||input.valid===false||![input.time,input.position?.[0],input.position?.[2],input.yaw].every(Number.isFinite))return this.reset(null,'invalid-input');
  if(!this.ready)return this.reset(input,'first-acquisition');
  if((input.epoch??0)!==this.epoch||(input.identity??'')!==this.identity)return this.reset(input,'identity-or-origin-change');
  const dt=input.time-this.time;
  if(dt===0)return this.output; // multipass calls at same sample time cannot integrate twice
  if(dt<0||dt>.12)return this.reset(input,dt<0?'time-rewind':'frame-stall');
  const dx=input.position[0]-this.px,dz=input.position[2]-this.pz,dyaw=angle(input.yaw-this.yaw);
  if(Math.hypot(dx,dz)>.3||Math.abs(dyaw)>1.0)return this.reset(input,'teleport');
  const c=SWISH_STYLES[this.style]||SWISH_STYLES.tennis;
  const alpha=1-Math.exp(-dt/.045),oldVX=this.filteredVX,oldVZ=this.filteredVZ;
  this.filteredVX+=(clamp(dx/dt,-4,4)-this.filteredVX)*alpha;
  this.filteredVZ+=(clamp(dz/dt,-4,4)-this.filteredVZ)*alpha;
  this.filteredTurn+=(clamp(dyaw/dt,-5,5)-this.filteredTurn)*alpha;
  const ax=clamp((this.filteredVX-oldVX)/dt,-12,12),az=clamp((this.filteredVZ-oldVZ)/dt,-12,12);
  const cy=Math.cos(input.yaw),sy=Math.sin(input.yaw);
  let tx=-(cy*this.filteredVX-sy*this.filteredVZ)*c.drag-(cy*ax-sy*az)*c.inertia;
  let tz=-(sy*this.filteredVX+cy*this.filteredVZ)*c.drag-(sy*ax+cy*az)*c.inertia;
  const r=Math.hypot(tx,tz);if(r>c.maxOffset){tx*=c.maxOffset/r;tz*=c.maxOffset/r;}
  const tt=clamp(-this.filteredTurn*c.turnLag,-c.maxTwist,c.maxTwist);
  const n=Math.ceil(dt*240),h=dt/n,k=c.omega*c.omega,d=2*c.damping*c.omega;
  for(let i=0;i<n;i++){
   this.vx+=(k*(tx-this.x)-d*this.vx)*h;this.vz+=(k*(tz-this.z)-d*this.vz)*h;this.vt+=(k*(tt-this.twist)-d*this.vt)*h;
   this.x+=this.vx*h;this.z+=this.vz*h;this.twist+=this.vt*h;
   const rr=Math.hypot(this.x,this.z);if(rr>c.maxOffset){this.x*=c.maxOffset/rr;this.z*=c.maxOffset/rr;const outward=(this.vx*this.x+this.vz*this.z)/(rr||1);if(outward>0){this.vx-=outward*this.x/c.maxOffset;this.vz-=outward*this.z/c.maxOffset;}}
   if(Math.abs(this.twist)>c.maxTwist){this.twist=clamp(this.twist,-c.maxTwist,c.maxTwist);if(this.twist*this.vt>0)this.vt=0;}
  }
  this.time=input.time;this.px=input.position[0];this.pz=input.position[2];this.yaw=input.yaw;
  this.output={x:this.x,z:this.z,twist:this.twist,visible:true,resetReason:null};return this.output;
 }
}
const flat=a=>Array.isArray(a?.[0])?a.flat():a;
export function prepareSwishMesh(positions,indices,meta={},normals=null){
 const base=new Float32Array(flat(positions)),index=new Uint32Array(flat(indices)),count=base.length/3;
 if(!Number.isInteger(count)||!Number.isFinite(meta.waistY)||!Number.isFinite(meta.hemY)||meta.waistY<=meta.hemY)throw Error('Invalid swish mesh bounds');
 const weights=new Float32Array(count),factors=meta.weights||meta.motionWeight||meta.clothWeight;
 const pin=meta.pinFraction??.4;
 for(let i=0;i<count;i++){
  const f=clamp(factors?.[i]??1,0,1),depth=(meta.waistY-base[i*3+1])/(meta.waistY-meta.hemY);
  weights[i]=meta.weightsAreFinal?f:smooth((depth-pin)/(1-pin))*f;
 }
 const profile=(meta.collisionProfile||[]).map(v=>({...v,cx:v.cx??0,cz:v.cz??v.centerZ??0})).sort((a,b)=>a.y-b.y);
 const radialProfile=(meta.radialCollisionProfile||[]).map(row=>({y:row.y,radii:Array.from(row.radii)})).sort((a,b)=>a.y-b.y);
 if(radialProfile.length){const N=radialProfile[0].radii.length;if(N<8||radialProfile.some(row=>!Number.isFinite(row.y)||row.radii.length!==N||row.radii.some(r=>!Number.isFinite(r)||r<=0)))throw Error('Invalid radial collision profile');}
 const layerOffsets=new Float32Array(count);
 // Inner/outer/cap vertices share authored angular/height samples. Preserve their radial shell thickness when projected.
 if(meta.clothT&&meta.clothAngle&&meta.clothKind){const layers=new Map(),center=radialProfile.length?(meta.radialCenterXZ||meta.centerXZ):meta.centerXZ,cx=center?.[0]||0,cz=center?.[1]||0;
  for(let i=0;i<count;i++){if(!meta.clothKind[i])continue;const key=[Math.round(meta.clothT[i]*1e6),Math.round(meta.clothAngle[i]*1e6),meta.clothKind[i]].join(','),r=Math.hypot(base[i*3]-cx,base[i*3+2]-cz);let g=layers.get(key);if(!g){g={min:r,ids:[]};layers.set(key,g);}g.min=Math.min(g.min,r);g.ids.push(i);}
  for(const g of layers.values())for(const i of g.ids)layerOffsets[i]=Math.hypot(base[i*3]-cx,base[i*3+2]-cz)-g.min;
 }
 const radialReserve=new Float32Array(count);
 if(radialProfile.length){const center=meta.radialCenterXZ||meta.centerXZ||[0,0];for(let i=0;i<count;i++){const x=base[i*3]-center[0],z=base[i*3+2]-center[1];radialReserve[i]=Math.max(0,Math.hypot(x,z)-radialRadius(radialProfile,base[i*3+1],Math.atan2(x,-z))-layerOffsets[i]-1e-7);}}
 const authored=normals?new Float32Array(flat(normals)):null,groups=new Map(),group=new Uint32Array(count),groupNormals=[];
 for(let i=0;i<count;i++){const j=i*3,key=[base[j],base[j+1],base[j+2],weights[i],...(authored?[authored[j],authored[j+1],authored[j+2]]:[])].map(x=>Math.round(x*1e6)).join(',');let id=groups.get(key);if(id===undefined){id=groups.size;groups.set(key,id);groupNormals.push(...(authored?[authored[j],authored[j+1],authored[j+2]]:[0,1,0]));}group[i]=id;}
 const normalData={group,authored:new Float32Array(groupNormals),rest:new Float32Array(groups.size*3),accum:new Float32Array(groups.size*3),rotated:new Float32Array(groups.size*3)};
 accumulateNormals(base,index,normalData.group,normalData.rest);
 for(let j=0;j<normalData.rest.length;j+=3){const r=Math.hypot(normalData.rest[j],normalData.rest[j+1],normalData.rest[j+2])||1;normalData.rest[j]/=r;normalData.rest[j+1]/=r;normalData.rest[j+2]/=r;}
 return {base,index,weights,layerOffsets,radialReserve,positions:base.slice(),normals:new Float32Array(base.length),baseNormals:authored,normalData,meta:{...meta,collisionProfile:profile,radialCollisionProfile:radialProfile},collisionCount:0,maxCorrection:0};
}
function accumulateNormals(p,index,group,n){
 n.fill(0);for(let i=0;i<index.length;i+=3){const ai=index[i],bi=index[i+1],ci=index[i+2],a=ai*3,b=bi*3,c=ci*3;const ux=p[b]-p[a],uy=p[b+1]-p[a+1],uz=p[b+2]-p[a+2],vx=p[c]-p[a],vy=p[c+1]-p[a+1],vz=p[c+2]-p[a+2];const nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,A=group[ai]*3,B=group[bi]*3,C=group[ci]*3;n[A]+=nx;n[A+1]+=ny;n[A+2]+=nz;n[B]+=nx;n[B+1]+=ny;n[B+2]+=nz;n[C]+=nx;n[C+1]+=ny;n[C+2]+=nz;}
}
function envelope(profile,y,out){
 if(!profile.length)return null;
 // Extend the endpoint proxy through the hem: crossing the original body cutoff must not toggle collision abruptly.
 if(y<=profile[0].y){Object.assign(out,profile[0]);return out;}if(y>=profile[profile.length-1].y){Object.assign(out,profile[profile.length-1]);return out;}
 let k=1;while(k<profile.length&&profile[k].y<y)k++;
 if(k===profile.length){Object.assign(out,profile[k-1]);return out;}const a=profile[k-1],b=profile[k],t=(y-a.y)/Math.max(1e-9,b.y-a.y);
 out.rx=a.rx+(b.rx-a.rx)*t;out.rz=a.rz+(b.rz-a.rz)*t;out.cx=(a.cx||0)+((b.cx||0)-(a.cx||0))*t;out.cz=(a.cz||0)+((b.cz||0)-(a.cz||0))*t;return out;
}
function radialRadius(profile,y,theta){
 let lo=0,hi=profile.length-1;
 if(y<=profile[0].y)hi=0;else if(y>=profile[hi].y)lo=hi;else while(hi-lo>1){const mid=(lo+hi)>>1;if(profile[mid].y<y)lo=mid;else hi=mid;}
 const a=profile[lo],b=profile[hi],N=a.radii.length,u=((theta/(Math.PI*2)+1)%1)*N,i=Math.floor(u),j=(i+1)%N,f=u-i;
 const ra=a.radii[i]*(1-f)+a.radii[j]*f,rb=b.radii[i]*(1-f)+b.radii[j]*f,t=hi===lo?0:clamp((y-a.y)/(b.y-a.y),0,1);return ra+(rb-ra)*t;
}
export function deformSwish(mesh,state,recomputeNormals=true){
 const {base,positions:p,weights:w,meta,index,normals:n}=mesh,cx=meta.centerXZ?.[0]||0,cz=meta.centerXZ?.[1]||0;
 const len=Math.max(.1,(meta.waistY-meta.hemY)*(1-(meta.pinFraction??.4)));
 const lift=Math.min(.018,(state.x*state.x+state.z*state.z)/(2*len));mesh.collisionCount=0;mesh.maxCorrection=0;const scratch={};
 for(let i=0;i<w.length;i++){
  const j=3*i,weight=w[i];if(weight===0){p[j]=base[j];p[j+1]=base[j+1];p[j+2]=base[j+2];continue;}
  const a=state.twist*weight,c=Math.cos(a),s=Math.sin(a),x=base[j]-cx,z=base[j+2]-cz;
  let xx=cx+c*x+s*z+state.x*weight,zz=cz-s*x+c*z+state.z*weight,yy=base[j+1]+lift*weight;
  const bound=meta.maxDisplacement??Infinity;const displacement=Math.hypot(xx-base[j],yy-base[j+1],zz-base[j+2]);if(displacement>bound*weight){const f=bound*weight/displacement;xx=base[j]+(xx-base[j])*f;yy=base[j+1]+(yy-base[j+1])*f;zz=base[j+2]+(zz-base[j+2])*f;}
  if(meta.radialCollisionProfile.length){
   const center=meta.radialCenterXZ||meta.centerXZ||[0,0],ex=xx-center[0],ez=zz-center[1],radius=Math.hypot(ex,ez),theta=Math.atan2(ex,-ez),targetRadius=radialRadius(meta.radialCollisionProfile,yy,theta)+Math.min(meta.radialSafetyMargin??.003,mesh.radialReserve[i])+mesh.layerOffsets[i];
   if(radius<targetRadius){const ox=xx,oz=zz;if(radius>1e-8){xx=center[0]+ex*(targetRadius/radius);zz=center[1]+ez*(targetRadius/radius);}else xx=center[0]+targetRadius;mesh.collisionCount++;mesh.maxCorrection=Math.max(mesh.maxCorrection,Math.hypot(xx-ox,zz-oz));}
  }else{
  const env=envelope(meta.collisionProfile,yy,scratch);
  if(env){const ex=xx-env.cx,ez=zz-env.cz,r=Math.hypot(ex/env.rx,ez/env.rz),radius=Math.hypot(ex,ez),targetRadius=(r>1e-8?radius/r:env.rx)+mesh.layerOffsets[i];if(radius<targetRadius){const ox=xx,oz=zz;if(radius>1e-8){xx=env.cx+ex*(targetRadius/radius);zz=env.cz+ez*(targetRadius/radius);}else xx=env.cx+targetRadius;mesh.collisionCount++;mesh.maxCorrection=Math.max(mesh.maxCorrection,Math.hypot(xx-ox,zz-oz));}}
  }
  p[j]=xx;p[j+1]=yy;p[j+2]=zz;
 }
 if(recomputeNormals){
  // Weld only coincident positions with matching authored normals/weights; retain intentional pleat creases.
  // Transport the authored normal through the rest-to-deformed geometric normal rotation.
  const g=mesh.normalData;accumulateNormals(p,index,g.group,g.accum);
  for(let j=0;j<g.accum.length;j+=3){const r=Math.hypot(g.accum[j],g.accum[j+1],g.accum[j+2]);const ax=g.rest[j],ay=g.rest[j+1],az=g.rest[j+2],bx=r>1e-12?g.accum[j]/r:ax,by=r>1e-12?g.accum[j+1]/r:ay,bz=r>1e-12?g.accum[j+2]/r:az;
   const vx=ay*bz-az*by,vy=az*bx-ax*bz,vz=ax*by-ay*bx,c=clamp(ax*bx+ay*by+az*bz,-1,1),nx=g.authored[j],ny=g.authored[j+1],nz=g.authored[j+2],cx=vy*nz-vz*ny,cy=vz*nx-vx*nz,cz=vx*ny-vy*nx;
   let x=nx+cx+(vy*cz-vz*cy)/Math.max(1e-8,1+c),y=ny+cy+(vz*cx-vx*cz)/Math.max(1e-8,1+c),z=nz+cz+(vx*cy-vy*cx)/Math.max(1e-8,1+c);if(c<-.9999){x=-nx;y=-ny;z=-nz;}const l=Math.hypot(x,y,z)||1;g.rotated[j]=x/l;g.rotated[j+1]=y/l;g.rotated[j+2]=z/l;
  }
  for(let i=0;i<w.length;i++){const j=i*3,k=g.group[i]*3;if(w[i]===0&&mesh.baseNormals){n[j]=mesh.baseNormals[j];n[j+1]=mesh.baseNormals[j+1];n[j+2]=mesh.baseNormals[j+2];}else{n[j]=g.rotated[k];n[j+1]=g.rotated[k+1];n[j+2]=g.rotated[k+2];}}
 }
 return mesh;
}
export function demoRoot(time){
 const t=Math.max(0,time),S=x=>{x=clamp(x,0,1);return x*x*x*(x*(x*6-15)+10)};let x=0,z=0,yaw=0;
 if(t<1){}else if(t<2.2){const u=S((t-1)/1.2);x=.32*u;yaw=.65*u;}
 else if(t<3.2){x=.32;yaw=.65;}
 else if(t<4.4){const u=S((t-3.2)/1.2);x=.32-.57*u;yaw=.65-1.3*u;z=.1*Math.sin(Math.PI*u)**2;}
 else if(t<5.4){x=-.25;yaw=-.65;}
 else if(t<7){const u=S((t-5.4)/1.6);x=-.25*(1-u);yaw=-.65*(1-u);}
 return {time:t,position:[x,0,z],yaw,valid:true,epoch:0,identity:'synthetic-move-turn-stop'};
}

// Column-major world transform of the native garment torso frame, AFTER group/root motion.
// Its heading may include the game's 180-degree bind-to-world rotation.
export function motionInputFromTorso(time,matrix,epoch=0,identity='garment-demo'){
 return {time,position:[matrix[12],matrix[13],matrix[14]],yaw:Math.atan2(matrix[8],matrix[10]),valid:matrix.length===16&&Array.from(matrix).every(Number.isFinite),epoch,identity};
}
