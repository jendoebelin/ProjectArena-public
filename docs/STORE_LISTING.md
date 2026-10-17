# Store listing (Meta Horizon Store)

Edit it at Developer Dashboard > Project Arena > App overview > **Edit app** (the product detail page). The package
name `com.projectarena.game` never changes. Art: `build/store_art/` (re-render with `python -I tools/gen_store_art.py`).

## Name

Project Arena

## Short description

A free 1-v-1 VR sport: throw, curve and punch energy balls past your rival's shield. Play online with Quest and PC players.

## Description

Project Arena is a one-on-one VR court sport. You stand at your end of a neon glass court, grab your ball and throw
it at your opponent. Bank it off the walls, curve it with a flick of the wrist, punch their shots away with your
gauntlets, and raise your shield while you hold a charged ball. Every strike makes your ball bigger and faster.

- Real throwing physics: your hand speed, spin and aim decide the shot.
- Play online: Quest and PC VR players meet in the same rooms on the community server, or play on your home Wi-Fi.
- Arena rooms: wait in the lobby, join the queue, and step into the court when it is your turn. Spectate any live match.
- Voice chat: push-to-talk by default, open mic if you prefer, with separate mutes for spectators and players.
- Make it yours: two body types, outfits, hair, headgear, face shields, masks, team colours, and medieval-style plate
  gauntlets.
- Practice against the bot any time.
- Free and open source: no ads, no accounts, no tracking. Report a bug from any menu.

Inspired by CCP Games' Sparc (2017). Project Arena is an independent game with its own code, art and sound, not
affiliated with or endorsed by CCP Games.

## Details for the dashboard

- Genre: Sports, Action
- Game modes: Single player (practice bot), Multiplayer (online and local network)
- Early Access: enabled (test release; turn it off at the full public launch)
- Play area: Movement (players step and lean to dodge; about 2 x 2 m of space, standing)
- Comfort: **Moderate**. Playing is fully stationary; the optional spectator mode lets you fly smoothly with the
  thumbsticks, and the rating follows the most intense part.
- Supported controllers: Touch controllers (no hand tracking yet)
- Internet: needed for online play (offline practice works without it)
- Languages: English
- Age group: Teens and adults (13+)
- Categorization (App metadata): art style Stylized, Retro-futurism, Minimalist; game theme Sci-fi; gameplay
  challenge Casual; gameplay mechanics Dodging, Battle arena; sports subgenre Precision sports > Air hockey
- Privacy policy URL: https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
- Support / website: vrsportsfan@gmail.com, https://github.com/jendoebelin/ProjectArena-public

## User Reporting (Dashboard > User Reporting; needed before the multiplayer player modes)

Choose the option for **your own reporting tool** (we do not use Meta's Platform SDK) and describe it. Text to paste
(true from 0.3.1; 0.3.0 has no player reports):

> In a room, or at an arena while spectating, players open the menu and choose REPORT PLAYER (top left). They pick
> the person from the list of everyone in that room, choose a reason (harassment or hate, offensive name or look,
> cheating, other), may add a note, and press SEND REPORT. The report goes to the Project Arena team on our server
> with the names of everyone in the room and the game's recent log; we review reports and ban players by name or
> address. On the same page MUTE silences that player's voice for the reporter at once, and SETTINGS > VOICE AND
> MUTES mutes spectators, players or everyone. Contact: vrsportsfan@gmail.com.

Player modes after that: **Single player**, **Player versus player (PVP)**, and the online multiplayer option.
Until 0.3.1 is on the channel, only Single player.

## Art (build/store_art/)

| Dashboard slot | File |
|---|---|
| Hero art (3000 x 900) | hero_cover.png |
| Cover art landscape (2560 x 1440) | cover_landscape.png |
| Cover art square (1440 x 1440) | cover_square.png |
| Cover art portrait (1008 x 1440) | cover_portrait.png |
| Mini / small landscape (1080 x 360) | mini_landscape.png |
| Icon (512 x 512) | icon.png (icon_1024.png if it asks for a bigger one) |
| Universal basic asset (optional; Meta crops every cover from it) | skip: upload the covers above instead |
| Screenshots (5 x 2560 x 1440) | screenshot_1.png ... screenshot_5.png (rally, rally from the side, main menu, whole court, how to play) |
| Spatialized tile (optional) | tile_background.png (512, solid) + tile_foreground.png (512, transparent, emblem in the 392 safe area) |
| Logo, 180 x 180 tile, trailer, 360 preview (optional) | not made yet |
