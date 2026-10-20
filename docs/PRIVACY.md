# Project Arena privacy policy

_Last updated: 20 October 2026. Contact: vrsportsfan@gmail.com._

Project Arena is a free, open-source VR game. This policy explains what the game and the community servers do with
information about you.

**Who is responsible for your data:** Jennifer Doebelin, United States, who runs Project Arena and its official
server. Contact: vrsportsfan@gmail.com.

## What stays on your device

The game keeps these in its own folder on your headset or PC (`%APPDATA%\ProjectArena` on PC):
- your player name;
- your avatar choices;
- your settings and camera positions;
- a diagnostic log;
- a private random player key (made once; it is what your statistics are kept under, below).

They are not sent to us, except in a bug report you choose to send (below). Uninstalling the game removes them on
Quest; on PC you can delete the folder.

## Passthrough and your room (Quest, only when you turn it on)

SETTINGS > PASSTHROUGH shows your room around the court through the headset's cameras. The camera images go from
the headset's system straight to the display; the game never receives them, and nothing is recorded or sent.
In the WINDOW mode on Quest 3 / 3S the game asks for access to **spatial data**: the headset's depth map of your
room, used frame by frame on the headset to hide real things inside the court. It is not stored or sent anywhere,
and you can refuse it (WINDOW then works without hiding them).

## Bug reports (only when you send one)

**MENU → REPORT BUG** sends the official server a report with:
- the category and note you chose;
- your player name, the time, the game version and headset model;
- your network, room and match state, your avatar choices and settings;
- the game's recent diagnostic log (gameplay and connection events; no voice, no camera or room images);
- the internet address it came from (to stop report spam).

Reports are used only to find and fix problems, are kept on the official server for up to 12 months, and are not
shared with anyone else. If a report cannot be sent, it waits on your device and is sent the next time the game
starts.

## Player reports (only when you send one)

**MENU → REPORT PLAYER** (in a room, or at an arena while spectating) sends the official server:
- the name of the player you report, the reason and note you chose;
- your player name;
- the names of the other people in that room;
- the same details as a bug report (above).

The server adds the internet address the reported player is connected from, so a ban can be applied. Only the people
who run the official server see reports and addresses; the reported player is not told who reported them. Reports
are used to keep the game safe (warnings and bans) and are kept for up to 12 months. Banned addresses and names are
kept on the server while the ban lasts.

**MUTE** on the same page silences one player's voice for you only, until you quit the game. Nothing is sent.

## What other players see

When you play online or on your local network, other players in the same arena receive:
- your player name and avatar look;
- your head and hand positions;
- game events.

They can also hear your **voice** while you talk, if voice chat is on and you are in a room with them:
- push-to-talk is the default;
- open mic is optional;
- you can mute spectators, other players or everyone.

Voice is streamed live and is **never recorded or stored** by the game or by the community server. The game asks
for microphone permission the first time you enter a room; you can refuse it and still play.

## Player statistics (online matches against another player)

When you play an online match against another person (never the practice bot), the official server keeps your career
statistics: matches, wins, throws and throws on target, hits, blocks and balls faced, catches, your top throw speed,
the player name you used, and when you were first and last seen.
- They are kept under your game's private player key. The server stores only a scrambled form of that key (a salted
  hash), never the key itself, and no IP address, real name or account.
- They are **public in the game**: other players and spectators see them on the screen over the court and in the
  streaming view, and you see your own in **MENU → MY STATS**. The game tells you this once, the first time you
  enter an online arena room.
- A match counts only when both players' games report the same result.
- The first-seen and last-seen times are used to spot abuse (for example spam, or the same player connected from two
  places at once).
- **MENU → MY STATS → DELETE MY STATS** erases them from the server at any time. Uninstalling the game (or deleting
  its folder on PC) makes a new player key, so a reinstalled game starts new statistics; ask
  vrsportsfan@gmail.com to remove old ones.
- Statistics are kept until you delete them, or for 24 months after you were last seen.

## Community servers

A community server forwards game and voice packets between players. It sees your IP address, as any internet
service does, and does not show it to other players. The official server keeps a short log of when a player name
joins and leaves, to keep the service running and to deal with abuse. It does not store voice or game content.
Community servers run by other people follow their own rules.

## What we do not do

- No accounts (statistics are tied to a random key, not to you).
- No advertising.
- No analytics or advertising tracking (the statistics above are game results shown in the game).
- No selling or sharing of personal information.
- The game does not use your Meta account data beyond what the Meta Horizon Store itself provides.

## How we protect it

- Players never see each other's internet addresses; the server forwards packets only between connected games.
- Statistics are kept under a scrambled form of your game's player key (a salted hash), never the key itself, and
  only the server's own account can read the table. A match counts only when both players' games agree on it.
- Voice and passthrough images are never stored.
- Data travels unencrypted between the game and the server (like most small game servers). It holds nothing secret:
  your player name and game statistics are public in the game anyway, and there are no passwords, emails or payments.
- The full technical list (what, where, who, how long) is `docs/DATA_AND_SECURITY.md` in the game's open-source
  repository.

## Your choices

- Delete your statistics any time: **MENU → MY STATS → DELETE MY STATS**.
- Ask us to delete anything else we hold about you (reports naming you, old statistics from a game you reinstalled):
  email vrsportsfan@gmail.com with your player name. We answer within 30 days.
- Play offline or on your home network (**MULTIPLAYER → ONLINE: OFF**): then nothing reaches our server.

## Children

The game is not directed at children under 13 (or the minimum age in your country).

## Changes

Changes to this policy are published on this page with a new date.
