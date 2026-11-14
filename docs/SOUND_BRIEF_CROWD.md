# Brief: new arena crowd sounds for Project Arena

Project Arena is a free VR disc sport for Meta Quest and PC VR. Two players (up to four) face each other in a neon
glass court inside a small indoor arena with stands around it, throwing charged balls, blocking with shields and
dodging. Players talk to each other over voice chat the whole time, so the crowd has to sit **under** their voices: a
crowd you feel, not one you have to talk over. Today's crowd sounds drown conversation out (the match-won roar
especially), and we want a new set made for that.

## What we need (seven sounds)

| Slot | Name | When it plays | Length | Loudness (RMS, today's in brackets) |
|---|---|---|---|---|
| S27 | Arena ambience | always, quietly, under everything | a 30 s seamless loop | about -38 dBFS (-36) |
| S29 | Crowd | a short crowd blip each time the fans on their phones cheer (at most one every 1.5 s; a little louder with their excitement) | under 1 s, a quick swell and fade (changed 2026-11-12: no longer a loop) | about -34 dBFS (-32) |
| S30 | Crowd cheer | when the fans' excitement peaks | 2-3 s | about -27 dBFS (-24) |
| S20 | Round won | the end of a round, for the winner | 2-3 s | about -27 dBFS (-26) |
| S21 | Round lost | the end of a round, for the loser | 2-3 s | about -27 dBFS |
| S22 | Match won | the end of the match, for the winner | 3-4 s (today's is 8) | about -27 dBFS (-23) |
| S23 | Match lost | the end of the match, for the loser | 3-4 s (today's is 7) | about -27 dBFS |

- **Leave room for speech.** Keep the 300 Hz - 3 kHz band (where voices live) light: more low warmth and high air,
  less dense midrange. No sudden spikes: the loudest moment no more than about 10 dB over the sound's average.
- **No words.** No chants, names, shouted words or anything that sounds like a person talking close by. A crowd, not
  people.
- **Loops loop.** The ambience loop must repeat with no click, gap or audible seam, and no stand-out
  moment (a whistle, one laugh) that becomes noticeable on the tenth repeat.
- **One family.** The seven should sound like the same place and the same crowd: a modern indoor arena, a few
  thousand people, a little futuristic (neon, glass) but real people.
- **Won and lost:** a short crowd reaction (a cheer, an "ohh") with or without a short musical sting, ending cleanly.
  Lost should be gentler than won, never mocking.

## Files

- WAV, 48 kHz, 16- or 24-bit, **mono** (the game places sounds in the room itself).
- Peaks no higher than -3 dBFS. Trim silence before and after (the loops: none at all).
- Name each file by its slot: `S27_ambience.wav`, `S29_crowd.wav`, `S30_cheer.wav`, `S20_round_won.wav`,
  `S21_round_lost.wav`, `S22_match_won.wav`, `S23_match_lost.wav`.

## Rights

- Your own recordings and synthesis, or sound libraries whose terms let us release the result. List every library
  sound you used, with its source and licence.
- Nothing from other games, films, TV or sports broadcasts.
- The finished sounds will ship in the game and in its open asset set under **CC BY 4.0**, credited to you (tell us
  the name to credit). Please confirm you can grant that.

## Hearing them in the game

Anyone can try sounds in the game before they're final: the game's **SOUND STUDIO** (main menu) plays each slot in the
moment it belongs to. Put WAV files in the game's `my_sounds` folder (named by slot, as above) and pick them under
CROWD and MATCH. Please check them there with voice chat going, if you can.

(Our side, at the same time: the crowd will also turn itself down while someone in the room is talking.)
