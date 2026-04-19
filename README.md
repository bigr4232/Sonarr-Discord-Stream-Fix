# MuteDiscordDevice

Mute Discord only on a specific audio output device — useful for streaming with Discord audio on your headphones but not on your stream.

## Why you'd use this

If you stream with OBS and route game audio through a virtual device (Voicemeeter, VB-Cable, a second sound card), you probably want your viewers to hear your game but **not** your Discord calls. Windows has no built-in way to say "mute Discord on this one device." MuteDiscordDevice sits in your tray and does exactly that: on the devices you pick, Discord's audio session is kept muted; on every other device (your headphones), Discord plays normally.

## Install

1. Download the latest `MuteDiscordDevice-Setup-X.Y.Z.exe` from the [Releases page](../../releases).
2. Run it. No admin prompt — it installs to your user profile (`%LOCALAPPDATA%\MuteDiscordDevice`).
3. Leave **Start MuteDiscordDevice automatically when Windows starts** checked unless you want to launch it manually.
4. Finish. The app launches and a tray icon appears near the clock.

Windows SmartScreen may warn the first time you run the installer (the binary is unsigned). Click **More info → Run anyway**.

## First run

1. Right-click the tray icon → **Configure Devices**.
2. Check the audio output devices where Discord should stay muted (typically your stream/virtual cable device — *not* your headphones).
3. Click **OK**. Your choice is saved to `devices.txt` next to the exe.
4. Launch Discord and join a voice call. The tool will mute Discord's session on the selected device(s) automatically.

## Filter modes

Right-click the tray → **Filter Mode** to choose which Discord processes get muted:

- **StreamOnly** (recommended) — auto-detects Discord's stream/voice child process and mutes only that. Leaves other Discord audio (notifications, etc.) alone on your stream device.
- **All** — mutes every Discord process on the selected device. Simplest; mute everything Discord makes noise about.
- **By Ordinal** — mute the Nth Discord audio session on the device. Useful if you know the session order is stable.
- **By Fingerprint** — mute a specific session matched by a stable identifier. Advanced; most users won't need this.

## Troubleshooting

- Right-click the tray → **Diagnose Discord Sessions**. This opens a text log showing what audio sessions Discord has open on each device. Attach this log when asking for help.
- If the tray icon doesn't appear after install, check the hidden-icons area (the `^` arrow near the clock) or launch **MuteDiscordDevice** from the Start Menu.
- If Discord isn't being muted, make sure Discord is *actually* playing audio on the device you selected — open Windows **Volume Mixer** while in a call and confirm Discord shows up on that device's mixer.

## Uninstall

**Settings → Apps → Installed apps → MuteDiscordDevice → Uninstall.**

Your `devices.txt` is preserved in the install folder. If you reinstall later, your device selection comes back automatically. To remove it fully, delete `%LOCALAPPDATA%\MuteDiscordDevice` after uninstalling.

## Building from source

See [BUILDING.md](BUILDING.md) for build prerequisites, build commands, and the release process.

## License

MIT — see [LICENSE](LICENSE).
