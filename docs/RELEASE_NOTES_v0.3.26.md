# Project Arena 0.3.26

## New
- **Simple setup**: a room's page (PRACTICE and hosting) now opens on **SIMPLE SETUP**: the room's few main options
  as big buttons, the bot's level, and START. Each button says its choice and steps through them: in the SQUARE and
  DIAMOND **MATCH** (TIMED / ROUNDS), **SHIELDS** (STANDARD / HOTDOG / MAIDEN) and **POWER** (STANDARD / FULL EFFORT);
  in FOUR CORNERS **TEAMS**, **SHIELDS** and **POWER**; in FIREBALL BLITZ **TEAMS**, **FIREBALLS** and **HEALTH**.
  **ALL STANDARD** puts them back. **CUSTOM SETTINGS**, beside it, opens the full page as before, and the game
  remembers which of the two you use.
- **MY SAVES**: on CUSTOM SETTINGS, three save slots for each kind of room. **SAVE HERE** keeps the room's options as
  they are, **LOAD SAVE** brings them back.
- **THIS ROOM**: when you join a room whose rules aren't the standard ones, a notice says what is different.
- **FULL EFFORT** (a community request): a third step for THROW POWER and KNUCKLE RETURNS. Top speed takes a throw
  half again as fast as NATURAL's, and the same return takes a punch half again as hard as PUNCH's. It plays when
  everyone in the match has 0.3.26 (with a 0.3.25 player in it, NATURAL and PUNCH; with an older one, regulation; the
  host is told).
- **BOUNCES in FOUR CORNERS**: WALLS: LIVELY, WALL GRIP and BANK ASSIST now play in FOUR CORNERS too, on its page's
  **BOUNCES & POWER**. Everyone in the match needs 0.3.26 (with an older player in it, standard, and the host is told).
- **Livelier bots**: between shots the bots breathe, shift their weight and glance at you, at the ball and at the
  stands. Hold your ball without throwing and they taunt you: a beckon, arms spread wide, a shrug, pounding their
  chest, BOXER's gloves, MONKEY's bounce. Throw while they're at it and you can catch them out. A point won gets a
  fist pump, a double pump, a point at you or a clap; a match won, their stance's own victory pose, RAISE THE ROOF, or
  AIR GUITAR, with a guitar. Online, everyone in the room sees them (the guitar when the host has 0.3.26).
- **Views while waiting**: waiting for your turn in a room, press READY's button (A, unless you've moved it) to change
  where you watch from: YOUR SPOT, two raised views outside the glass, and HIGH ABOVE. You keep your place in the
  queue, you're told when you're next, and you're brought back to the court for your turn.
- **HAND POSITION** (a community request): **SETTINGS > CONTROLLERS** has HAND POSITION beside CONTROLLER ANGLE:
  **UP**, **AHEAD** and **OUT**, a centimetre a press, up to 8 cm each way, to line the hands in the game up with
  your real ones. You see them move as you press. Saved on your headset.
- **One scoreboard in the PC spectator**: **O** (or Spectator Controls > Screens) chooses where the score is:
  **BANNER** across the top, **CORNER**, or **JUMBOTRON**, the arena's own screen (from a camera that can't see it,
  the banner stands in).
- **The asset viewer** (jendoebelin.github.io/ProjectArena-public) plays the bots' moves: at ease, the taunts, the
  celebrations and the victory poses, and the TENNIS SKIRT's and KILT's swing. The guitar is P13.

## Better
- **Watching is smoother**: from the waiting area and as a spectator, the players and balls move evenly between the
  host's updates instead of in steps, and a host on 0.3.26 sends a steady 30 updates a second at any frame rate.
- **FIST BUMP FIRST starts on**: practice against the bot begins face to face in the centre with a fist bump (no bump
  in 10 seconds and the countdown starts anyway). It is turned on once for everyone; its switch is on the practice
  room's CUSTOM SETTINGS, and off stays off.
- **SAVE AND BACK**: the settings pages end with SAVE AND BACK, and DEFAULTS small beside it. Your changes still take
  effect and are saved as you make them.
- **CHANGE ROOM** is on the multiplayer page while you host, and APPLY closes the menu when you enter the new room.
- **The PC spectator's win screen** shows the bot's whole guitar and both its hands.

## Fixed
- **From spectating, START PRACTICE and hosting a room work** (you stayed a spectator), and the spectator's page has
  HOST A ROOM.
- **PC spectator:** on the VS screen and the win screen a player's body no longer disappears, leaving only the
  gauntlets, while the camera is on that player's eyes.

## PICO
- **A build for PICO headsets** (PICO 4, 4 Pro and 4 Ultra): `ProjectArena-0.3.26-pico.apk`, on GitHub and SideQuest,
  to install by hand. It is the same game, in the same online rooms as Quest and PC. PASSTHROUGH isn't in it yet, and
  on PICO you recenter with B. It is our first PICO release: tell us how it runs. PICO's own store is to follow, after
  PICO's review.

0.3.14 to 0.3.26 play together online. From December 2, versions before 0.3.20 no longer play online: update to keep
playing.
