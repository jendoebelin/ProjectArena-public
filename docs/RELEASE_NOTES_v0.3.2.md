# Project Arena 0.3.2 (draft, not released; also the first Steam Playtest build)

## New
- **PC spectator window for streamers**: run the PC version without a headset (or with `--spectate`) and it opens a
  window that finds a live match by itself, with player names, the score and the rules. Cameras: 1-5 (side, high
  side, blue end, orange end, top), A auto camera, F free camera (right mouse + WASD), mouse wheel zoom, Tab next
  match, H the spectator hub, F11 full screen. A separate Spectator Controls window has a button for every one of
  these and stays off the stream (F1 shows / hides it). Capture only the spectator window in OBS. `--server host:port` watches another server.
- **Developer mode: a tuning tool** (MENU > SETTINGS > TUNING): move and turn every gauntlet part (the whole
  gauntlet on the controller, the wrist cuff, the held ball, the shield, the knuckle guard) and the avatar's skeleton
  live in front of a mirror, then export the values. Hidden unless developer mode is on; nothing changes for players.
