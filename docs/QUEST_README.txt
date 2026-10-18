Project Arena for Meta Quest (standalone, no PC needed)
=====================================================

What you need
  - A Meta Quest 3 (tested), Quest 3S, Quest 2 or Quest Pro, signed in to your Meta account
  - Wi-Fi for online play, and a clear play space of about 2 x 2 m
  - Touch controllers (hand tracking is planned for a later version)

There are two ways to get it. Option A is the easiest and keeps you updated automatically.


A. Join the test in the Meta Horizon Store (recommended, no developer mode)
-----------------------------------------------------------------------------
  1. Email vrsportsfan@gmail.com with the subject "Project Arena test access" and the email address of the
     Meta account your headset is signed in to.
  2. You get an invite from Meta by email (or in the Meta Horizon phone app). Accept it.
  3. On the headset, open the Store and search for "Project Arena" (or follow the link in the invite), then
     install it like any other app.
     New test versions arrive as normal updates.


B. Sideload the APK (developer mode)
------------------------------------
  1. Turn on developer mode (once): in the Meta Horizon phone app go to Devices > your headset > Headset settings
     > Developer mode. Meta may ask you to create a free developer organization first
     (https://developers.meta.com/horizon/manage/organizations/create/). Restart the headset.
  2. Connect the headset to a computer with a USB-C cable. Put it on and choose "Allow USB debugging"
     (tick "Always allow from this computer").
  3. Install ProjectArena-<version>.apk with either:
       - SideQuest (https://sidequestvr.com/setup-howto): when the dot at the top left is green, click the
         "install APK" icon (box with a down arrow) and pick the file; or
       - adb (Android platform-tools):   adb install -r ProjectArena-<version>.apk
  4. On the headset: Library > the drop-down at the top right > Unknown Sources > Project Arena.
  Updates: install the new APK the same way (your name, avatar and settings are kept).

  Coming from the 0.3.0 APK? Uninstall it first (from 0.3.1 the GitHub APK has a new signature); your name,
  avatar and settings start fresh. Later APKs install over 0.3.1 as usual.

  Use one or the other: if you have the store test version, uninstall it before sideloading (and the other way
  round). Uninstall: Settings > Apps > Installed apps > Project Arena > Uninstall.


Playing
-------
  - You start in the lobby, a balcony over the practice court where two bots play. Point at a poster (or touch it)
    and pull the trigger: PLAY THE BOT, MULTIPLAYER, CUSTOMIZE, SPECTATE, HOW TO PLAY, SETTINGS. Point at the court
    to play the bot. The side menu's LOBBY brings you back; QUIT GAME is at its bottom.
  - Online is on by default: MENU > MULTIPLAYER shows the rooms on the community server, where Quest and PC
    players meet. HOST A ROOM, or JOIN someone's; in the waiting area press JOIN QUEUE (the red button) to play.
  - Grab your ball: hold the trigger and reach for it (or swing through it on the spawner). Throw: swing and let go.
  - Holding a charged ball raises your shield; your fists punch balls away.
  - MENU button (left controller): menus. B or a thumbstick click (or holding the Meta button) re-centres you.
  - Two players start a match face to face in the centre: hold your trigger and bump fists (or wait 10 seconds).
  - Hold the left grip to talk. MENU > REPORT BUG sends us a note and the game's recent log; MENU > REPORT PLAYER
    reports or mutes someone in your room.

Project Arena is free and open source: https://github.com/jendoebelin/ProjectArena-public
Questions: vrsportsfan@gmail.com
