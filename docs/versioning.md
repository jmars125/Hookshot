# Versioning and releases

Hookshot follows [Semantic Versioning 2.0.0](https://semver.org/) and the conventions of
[semantic-release](https://semantic-release.org/). The current version is **0.1.0**.

The git tag is the only source of truth. No file carries a version that has to be edited for a release: the game reads
the tag through `nx2/version.py`, and the builder and the AppImage are stamped from the same tag when they are
packaged.

## The version number

`MAJOR.MINOR.PATCH`, with an optional prerelease suffix: `0.1.0`, `0.2.0`, `0.2.0-rc.1`. A release is tagged
`v0.1.0`: the `v` belongs to the tag, not to the version. Build metadata is not used in tags.

One version covers the whole project: the game (`hookshot.exe` / `hookshot`), the Vulkan back end, the lift pipeline
and its scripts, Hookshot Builder and the Linux AppImage. They are built from the same commit and released together,
so they never carry different versions.

The "public" surface the rules protect is the command line of `make_game.py` and the builder, the `RX_` and `HOS_`
switches, the files a build writes into a game folder (`hookshot_options.ini`, `hookshot_env.txt`, `language.txt`,
`hookshot_version.txt`), saves, and the format of `hookshot_log.txt`. The C sources and internal headers are not an
API and may change in any release.

## Commits decide the version

Releases are computed from the commits on the release branch since the last tag, so commit subjects must be
[Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): description
type(scope)!: description        (a breaking change)
```

`scope` is optional. A `!` before the colon, or `BREAKING CHANGE:` / `BREAKING-CHANGE:` in the commit body, marks a
breaking change. The description is written as an imperative ("Stop scanning...", "Fix the..."), because it becomes
the changelog line.

| Commit | Next version |
|---|---|
| any commit with `!` or `BREAKING CHANGE:` | major (`0.1.0` -> `1.0.0`) |
| `feat` | minor (`0.1.0` -> `0.2.0`) |
| `fix` | patch (`0.1.0` -> `0.1.1`) |
| `perf` | patch |
| `revert` | patch |
| `docs`, `refactor`, `style`, `test`, `build`, `ci`, `chore` | no release |

`git revert` writes `Revert "..."`, which is not a Conventional Commit: use `git revert --edit` and write the subject
as `revert: ...`, or the revert makes no release.

A release takes the highest bump among its commits. Merge commits are ignored, and a commit with `[skip release]` or
`[release skip]` in its message is left out of the analysis. `chore(release): 0.2.0 [skip ci] [skip release]`, the
commit the release itself makes, is not a release (the notes in its body can mention a breaking change).

There is no manual version bump. If a change should not make a release, use one of the types in the last row; if it
should, use `feat` or `fix`.

## 0.x and 1.0.0

semantic-release does not treat major version zero specially: a breaking change is a major bump even from `0.1.0`.
This is deliberate. Its documentation says that SemVer's different rules for `0.x` are out of scope, and it recommends
`1.0.0` as the first release.

Hookshot starts at `0.1.0` anyway, because the first versioned release is not stable. That tag is made once by hand
(see below); after it, the release workflow is the only thing that creates tags. Consequence: the first breaking change
merged to `main` releases `1.0.0`, not `0.2.0`.

`1.0.0` means the public surface above is considered stable. Until then, keep breaking changes on the `next` branch
and release them there as `1.0.0-rc.N`, or accept that the release that carries one becomes `1.0.0`.

## Branches and the release workflow

| Branch | Purpose | Version |
|---|---|---|
| `main` | stable releases | `vX.Y.Z` |
| `next` | release candidates | `vX.Y.Z-rc.N` |

Every push to `main` or `next` runs the [Release workflow](../.github/workflows/release.yml). It:

1. finds the last release tag and the commits since it;
2. takes the highest bump in the table above;
3. runs `scripts/changelog.py` to put the new entry at the top of `changelog.txt`;
4. commits `changelog.txt` as `chore(release): <version> [skip ci] [skip release]`;
5. tags the release commit. The tag starts the Linux AppImage workflow, which publishes the GitHub release with the
   AppImage attached.

A merge with no releasable commit (only docs, chores, and so on) makes no release. A release is never edited by hand,
never re-tagged and never deleted. Require the *Game-free source regressions* workflow on `main` and `next` (branch
protection): the release then runs for a commit whose tests have passed.

The workflow pushes back to the repository, so it needs a token with `contents: write` that may bypass `main`'s branch
protection. Use a `RELEASE_TOKEN` secret (a fine-grained personal access token) rather than the built-in
`GITHUB_TOKEN`: a tag pushed with `GITHUB_TOKEN` does not start another workflow, and the Linux AppImage workflow
publishes the release from the tag. With only `GITHUB_TOKEN`, build the AppImage with `workflow_dispatch` and create the
release by hand.

## The first tag: 0.1.0

semantic-release's first release is `1.0.0` when it finds no tag. To make `0.1.0` the first version, tag the merge
commit that introduces this scheme once:

```sh
git tag -a v0.1.0 -m "Hookshot 0.1.0"
git push origin v0.1.0
```

The Release workflow does nothing while no version tag exists, so the merge cannot release `1.0.0` first. The pushed
tag starts the Linux AppImage workflow, which creates the `0.1.0` GitHub release with the AppImage attached; create the
release by hand if you do not want to wait for it. The `0.1.0` entry in `changelog.txt` is written for that tag. After
it, do not tag by hand.

## Pre-releases

A release candidate is released from `next` with the same commands. semantic-release numbers it `v0.2.0-rc.1`,
`v0.2.0-rc.2`, and so on; the GitHub release is marked as a prerelease and never becomes "latest". A prerelease is not
written to `changelog.txt`: Factorio's parser accepts only `major.minor.patch`, and two release candidates share a base
version. Its notes are in the GitHub prerelease. Merge `next` into `main` to release the final `0.2.0`, whose entry
lists every commit since the last stable tag.

## Hotfixes

Branch from the release tag, not from `main`, when a fix must not include `main`'s unreleased features:

```sh
git switch -c fix/crash v0.1.0
```

Commit with `fix: ...` and open a pull request to `main`. If `main` has no unreleased feature commits, the release is a
patch. The project does not keep maintenance branches for older minor versions: only the newest release is supported.

## Where the version appears

| Place | Where it comes from |
|---|---|
| git tag | semantic-release; the source of truth |
| `changelog.txt` | `scripts/changelog.py` during the release |
| `hookshot_log.txt` and the Insert menu | `nx2/version.py`: `HOOKSHOT_VERSION`, then `VERSION`/`version.txt`, then `git describe --tags --match "v[0-9]*"` |
| `hookshot_version.txt` in a build | the `VERSION` file written when the tools are packaged (`nx2/make_game.py`) |
| AppImage file name | `git describe --tags --match "v[0-9]*"` (`scripts/linux/appimage.sh`) |
| `HookshotBuilder.exe` | the MSBuild `Version` passed at publish time (`dotnet publish -p:Version=<version>`); the `csproj`'s `0.1.0` is only the default for a local build |

`nx2/version.py` also appends a fingerprint of the source files, `(source 1a2b3c4d)`. It is not a version: it tells
two builds of the same release apart, and it is the only identifier a build from a modified checkout has.

`package.json` and `package-lock.json` (if present) are the release tooling's, not the project's: `package.json` keeps
the placeholder `0.0.0-development` on purpose, and the tag remains the version. The game never reads them.

## The changelog

`changelog.txt` follows [Factorio's changelog format](https://lua-api.factorio.com/latest/auxiliary/changelog-format.html).
Newest first, with a 99-dash separator before each version:

```
---------------------------------------------------------------------------------------------------
Version: 0.1.0
Date: 07. 11. 2026
  Features:
    - Something new.
  Bugfixes:
    - Something fixed.
```

The date is day. month. year (`07. 11. 2026`), as in Factorio's own changelog. Section names are indented two
spaces, bullets four, there are no tabs or trailing spaces, and a section is omitted when it is empty. The version is
always three numbers: a prerelease has no entry (see above).

| Commit | Section |
|---|---|
| `feat` | Features |
| `fix`, `revert` | Bugfixes |
| `perf` | Optimizations |
| `refactor` | Changes |
| breaking change | Changes, as `Breaking: ...` |
| `docs`, `style`, `test`, `build`, `ci`, `chore` | not listed |

A stable release writes the entry. Do not renumber, reorder or rewrite a released entry; a correction is a new commit
and a new release. The `0.1.0` entry was written by hand because the history before this scheme does not use
Conventional Commits.

## History

Tags from before this scheme (`Release`, `amd`, `text-hotfix`, and so on) are snapshots of the old workflow. They stay
in the repository for history but are not releases: the tooling matches only `v[0-9]*` tags. `v0.1.0` is the first
versioned release.

## Rules

- Do not edit a version number in a file, move a tag or reuse a version.
- Do not tag a branch other than `main` (or `next` for a release candidate).
- Do not release from a dirty working tree; the release commit contains `changelog.txt` and nothing else.
- Do not put game files, keys, builds or anything made from a dump in a release.
