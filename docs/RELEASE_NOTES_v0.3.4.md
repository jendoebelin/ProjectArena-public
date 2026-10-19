# Project Arena 0.3.4

Store build: versionCode 8, versionName 0.3.4. Everything in 0.3.3, plus:

## New
- **Outfits:** CIRCUIT RUNNER (raised circuit traces with pulses running along them) and three LIVE suits
  (Grid Runner, Echo Suit and Circuit Vest with their glow lines raised and pulsing). Modelled details on the other
  suits: jersey bands and a number panel, a jacket zipper, the hoodie's pocket and drawstrings, tank bindings, the
  pilot's harness and buckles, tee seams.
- **Faces:** four faces for each body type (avatar builder, FACE). Face shields and masks are fitted to each face.
- **Hair that moves:** every style except the buzz cut sways, bounces or swings as you move (the ponytail swings,
  the bob hangs, the mohawk is rebuilt as a raked crest, the top knot has two loose ends). Everyone sees it on everyone.
- **Hair under headgear:** helmets, the hood and the cap no longer hide your hairstyle or swap it. The hair stays
  inside the headgear, and what would show does: a fringe in a helmet's face opening, a ponytail out under the rim.
- **Warm-up throws:** while you wait for an opponent you can throw; nothing scores.
- **Update notice:** when a newer version is out, the title panel says UPDATE AVAILABLE.
- **Developer tuning tool:** pieces turn about their own axes, you can place a piece by hand (hold grip on the
  other hand), the picked value's axis is drawn, and the knuckle bumper has size controls. The owner's latest tuning
  (gauntlet on the controller, bracer, ball in the fist, shield) is the new default.

## Spectating on the PC (no headset)
- **Game controllers:** Xbox controllers, and the PlayStation 4 controller over USB or Bluetooth.
- **Broadcast moments:** at the fist bump the camera cuts to a close side view of the two fists; a won match shows
  the winner and the loser side by side, each alone in a close-up, with confetti over the winner.
- **View layers for streaming (V, or D-pad up / down):** full, no walls (the court outline instead of the glass),
  minimal (no stadium, strike zones or scoreboard), and key (only the players and the balls on green, magenta or
  blue, K) for a chroma key in OBS.
- **An icon** in the taskbar, the title bar and Explorer.

## Fixed
- Spectators in the headset see BLUE WINS / ORANGE WINS instead of the players' YOU WIN / YOU LOSE.
- PC players no longer get negative default names (like PLAYER -648).

## Under the hood
- The APK is about a third of its old size (models, textures and collision are compressed).
- The log's network stats now show the ping between the host and each player, and bug reports carry it.
- Push-to-talk is being looked into: the voice log now shows what the left grip and the microphone did. If push-to-talk
  does not send for you, send a bug report while holding the left grip.

## Not tried in a live match yet
Push-to-talk, the PC spectator's broadcast moments, view layers in OBS, and the ping numbers have been checked in tests
and PC pictures only. Bug reports from real matches help.
