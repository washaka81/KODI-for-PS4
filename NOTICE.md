# Notices and third-party licenses

This repository is distributed under the **GNU General Public License
version 3.0**; see `LICENSE`.

The Kodi source tree is not vendored. It is fetched from
<https://github.com/xbmc/xbmc> and is licensed under
**GPL-2.0-or-later**. The port patches and overlay are applied on top of
that tree.

Third-party components keep their own licenses. The main ones are:

| Component | License / notice |
|---|---|
| Kodi | GPL-2.0-or-later |
| FFmpeg | LGPL-2.1-or-later or GPL, depending on the build configuration |
| curl | curl license |
| OpenSSL | Apache-2.0 |
| CPython | PSF-2.0 |
| OpenOrbis PS4 Toolchain | see the toolchain distribution |
| `tools/liborbispkg` / LibOrbisPkg | LGPL-3.0 (see `tools/liborbispkg/LICENSE.txt`) |
| `tools/gnm-engine/shared/src/libs` | see the headers and license files in each directory |

If a file or directory contains its own license, that license governs that
part. Do not remove copyright or license headers when contributing.

No personal data, credentials, console identifiers or provider assets are
part of this repository. See `PRIVACY.md`.
