# Project Arena 0.3.13 (ALPHA test build, in progress)

Not released yet. Everything in 0.3.12, plus (so far):

## New
- **EXPERIMENTAL: FIREBALL BLITZ**, a room for up to four (PRACTICE > EXPERIMENTAL > ROOM: FIREBALL BLITZ). An
  octagon with its floor level with yours. Every throw is a **fireball**: it drops to the floor and hops along it up
  to your waist, and fizzles out at its fourth bounce. Hits count from your hips up: step aside, duck as it rises, or
  punch it away with your knuckle guards. One coming at you turns red, with a red ring at your feet, and a card before
  each match shows where you can be hit and how to get away.
  - **Health**: everyone has a health bar (over their head; yours in front of you). A hit takes one; at none
    you're out, and you watch from your place. The **last one standing wins**.
  - **The room closes in**: when someone is knocked out, the walls warn you, and two seconds later close in and
    everyone left is moved round a smaller ring (a quick blink takes you to your new place).
  - **MATCH OPTIONS**: HEALTH 1, 3, 5 or 10 (5); KNUCKLES (on) and SHIELDS (off); BALLS: ONE (thrown from either
    hand), TWO, THREE or **RAPID FIRE** (up to three of yours in the room, the tee refilling at once); MATCH TIME (at
    time the most health wins, a tie goes to sudden death at one hit each); in practice PLAYERS 2 to 4 (you and bots).
  - The bots throw fireballs aimed to hop into you, and step out of the way of yours (better the higher their level).
  - Online for up to four, with bots in the empty places when BOT FILLS IN is on; spectators can watch it.

- **Discord status (PC)**: with the Discord app running, your profile shows **Playing Project Arena** (or **Project
  Arena - Spectator** while you watch), with what you're doing, the rules, the time and the room's size. Off in
  SETTINGS > GRAPHICS AND VIEW > DISCORD STATUS.
- **The PC spectator view: MY CAMERAS.** Ctrl + 1 to 8 saves the view as one of your cameras, Shift + 1 to 8 shows
  it, and AUTO goes round them (each its own hold, a glide or a cut; action cameras jump to the hits near them,
  follow cameras slide with the ball). The same list as the headset's director.
  **6 / 7: Blue's and Orange's eyes**: each player's own view from their headset, smoothed for the audience (AUTO
  includes them).

- **SELFIE (PC spectator view)**: your webcam in a corner of the picture, for streamers talking to their audience.
  C (or the Selfie button) turns it on; the Spectator Controls window picks the corner, the size, round or square, and
  the camera. It stays on your PC.

