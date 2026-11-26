# Project Arena 0.3.26.1

A fix for 0.3.26, on Quest, PC and PICO.

## Fixed
- **TRAINING's leaderboards after a lesson**: finishing a lesson and opening its leaderboard from the result card
  said NO ANSWER (THE SERVER MAY NOT HAVE LEADERBOARDS YET). The game stopped talking to the community server during
  a lesson, and the server let the connection go after 10 seconds. It now stays connected through the lesson: your
  result reaches the server as soon as you finish, and the leaderboard answers on the result card. (Opening the
  leaderboards from TRAINING's page always worked.)

Nothing else changes on Quest and PC: the rules, the rooms and the settings are 0.3.26's.

## PICO
- `ProjectArena-0.3.26.1-pico.apk` on GitHub and SideQuest, with the same fix. PICO's own store gets it as well.
- New on PICO: **SETTINGS > GRAPHICS AND VIEW > RESOLUTION** has **2X** and **2.25X**, as a test (asked for by a
  PICO 4 player). If the game stutters, turn it down.

0.3.14 to 0.3.26.1 play together online. From December 2, versions before 0.3.20 no longer play online: update to
keep playing.
