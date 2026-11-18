// CPU procedural deformation shared by the WebGL and software preview paths.
// Static-fixture V8b forearm twist, including its spatial normal derivative.
const add=(a,b)=>a.map((v,i)=>v+b[i]), sub=(a,b)=>a.map((v,i)=>v-b[i]), scale=(a,s)=>a.map(v=>v*s);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
function transform(p,m){return [m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]]}
function normalTransform(n,m){const a=[m[0],m[1],m[2]],b=[m[4],m[5],m[6]],c=[m[8],m[9],m[10]],co=[cross(b,c),cross(c,a),cross(a,b)],det=dot(a,co[0]);if(Math.abs(det)<1e-12)throw Error('Singular skin matrix');return [0,1,2].map(i=>(co[0][i]*n[0]+co[1][i]*n[1]+co[2][i]*n[2])/det)}
function rotate(v,axis,t){return add(add(scale(v,Math.cos(t)),scale(cross(axis,v),Math.sin(t))),scale(axis,dot(v,axis)*(1-Math.cos(t))))}
export function skinVertex(p,n,indices,weights,matrices,foreTwist={}){let P=[0,0,0],N=[0,0,0];for(let k=0;k<4;k++){const w=weights[k]/255;if(!w)continue;const bone=indices[k],m=matrices[bone];let pos=transform(p,m),normal=normalTransform(n,m);const f=foreTwist[bone];if(f){const delta=sub(f.bw,f.be),length2=dot(delta,delta),raw=dot(sub(p,f.be),delta)/length2,s=Math.max(0,Math.min(1,raw/.96)),smooth=s*s*(3-2*s),roll=f.roll*f.confidence,theta=-roll*(1-smooth);pos=add(f.E,rotate(sub(pos,f.E),f.axis,theta));normal=rotate(normal,f.axis,theta);const grad=scale(delta,roll*6*s*(1-s)/(length2*.96)),ag=rotate(normalTransform(grad,m),f.axis,theta);normal=sub(normal,scale(ag,dot(cross(f.axis,sub(pos,f.E)),normal)))}P=add(P,scale(pos,w));N=add(N,scale(normal,w))}return {p:P,n:N}}
