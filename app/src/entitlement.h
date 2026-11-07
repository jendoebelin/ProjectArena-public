// The Meta Horizon Store's entitlement check (VRC.Quest.Security.1): at start, Meta's Platform SDK confirms that the
// player's account owns the game. Store builds only (./build.sh release open store defines PA_STORE and puts Meta's
// libovrplatformloader.so in the APK); the GitHub / SideQuest builds and the PC build never check.
#pragma once

namespace entitlement {

enum class State { Off, Checking, Owned, Failed };

// Starts the check (call it at launch: Meta asks for it within 10 s). jniEnv: the calling thread's JNIEnv*;
// activity: the NativeActivity's jobject. Without PA_STORE it does nothing and the state stays Off.
void start(void* jniEnv, void* activity);

// Reads Meta's answers (call every frame). Owned / Failed are final.
State poll();

// Why it failed, for the log (empty otherwise).
const char* why();

}  // namespace entitlement
