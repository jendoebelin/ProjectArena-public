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
- **Summary** (unchanged): Project Arena is an open-source VR court sport for Meta Quest and PC VR where players
  throw, curve, and punch energy balls in fast-paced competitive matches.
- **Search tags** (unchanged): multiplayer, fitness, vr_sports, cross_platform.

## Description (2026-11-08; the earlier text left out the modes added since and ended "currently in active development"; 2026-11-19, live: the SQUARE and DIAMOND lines without Basic, Advanced or EXPERIMENTAL)

```
Project Arena is a free, open-source VR court sport for Meta Quest and PC VR built around fast reactions, physical movement, and competitive play.

Step into a neon glass arena and face another player across the court. Throw energy balls with speed and precision, put curve on your shots, block incoming attacks, punch balls back across the arena, and outplay your opponent through positioning, timing, and reflexes. It's a real workout: you'll be ducking, leaning and sidestepping from the first point.

Play against bots from ROOKIE to UNBEATABLE, or against other people. Multiplayer works on your local network or online through the community server, with Quest and PC VR players in the same rooms, waiting areas, player queues, voice chat, and spectating.

## GAME MODES

- The SQUARE: the classic 1-v-1 court, a timed match or rounds
- FIREBALL BLITZ: up to four players, bouncing fireballs, health bars, last one standing
- FOUR CORNERS: free-for-all, 2 v 2 or 3 v 1
- DIAMOND: the court rolled 45 degrees
- TRAINING: throwing, blocking and dodging lessons with worldwide leaderboards
- SOUND STUDIO: try the community's sounds, vote on them, or play your own

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

## REPRESENT YOUR COUNTRY

Project Arena includes team presets for 202 countries.

Choose a country and your player receives a matching team kit with the national flag printed on the jersey. Your side of the arena also changes with you: your court end, strike zone, energy balls, and arena flags take on your country's colors.

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

- Seven selectable cameras
- Automatic camera switching
- Free camera movement
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

Fans can cheer for either side, wave flags, and predict who will score next.

Those reactions are reflected back inside the VR arena. Crowd noise grows as spectators cheer, arena lighting reacts to bursts of excitement, flags appear in the stands, and visual cheer effects rise from the crowd. Players and spectators can see and hear the audience becoming part of the match.

## BUILT FOR VR FROM THE GROUND UP

Project Arena runs natively on Meta Quest using OpenXR and OpenGL ES and was built without a traditional game engine.

The PC version supports VR through SteamVR or Quest Link.

The project is also fully open source. Players and community members can inspect the code, report bugs, contribute ideas, create artwork, make sounds, design assets, or submit code changes.

Project Arena includes an asset catalog containing models, textures, sounds, downloadable source assets, and templates for people who want to create their own contributions.

## FREE, AND STILL GROWING

Project Arena is free to play, with no ads. It keeps growing through frequent updates with new gameplay, multiplayer, customization, spectator, streaming, statistics, and community features, shaped by feedback from players.

Enter the arena, choose your identity, and see how well you can throw, curve, block, punch, and react when another player is standing across the court.
```
