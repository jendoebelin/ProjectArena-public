# Project Arena 0.3.15

## New (experimental, off unless you turn it on)
- **BALL LOOK-AHEAD ONLINE** (SETTINGS → GRAPHICS AND VIEW). For long-distance matches: when you play in someone
  else's room, everything you do with the ball used to wait for a round trip to the host. Across the world that's a
  quarter of a second: your ball hung in your hand after you let go, it reached your hand late when you grabbed it,
  and the other player's ball arrived later than it really did. With BALL LOOK-AHEAD ON:
  - your throw leaves your hand the moment you let go, and a grab or catch is in your hand at once (your shield too);
  - the ball is drawn where the host will have it when your hands get there, so blocks and dodges line up with what
    you see. Near your end it is a full round trip ahead, at the other end none (their blocks and catches show where
    they happen), so a ball coming at you looks a little quicker and one going away a little slower;
  - the ball moves smoothly instead of stuttering between network updates.

  The host still decides every throw, block and point; only what you see changes, and nothing changes for the host.
  When the host disagrees (it happens now and then), the ball glides over to the real flight. It's off by default
  while we try it: turn it on and tell us how it plays (MENU → REPORT BUG works for that too).

## Fixed
- **HAND OFF HOST** no longer covers the GAMES ONLINE heading in the multiplayer menu: while you host, it takes
  CUSTOMIZE AVATAR's place (which only opens outside rooms).
- **The masculine body's chest** sat too low and too close together: the pectorals are now about 3 cm higher and a
  little wider apart, in every outfit.
- **The practice bot** picks between equally good dodging spots the same way on every headset and PC.

## Docs
- The PC spectator window's keys are listed in full in `docs/INSTALL.md`: cameras 1-7, the free camera (hold the right
  mouse button to look around), saved cameras and a game controller.

## For other builds
- Snapshots carry a few more bytes at their end (each ball's spin and bounces, and the host's game clock). Older
  builds stop reading before them, so 0.3.14 and 0.3.15 play together in both directions.
