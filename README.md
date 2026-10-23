# Battl3Bots

AI bots for **Battlefield 3 dedicated servers** (`bf3_server_Final.exe`, offline / LAN). Formerly "BF3 BotMod".

The bots fill your server and play the mode on their own. They take objectives, drive and fly vehicles, and revive, heal and resupply each other. They also answer your in-game commands ("Get in", "Need medic", "Need ammo", "Get out").

Supported modes:

- Conquest (Large and Assault)
- Rush, which runs from the first round with no human on the server
- Capture the Flag
- Air Superiority
- Tank Superiority

## Install

1. Download `Battl3Bots-beta.zip` from the [latest release](../../releases/latest).
2. Unzip it.
3. Run `BF3-BotMod-beta\Install-BotMod.bat` and pick your server's `bf3_server_Final.exe`.
4. Start the server as usual (ALP, bf4play, BF-Portable or a .bat). A **Battl3Bots** console window opens with it.

Full instructions, settings and admin commands are in [`BF3-BotMod-beta/README.txt`](BF3-BotMod-beta/README.txt).

Note: the per-map navigation data (over 100 MB) ships only in the release zip, not in this repository.

## Internal names

The files keep their old internal names (`BotModCore.dll`, the `BotMod` folder, `BotMod_*.txt`), so existing installs and settings carry over when you update.
