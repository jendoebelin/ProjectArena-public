# Project Arena 0.3.1 (draft, not released)

Store build: versionCode 5, versionName 0.3.1.

## New
- **REPORT PLAYER** (menu, top left, in a room or at an arena while spectating): pick a person in the room, a reason
  and an optional note, and send it to the Project Arena team. **MUTE** on the same page silences that one person
  for you.
- Player names are shared with the people in your room (for the report list). Players still on 0.3.0 show as
  "PLAYER 1A2B" until they update; both versions still play together.
- Server: player reports, and a ban list the operator can change while the server runs.
- **Choose the rules**: HOST A ROOM and PLAY PRACTICE ask for ADVANCED (best of 3 rounds, first to 4) or BASIC
  (3 minute match). Advanced is the default for new players and your last choice is remembered. Every room shows its
  rules in the room list, the waiting area and on the scoreboard. EXPERIMENTAL rules are coming later.

- **Side menu** on every menu page: MAIN MENU, PRACTICE, MULTIPLAYER, SPECTATE, AVATAR, SETTINGS, REPORT PLAYER,
  REPORT BUG, HOW TO PLAY; the current area is lit.
- Fixed: headsets that had an older test build could look for players on an old test server (settings now move to
  the community server once; SETTINGS > SERVER has USE DEFAULT, and MULTIPLAYER shows the address while connecting).
- Fixed: after changing your name online, the server kept the old one (player reports could not find you).
- Fixed: players waiting in a room could see the wrong rules while nobody was playing.
- **Fist bump start**: two players begin a match face to face in the centre of the court; bump fists with both
  triggers pulled to go straight to your ends, or the match starts by itself after 10 seconds (bot matches skip it).
  A glowing BUMP HERE mark, a light reminder buzz, a buzz that grows as your fists get close, and FIST BUMP! with a
  strong buzz and a knock when it lands.
- Fixed: loose tops showed a second pair of nipples (the cloth under the bust ended in a hard crease).
- Fixed: long lines at the top of the menu ran past the panel's edges.
- Plate gauntlets (Striker, Titan, Phantom): the cuff turns 30 degrees up, so the wrist lines up with the forearm
  and the suit's bracer instead of looking cocked.
- The gauntlet sits at a new angle (-25 degrees), so the fist points along the forearm; tune it in developer mode.
- The shield appears on the back of the hand, running along the forearm (it was a flat plate under the hand).
- Fixed: the orange player heard the "you were hit" sound when scoring, and the losing sounds when winning.
- Fixed: the "no games found yet" hint was drawn over the room list (it looked like a second JOIN button).

- **The lobby**: you start on a balcony over the practice court, where two bots play each other. Posters around
  you (PLAY THE BOT, MULTIPLAYER, HOW TO PLAY, CUSTOMIZE, SPECTATE, SETTINGS) work by pointing and pulling the
  trigger or by touching them; point at the court to play the bot. Leaving a room, spectating or practice brings you
  back here, and LOBBY is the first entry of the side menu.
- QUIT GAME at the bottom of the side menu (press it twice).
- Fist bump: measured at the knuckles (it never registered before); hold your triggers and bump.
- The menu and the director panel close by themselves when you move into a room, the court or spectating.
- Report notes up to 120 characters, with a countdown.
- The shield sits a little further forward; a held ball sits in your fist; the shield shatters like glass.

## Store page notes (for the upload)
- What's new text: "New lobby: start on a balcony over the court and watch two bots play; posters take you
  anywhere. Two-player matches start with a fist bump. Choose Advanced or Basic rules. Report or mute a player
  (REPORT PLAYER). Quieter, softer sounds; better gauntlets; online is on by default."
