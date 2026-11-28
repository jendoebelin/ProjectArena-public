# SideQuest listing (app 63840)

Released on SideQuest 2026-11-08 (Early Access off), ahead of the Meta Horizon Store launch, for a SideQuest promotion.
SideQuest installs the GitHub build, so its release doesn't depend on Meta's review.

- **Banner** ("app banner"): the long side of the arena, an empty court with no bots or avatars
  (`build/store_art/Untitled.png`, 1278x510; the owner, 2026-11-08: "I like having the arena long side"). It replaced
  `library_hero.png`, which had bots at the ends. A real-footage alternative exists, unused:
  `build/store_art/sidequest_banner.png` from `tools/gen_store_art_real.py`.
- **App icon** (square, at least 512x512; the API's `image_url`): `cover_square.png`. The search results show it in
  a 16:9 tile (283x159, CSS background-size cover, centred: only rows 315..1125 of a 1440 square), so the title at
  its foot was cut off. **Now `build/store_art/sidequest_icon.png`** (uploaded and checked pixel for pixel
  2026-11-08): the landscape cover in the middle band with the text-free picture mirrored and darkened above and
  below, so search shows the whole cover. Made by `tools/gen_store_art_real.py`.
- **Banner field** asks for 16:9, at least 1280 px wide; it isn't shown on the web app page (the SideQuest apps use
  it). The owner's empty court is 1278x510, so a 16:9 render of the same view (2560x1440) would fit it exactly.
- **Screenshots:** `build/store_art/screenshot_1..5.png`, in that order (done 2026-11-08).
- **Trailer:** the launch trailer, https://www.youtube.com/watch?v=YuG306OO5O0 (public; the alpha video is private
  now, so the old link shows "Video unavailable").
- **Summary** (2026-11-24, PICO added): Project Arena is an open-source VR court sport for Meta Quest, PICO and PC VR where players
  throw, curve, and punch energy balls in fast-paced competitive matches.
- **Search tags** (unchanged): multiplayer, fitness, vr_sports, cross_platform.

## Description (2026-11-08; the earlier text left out the modes added since and ended "currently in active development"; 2026-11-19, live: the SQUARE and DIAMOND lines without Basic, Advanced or EXPERIMENTAL; 2026-11-21, for 0.3.24: FIREBALL BLITZ in teams, the ball heard in flight, NEW BOT, lasting mutes, MY LOOKS, the country kit, twelve camera views, The Crowd's four players and notifications, and no crowd noise in the arena any more; 2026-11-24, for 0.3.26 (the owner: "add PICO and the 0.3.26 features to the description"): PICO beside Quest and PC VR, the livelier bots, views while waiting, JUMP IN, OR SET IT UP YOUR WAY (simple setup, custom settings, FULL EFFORT, bounces in FOUR CORNERS, save slots), the PC spectator's one scoreboard, HAND POSITION, the bots' moves in the asset catalog)

