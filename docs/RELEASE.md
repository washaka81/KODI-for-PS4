# Release process

The project uses semantic versioning (`MAJOR.MINOR.PATCH`) stored in
`VERSION`. Every published version must be validated on a PS4 first.

## 1. Prepare the version

```bash
./build.sh --bump patch      # 0.0.1 -> 0.0.2
# or: ./build.sh --set-version 0.1.0
```

Edit `CHANGELOG.md` and describe the change. Keep the entry short and
user-facing.

## 2. Build and test

```bash
# One command from a fresh clone (toolchain, Kodi, deps, port and PKG).
./build.sh --all --diag
```

Use `./build.sh --all` for the release series and `./build.sh` for
subsequent incremental builds.

The PKG is written to `dist/kodi-ps4-<version>.pkg`; the script prints its
SHA-256 and runs the privacy check before packaging.

Install it on the test console, exercise the changed area and keep the
logs. Do not continue if the PKG does not boot or if a new coredump
appears.

## 3. Commit and tag

```bash
git add VERSION CHANGELOG.md
git commit -m "release: v<version>"
./build.sh --tag
```

`--tag` builds again if needed and creates the local annotated tag
`v<version>`. It refuses to tag a dirty working tree.

## 4. Publish to GitHub

Only after console validation:

```bash
./build.sh --push --validated
```

`--validated` is the explicit confirmation that the PKG was tested on
console. The script pushes the current branch and tags to `origin`.

If no remote exists yet:

```bash
git remote add origin https://github.com/<owner>/<repo>.git
git push -u origin master
```

Then create the GitHub release from the tag and attach
`dist/kodi-ps4-<version>.pkg` plus its SHA-256. Do not attach logs,
databases, M3U/EPG lists or console captures.

## 5. Privacy checklist

- `scripts/privacy-check.sh` passes.
- The PKG contains no `userdata`, `addon_data`, logs, databases, keys or
  private IP/MAC addresses.
- The release notes contain no personal data or console identifiers.
- No secrets or tokens are committed.

See `PRIVACY.md` for the full policy.
