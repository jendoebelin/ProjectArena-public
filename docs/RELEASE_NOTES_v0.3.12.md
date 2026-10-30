# Project Arena 0.3.12 (ALPHA test build)

Store build: versionCode 16, versionName 0.3.12. Everything in 0.3.11, plus:

## New
- **TRAINING** (main menu): lessons in three tracks, each opening one by one as you pass them:
  - **THROWING**: FIRST THROWS and PICK YOUR SPOT (targets floating near and far, high and low, a few at a time),
    POWER (only fast balls break the targets), BANK SHOTS (off a side wall), CURVE (through a ring, then into its
    target), MOVING TARGETS, STRIKE ZONE, and CATCH;
  - **BLOCKING**: SHIELD, SHIELD ANGLES, KNUCKLE GUARDS, SHIELD OR FIST, and SEND IT BACK (block or punch the throws
    into targets);
  - **DODGING**: SIDESTEP, DUCK, LEAN, and EVERYTHING.

  Your best time and accuracy are kept, and go to worldwide **leaderboards**: the fastest and the most accurate for
  each lesson, and an overall board (every lesson's best accuracy added up). SHOW ME HOW videos of real runs come in
  an update.
- **EXPERIMENTAL: FOUR CORNERS**, a room for four (PRACTICE > EXPERIMENTAL > ROOM: FOUR CORNERS). A square glass room
  turned 45 degrees, a floating platform in each corner, everyone facing the centre. Your ball flies through the
  platforms; past your platform's front edge a throw recharges it; the corner behind you is your back wall. Choose
  the TEAMS:
  - **FREE-FOR-ALL**: everyone for themselves, in blue, orange, green and violet.
  - **2 V 2**: partners in opposite corners, so you face both opponents, one to each side.
  - **3 V 1**: THE ONE against the other three. THE ONE's hits count double and their shield never breaks; the
    three share one score. In practice you are THE ONE; online a different player takes the part each match.

  Your ball passes through your teammates. Basic's clock (3 minutes, or FREE PLAY): the most points wins, and a tie
  goes to sudden death. No fist bump: the countdown starts the match. The usual options: shields or HOTDOG, knuckle
  guards, match time. In practice you play three bots.
  The room leans toward whoever leads: their corners pulse, the grid takes on their colour, and a new leader gets a
  chime and "ORANGE LEADS" (or "YOU LEAD").
- **The EXPERIMENTAL rooms online**: open your arena with EXPERIMENTAL to host DIAMOND or FOUR CORNERS. In FOUR
  CORNERS up to four people play, with bots in the empty corners when BOT FILLS IN is on; everyone else waits their
  turn as usual. Spectators can watch both rooms.
- FOUR CORNERS is brand new and may need work. Tell us how it plays (REPORT A BUG, or the Discord): your feedback
  decides which team modes stay.

## Changed
- **The slap shot**: PRO and better bots catch their returning ball on the swing and send it straight back, no hold
  and no wind-up (more often the better the bot). ROOKIE and AMATEUR don't.
- **Bot level in rooms**: when BOT FILLS IN is on, a room's MATCH OPTIONS let the host set the bot's level (FOUR
  CORNERS too).
- **Spectating the new rooms**: the spectator hub shows DIAMOND and FOUR CORNERS matches as their own rooms; the PC
  spectator's banner and win screen show all four players (or both teams).
- **Passthrough**: GLASS WALLS and COURT FLOOR work in the experimental rooms too.
- **Fixed**: the waiting area's JOIN QUEUE kiosk faced away from you and couldn't be clicked; leaving someone's
  room could leave you with their EXPERIMENTAL room and teams in your own practice.
- **The orange player's sounds**: in an online match the player on the orange end heard the other player's hit and
  win/lose sounds, with left and right swapped. Fixed.
- FOUR CORNERS matches don't count in careers (careers compare one-on-one matches).

## Online
0.3.12 and earlier versions don't play together online: everyone needs to update.

## Your data
TRAINING sends each lesson's best time and accuracy, with your player name, to the official server for the
leaderboards (kept under the same scrambled key as your statistics; DELETE MY STATS erases them too). TEAMS, the
EXPERIMENTAL room and your TRAINING bests are kept on your device. Details: the privacy policy,
https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