```
Project Arena is a free, open-source VR court sport for Meta Quest, PICO and PC VR built around fast reactions, physical movement, and competitive play.

Step into a neon glass arena and face another player across the court. Throw energy balls with speed and precision, put curve on your shots, block incoming attacks, punch balls back across the arena, and outplay your opponent through positioning, timing, and reflexes. Every ball that can hurt you can be heard as it flies at you, on your left or your right. It's a real workout: you'll be ducking, leaning and sidestepping from the first point.

Play against bots from ROOKIE to UNBEATABLE, with a new bot of its own look and fighting stance every match, or against other people. The bots feel alive: between shots they breathe, shift their weight and glance at you, they taunt you if you hold your ball too long, and they celebrate their points and wins, one of them on air guitar. Multiplayer works on your local network or online through the community server, with Quest, PICO and PC VR players in the same rooms, waiting areas, player queues, voice chat with mutes that last, and spectating. While you wait your turn, you can watch from your spot, from two raised views outside the glass, or from high above the court.

## GAME MODES

- The SQUARE: the classic 1-v-1 court, a timed match or rounds
- FIREBALL BLITZ: up to four players, bouncing fireballs, health bars, free-for-all, 2 v 2 or 3 v 1
- FOUR CORNERS: free-for-all, 2 v 2 or 3 v 1
- DIAMOND: the court rolled 45 degrees
- TRAINING: throwing, blocking and dodging lessons with worldwide leaderboards, and your own bests in the lobby
- SOUND STUDIO: try the community's sounds, vote on them, or play your own

## JUMP IN, OR SET IT UP YOUR WAY

Every room opens on a simple setup: a few big buttons for the options that matter most, and START. When you want more, CUSTOM SETTINGS has everything:

- Shields, rounds and match length
- Lively walls, wall grip and bank assist, in the SQUARE, DIAMOND and FOUR CORNERS
- Throw power and knuckle returns, up to FULL EFFORT, where only a real full swing and a real punch reach top speed
- Three save slots for each kind of room, so your favourite setups are a press away

When you join someone else's room, the game tells you what is different about its rules.

## BUILD YOUR PLAYER

Create your own arena identity with an avatar system featuring two body types and a growing collection of customization options, including:

- Outfits and shorts
- Hair styles
- Headgear
- Face shields
- Masks
- Gauntlets
- Colors and patterns

Choose between different plate gauntlets, including Striker, Titan, and Phantom, or use the classic controller-glove style.

Save up to three complete looks of your own and switch between them.

## REPRESENT YOUR COUNTRY

Project Arena includes team presets for 202 countries.

Choose a country and your player receives a matching team kit with the national flag printed on the jersey, or keeps your own outfit in your country's team colors. Your side of the arena also changes with you: your court end, strike zone, energy balls, and arena flags take on your country's colors.

This makes international matches immediately recognizable both to the players and to spectators.

## BUILD A CAREER

Online matches between two human players contribute to persistent career statistics.

Track performance such as:

- Wins
- Throw accuracy
- Blocks
- Strikes
- Top ball speed
- Other match statistics

Players can view their career from the in-game MY STATS screen, while spectators can see player information and head-to-head presentation from the PC spectator view.

## SPECTATING AND STREAMING

Project Arena was designed not only for the people playing, but also for people watching.

The PC version can launch without a VR headset as a dedicated spectator system. It can automatically locate a live match and provide multiple camera views, including:

- Twelve camera views, three of them following the action (TRACKING, BALL CAM and UNDER FIRE)
- Automatic camera switching
- Free camera movement
- One scoreboard, where you want it: a banner across the top, a corner, or the arena's own jumbotron
- A separate control window that stays off the broadcast

A dedicated streaming kit is also available for creators using OBS. It includes animated backgrounds, camera guides, transparent spectator-view instructions, VS presentation screens, and win-screen resources.

## THE CROWD

People don't even need a headset to participate in a live Project Arena match.

Friends, viewers, or stream audiences can open The Crowd from a phone and follow an online match live without installing an app or creating an account.

They can see:

- Live score
- Match clock or round
- Player countries and flags
- Throws
- Accuracy
- Hits
- Strikes
- Blocks
- Catches
- Top speed
- Player career statistics
- All four players in FOUR CORNERS and FIREBALL BLITZ

Fans can cheer for either side, wave flags, and predict who will score next. They can also tap the bell to get a notification on their phone or computer when someone hosts an arena or a match starts.

Those reactions are reflected back inside the VR arena. Arena lighting flares with bursts of excitement, flags wave in the stands, and visual cheer effects rise from the crowd. Players and spectators can see the audience becoming part of the match.

## BUILT FOR VR FROM THE GROUND UP

Project Arena runs natively on Meta Quest and PICO (PICO 4, 4 Pro and 4 Ultra) using OpenXR and OpenGL ES and was built without a traditional game engine. The PICO version is new: install its APK from here, and tell us how it runs.

The PC version supports VR through SteamVR or Quest Link.

Your hands in the game can be lined up with your real ones: CONTROLLER ANGLE turns them and HAND POSITION moves them, a centimetre at a time.

The project is also fully open source. Players and community members can inspect the code, report bugs, contribute ideas, create artwork, make sounds, design assets, or submit code changes.

Project Arena includes an asset catalog containing models, textures, sounds, the bots' moves, downloadable source assets, and templates for people who want to create their own contributions.

## FREE, AND STILL GROWING

Project Arena is free to play, with no ads. It keeps growing through frequent updates with new gameplay, multiplayer, customization, spectator, streaming, statistics, and community features, shaped by feedback from players.

Enter the arena, choose your identity, and see how well you can throw, curve, block, punch, and react when another player is standing across the court.
```
