# Security Policy

## Supported versions

Security fixes are applied to the latest package and the current
diagnostic/release series. Archived packages (`dist/` and old
`/user/data/pkg` files) are kept for rollback only and are not supported.

## Reporting a vulnerability

**Do not open a public issue with exploit details, credentials, personal
data or console/network identifiers.**

Use the repository's private GitHub Security Advisory form:

1. Open the **Security** tab.
2. Choose **Report a vulnerability**.
3. Include the affected version, package SHA-256, PS4 model/firmware,
   impact, reproduction steps and sanitized logs.

If private reporting is unavailable, open a minimal public issue titled
`Security contact request` **without details** and a maintainer will provide
a private channel.

## What to include

- Kodi/package version and SHA-256.
- PS4 model and firmware.
- Whether the issue affects the release series, the diagnostic series or
  both.
- Steps to reproduce and the observed result.
- Sanitized logs: remove IP/MAC addresses, SSIDs, user paths, account names,
  tokens, certificates and `userdata` content.

## Scope

In scope:

- the PS4 platform code, GLES/EGL shim, GNM/VideoOut, audio, input, storage;
- network/TLS/DNS/FFmpeg translators and the package/build scripts;
- addon and Python integration shipped by this repository.

Out of scope:

- vulnerabilities in upstream Kodi, FFmpeg, curl, OpenSSL, CPython or
  addons that are not caused by the port; report those upstream;
- issues that require publishing credentials, personal data or a modified
  console;
- denial of service caused only by a malicious provider or playlist.

## Privacy

Follow `PRIVACY.md` and `CONTRIBUTING.md`. Never include personal data,
prompts, tool traces, model names, session identifiers or other automated-tooling
metadata in a report.

## Disclosure

We aim to acknowledge a report within a reasonable time, confirm the
affected versions, prepare a fix and coordinate disclosure. There is no bug
bounty. Please give us time to ship a validated package before publishing
details.
