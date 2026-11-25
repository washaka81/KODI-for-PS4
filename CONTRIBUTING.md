# Contributing to Kodi PS4

Thanks for helping. This is a port of Kodi to the PlayStation 4; the goal is
to keep Kodi as close to upstream as possible and to isolate console-specific
work in translators and platform code.

## Before you start

Read, in this order:

1. `README.md` — what works and how the port is organized.
2. `port/README.md` — patch series, overlays and the capture/apply workflow.
3. `docs/BUILD.md` — toolchain, build and packaging.
4. `docs/ARCHITECTURE.md` — video, audio, network and add-on layers.
5. `PRIVACY.md` — what must never be committed.

Do not change Kodi's logic, UI or behavior. The port provides translators
(GLES/EGL→GNM, FFmpeg/network/TLS/DNS/audio) and the platform glue. A change
that alters Kodi behavior needs a documented exception in the
relevant port documentation or issue.

## Development workflow

1. Run the upstream check and read `build/upstream-check.txt`:
   ```bash
   ./scripts/check-upstream.sh
   ```
2. Edit `src/kodi` (a disposable working tree), then capture and verify:
   ```bash
   ./scripts/kodi-port.sh capture
   ./scripts/kodi-port.sh check
   ```
3. Build and package:
   ```bash
   ./scripts/package-port.sh <name>
   ```
4. Validate the PKG on a real console before publishing:
   - install it on the PS4;
   - launch Kodi and exercise the changed area;
   - collect `kodi.log`, `crash.log`, `mem.log` and the shim log;
   - check `pkg_validate` and the SHA-256 of the uploaded file.

Diagnostic canaries, probes, memory guards and crash handlers belong in
`port/diag/`, never in the release series (`port/`).

## What we accept

- Fixes to the GLES/EGL shim, GNM/VideoOut, audio, input, storage, network
  translators and packaging.
- Minimal ABI/toolchain compatibility patches for upstream Kodi.
- Documentation, tests and reproducible evidence.
- Addon/Python support improvements that keep the package generic.

We do not accept:

- provider assets, channel lists, EPG files, logos or credentials;
- hardcoded console IP/MAC/SSID, user paths or memory sizes for one console;
- changes that make Kodi's UI or behavior different from upstream;
- binary blobs without source, license and provenance.

## Coding style

- Match the surrounding code and C++ standard.
- Keep diffs minimal; do not reformat unrelated code.
- Validate every buffer, pitch, row, alignment and size before writing.
  Memory corruption shows up later as `bad_alloc`, corrupt textures or
  `CE-34878-0`.
- For C/assembly fixes, analyze the real artifact with REA/Ghidra and record
  the function, address, instructions and the limit check in the session
  notes.
- Do not add per-frame or per-8-KiB logging to release builds.

## Validation and evidence

A change is not ready until it has been tested on a PS4. Include in the PR:

- PS4 model and firmware;
- package name, version and SHA-256;
- `pkg_validate` result;
- the exact steps and the observed result;
- sanitized logs (no personal data, see below);
- memory figures (`flex`, `rssmax`, `alloc-fail`) when relevant.

If you cannot test on hardware, say so explicitly. The project does not
publish untested changes.

## Privacy and repository hygiene

Never commit or attach:

- names, email addresses, account names or other personal data;
- console or home-network IP/MAC addresses, SSIDs, hostnames or user paths;
- `userdata`, `addon_data`, databases, playlists, EPG, logos or credentials;
- logs, screenshots or videos that were not sanitized;
- tokens, keys, certificates or `.env` files.

Use placeholders such as `<console>`, `<host>`, `<server>` and
`<user-data>`. Run:

```bash
./scripts/privacy-check.sh
```

Do not include prompts, tool traces, model names, session identifiers or
other automated-tooling metadata in commits, issues, pull requests or documentation.
Contributions must be understandable from the code, the tests and the
evidence alone.

## Commits and pull requests

- One logical change per commit; imperative subject, short body when needed.
- Reference the issue or session note.
- Fill in `.github/PULL_REQUEST_TEMPLATE.md`.
- Keep the history reviewable; do not mix formatting, refactors and behavior.
- Contributions are accepted under the repository license (see `LICENSE`).
  Third-party code keeps its own license; add the license and provenance.

## Reporting

- Bugs and feature requests: use the issue templates.
- Security problems: follow `SECURITY.md`; do not open a public issue with
  details.
- Conduct problems: follow `CODE_OF_CONDUCT.md`.

## License

This repository is distributed under the GNU General Public License v3.0;
see `LICENSE` and `NOTICE.md`. Kodi is GPL-2.0-or-later. By contributing you
agree that your contribution can be distributed under the repository
license.

## Language

All GitHub-facing content is written in English: README and docs,
changelog entries, release notes, commit messages, issues, pull requests
and review comments. Development notes that are not part of the public
tree may use another language.
