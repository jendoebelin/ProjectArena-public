#include "torso_follow.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
int main(int argc,char**argv){float hz=argc>1?atof(argv[1]):90;
 puts("hz,scenario,body,frame,time,targetSpeed,gap,upperErr,foreErr,shoulderSpeed,elbowSpeed,wristSpeed,torsoDegSec,headErr");
 for(int test=0;test<6;test++)for(int b=0;b<2;b++){avatar::TorsoFollowState state;avatar::Rig prev;Vec3 prevTarget[2];bool have=false;
 AvatarDesc d;avatar::fromText("body="+std::to_string(b)+",suit=0,hair=0,lower=1",d);const auto&K=avatar::bodyBind(b);const auto&T=avatar::rigTuning();
 for(int n=0;n<int(hz*8);n++){float t=n/hz;PlayerInput in;in.avatar=d;in.headTracked=true;Quat face=Quat::axisAngle({0,1,0},kPi);in.head={face,{0,1.62f,0}};
 if(test==5)in.head.p.x=.06f*sin(t*2);float f=test==1?1.f:.5f,a=sin(std::max(0.f,t-1)*kPi*f);if(test==2)a=t<2?0:t<4?1:-1;
 Quat q=face*Quat::axisAngle({0,1,0},deg2rad(a*35))*Quat::axisAngle({0,0,1},deg2rad(a*12))*Quat::axisAngle({1,0,0},deg2rad(a*22));
 Vec3 top=in.head.transform(K.neckTop-K.eye+T.neck);Pose torso{q,top-q.rotate(K.neckTop)};Vec3 targets[2];float lengths[2][2];
 for(int k=0;k<2;k++){float side=k?1:-1;lengths[k][0]=length(K.elbow[k]-K.shoulder[k]);lengths[k][1]=length(K.wrist[k]-K.elbow[k]);float L=lengths[k][0]+lengths[k][1];Vec3 dir=normalize(Vec3{side*.8f,-.15f,-.6f});float frac=.985f;
 if(test==3)frac=1.6f;if(test==4)frac=.95f+.25f*(.5f+.5f*sin(t*2));if(test==5&&k==1)frac=1.6f;
 targets[k]=torso.transform(K.shoulder[k])+q.rotate(dir)*(L*frac);Vec3 off=avatar::cuffPose(k==0).transform({side*T.forearmEnd.x,T.forearmEnd.y,T.forearmEnd.z});in.hand[k].tracked=true;in.hand[k].grip={face,targets[k]-face.rotate(off)};}
 auto r=avatar::solveRigFollow(in,d,state,1/hz);float gap=0,ue=0,fe=0,ss=0,es=0,ws=0,ts=0;
 for(int k=0;k<2;k++){gap=std::max(gap,length(r.fore[k].b-targets[k]));ue=std::max(ue,std::abs(length(r.upper[k].b-r.upper[k].a)-lengths[k][0]));fe=std::max(fe,std::abs(length(r.fore[k].b-r.fore[k].a)-lengths[k][1]));if(have){ss=std::max(ss,length(r.upper[k].a-prev.upper[k].a)*hz);es=std::max(es,length(r.upper[k].b-prev.upper[k].b)*hz);ws=std::max(ws,length(r.fore[k].b-prev.fore[k].b)*hz);ts=std::max(ts,length(targets[k]-prevTarget[k])*hz);}}
 float angle=0;if(have){auto a=r.torso.q,c=prev.torso.q;float dot=std::abs(a.x*c.x+a.y*c.y+a.z*c.z+a.w*c.w);angle=2*acos(clampf(dot,-1.f,1.f))*180/kPi*hz;}
 printf("%.0f,%d,%d,%d,%.7f,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",hz,test,b,n,t,ts,gap,ue,fe,ss,es,ws,angle,length(r.neck.a-top));prev=r;for(int k=0;k<2;k++)prevTarget[k]=targets[k];have=true;
 }}
}
