# Streaming Project Arena

The PC spectator can draw **only the players and the balls, with nothing behind them**. Put any background you like
behind them in OBS: a picture, a video, your webcam, or one of the moving backgrounds in this folder.

## What's here

| File | Scenes (`scene=`) | Looks (`look=`) |
| --- | --- | --- |
| `backgrounds/tron_grid.html` | `grid` (blue against orange), `canyon` (a circuit rooftop), `vector` (a drawn wireframe grid) | `legacy` (cyan / orange), `edgerunner` (pink / cyan), `ares` (red), `matrix` (green) |
| `backgrounds/dark_waterfall.html` | `ledge` (a flooded ledge over a waterfall), `cliff` (a cliff-top waterfall) | `demise` (amber storm), `abyss` (cold blue), `crimson` (red) |

Each background is a single file that runs on its own, with no internet connection needed. Each one moves: light
pulses and data rain on the Tron grid; water, mist and the storm sky on the waterfall.

**Video backgrounds** (`backgrounds/video/`, a 5-second seamless loop each, 1280 x 720, no sound) and the same scenes
as still pictures (`backgrounds/stills/`, 1376 x 768):

| Video | Still | Scene |
| --- | --- | --- |
| `video/golden_hour_farm.mp4` | `stills/golden_hour_farm.jpg` | a red barn and fields at sunset |
| `video/holodeck.mp4` | `stills/holodeck.jpg` | a gold-lined holodeck room |
| `video/cosmic_cliffs.mp4` | `stills/cosmic_cliffs.jpg` | a nebula's "cosmic cliffs" under the stars |
| `video/orbital_space_station.mp4` | `stills/orbital_space_station.jpg` | a space station window over the Earth |
| `video/roman_colosseum.mp4` | `stills/roman_colosseum.jpg` | inside the Colosseum in Rome |
| `video/sports_arena_crowd.mp4` | `stills/sports_arena_crowd.jpg` | a packed arena under a scoreboard |
| `video/tron_city.mp4` | `stills/tron_city.jpg` | a neon circuit city |
| `video/holodeck_side_camera.mp4` | `stills/holodeck_side_camera.jpg` | the holodeck, **matched to the SIDE camera** |
| `video/tron_city_side_camera.mp4` | `stills/tron_city_side_camera.jpg` | the circuit city, **matched to the SIDE camera** |

