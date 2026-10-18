# Project Arena 0.3.2

Store build: versionCode 6, versionName 0.3.2. Also the first Steam Playtest build (PC).

## New
- **Passthrough** (Quest; MENU > SETTINGS > PASSTHROUGH): see your own room around the court. It is always off
  when the game starts.
  - Modes:
    - **ARENA**: the stadium is gone and the court stands in your room.
    - **WINDOW**: inside the court is all game, with a solid floor and a night sky above. Your room looks in through
      the glass. On Quest 3 / 3S it asks for access to spatial data (your room's depth, used on the headset only), so
      that things in your room that are inside the court are hidden and only what lies beyond the glass shows.
    - **OUTLINE**: only the court's neon edges and its floor lines, so you can see your whole room.
  - **INDOOR** and **OUTDOOR** presets.
  - Adjustments: court floor, glass walls and room brightness, the room in colour or grey, and **object edges**
    (glowing outlines on your real furniture and walls).
  - **Room at edges** (on by default): the game fades and your room shows as you come close to the edge of your play
    space. With a small stationary boundary only your hands count.
  - **Peek** (on by default): hold **Y** during play to see your room.
  - Menus stay solid over your room.
  - On PC it works where the headset's runtime offers passthrough to games (Quest Link, with passthrough allowed in
    the Meta app). SteamVR doesn't, and the button shows N/A.
- **PC spectator window for streamers**: run the PC version without a headset (or with `--spectate`) and it opens a
  window that finds a live match by itself, with player names, the score and the rules.
  - Cameras: 1-5 (side, high side, blue end, orange end, top), A auto camera, F free camera (right mouse + WASD).
  - Mouse wheel zoom, Tab next match, H the spectator hub, F11 full screen.
  - A separate Spectator Controls window has a button for every one of these and stays off the stream (F1 shows or
    hides it). In OBS, capture only the spectator window.
  - `--server host:port` watches another server.
- **New gauntlet placement**, tuned in the headset:
  - the angle of the gauntlet on the controller, the wrist cuff, the held ball and the shield;
  - the knuckle glass sits on the fist, and the area that hits the ball moved with it. Punches reach about 8 cm less
    than before; tell us how it feels.
- **Developer mode: a tuning tool** (MENU > SETTINGS > TUNING): move and turn every gauntlet part (the whole
  gauntlet on the controller, the wrist cuff, the held ball, the shield, the knuckle guard) and the avatar's skeleton
  live in front of a mirror, then export the values. It is hidden unless developer mode is on.

## For testers
Please try and tell us (MENU > REPORT BUG > OTHER, with a short note):
- each passthrough mode, the presets, room at edges and peek: which ones you would use, and anything that looks wrong;
- WINDOW on Quest 3 / 3S: do things in your room inside the court stay hidden?
- punching balls with the knuckle glass: does it reach far enough?
