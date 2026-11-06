# Project Arena 0.3.16

## New: the SOUND STUDIO
- **SOUND STUDIO** on the main menu, next to CUSTOMIZE AVATAR: try new sounds in the game, in the moment they belong to,
  and vote on them. Six sections cover every sound: SHIELD, THROWS, BALL, HITS, MATCH and CROWD. For SHIELD you take
  your ball, your shield comes up and the coach throws straight at it, so you hear the block, the break and the
  recharge as in a match; the page stays open beside you while the moment plays, so you can change a sound and hear it
  at once.
- For every sound, `<` and `>` step through the official one, the library's new sounds, and the game's own sounds of
  every other slot (try the strike's ding as the recharge, say). **YOUR VOTE: UP / DOWN** on the new ones (press it
  again to take it back); one vote per player per sound, and you only see your own.
- The sounds come from the community server's library: new ones arrive without a game update, and they play only inside
  the studio, so matches always keep the official sounds. The favourites become official sounds in a release.
- **Try your own first:** put `.wav` files (WAV only) in the game's `my_sounds` folder (on Quest
  `Android/data/com.projectarena.game/files/my_sounds`, on PC `%APPDATA%\ProjectArena\my_sounds`) and they show in the
  studio as MINE, only on your headset. Name one `S12_glass.wav` to put it in that slot. DELETE removes a file again.
- **Propose your own** on GitHub (the new Sound proposal form) or Discord: `docs/SOUND_STUDIO.md` has the slots (S01-S33,
  the same codes as the asset catalog), the format, and how sounds are used.
- DELETE MY STATS removes your sound votes too.

## Fixed
- **Rooms left open by an unattended headset**: a room closes after 10 minutes with nobody moving, but a headset left
  running whose tracking kept dropping and coming back counted each jump as movement, so the room stayed open. Only
  tracked movement counts now, and a headset that isn't being worn counts as still.
- **The spectator hub**: a room where nobody is playing shows **WAITING FOR PLAYERS** (it used to count as a live
  match); the PC window no longer draws the headset's hub sign into its title; each miniature arena's glow is on the
  floor, no longer showing through its glass.

## For hosts and the server
- A host's log now says every minute what kept each person in the room active (no positions, only counts), so a room
  that stays open can be explained from a bug report.
- The community server keeps the SOUND STUDIO's library and votes (server/projectarena_relay.py `--sounds`; the owner's
  `--sound-totals` and `--sound-remove`). The server goes up first; 0.3.14 and 0.3.15 keep playing with it.

0.3.14, 0.3.15 and 0.3.16 play together online.
