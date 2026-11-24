Project Arena for PICO (standalone, no PC needed)
=================================================

What you need
  - A PICO 4 (tested with 0.3.25), PICO 4 Pro or PICO 4 Ultra, signed in to your PICO account
  - Wi-Fi for online play, and a clear play space of about 2 x 2 m
  - The PICO controllers

This is our first PICO release: tell us how it runs (Discord, https://discord.gg/KuNkSbVHzh, or email
vrsportsfan@gmail.com). PICO's own store is to follow, after PICO's review.


Install the APK (developer mode)
--------------------------------
  1. Sign the headset in to your PICO account: PICO needs it to run an app installed by hand.
  2. Turn on developer mode (once): on the headset go to Settings > General > About and press Software Version
     several times until Developer appears, then in Developer turn on USB Debug.
     (PICO 4 Ultra: Control Center > Settings > About.)
  3. Connect the headset to a computer with a USB-C cable. Put it on and allow USB debugging if it asks.
  4. Install ProjectArena-<version>-pico.apk with either:
       - adb (Android platform-tools):   adb install -r ProjectArena-<version>-pico.apk
       - SideQuest (https://sidequestvr.com/setup-howto): when it shows your headset as connected, click the
         "install APK" icon (box with a down arrow) and pick the file.
  5. On the headset: open your Library and start Project Arena. If it isn't with your other apps, look for the
     list of apps from unknown sources.
  Updates: install the new APK the same way (your name, avatar and settings are kept).

  Use one or the other: when PICO's store has the game, uninstall this copy before you install the store's (and the
  other way round). Uninstalling removes what the game keeps on the headset.


Playing
-------
  - You start in the lobby, a balcony over the practice court where two bots play, with the main menu in front of
    you: point at a button and pull the trigger. Point at the court to play the bot. The side menu's LOBBY brings
    you back; QUIT GAME is at its bottom.
  - Online is on by default: MENU > MULTIPLAYER shows the rooms on the community server, where Quest, PICO and PC
    players meet. HOST A ROOM, or JOIN someone's; in the waiting area press JOIN QUEUE (the red button) to play.
  - Grab your ball: hold the trigger and reach for it (or swing through it on the spawner). Throw: swing and let go.
  - Holding a charged ball raises your shield; your fists punch balls away.
  - MENU button (left controller): menus. B (or holding the Home button) re-centres you.
  - Two players start a match face to face in the centre: hold your trigger and bump fists (or wait 10 seconds).
  - Hold the left grip to talk. MENU > REPORT BUG sends us a note and the game's recent log; MENU > REPORT PLAYER
    reports or mutes someone in your room, or votes to remove them from the arena (more than half the room).
  - SETTINGS > GRAPHICS AND VIEW > RESOLUTION (1X, 1.25X as standard, 1.5X) sets how sharp the picture is; a change
    shows the next time the game starts. PASSTHROUGH isn't in the PICO version yet.
  - CUSTOMIZE > PRESET > COUNTRY FLAG: play for your country (a team kit, and your end of the court in its colours).
  - MENU > MY STATS: your career from online matches against other players (public in the game; DELETE MY STATS
    erases it). Privacy: https://github.com/jendoebelin/ProjectArena-public/blob/main/docs/PRIVACY.md

Project Arena is free and open source: https://github.com/jendoebelin/ProjectArena-public
Questions: vrsportsfan@gmail.com
