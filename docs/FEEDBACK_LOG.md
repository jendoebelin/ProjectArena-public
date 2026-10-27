# Community feedback log

Every suggestion and report we receive is discussed before anything changes. This page records what was suggested,
by whom (the name they used: a GitHub or in-game name; never anything more), what we decided, and the version it
shipped in. Thank you to everyone who takes the time to report.

Decisions: **Adopted**, **Partly** (some of it, or in another way), **Planned**, **On the Quest Board** (we are looking
at it), **Mock-up** (pictures first; the owner decides), **Not adopted** (with the reason).

| # | From | Suggestion | Decision | What we did, or why not | Version |
|---|---|---|---|---|---|
| [#14](https://github.com/jendoebelin/ProjectArena-public/issues/14) | astrobleem | Career statistics refused real matches when a returned ball scored | Adopted | THROW ACCURACY counts only your own throws, each once; points scored with a ball you blocked or punched back count as hits and FROM RETURNS (MY STATS). The server checks the new numbers | 0.3.9 |
| [#15](https://github.com/jendoebelin/ProjectArena-public/issues/15) | astrobleem | Voice stayed off if the microphone prompt was answered late | Adopted | The game asks once and checks again every second; a late yes (or one from the headset settings) turns voice on | 0.3.9 |
| [#16](https://github.com/jendoebelin/ProjectArena-public/issues/16) | astrobleem | Recentering: the court can move too early | Planned | Apply the headset's recenter at the time it takes effect; needs testing on Quest and SteamVR |  |
| [#17](https://github.com/jendoebelin/ProjectArena-public/issues/17) | in-game reports | Passthrough didn't turn off; real objects inside the court in WINDOW | Adopted | Passthrough was rebuilt after 0.3.2; checked on 0.3.8: OFF turns it off, WINDOW shows only the game inside the court | 0.3.8 |
| [#18](https://github.com/jendoebelin/ProjectArena-public/issues/18) | in-game report | Hosted room text overlapped the join options | Adopted | The multiplayer menus were rebuilt after 0.3.1; checked on 0.3.8 with a hosted room: no overlap | 0.3.8 |
| [#19](https://github.com/jendoebelin/ProjectArena-public/issues/19) | in-game report | Stuck in a practice match against the bot | Adopted | Menus close when you change rooms and have a LOBBY button; checked on 0.3.8: MENU > LOBBY leaves a practice match | 0.3.8 |
| [#20](https://github.com/jendoebelin/ProjectArena-public/issues/20) | in-game report | Ball physics felt loose | Needs info | What feels off: the throw, the curve, bounces or the return? |  |
| [#21](https://github.com/jendoebelin/ProjectArena-public/issues/21) | in-game report | OBS Game Capture doesn't pick up the PC spectator | Needs info | Window Capture works today; is Game Capture needed? |  |
| in-game report | a player | A second pair of nipples on loose tops | Adopted | The cloth under the bust no longer ends in a hard crease | 0.3.1 |
| in-game report | a player | The gauntlet's wrist doesn't line up with the arm bracer | Adopted | Plate gauntlets' cuffs turn 30 degrees up to meet the forearm | 0.3.1 |
| in-game report | a player | The shield should sit at 90 degrees to how it was | Adopted | The shield sits on the back of the hand, along the forearm | 0.3.1 |
| in-game reports | two players | The fist bump didn't work, and once crashed the game | Adopted | Measured at the knuckles; the crash (an event name table that fell behind) is fixed | 0.3.1 |
| in-game reports | two players | Menus and the director panel stay open between rooms and matches | Adopted | They close by themselves when you move into a room | 0.3.1 |
| in-game report | a player | The ball can't be thrown outside a match | Adopted | Warm-up throws while you wait (nothing scores) | 0.3.4 |
| in-game reports | a player | A fist bump view and a big win screen for the PC spectator | Adopted | Broadcast moments (fist bump close-up, VS card) and WIN SCREEN | 0.3.4, 0.3.5 |
| in-game report | a player | A foul ball should bounce, not dissolve | Adopted | A foul ball stays in the arena and is drawn back home | 0.3.8 |
| [#11](https://github.com/jendoebelin/ProjectArena-public/issues/11) | R0gueICE (and an in-game report) | Smooth turning in the spectator view | Adopted | SETTINGS > TURNING: SNAP, SMOOTH or SMOOTH FAST, chosen by each player | 0.3.8 |
| in-game report | PLAYER 571 (R0gueICE) | An apply button in the avatar builder | Adopted | SAVE AND APPLY (the look shows live as you change it; this saves it) and CANCEL (puts the old look back) | 0.3.8 |
| [#8](https://github.com/jendoebelin/ProjectArena-public/issues/8) | R0gueICE | Frame-time spikes | Partly | Pictures that only appear mid-match (the cheer bursts) now load at start-up. Loading every country flag at start-up took 1.6 s, so flags still load when a player first shows one (before the match). More to look at: on the Quest Board | 0.3.8 |
| from his logs | R0gueICE | (not reported: his logs showed the microphone open all the time) | Adopted | Push-to-talk suits each controller; on Valve Index a firm squeeze, not just holding the controller | 0.3.8 |
| [#11](https://github.com/jendoebelin/ProjectArena-public/issues/11) | R0gueICE | Move the side menu tiles above the main menu | On the Quest Board | We want to be sure which tiles are meant (a screenshot would help) | |
| [#12](https://github.com/jendoebelin/ProjectArena-public/issues/12) | R0gueICE | The body doesn't turn when dodging; ducking and bending; smoother bot movement | Partly | The practice bot now moves as a person can: it steps, leans and ducks out of the way, uses either hand, keeps to an arm's reach; recordings of real matches tune it. The body turning with a dodge is still to do | 0.3.10 |
| in-game report | a player | The avatar's shoulders folded in with the arms raised overhead | Adopted | Collarbones: the shoulders rise with the arms, the elbows turn out | 0.3.10 |
| [#6](https://github.com/jendoebelin/ProjectArena-public/issues/6) | R0gueICE | A country banner separate from the avatar's kit | On the Quest Board | Needs a small network change | |
| [#7](https://github.com/jendoebelin/ProjectArena-public/issues/7) | R0gueICE | Higher process priority on PC | On the Quest Board | | |
| [#10](https://github.com/jendoebelin/ProjectArena-public/issues/10) | R0gueICE | Z-fighting (flicker) on the coloured borders | On the Quest Board | Where exactly? A screenshot would help | |
| [#1](https://github.com/jendoebelin/ProjectArena-public/issues/1) | R0gueICE | The light-slat walls look tacky | Partly | The owner chose smooth walls: the gallery panels round the court are now smooth, softly lit panels (no bars). The banner-wall and wave ideas are part of the look mock-ups | 0.3.8 |
| [#2](https://github.com/jendoebelin/ProjectArena-public/issues/2), [#3](https://github.com/jendoebelin/ProjectArena-public/issues/3), [#4](https://github.com/jendoebelin/ProjectArena-public/issues/4), [#9](https://github.com/jendoebelin/ProjectArena-public/issues/9) | R0gueICE | The arena's look: light lines up the posts and through the crowd, black frame edges and floor, a sky, less grey (and #1's banner walls) | Mock-up | Pictures of the suggested looks come first; the owner decides | |
| [#5](https://github.com/jendoebelin/ProjectArena-public/issues/5) | R0gueICE | The crowd's phone lights are too dim | Mock-up | Part of the arena look pictures | |
| player request | a player | Turn shields off before a match | Adopted | MATCH OPTIONS for hosts: shields (HOTDOG MODE), knuckle guards, match time (incl. FREE PLAY), round timers | 0.3.8 |
