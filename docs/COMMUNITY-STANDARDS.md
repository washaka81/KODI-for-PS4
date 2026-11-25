# Community standards checklist

Status of the repository against GitHub's recommended community standards.

| Item | Status | File |
|---|---|---|
| README | ✅ | `README.md` |
| Code of conduct | ✅ | `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1) |
| Contributing | ✅ | `CONTRIBUTING.md` |
| Writing contributing guidelines | ✅ | `CONTRIBUTING.md` |
| License | ✅ | `LICENSE` (GPL-3.0) + `NOTICE.md` |
| Security policy | ✅ | `SECURITY.md` |
| Set up a security policy | ✅ | private GitHub Security Advisory workflow documented |
| Issue templates | ✅ | `.github/ISSUE_TEMPLATE/bug_report.yml`, `feature_request.yml`, `config.yml` |
| Pull request template | ✅ | `.github/PULL_REQUEST_TEMPLATE.md` |

## Privacy rules

- No personal data, names, emails, account names or user paths.
- No console/home-network IP or MAC addresses, SSIDs or hostnames.
- No `userdata`, `addon_data`, databases, playlists, EPG, logos, credentials
  or tokens.
- Logs, screenshots and videos must be sanitized.
- No prompts, tool traces, model names, session identifiers or other
  automated-tooling metadata. Local tool notes are ignored by `.gitignore`.

See `PRIVACY.md` and `CONTRIBUTING.md`.

## Repository hygiene

- `scripts/privacy-check.sh` runs before packaging and aborts on private
  data or provider assets.
- Release packages are published only after console validation.
- `src/kodi` is disposable; the source of truth is `port/`.
- Diagnostic canaries and probes stay in `port/diag/`, never in release.

## Before publishing the repository

- Review the public tree for internal workflow or tool files; keep them
  out of the published tree.
- Review local tool notes; they are ignored and should stay out
  of the public tree.
- Run the privacy scan and check the issue/PR templates render correctly on
  GitHub.
- Confirm the license and third-party notices with the maintainers.

## Language

Public repository content is written in English: documentation,
changelog, release notes, commit messages, issues and pull requests.
