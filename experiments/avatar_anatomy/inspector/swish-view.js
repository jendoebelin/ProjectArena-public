import * as T from './three.module.js';
import {SwishState,prepareSwishMesh,deformSwish,demoRoot} from './swish.mjs';
const $=id=>document.getElementById(id);
export class SwishView {
 constructor(group,rebuild){this.group=group;this.rebuild=rebuild;this.item=null;this.playing=false;this.frame=0;
 $('swishPlay').onclick=()=>{if(this.playing){this.playing=false;this.label();return}if($('pose').value!=='rest'){$('pose').value='rest';this.rebuild()}if(!this.item)return;if(this.frame>=900)this.seek(0);this.start=performance.now()-this.frame/90*1000;this.playing=true;this.label()};
 $('swishReset').onclick=()=>{this.playing=false;this.seek(0)};
 $('swishFrame').oninput=()=>{const requested=Number($('swishFrame').value);this.playing=false;if($('pose').value!=='rest'){$('pose').value='rest';this.rebuild()}this.seek(requested)};
 }
 clear(){this.item=null;this.playing=false;this.frame=0;this.group.position.set(0,0,0);this.group.rotation.set(0,0,0);$('swishControls').hidden=true;this.label()}
 register(m,g,part){if(!m.swish||!part.skin)return;const matrix=new T.Matrix4().fromArray(part.m).multiply(new T.Matrix4().fromArray(part.skin[0]));this.item={mesh:prepareSwishMesh(m.p,m.idx,m.swish,m.n),geometry:g,matrix:matrix.elements,state:new SwishState(m.swish.type),bindYaw:Math.atan2(matrix.elements[8],matrix.elements[0])};$('swishControls').hidden=false;this.seek(0)}
 input(frame){const r=demoRoot(frame/90),m=this.item.matrix,c=Math.cos(r.yaw),s=Math.sin(r.yaw);return {...r,position:[r.position[0]+c*m[12]+s*m[14],m[13],r.position[2]-s*m[12]+c*m[14]],yaw:r.yaw+this.item.bindYaw}}
 seek(frame){if(!this.item)return;this.frame=0;this.item.state.reset(this.input(0),'seek');for(let n=1;n<=frame;n++)this.item.state.update(this.input(n));this.frame=frame;this.draw();this.label()}
 tick(now){if(!this.playing||!this.item)return;const target=Math.min(900,Math.floor((now-this.start)*.09));for(let n=this.frame+1;n<=target;n++)this.item.state.update(this.input(n));this.frame=target;this.draw();if(target===900)this.playing=false;this.label()}
 draw(){const a=this.item;if(!a)return;deformSwish(a.mesh,a.state.output);const p=a.geometry.attributes.position.array,n=a.geometry.attributes.normal.array,M=a.matrix,P=a.mesh.positions,N=a.mesh.normals;for(let j=0;j<P.length;j+=3){for(let c=0;c<3;c++){p[j+c]=M[c]*P[j]+M[c+4]*P[j+1]+M[c+8]*P[j+2]+M[c+12];n[j+c]=M[c]*N[j]+M[c+4]*N[j+1]+M[c+8]*N[j+2]}}a.geometry.attributes.position.needsUpdate=true;a.geometry.attributes.normal.needsUpdate=true;a.geometry.computeBoundingSphere();const root=demoRoot(this.frame/90);this.group.position.set(...root.position);this.group.rotation.y=root.yaw}
 label(){$('swishPlay').textContent=this.playing?'Pause swish demo':'Play swish demo';$('swishFrame').value=this.frame;$('swishStatus').textContent=`Synthetic ${ (this.frame/90).toFixed(2)} / 10 s · frame ${this.frame} / 900. ${this.playing?'Playing':'Paused'}. Fixed 90 Hz state; display may skip frames. Waist and opaque underlayer pinned.`}
}
