# Project Arena 0.3.5

Store build: versionCode 9, versionName 0.3.5. Everything in 0.3.4, plus:

## Fixed
- **PEEK (hold Y) works with passthrough OFF:** your room fades in while you hold Y and the game comes back when
  you let go. It also works with a menu open now.
- **The room no longer shows through all the time in a small stationary boundary:** ROOM AT EDGES is ignored while
  your head is outside the boundary (the Quest shows your room itself when you really leave it).

## Spectating on the PC (no headset)
- **TRANSPARENT key (K):** VIEW LAYERS > KEY can now leave nothing at all behind the players, for an OBS Game
  Capture source with "Allow Transparency" ticked. No outfit or team colour can clash with a key colour any more
  (green, magenta and blue are still there for a chroma key).
- **WIN SCREEN (R, or click the right stick):** how a won match is shown, on any camera, the free camera included:
  SPLIT (the winner and the loser side by side), WINNER (the camera flies to the winner) or YOUR CAMERA (the camera
  stays put, the winner's name over it).
- **Several spectator windows for one stream:** each window keeps its own settings, and they can be set when it
  starts: `ProjectArena.exe --spectate --layer 3 --keycol 3 --winscreen winner --tag winner`. `--tag NAME` gives the
  window its own title ("Project Arena - Spectator - WINNER") so each OBS source can pick its own window.

## Still being looked into
Push-to-talk: if it does not send for you, send a bug report while holding the left grip.
