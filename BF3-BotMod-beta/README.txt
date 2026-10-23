Battl3Bots - beta  (Battlefield 3 server bots; formerly "BF3 BotMod") 
=================
Mod originally for Automated LAN Party: http://alp.sandiegolan.net/
Donate to keep the download alive: https://www.patreon.com/SanDiegoLAN/

AI soldiers ("bots") for the Battlefield 3 dedicated server (ALP "Battlefield 3 Server" folder, bf4play, or BF-Portable).
Bots fill the teams, play Conquest (attack / defend flags in squads), fight, take cover, revive, resupply,
repair, use ladders, stairs and escalators, swim, knife chain-link fences, drive light vehicles (jeeps,
buggies, boats), throw grenades at slow vehicles, lay AT mines, crouch on held positions and sprint.

INSTALL
  1. Shut the BF3 server down (from your launcher).
  2. Extract this zip anywhere.
  3. Double-click Install-BotMod.bat. A small setup window opens: use Browse... to pick your server's
     bf3_server_Final.exe (pre-filled when the server is in a usual place), then press Install / update.
     BotMod goes next to that exe, its settings into a "BotMod" folder beside the exe's folder, anything it
     replaces is backed up into "BotMod_backup_<date>", and an uninstall.bat for that server is written
     into the BotMod folder.
  4. Start the server from your launcher as usual (ALP, bf4play GUI, BF-Portable, or a .bat) - BotMod loads with
     it whichever way it is started, and a "Battl3Bots" console window opens alongside.
  Remove: BotMod\uninstall.bat (written at install), or Uninstall-BotMod.bat here (the same setup window) -
  either puts the original XInput DLL back and the server runs stock again.
  Update to a newer BotMod: just run the new Install-BotMod.bat - your settings files are kept.

  Where things go (the folder holding bf3_server_Final.exe, and "BotMod" beside it), e.g.:
    ALP / bf4play:  ...\Battlefield 3 Server\Battlefield_3_Server\   and  ...\Battlefield 3 Server\BotMod\
    BF-Portable:    ...\BF-Portable\Battlefield_3_Server\            and  ...\BF-Portable\BotMod\

REQUIREMENTS
  - The ALP / bf4play / BF-Portable BF3 server build (bf3_server_Final.exe). The installer warns if yours is
    different - BotMod hooks that exact build and would crash another one.
  - Clients need nothing extra (bots are server-side). Expansion maps still need the expansions on clients.

IN-GAME CHAT COMMANDS
  !bots              bot status
  !bots <n>          total player count to fill with bots (humans included), e.g. !bots 32;  !bots fill
  !bots off / on     pause / resume all bots
  !map <name>        switch map (!map alone lists them)
  !pos               your position + how much of the bots' map you can reach (for reporting stuck spots)
  Admin-only commands (!bots <n>, !bots off / on, !map): players listed in BotMod\admins.txt - it ships
  with "Administrator"; add your own player name(s), one per line. Delete the file to let everyone use them.

SETTINGS (in the "BotMod" folder next to Battlefield_3_Server)
  admins.txt             player names allowed to use the admin chat commands
  botcount.txt           default bot count (total players incl. humans)
  botcount_<port>.txt    per-server-instance override, e.g. botcount_25301.txt = 0 for a no-bot 2nd instance
  kitskip.txt            items bots should not carry on a map
  Battlefield_3_Server\BotMod_kitextra.txt   kit overrides (engineers carry AT mines; all kits get the Ammo perk)

FIRST RUNS
  Navigation maps for all stock Conquest maps are included. A map without one (e.g. some expansion maps) is
  surveyed automatically the first time it loads - bots walk straight at objectives until that finishes.

REPORTING PROBLEMS (beta)
  Please send: what happened + roughly when, the map, and these files from Battlefield_3_Server:
    BotMod_<port>.log (e.g. BotMod_25300.log)  and any new  Instance\Logs\CrashDump_*.dmp
  For a stuck bot: stand where it is stuck, type !pos, and include the reply.
