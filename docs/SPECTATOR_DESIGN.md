# Spectator lobby, matchmaking queue and streamer tools (owner design, 2026-10-14)

Goal: a great experience for spectators and streamers, to motivate streaming of matches.

## Built (v0.2 test build)
- Watching a match: fly with the sticks (left: move, right: snap turn / up-down).
- Warp points: A saves the current view (up to 8, saved in warps.cfg, valid in every arena); X goes to the next one.
- Auto camera: Y cycles the warps every 8 s.
- Transitions cross-fade from the previous view into the next warp camera (never fade to black).

## Built (2026-10-14, after the owner's answers; PC- and probe-tested, not yet seen in a headset)
- Rooms (protocol v8): two tank slots, up to four waiting players in a holding area, a queue of two, rotation
  (both players out when a match ends, the queue in), ARENA FULL for a seventh, the bot fills a free slot.
- JOIN QUEUE kiosk at your holding spot (and in the menu); NEXT / 2ND tags; waiting players can't enter the tank;
  SETTINGS > OUTSIDE: HIDDEN hides them from the players.
- Presence: blue lights for spectators (lobby and near an arena, never shown to the players); the lobby miniatures
  show red (waiting / queued) and blue markers; empty tank slots are not drawn.
- Voice: push-to-talk on the left grip by default (HOLD LEFT GRIP TO TALK beside the hand), open mic, off; mute
  spectators, mute other players, mute all; only people at that arena hear each other.
- Director tools (MENU > DIRECTOR TOOLS while watching): per camera hold time, cut / cross-fade, timer / action /
  follow; clicking an option opens an info panel with a rendered preview (wide view of the camera's path, then what
  the camera sees); auto camera; cameras shared with a team through the arena hosts.

## To build next
1. **Overlord lobby view.** Entering the lobby shows every live arena from above. Everyone connected appears:
   - players in a tank: real avatar miniatures (as now);
   - waiting / queued players: **red** markers;
   - spectators (crowd seat or flying near the arenas): **blue** markers.
   Markers are not identified, only coloured, and they move as the people move (presence updates).
2. **Real matchmaking queue.** Per arena, at most **2** players in the queue, standing in a holding area outside
   the tank. Queued players can hear and talk to the players in the tank (voice; needs the microphone permission
   and a mute / report option).
   **Rotation (owner, confirmed):** when a match ends, **both** players leave the tank and the next two in the queue
   enter. The two who just played appear outside the tank, still in the arena room, with a **JOIN QUEUE** button.
   This happens even when nobody is queued. Up to four players (two finished, two queued) can be outside the tank
   before queuing, and they can all talk to each other there.
3. **Isolation rule.** Spectators, waiting and queued players never interact with, obscure, or appear inside the
   players' view or the arena area. A spectator may place a warp camera inside the tank; players are never made
   aware of it.
3a. **Waiting players (owner, confirmed).** The up to four people outside the tank (two who just played, up to two
   queued) are **waiting players**, not spectators. They stand in a holding area in the arena room, on their own
   visibility / collision layer: they can never walk into the tank or obscure the players' view. Players in the tank
   can see and hear them if they want; a **HIDE PEOPLE OUTSIDE** option (mute visibility outside the arena) removes
   them from the players' view.
3b. **Voice (owner, confirmed).**
   - **Push-to-talk is on by default**, with a text overlay naming the push-to-talk button. Open mic is an option.
   - Separate mutes: **mute spectators**, **mute the other player**, **mute all**.
   - Voice zones: only people **in the arena room** (players, the up to four outside the tank, and spectators
     standing in that arena area) hear each other. The overlord lobby and other arenas cannot talk to players.
4. **Shareable warp sets** for streaming teams (a team name; warp sets shared through the server).
5. **Per-camera options** for the auto camera, set per warp: hold time, transition (cut / cross-fade), trigger
   (timer, or cut to action: hit, strike, match point, return).
6. **Director toolbar.** A tool bar for the person setting up cameras. Each option has an info panel that plays an
   example of what it does when clicked. **Example (owner):** for a "follow the action" camera, the preview first
   shows a wide view of the arena with the camera's path as it follows the action, then shows what that camera
   captured from its own view.
7. **PC broadcast camera** (with the PC version): a separate camera window for OBS, so streamers' headset views stay
   steady.

## Avatars (owner review, 2026-10-14)
The default head, hair and body form need work (details to follow from the owner).
