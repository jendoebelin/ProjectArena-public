#pragma once
#include "avatar.h"
namespace avatar {
struct TorsoFollowState {
    bool initialized=false;
    Vec3 clavicleAimOffset[2]{};
    Vec3 worldAngles{}; // heading, pitch, roll; owned by one avatar, never global
};
Rig solveRigFollow(const PlayerInput&,const AvatarDesc&,TorsoFollowState&,float dt);
}
