# Project Arena 0.3.8 (ALPHA test build)

Store build: versionCode 12, versionName 0.3.8. Everything in 0.3.7, plus:

## New
- **MATCH OPTIONS for hosts.** After choosing Basic or Advanced, the host sets:
  - **SHIELDS ON / OFF.** No shields is **HOTDOG MODE**, the community's name for it: dodge, catch and punch.
  - **KNUCKLE GUARDS ON / OFF**, in either rules.
  - **MATCH TIME** (Basic): **FREE PLAY** (no clock and no winner: just play; when others are waiting, the board says
    so, and forfeiting hands over), 1, 2, 3, 5, 10 or 15 minutes.
  - **ROUND TIME** (Advanced): off (as before), 1, 2, 3 or 5 minutes. At time, more points wins the round; a tie goes to
    sudden death.
  Only the host can change them (SETTINGS > RULES while in your arena); a change during a match starts with the next
  one. The games list, the board over the court, the PC spectator and The Crowd show the options in play.
- **Hosts can remove a player** from their arena (REPORT PLAYER > the person > REMOVE FROM ARENA). Everyone else can
  still VOTE TO REMOVE.
- **Statistics by mode:** careers count every match, and also keep standard, HOTDOG and other custom matches apart.
  The Crowd shows each player's HOTDOG record.
- **The Crowd:** in HOTDOG matches the cheers burst out as hot dogs and smiley faces. A player without a country gets
  **foam fingers** in their end's colour, waving in the stands and bursting out with the cheers.

## Changed
- **A foul ball stays in the arena.** It bounces off the wall it was heading for and flies back to its launch spot
  (it can't hit anyone on the way). A ball that never crossed the foul line springs straight back to its float
  position, instead of rising from under the floor.
- **SETTINGS > TURNING: SNAP, SMOOTH or SMOOTH FAST** for spectating (each player chooses).
- **Avatar builder:** **SAVE AND APPLY** saves your look (it shows on you as you change it), and **CANCEL** puts back
  the look you had.
- **Push-to-talk suits your controller:** Quest Touch, the left grip as before; Valve Index, a firm squeeze of the left
  grip (just holding the controller no longer opens the microphone); Vive and Windows Mixed Reality, the left grip
  click.
- The cheer bursts' pictures load when the game starts, so nothing loads in the middle of a match.
- SETTINGS > CROWD: FULL, FLAGS ONLY or OFF.
- **The arena:** stairs up aisles through the stands (soft blue and pink, with glowing step edges) show how the fans
  got to their seats; the walls around the court are smooth panels instead of light slats; the country flags on the
  walls sit between the pillars, and the court's rings show in front of the flag behind you.

## Thanks
- **@R0gueICE**: the walls' light slats looked tacky (GitHub #1: now smooth walls), smooth turning and the avatar builder's apply button (reports from the headset), the frame-time spikes
  (GitHub #8), and push-to-talk on Index controllers (his game logs showed it).
- A player's request: turning shields off before a match (now MATCH OPTIONS).

## Your data
Statistics are now also kept per kind of match (standard, HOTDOG, custom); nothing else new is collected. Details:
the privacy policy, https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
