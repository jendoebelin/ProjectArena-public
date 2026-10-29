# Project Arena 0.3.11 (ALPHA test build)

Store build: versionCode 15, versionName 0.3.11. Everything in 0.3.10, plus:

## New
- **EXPERIMENTAL: the DIAMOND room** (PRACTICE > EXPERIMENTAL). The court rolled 45 degrees along its length: no flat
  floor, corners straight up, down and to each side, 19 m end to end.
  You each stand on a floating platform; the ball flies through the platforms and the V below them, so it can come up
  at you from underneath. No foul zone. Basic's rules by default, with the usual options (shields or HOTDOG, knuckle
  guards, match time). Practice against the bot for now; online rooms are next.
- **FIELD OF VIEW** (SETTINGS > GRAPHICS AND VIEW): how big the arena feels, from -20% to +20%. The headset's lenses
  set your real field of view, so this changes the eyes' separation: lower and the arena feels bigger, higher and it
  feels closer. Your hands, reach and the court stay where they are. The arena shows around the page while you set
  it. HIT EFFECTS moved to this page.
- **The fist bump has a meeting point**: a ring for each player over the centre line. Hold a closed fist in your ring
  to offer it (it lights up), or meet the other player's offered fist there; the rings glow stronger as the fists come
  together. A bump only counts in the rings, one fist offered first, so nobody can force the start. The bot meets
  your fist, or offers its own and waits.
- **Team lighting**: each half of the court glows low in its player's colour (a country's with NATIONS).

## Changed
- **The bot**: its elbows stay down in every stance; it stands still at the fist bump (its arm no longer clips into
  its body).
- **The ball**:
  - a ball landing on the foul line's edge no longer bounces straight up and between the floor and the ceiling;
  - a ball that gets nowhere along the court for 2 seconds, or slows to a crawl, comes home to you;
  - your own throw into your own foul zone comes home faded, with FOUL shown where it landed, at its own speed;
  - your ball blocked into the other player's own foul zone gives them a strike and comes back at you, live (catch it
    or move), as from their back wall.
- **NATIONS**: the flag on the back of the jersey sits high, under the collar; the blank panel below it is gone.
- **CHANGE NAME** on the main menu opens the keyboard again (SETTINGS > SERVER too).

## Thanks
- **astrobleem**: CHANGE NAME showing no keyboard (bug report).
- A player's idea: limit the bounces when a ball gets stuck in a corner.

## Your data
The game now sends its version number to the official server when it connects, written in the server's log next to
your player name, so we can see who needs an update. FIELD OF VIEW and the EXPERIMENTAL choice are settings on your
device. Details: the privacy policy,
https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
