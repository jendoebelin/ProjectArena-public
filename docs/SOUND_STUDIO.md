# SOUND STUDIO: trying and proposing sounds

The SOUND STUDIO (main menu, next to CUSTOMIZE AVATAR; from 0.3.16) is where new sounds are tried in the game, in the
moment they belong to, before any of them becomes one of the game's official sounds.

## In the game
- Six sections cover every sound: **SHIELD, THROWS, BALL, HITS, MATCH, CROWD**. Choosing one plays its moment around
  you, with the page open beside you: for SHIELD, take your ball, your shield comes up and the coach throws at it (the
  block, the break, then the recharge); THROWS has targets; BALL your own ball coming back to catch; HITS the coach
  throwing at you. MATCH and CROWD sounds are tried with PLAY.
- For each sound slot, `<` and `>` step through **OFFICIAL**, the library's sounds made for that slot, every other
  sound in the library, then the game's own sounds of the other slots (any sound can be tried anywhere, even with the
  library empty). Each plays as you choose it.
- **YOUR VOTE: UP or DOWN** for each library sound; press your vote again to take it back. One vote per player per
  sound. Votes are sent when the game is connected to the community server; you only ever see your own.
- The library's sounds play only inside the studio. Matches always use the official sounds.
- The library comes from the community server and is kept in step: new sounds appear without a game update, and a sound
  taken off the server is deleted from your headset too. It needs ONLINE: ON.

## Trying your own sound first (MINE)
Before proposing a sound, you can try it in the studio on your own headset or PC. Nobody else hears it.
- **WAV files only** (`.wav`). Other formats (MP3, OGG, FLAC, M4A) are not read on the headset: export your sound as WAV
  from Audacity or any sound editor first. Any sample rate, mono or stereo, 16-bit, 24-bit or 32-bit float; up to 30 s.
- **Where to put it:**
  - Quest: `Android/data/com.projectarena.game/files/my_sounds` (copy it there with SideQuest, or over USB from a PC).
  - PC: `%APPDATA%\ProjectArena\my_sounds` (paste it into File Explorer's address bar).
  - The folder is made, with a README, the first time you open the SOUND STUDIO. Open the studio again to see new files.
- **Naming is optional here:** a name starting with the slot's code or event (`S12_glass.wav`, `ShieldBreak__glass.wav`)
  puts it first in that slot; any other name can be tried in every slot. Your sounds show as **MINE - ONLY ON THIS
  HEADSET**.
- **Freeing space:** choose one of your sounds and press **DELETE** (then **SURE?**) to remove the file. The studio shows
  how many of your files there are and how much room they take. (The library's sounds take care of themselves: at most
  40, and they're removed from the headset when they leave the library.)

## The sound slots
Every sound in the game has a code (the asset catalog uses the same ones, so you can hear the current sound there).

| Code | Sound | Event | Studio section |
|---|---|---|---|
| S01 | Ball appears | `Spawn` | BALL |
| S02 | Launcher rises | `SpawnRise` | BALL |
| S03 | Grab | `Grab` | BALL |
| S04 | Catch | `Caught` | BALL |
| S05 | Throw (slow) | `ThrowSlow` | THROWS |
| S06 | Throw (medium) | `ThrowMed` | THROWS |
| S07 | Throw (fast) | `ThrowFast` | THROWS |
| S08 | Throw (full power) | `ThrowFull` | THROWS |
| S09 | Dropped throw | `ThrowFail` | THROWS |
| S10 | Wall bounce | `Bounce` | BALL |
| S11 | Shield block | `ShieldBlock` | SHIELD |
| S12 | Shield break | `ShieldBreak` | SHIELD |
| S13 | Knuckle punch | `KnuckleBlock` | HITS |
| S14 | You scored | `HitPlayerOpponent` | HITS |
| S15 | You were hit | `HitPlayerSelf` | HITS |
| S16 | Ball returns | `Returned` | BALL |
| S17 | Foul | `Foul` | HITS |
| S18 | Countdown beep | `Countdown` | MATCH |
| S19 | Round start | `RoundStart` | MATCH |
| S20 | Round won | `RoundEndWin` | MATCH |
| S21 | Round lost | `RoundEndLose` | MATCH |
| S22 | Match won | `MatchEndWin` | MATCH |
| S23 | Match lost | `MatchEndLose` | MATCH |
| S24 | Recharge | `Recharge` | SHIELD |
| S25 | Strike | `Strike` | HITS |
| S26 | Sudden death | `Overtime` | MATCH |
| S27 | Arena ambience | `Ambience` | CROWD |
| S28 | Fist bump | `FistBump` | MATCH |
| S29 | Crowd | `Crowd` | CROWD |
| S30 | Crowd cheer | `Cheer` | CROWD |
| S31 | Clock tick | `ClockTick` | MATCH |
| S32 | Lead change | `LeadChange` | MATCH |
| S33 | Target hit | `TargetHit` | THROWS |

## Proposing a sound
- **GitHub:** open an issue with the **Sound proposal** form (it asks for the slot, a name, the file, how you would
  like it used and what it is made from), or a pull request adding the file. **Discord:** post it in the server with
  the slot's code.
- You choose how it is used: tried in the SOUND STUDIO first, or proposed straight for the game and the public
  repository (the owner decides what goes in either way). Nothing is put on the server or into the game without your
  agreement.
- **What it is made from:** your own recording or synthesis, or built from other material: then say which (with
  links) and its licence. The open assets are CC BY 4.0; assets under other terms that allow use in a free, open game
  (CC0, CC BY, or the Pixabay Content License for new works made from Pixabay sounds, for example) are listed in NOTICE
  with their sources.

## Format
- Any common format is fine (WAV, OGG, MP3, FLAC): the owner's tool converts it to the game's format (WAV, mono, 48 kHz,
  16-bit), trims the silence before and after, and makes it as loud as the game's own sound for that slot.
- Length: up to 3 seconds (the crowd sounds, ambience and the match-end fanfares up to 12).
- A clean recording without its own reverb works best: the game adds its own position and distance.

## What happens to a library sound
- Players try it and vote. The owner takes sounds out of the library when the community has decided: either
  **adopted** (it becomes the game's official sound for its slot, in a release) or **removed** (the community didn't
  take to it). Either way the result (its votes and why it left the list) is posted on Discord.
- The library holds 40 sounds at most, 5 per slot.
