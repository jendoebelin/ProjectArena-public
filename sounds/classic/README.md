# Classic sound set (original, kept for reverting)

The first Project Arena sounds (synthesized, CC BY 4.0, no samples): punchier and brighter than the current set.
Kept exactly as shipped in 0.3.0.

Use them again:

    cp sounds/classic/*.wav sounds/classic/events.txt app/assets_open/audio/

(then rebuild). Regenerate this folder from scratch: `python -I tools/gen_open_audio_classic.py`.
