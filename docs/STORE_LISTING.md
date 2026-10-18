# Store listing (Meta Horizon Store): every answer, in the order the dashboard asks

Dashboard > App submissions > the draft (**v1**) > **App metadata**. The package name `com.projectarena.game` never
changes. Art: `build/store_art/` (re-render with `python -I tools/gen_store_art.py`). Save each page; do not press
Submit for Review until the public launch (ROADMAP 4c).

## Basic details

- **Name:** Project Arena
- **Short description:**
  > A free 1-v-1 VR sport: throw, curve and punch energy balls past your rival's shield. Play online with Quest and PC players.
- **Description:**
  > Project Arena is a one-on-one VR court sport. You stand at your end of a neon glass court, grab your ball and throw
  > it at your opponent. Bank it off the walls, curve it with a flick of the wrist, punch their shots away with your
  > gauntlets, and raise your shield while you hold a charged ball. Every strike makes your ball bigger and faster.
  >
  > - Real throwing physics: your hand speed, spin and aim decide the shot.
  > - Play online: Quest and PC VR players meet in the same rooms on the community server, or play on your home Wi-Fi.
  > - A lobby to start from: watch two bots play from a balcony above the court, then jump in.
  > - Arena rooms: host or join a room, wait your turn, and bump fists with your opponent to start. Spectate any live match.
  > - Advanced or Basic rules: best of three rounds, or a three-minute match.
  > - Voice chat: push-to-talk by default, open mic if you prefer, with mutes for spectators, players or one person.
  > - Make it yours: two body types, outfits, hair, headgear, face shields, masks, team colours, and medieval-style
  >   plate gauntlets.
  > - Free and open source: no ads, no accounts, no tracking. Report a bug or a player from any menu.
  >
  > Inspired by CCP Games' Sparc (2017). Project Arena is an independent game with its own code, art and sound, not
  > affiliated with or endorsed by CCP Games.

## Categorization

- **Genre:** Sports (and Action; Arcade if offered)
- **Sports subgenre:** Precision sports > Air hockey
- **Art style:** Stylized, Retro-futurism, Minimalist
- **Game theme:** Sci-fi
- **Gameplay challenge:** Casual
- **Gameplay mechanics:** Dodging, Battle arena

## Specs

- **Early Access:** Enabled (turn it off at the full public launch)
- **Player modes:** Single player, Player versus player (PvP), and the online multiplayer option (User Reporting is
  set up: Engagement > User Reporting). If multiplayer stays greyed out: Single player only.
- **Play area:** Movement (players step and lean to dodge; about 2 x 2 m, standing)
- **Comfort:** Moderate (play is stationary; the optional spectator mode flies smoothly with the thumbsticks)
- **Supported controllers / input:** Touch controllers (no hand tracking yet)
- **Internet:** Required for some features (online play; practice works offline)
- **Subscription:** No
- **Purchase-blocked devices:** Quest (Quest 1) blocked
- **Social features / users interact:** Yes
- **Languages:** English

## Details (developer and links)

- **Developer:** Project Arena (untick "use team name" if it would show the team's name)
- **Publisher:** Project Arena
- **Website:** https://github.com/jendoebelin/ProjectArena-public
- **Privacy policy:** https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md
- **External support link:** https://github.com/jendoebelin/ProjectArena-public/issues
- **Terms of service:** leave empty
- **Support email (where asked):** vrsportsfan@gmail.com

## Assets (build/store_art/)

| Dashboard slot | File |
|---|---|
| Universal basic asset (optional) | skip: upload each cover instead |
| Cover art: Landscape (2560 x 1440) | cover_landscape.png |
| Cover art: Square (1440 x 1440) | cover_square.png |
| Cover art: Portrait (1008 x 1440) | cover_portrait.png |
| Hero cover (3000 x 900) | hero_cover.png |
| Mini / small landscape (1080 x 360), if asked | mini_landscape.png |
| Icon (512 x 512) | icon.png (icon_1024.png if it asks for a bigger one) |
| Logo (optional) | skip |
| Screenshots (5 x 2560 x 1440), in this order | screenshot_1.png ... screenshot_5.png |
| Spatialized tile (optional) | Background: tile_background.png; Foreground: tile_foreground.png |
| 180 x 180 tile, trailer, 360 preview (optional) | skip |

The VRC circles on the Assets page (VRC.Quest.Asset.*) are confirmations: tick each one once the art meets it.

## Content rating (IARC questionnaire)

- **Country of origin:** United States. **Developer contact email:** vrsportsfan@gmail.com. **Certificate:** create new.
- Violence, Blood or Gory Images: **No**
- Fear: **No**
- Sexuality, Suggestiveness or Dating Games: **No**
- Gambling: **No**
- Language: **No**
- Controlled Substance: **No**
- Crude Humor: **No**
- Digital Purchases, Cash-convertible Rewards or NFTs: **No**
- Users interact (voice, text, images or audio): **Yes**
  - Ability to block users: **No**
  - Ability to report users: **Yes** (REPORT PLAYER, and Meta's User Reporting)
  - Chat moderation: **No**
  - Limited to invited friends only: **No**
- Shares precise physical location: **No**
- Nazi symbols (Germany): **No**
- Anti-national content (Korea): **No**
- Advocates terrorism: **No**
- Realistic descriptions of crimes: **No**

## User Reporting (Engagement > User Reporting; done 2026-10-17)

Delivery to vrsportsfan@gmail.com (or the dashboard inbox); custom reasons "Cheating or exploiting the game" and
"Offensive name or avatar"; branding image `cover_landscape.png`. The decryption key stays with the owner. If a form
asks for a description of our own tool:

> In a room, or at an arena while spectating, players open the menu and choose REPORT PLAYER. They pick the person
> from the list of everyone in that room, choose a reason (harassment or hate, offensive name or look, cheating,
> other), may add a note, and press SEND REPORT. The report goes to the Project Arena team on our server with the
> names of everyone in the room and the game's recent log; we review reports and ban players by name or address. On
> the same page MUTE silences that player's voice for the reporter at once, and SETTINGS > VOICE AND MUTES mutes
> spectators, players or everyone. Contact: vrsportsfan@gmail.com.

## Builds (not in the draft)

Testers' builds go to **Distribution > Release channels > ALPHA**. 0.3.1 = versionCode 5,
`build/release-0.3.1/ProjectArena-0.3.1.apk`; "what's new" text in docs/RELEASE_NOTES_v0.3.1.md.
