# Pull request

## Summary

<!-- What changes, and why? Keep it short. -->

## Related issue / session note

<!-- Link the issue or the relevant documentation. -->

## Type of change

- [ ] Translator / platform fix (no Kodi behavior change)
- [ ] ABI/toolchain compatibility (minimal, documented)
- [ ] Addon/Python support
- [ ] Packaging / build
- [ ] Documentation / tests
- [ ] Behavior or UI change (requires a documented exception)

## Port workflow

- [ ] I ran `./scripts/check-upstream.sh` and reviewed `build/upstream-check.txt`.
- [ ] My changes are captured in `port/` (`capture`), not only in `src/kodi`.
- [ ] `./scripts/kodi-port.sh check` passes.
- [ ] Diagnostic-only changes are in `port/diag/`, not in `port/`.
- [ ] I did not change Kodi logic, UI or behavior without a documented exception.

## Validation

- [ ] Package built with `./scripts/package-port.sh`.
- [ ] `pkg_validate` passes.
- [ ] Tested on a real PS4.
- [ ] PS4 model/firmware:
- [ ] Package version / SHA-256:
- [ ] Steps and result:
- [ ] Relevant sanitized logs (`kodi.log`, `crash.log`, `mem.log`, shim):
- [ ] Memory figures when relevant (`flex`, `rssmax`, `alloc-fail`):
- [ ] If not tested on hardware, explained why:

## Privacy and hygiene

- [ ] No personal data, names, emails, account names or user paths.
- [ ] No IP/MAC addresses, SSIDs, hostnames or console identifiers.
- [ ] No `userdata`, `addon_data`, databases, playlists, EPG, logos, credentials or tokens.
- [ ] Logs/screenshots/videos are sanitized.
- [ ] No prompts, tool traces, model names, session identifiers or automated-tooling metadata.
- [ ] `./scripts/privacy-check.sh` passes when packaging is involved.

## License

- [ ] My contribution can be distributed under the repository license (GPL-3.0).
- [ ] Third-party code keeps its license and provenance.
- [ ] I did not remove copyright or license headers.
