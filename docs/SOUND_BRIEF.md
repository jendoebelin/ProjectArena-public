# Sound brief: regenerating Project Arena's sounds with ElevenLabs and Suno

Goal (owner, 2026-10-17): better, **more subtle** sounds for every effect. Today's set is synthesized by
`tools/gen_open_audio.py`; it works but is loud and busy. This brief lists every sound the game plays, when it plays,
and a prompt for each.

## How to make them

**ElevenLabs → Sound Effects** (every effect, the crowd, the ambience):
- Paste the prompt; set **Duration** to the "length" given (or Auto for the short ones); **Prompt influence** about
  0.5 (higher = follows the words more literally).
- Make 4 versions, keep the best; regenerate rather than settle. Short is better: the game layers many sounds.
- Download as **WAV** and name the file after its slot: `Grab.wav`, `ThrowFast.wav`, ... (`Grab_v3.wav` is fine).

**Suno** (optional: the win / lose fanfares and an ambience bed with music in it):
- Custom mode, **Instrumental** on. Suno writes whole tracks: use the style text below, then pick the 5-8 s you want
  and write it in `trim.txt` next to the files: `MatchEndWin 12.5 20.0` (start and end, in seconds).
- Download WAV (Pro / Premier plans).

**Into the game:** put the WAVs in one folder (e.g. `build/new_sounds/`) and run

    python -I tools/import_sounds.py build/new_sounds --dry-run
    python -I tools/import_sounds.py build/new_sounds

It trims the silence, cuts each to its slot's length, sets each slot's loudness (frequent sounds quiet), makes the
ambience loop seamlessly, and writes `app/assets_open/audio/`. Slots you have not made yet keep today's sound, so it
can be done a few at a time. Then rebuild and listen in the headset. To go back: `python -I tools/gen_open_audio.py`.

