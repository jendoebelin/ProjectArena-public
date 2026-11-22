# Project Arena 0.3.24

## New
- **My looks**: CUSTOMIZE AVATAR keeps up to three looks of your own. **SAVE** puts the look you have on into a slot,
  **WEAR** puts it back on (SAVE AND APPLY keeps it, CANCEL goes back), **CLEAR** empties the slot. Saving over a look
  or clearing one asks SURE? first. Each slot is named by its country, or else its suit.
- **Mutes that last**: when you **MUTE** someone on REPORT PLAYER, they stay muted whenever you meet them again, even
  under a new name, until you unmute them. **SETTINGS > VOICE AND MUTES > MUTED PLAYERS** lists everyone you've muted,
  each with **UNMUTE**. Your mutes are kept on your headset only; nobody is told.
- **NEW BOT**: a new bot, its outfit and its stance (BOXER, KARATE, MANTIS, ...), **EVERY MATCH** or **ON PUSH**
  (the option under its level, in practice or as a host on the room's page), and **NEW BOT NOW** for a new one at
  once. Every time the game starts, a new bot. In a room, everyone sees the host's bot as the host has it. In FOUR
  CORNERS and FIREBALL BLITZ each bot gets its own.
- **FIREBALL BLITZ in teams**: its page has **TEAMS**: FREE-FOR-ALL, 2 V 2 or 3 V 1 (the same setting as FOUR CORNERS').
  Your team's fireballs pass through you, **THE ONE** starts with three times the HEALTH, and the last team standing
  wins. Online, everyone in the room needs 0.3.24 (with an older player in it, everyone for themselves). A team with
  nobody in it plays as everyone for themselves too, and the host is told.
- **Hear the ball coming**: every ball and fireball that can hurt you makes a soft rush of air as it flies, louder
  as it comes at you, on your left or your right as it is (not your own or your team's). **SETTINGS > VOICE AND
  MUTES > BALL SOUND** turns it off.
- **Music**: a quiet background track while you're in the lobby and the main menu. **SETTINGS** has a **MUSIC**
  row at the top: its volume (30% to start), **MENU** (on: in the lobby) and **GAME** (off: turn it on to have the
  music while you play, watch or train).
- **MY BESTS**: the main menu shows your TRAINING bests on a board to its left: your TRAINING SCORE, the lessons you've
  passed, and your best time and accuracy in each, with a **TRAINING** button. They're kept on your headset.
- **Three PC spectator cameras that follow the action**: **TRACKING** (T) slides along the side with the play,
  **BALL CAM** (B) runs low beside the glass with the ball, and **UNDER FIRE** (U) looks over the shoulder of the
  player the ball is coming at, for blocks and knuckle punches. They work in every arena (in FIREBALL BLITZ they follow
  the fireballs), sit in Spectator Controls beside the other cameras, and are in the AUTO list unticked (AUTO plays as
  before until you tick them).
- **Notifications from The Crowd**: tap the bell on crowd.aftersparc.com to get a notification on your phone or
  computer when someone hosts an arena or a match starts (or only when a match starts), at most once every 30
  minutes, hour or 3 hours, with quiet hours (10 PM to 8 AM to start). No sign-in; turning them off deletes them. On
  iPhone, add The Crowd to your home screen first.

## Better
- **BALL LOOK-AHEAD in FOUR CORNERS and FIREBALL BLITZ**: in someone else's four-player room the balls and fireballs
  are drawn where the host will have them, as in a one-on-one: your throw leaves your hand at once, a grab is in your
  hand at once, and a block lands where you see the ball. Needs a host on 0.3.24 (an older host's room: as before).
  On Quest the setting is now BALL LOOK-AHEAD (ONLINE).
- **Four-player matches on The Crowd** (crowd.aftersparc.com): FOUR CORNERS and FIREBALL BLITZ show all four players,
  their points (or health, and who is out), and everyone's statistics for the match. In 2 v 2 and 3 v 1 fans cheer
  for a team (FIREBALL BLITZ in teams too). (Hosts need 0.3.24.)
- **Your countries first**: the COUNTRY row lists the countries you choose most at the top, then all the others from A
  to Z (A to Z the first time).
- **PC VR runs at a higher priority** (a community request): while you play in VR on a PC, the game asks Windows for
  ABOVE NORMAL priority, so other programs get in its way less. The spectator window stays at normal priority, so OBS
  keeps up. Start the game with `--priority normal` or `--priority above` to choose yourself.

## Changed
- **The Crowd's cheers make no sound in the arena** any more: the fans' flags and the flaring lights at the ends show
  them. SETTINGS > CROWD is now FLAGS AND LIGHTS or OFF.
- **The lobby**: the main menu stands farther out on the balcony, with MY BESTS folded out on its left and your avatar,
  smaller, on its right, both turned toward you. The posters around the balcony are gone (the main menu has all of
  them); point at the court and pull the trigger to play the bot, as before.
- **A smoother start screen**: the lobby with the main menu open now runs at the headset's full frame rate (it could
  drop to about 70 frames a second), and all text in the game takes less for the headset to draw.

## Fixed
- **MOVE PLAYERS in FIREBALL BLITZ** shows THE ONE, MAKE THE ONE and the teams only when the room plays in teams (it
  went by the teams last set for FOUR CORNERS).
- **Four-player rooms:** a bot in the third or fourth corner is named BOT (on The Crowd and over its head), not by an
  end's colour.
- **GRAPPLER, ARC CLAW and REGALIA:** their glowing tether ends at the ball in your palm, not in a long beam 25 cm
  past your hand.

## The server
- Its data (statistics, leaderboards, SOUND STUDIO votes and reports) is now backed up once a day, encrypted, and the
  last 14 days are kept. If you delete your statistics, they stay deleted even if a backup is ever restored. The
  privacy policy says how.

0.3.14 to 0.3.24 play together online. From December 2, versions before 0.3.20 no longer play online: update to keep
playing.
