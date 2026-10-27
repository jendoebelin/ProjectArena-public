# Project Arena 0.3.9 (ALPHA test build)

Store build: versionCode 13, versionName 0.3.9. Everything in 0.3.8, plus:

## Fixed
- **Statistics: matches with a returned ball now count.** A point scored with a ball you blocked or punched back
  made the server refuse the whole match, for both players. Now:
  - **THROW ACCURACY** counts only your own throws, each one once.
  - A point from a returned ball counts as a hit, and also under **FROM RETURNS** (a new tile in MY STATS, and on the
    screen over the court).
  - Matches already counted keep their numbers.
- **Voice turns on after a late yes.** If you answered the microphone question late (or allowed it later in the
  headset's settings), voice stayed off until you restarted the game. The game now checks again every second. The
  HUD says **MIC NEEDS PERMISSION** until it is allowed, and **MIC NOT AVAILABLE** if the microphone won't open.

## Changed
- The career lines on the screen over the court shrink to fit, so big numbers no longer run off the edge. Catches
  now sit next to blocks.

## Thanks
- **@astrobleem**: found both bugs by reading the code, with exact traces (GitHub #14 and #15).

## Your data
The server now also keeps how many of your hits came from returned balls; nothing else new is collected. Details:
the privacy policy, https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