**Rights:** use the outputs from your paid plans (both allow commercial use on paid tiers; check the current terms).
The art licence (`LICENSE-ASSETS.md`, CC BY 4.0) lets anyone reuse our assets: before the public release, decide
whether the generated sounds go under it and add a credit line ("Sound effects generated with ElevenLabs; music
generated with Suno"). Never ask either tool for a sound "like Sparc" or any other game.

## The style (put in mind for every prompt)

- A clean, premium **sci-fi sport**: energy, glass and soft metal; think a modern indoor arena, not a battlefield.
- **Subtle**: soft attacks, rounded lows, no harsh hiss or ear-piercing highs, no distortion, no explosions.
- **Short and dry**: a small natural room tail at most (the game places sounds left / right itself).
- **No music, no voices, no words** in effects (except the crowd slots).
- Players hear some sounds hundreds of times a match (Bounce, Throw, Spawn): those must be the quietest and plainest.

## Every sound

Length = the longest the game keeps (the importer cuts the rest). Level = how loud the slot sits in the mix.

### The ball

| Slot | When it plays | Length / level | ElevenLabs prompt |
|---|---|---|---|
| **SpawnRise** | The ball launcher rises out of the floor at your end, ~1.3 s before a new ball appears | 1.6 s / quiet | Soft rising energy hum, a gentle airy swell that lifts upward in pitch over one second, smooth sci-fi charging tone, clean, subtle, no click at the end |
| **Spawn** | A new energy ball appears on its launcher (every point) | 1.0 s / quiet | A small glowing energy orb materializing: a soft bright "bloom" with a gentle sparkle tail, delicate and clean, short, sci-fi |
| **Grab** | You take your ball (trigger) | 0.6 s / quiet | Hand closing around a humming energy ball: a soft muted thump with a brief warm electric hum, very short, close, subtle |
| **Caught** | You catch your ball as it flies back to you | 0.8 s / medium | Catching a fast energy ball in a gauntlet: a firm soft-padded thump plus a quick rising electric zip, satisfying, short |
| **ThrowSlow** | Release at low hand speed | 0.9 s / quiet | A gentle low whoosh of a glowing energy ball leaving the hand, soft and airy, short |
| **ThrowMed** | Medium throw | 0.9 s / medium-quiet | A clean energy-ball whoosh with a faint electric shimmer, medium speed, smooth, short |
| **ThrowFast** | Fast throw | 1.0 s / medium | A fast sharp whoosh of an energy ball cutting through air, a thin electric streak trailing behind, crisp but not harsh |
| **ThrowFull** | Hardest throw | 1.1 s / medium-loud | A powerful energy-ball launch: a deep punchy whoosh with a brief low thump at the start and a bright electric trail, controlled, no explosion |
| **ThrowFail** | Released too slowly: the ball drops and floats back | 1.0 s / quiet | A deflating energy sound: a soft descending "fizzle" as a glowing orb loses power, gentle, slightly disappointed, short |
| **Bounce** | The ball hits the glass walls, floor or ceiling (many times a rally) | 0.5 s / quietest | A small energy ball tapping thick glass: a short soft "tok" with a tiny glassy ring, clean, very subtle, no echo |
| **Returned** | The ball reaches the far end and flies home to its owner | 1.4 s / quiet | An energy ball zooming away into the distance: a soft whoosh that fades and drops slightly in pitch, smooth and airy |
| **Recharge** | Your ball recharges (thrown past your foul line) | 1.2 s / medium-quiet | A quick pleasant power-up: a short rising chime with a soft electric shimmer, bright but gentle, sci-fi |

### Defence and hits

| Slot | When it plays | Length / level | ElevenLabs prompt |
|---|---|---|---|
| **ShieldBlock** | The ball bounces off your energy shield | 1.2 s / medium-loud | An energy ball deflecting off a glowing hexagonal force field: a solid resonant "thoom" with a short shimmering glassy ring, powerful but smooth, no distortion |
| **ShieldBreak** | Your shield is hit and shatters (the ball loses its charge) | 1.4 s / medium-loud | A translucent energy shield shattering into fading sparks: a soft crystalline crack followed by a quick glittering dissolve, dramatic but not harsh |
| **KnuckleBlock** | You punch the ball away with the knuckle guard (Basic rules) | 0.7 s / medium | A fist in an armoured gauntlet punching away a small energy ball: a tight padded metallic knock with a short electric snap, short and punchy |
| **HitPlayerOpponent** | Your ball hits the other player: your point | 1.6 s / loud | A satisfying score impact: an energy ball hitting a body-suit with a soft deep thud and an electric discharge crackle, then a short bright rising chime of success |
| **HitPlayerSelf** | You are hit: their point | 1.4 s / loud | Getting hit by an energy ball: a muffled heavy thud with a short downward electric buzz, a gentle low "wobble", disappointing but not painful or harsh |
| **Strike** | A strike is awarded (a ball passed through a strike ring) | 1.6 s / medium-loud | A clean two-note electronic alert: a bright rising "ding-ding" with a soft energy swell beneath it, clear and important but not alarming |
| **Foul** | Foul (rule broken) | 1.2 s / medium | A soft referee-style penalty tone: a short low two-note descending synth buzz, rounded, polite, not an alarm |

### Match flow

| Slot | When it plays | Length / level | ElevenLabs prompt |
|---|---|---|---|
| **FistBump** *(new)* | The two players bump fists in the centre to start the match | 0.9 s / medium | Two armoured gauntlets bumping fists: a solid friendly metallic knock with a warm energy pulse that blooms outward, short, satisfying |
| **Countdown** | Each second of 3-2-1 (the same sound three times) | 0.6 s / medium-quiet | A single soft futuristic countdown tick: a short clean low beep with a gentle tail, calm, one beep only |
| **RoundStart** | GO: the round starts | 1.8 s / loud | A clear energetic start tone: a bright rising synth swell ending on a confident soft chime, short arena "go" signal, no voice |
| **Overtime** | Sudden death (Basic: tied at the buzzer) | 2.5 s / loud | A tense sudden-death signal: a deep pulsing synth swell with a heartbeat-like low thump, building suspense, short, no voice |
| **RoundEndWin** | You win a round (Advanced) | 4.5 s / loud | A small indoor arena crowd cheering and clapping for a point won, warm and excited, about four seconds, no announcer, no music |
| **RoundEndLose** | You lose a round | 4.0 s / medium-loud | A small indoor arena crowd reacting with a sympathetic "ohh" groan and a few scattered claps, about four seconds, no announcer, no music |
| **MatchEndWin** | You win the match | 8.0 s / loud | A large indoor arena crowd erupting in cheers, whistles and applause, then settling, about eight seconds, no announcer |
| **MatchEndLose** | You lose the match | 7.0 s / medium-loud | An indoor arena crowd giving warm sportsmanlike applause after a defeat, polite and fading, about seven seconds, no announcer |

### The arena

| Slot | When it plays | Length / level | ElevenLabs prompt |
|---|---|---|---|
| **Ambience** | All the time, looped, under everything | 30-40 s / very quiet | Calm ambience of a large modern indoor sports arena before play: a low soft electrical hum, distant air handling, a faint murmuring crowd far away, steady, no sudden sounds, seamless |

## Suno (optional music)

The crowd slots above work without music. If you want a musical sting under the result, generate with Suno and use
it **instead of** the crowd version for that slot (one file per slot):

- **MatchEndWin**: style `instrumental, short triumphant electronic sting, modern sports broadcast, bright synths,
  punchy drums, uplifting, 120 bpm`. Pick the 6-8 s ending or the strongest bar.
- **MatchEndLose**: style `instrumental, short reflective electronic sting, soft synth pads, gentle, sportsmanlike,
  not sad, 90 bpm`. Pick 5-7 s.
- **Ambience** with music: style `instrumental, minimal ambient electronic, arena warm-up, soft pulse, no melody
  leads, low energy, loopable`. Pick 30-40 s that stays even; the importer crossfades the loop.

A menu music track would need a small code change (there is no menu-music slot yet); ask for it if you want one.
