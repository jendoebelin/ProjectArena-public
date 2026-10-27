# Tuning the practice bot from real play

The practice bot's levels (`app/src/game.cpp`, `kBotSkills`; what each number means: `BotSkill` in `game.h`) set how
fast it reacts, how fast its hands and body move, how hard it throws and how often it does something about a ball.
They started as estimates; recordings of real matches show what real players do, so the levels can match them.

## 1. Record

On the headset (or PC): **MENU → MY STATS → RECORD MY MATCHES: ON**. Every match you play is then saved, from the
3-2-1 to the end, in the game's folder: `recordings/match_YYYYMMDD_HHMMSS.par` (about 5 MB for 3 minutes; the newest
20 are kept). Only your own movement is kept (with a bot opponent, its movement too); `app/src/record.h` has the
file's layout. A tester who wants to help sends their files by hand (the privacy policy says how they are used).

Practice against the bot at a set level, against MATCH MY LEVEL, and online matches all record the same way.

## 2. Copy them off

Quest (from the repository folder, with the headset connected):

```
adb pull /sdcard/Android/data/com.projectarena.game/files/recordings recordings_quest
```

PC: `%APPDATA%\ProjectArena\recordings`.

## 3. Analyze

```
python -I tools/analyze_play.py recordings_quest
python -I tools/analyze_play.py recordings_quest --json player.json
```

It reads every recording in the folder and prints, for the person who recorded them:

- **THROWS**: speed (and the top tenth), the hand's top speed in the swing, how many were curved or off a wall;
- **DEFENDING**: of the balls that came at them, how many they took on the shield, punched, dodged or were hit by,
  and their **reaction**: how soon after the throw their head or a hand starts to move (median, and their quickest
  quarter);
- **MOVING**: how fast their hands and head move (the top 5%), how far they reach, lean and duck; catches;
- **their level from their results** against the bot: each match, the bot's level moved up or down by how the
  points went (winning by as many as were scored is two levels up), weighted by the points played. This is the guide
  to trust: a skilled player often moves little (the owner's first recordings: a level of 2.6 from results, but
  ROOKIE by speed alone);
- the level their **speeds alone** are nearest, and their numbers as a `BotSkill` row (reaction, defend, handSpeed,
  bodySpeed, throwHand) to compare with or put in the table.

Recordings keep the raw match, so a better analyzer can be run again on old recordings: nobody has to play again.

## 4. Tune

Gather recordings from several players of different skill, line their rows up, and set the levels to match: for
example PRO as the middle player, CHAMPION as the best. The limits every level keeps (reach, a person's top speeds)
are in `Tunables` (`botArm`, `botBodyRange`, ...) and should stay within what the fastest real player shows. After a
change, `tools/run_tests.bat`: the bot's tests check it still moves within those limits, that UNBEATABLE is not hit,
and that MATCH MY LEVEL evens the points out.

## MATCH MY LEVEL

The other way the bot meets real players: on the practice MATCH OPTIONS page, **MATCH MY LEVEL** lets the level
follow the player (`Tunables::botAdapt`, `botMatch`): up 0.15 of a level each time the player scores against it, down
each time it scores, between ROOKIE and UNBEATABLE, blending the levels' numbers in between (`botSkillAt`). It settles
where both score about as often. The level is kept in `settings.cfg` (`botmatch`).