- **The version** in the bottom corner of the main menu and SETTINGS (and the PC spectator's hub).
- **UPDATE NOW**: when a newer version is out, the main menu's UPDATE AVAILABLE notice has an UPDATE NOW button.
  - From the Meta store: it opens the game's store page.
  - Installed with SideQuest: it opens the GitHub page for the new APK.
  - PC (the zip from GitHub): it downloads the new version, checks it, and once you say so closes the game, puts the
    new version in place (your settings stay) and starts it again. A copy in a protected folder opens the download
    page instead; Steam copies update through Steam.

- **ROOM TALK: PRIVATE** (SETTINGS > VOICE AND MUTES, for rooms you host): spectators still see the match but don't
  hear the room's voice chat, so a team can talk strategy where rivals can watch. The players and those waiting
  still hear each other.
- **SELFIE in the headset** (spectators): the spectator menu's SELFIE puts a camera in front of your avatar and its
  picture in the lower right of your view, so a cast or recording of your headset shows you as you talk.
- **The room outlives its host.** If the host's headset goes to sleep, dies or loses its connection, the room no longer
  ends for everyone: after about 5 seconds the person who has been in it longest becomes the host, and everyone else
  follows. The room keeps its name, rules and settings. A match being played pauses and picks up where it was: the
  score, the clock and the round stay; balls in the air go back to their throwers, and a 3-second countdown restarts
  play. (If the old host was one of the players, the match starts again, as it always does when a player leaves.)
  When the old host's headset wakes up, it rejoins the room as a player in the waiting area.
- **HAND OFF HOST** (the menu, while you host): pick anyone in the room to host it from now on. You stay in it, in
  your place; a match pauses for a moment, then goes on. **LEAVE GAME** while hosting now hands the room to the next
  person instead of closing it.
- **Idle rooms**: a player whose headset sits still (no movement, no buttons) for 2 minutes moves out of the tank to
  the waiting area, so the match isn't stuck waiting for them. A room where nobody has moved for 10 minutes closes,
  after a warning a minute before (ROOM CLOSING - MOVE TO STAY). Spectators don't keep a room open.

## Changed
- **CHAMPION and UNBEATABLE play to attack.** They throw much sooner after taking their ball, and slap nearly every
  returning ball straight back (UNBEATABLE 90%, CHAMPION 80%). Their plan:
  - **strikes first**: through the strike ring beside you, left side then right side in turn (curved round you when
    your body is in the way), and their throws go exactly where they aim;
  - about **one throw in seven at you**, aimed at where you usually stand;
  - once they hold **3 strikes** (a big, fast ball), nearly every throw is at you: mostly straight, some off a side
    wall.

  Every throw has a shape, picked for it: straight, **curved** round you, **off a side wall** (the near one or across
  off the far one), **curved off a wall**, or **off the floor**. Each is tried out as it would fly before it goes,
  slap shots too, so they land; at you, straight throws stay the most common.
  ROOKIE to ALL-STAR play as before. (UNBEATABLE was never hit by our test player's 1200 hard throws; attacking like
  this, in Basic it now is, a few times: its curves and banks come back slower, leaving it without its shield longer.) A slap shot never goes while a ball is coming at the bot (its shield would go
  with the ball).
- **The foul rule for a blocked ball**: your opponent's ball that you block or punch into your own end only counts
  against you if it lands on your end's floor (their strike, and it goes back to them), and without a FOUL sign; off
  your end's walls or ceiling it just bounces.
- **3 V 1: who is THE ONE.** In practice you always are (a value left from hosting could change that). Hosting,
  MATCH OPTIONS' TEAMS button now goes FREE-FOR-ALL, 2 V 2, **3 V 1: YOU ARE THE ONE**, and **3 V 1: ONE TAKES
  TURNS** (a different player each match, as before; it used to start with the player after the host).
- **FOUR CORNERS' floor** is 1 m below the platforms (it was 3 m: throwing felt like throwing down from a height).
- **MOVE PLAYERS** (hosting a room for four): SETTINGS > MATCH OPTIONS > MOVE PLAYERS lists the four corners and
  who plays each (or the bot). Point at one corner, then another, and they swap; the teams go with the corner (2 V 2:
  corners 1 and 2 are blue, 3 and 4 orange), THE ONE goes with the player, and in 3 V 1 MAKE THE ONE picks THE ONE.
  The match starts again. Players now keep their corners when someone leaves (a bot takes the empty one).
- **"Sudden death!"**: a tie at time (Basic, Advanced's timed rounds, FOUR CORNERS, FIREBALL BLITZ) is now called
  by a player's voice.
- **FIELD OF VIEW**: a wider range, from -50% to +50% in 10% steps.
- **The voice hint by your left hand** (HOLD LEFT GRIP TO TALK) shows only for your first 10 seconds in a room, and
  never while spectating; TALKING and microphone problems still show.

## Fixed
- **Spectator hub**: the miniature arenas sit on their tables again. The table's post came up through the middle of
  the play, and DIAMOND sank into the table.
- **FOUR CORNERS waiting area**: a deck with a rail and a spot for each waiting player, outside the glass at the
  platforms' height (you used to wait in mid-air beside the lowered stands).
- **Joining a host's EXPERIMENTAL room**: the host saw a joining player inside the arena, at the waiting ring; they
  now appear where they really stand.
- A DIAMOND match in Advanced shows DIAMOND on its spectator hub label.
- **BOT FILLS IN: OFF** is remembered after a restart (it showed OFF, but rooms still filled in with the bot).
- REPORT OR MUTE A PLAYER lists all four players of a FOUR CORNERS or FIREBALL BLITZ match (it listed two).
