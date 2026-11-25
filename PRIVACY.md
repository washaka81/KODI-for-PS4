# Privacy and data policy — Kodi PS4

This project is a free Kodi port for PS4. **It does not collect, transmit
or publish personal data.**

## 1. What is not collected

- No telemetry, analytics, accounts or user identifiers.
- Logs, databases, M3U/EPG lists, logos, credentials, IP addresses, MAC
  addresses, SSIDs and user paths are not sent to this repository or to
  GitHub.
- The PKG does not include `userdata`, `addon_data`, `TV*.db`, `Epg*.db`,
  playlists, keys, private certificates or a specific console
  configuration.

## 2. Data on the console

- User configuration lives in the PS4 `userdata` directory and is never
  copied to the repository.
- Debug logs stay on the console or in the local `build/` directory; they
  are never published.
- Uploading the PKG to the console is a local user operation; there is no
  central server.

## 3. Repository and GitHub

- Commits must not contain personal data, secrets, tokens, SSH keys,
  passwords, home-network IP/MAC addresses or console captures.
- `scripts/privacy-check.sh` checks `port/`, `packaging/` and the PKG
  staging area before packaging. It aborts on `userdata`, `addon_data`,
  M3U/EPG files, logs, databases, keys or private addresses.
- GitHub releases are published only after the PKG has been validated on
  console (project rule).

## 4. Third parties

- Kodi, FFmpeg, curl, OpenSSL, CPython, SDL/OpenOrbis and the addons have
  their own licenses and data policies. This repository does not change
  their data practices.
- User URLs (M3U, EPG, logos) are local configuration and are not part of
  the project.

## 5. Contact

To report an accidental data exposure, open an issue **without** the
sensitive value and ask for a private channel.