The two `_side_camera` backgrounds are drawn for the default SIDE camera (camera 1, auto camera off, no zoom): their
horizon and floor grid line up with the court, so the players stand on the floor. They loop every 4 seconds and the
stills are 1920 x 1080. The others are backdrops for any camera (their floors don't line up with the players).

## Set up OBS (Windows)

1. **Start the spectator with a transparent background.** Run the game with:

   ```
   ProjectArena.exe --spectate --layer 3 --keycol 3
   ```

   Or, in the spectator window, press **V** until the view layer is KEY, then **K** until the key is TRANSPARENT.
   The window itself looks black behind the players in this mode; OBS sees the transparency.

2. **Add the background.** In OBS, under Sources, click **+** and choose **Browser**.
   - For the default scene and look: tick **Local file** and pick `tron_grid.html` or `dark_waterfall.html`.
   - To choose a scene, look or effect strengths: leave **Local file** unticked and type the file's address in
     **URL**, with the settings after a `?`, for example:

     ```
     file:///C:/Games/ProjectArena/streaming/backgrounds/tron_grid.html?scene=canyon&look=edgerunner
     ```

   - Set **Width 1920** and **Height 1080**.
   - **Or a video background:** click **+**, choose **Media Source**, tick **Local File** and pick a file from
     `backgrounds/video`, tick **Loop**, and untick **Restart playback when source becomes active** (it then keeps
     looping while you switch scenes). A still picture: **+**, **Image**, a file from `backgrounds/stills`.
     In the source's right-click menu, **Transform > Fit to screen** (the videos are 1280 x 720; OBS scales them).

3. **Add the game.** Click **+** and choose **Game Capture**.
   - Mode: **Capture specific window**.
   - Window: **[ProjectArena.exe]: Project Arena - Spectator**.
   - Tick **Allow Transparency**.

4. **Put the game above the background** in the Sources list. The list draws from the bottom up, so the game's
   capture must be higher in the list than the Browser source.

## Settings in the address

Add settings after `?` in the background's address, joined with `&`. Leave out any you don't need; they keep their
default.

**Tron Grid** (`tron_grid.html`)

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `scene` | `grid` | see the table above | which scene |
| `look` | `legacy` | see the table above | the colours |
| `pulseSpeed` | 1.3 | 0 to 2.5 | how fast the light pulses and grid run |
| `circuitGlow` | 0.85 | 0 to 1 | the floor's energy waves and pulses |
| `dataRain` | 0.75 | 0 to 1 | data rain, light-cycle trails and fog |
| `laserSweep` | 0.70 | 0 to 1 | the skyline's sweeping lasers |

**Dark Waterfall** (`dark_waterfall.html`)

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `scene` | `ledge` | see the table above | which scene |
| `look` | `demise` | see the table above | the mood |
| `waterfallSpeed` | 1.2 | 0 to 2.5 | how fast the water falls and flows |
| `rippleIntensity` | 0.75 | 0 to 1 | ripples and glints on the water |
| `mistDensity` | 0.70 | 0 to 1 | rising mist and floating motes |
| `stormPulse` | 0.60 | 0 to 1 | the storm light in the sky |

Example: a slower, darker canyon with no lasers:

```
tron_grid.html?scene=canyon&pulseSpeed=0.6&laserSweep=0
```

## More than one spectator window

Each spectator window keeps its own settings, so a stream can switch between several. Give each window a name with
`--tag` so each Game Capture source can pick its own window:

```
ProjectArena.exe --spectate --layer 3 --keycol 3 --winscreen split --tag split
ProjectArena.exe --spectate --layer 3 --keycol 3 --winscreen winner --tag winner
```

The windows are then called "Project Arena - Spectator - SPLIT" and "Project Arena - Spectator - WINNER".

`--winscreen` chooses how a won match is shown: `split` (the winner and the loser side by side), `winner` (the camera
flies to the winner) or `camera` (the camera stays put). In the window, **R** changes it.

## Other ways to do it

- **A colour key (green screen):** if you can't use Game Capture (for example, when OBS records the game from another
  PC), use KEY with **green**, **magenta** or **blue** instead of TRANSPARENT. Capture the window with Window Capture
  and add **Filters > Chroma Key** with the same colour. Choose a colour that nobody's outfit or glow uses.
- **Your own background:** anything OBS can show works behind the game: an Image, a Media Source (video) or a
  Browser source.

## Make a background that fits the players

A background looks right when its floor and horizon match the game's camera; otherwise the players look too small or
float above the floor. `camera_guides/` has a picture for each preset camera at 1920 x 1080 with the horizon, a 1 m
floor grid, the court's centre line, and two 1.75 m players on their start spots. Draw (or ask an image tool for) a
background that matches one of them, and use that camera while streaming. A background can only match one camera, so
turn the auto camera off (**A**).

| Camera (key) | Horizon (from the top, at 1080 high) | Players |
| --- | --- | --- |
| SIDE (1, the default) | 346 px (32 %) | 188 px tall, feet at 645 px, 206 px and 1714 px from the left |
| HIGH SIDE (2) | above the picture (the camera looks down 35 degrees) | 155 px tall, feet at 589 px |
| BLUE END (3) / ORANGE END (4) | 436 px (40 %) | near player 269 px tall, far player 76 px, both in the middle |

All four cameras have a 66 degree vertical view (16:9). Zooming in the game (mouse wheel or the triggers) changes
the view, and the background stops matching, so leave the zoom alone with a matched background.

On the SIDE camera a 1 m floor tile at the players' spot is about 107 px wide, and the two players stand 14 m apart.
A grid or tiled floor with much bigger tiles makes the players look like toys.

## Tips

- The spectator's other controls (cameras, view layers, a game controller) are in `README-PC.txt`, or press **F1**
  in the spectator window.
- In the Browser source's properties, **Shutdown source when not visible** saves CPU while another scene is showing.

## Credits and license

The backgrounds were made for Project Arena by the Project Arena team. Like the game's other art, they are under the
Creative Commons Attribution 4.0 license (CC BY 4.0): see `LICENSE-ASSETS.md`.

`cosmic_cliffs` is generated, inspired by the James Webb Space Telescope's view of the Carina Nebula's "Cosmic
Cliffs"; it is not NASA's picture.
