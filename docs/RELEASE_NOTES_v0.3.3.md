# Project Arena 0.3.3

Store build: versionCode 7, versionName 0.3.3. Everything in 0.3.2, plus:

## Fixed
- **The view dropping into the floor** (or floating far above it): when the headset re-estimated your floor, the game
  kept the old height. It now follows the headset's floor. If the headset loses its floor altogether (more likely with a
  stationary boundary), the game holds your view at your usual standing height until the floor is back.
