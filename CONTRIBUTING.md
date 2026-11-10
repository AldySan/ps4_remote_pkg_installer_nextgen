# Contributing to Remote PKG Installer NextGen

Thanks for wanting to help. Whether you found a typo, hit a bug, or want
to build something new, this guide walks you through it. If you have never
contributed to an open-source project before, this is a decent place to
start: the codebase is small, and the maintainers are happy to help.

## Ways to contribute

You do not have to write C to be useful here:

- Report bugs and request features using the
  [issue templates](https://github.com/AldySan/ps4_remote_pkg_installer_nextgen/issues/new/choose).
- Improve the docs. Small fixes count.
- Test new releases on your console and report what works and what does
  not. Hardware reports are genuinely valuable, since not every
  contributor owns a jailbroken PS4.
- Pick up an issue marked `bug` or `enhancement` and send a pull request.

If you plan a larger change, please open an issue first and talk it
through before investing a lot of time. It saves everyone rework.

## Setting up the repository

Development happens on the `dev` branch. `main` only receives releases.

```sh
git clone https://github.com/AldySan/ps4_remote_pkg_installer_nextgen.git
cd ps4_remote_pkg_installer_nextgen
git checkout dev
```

If you are contributing for the first time, fork the repository on
GitHub first, then clone your fork and add the upstream remote:

```sh
git remote add upstream https://github.com/AldySan/ps4_remote_pkg_installer_nextgen.git
```

For building, you need the
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
(`OO_PS4_TOOLCHAIN` environment variable), plus `clang`, `lld`, and
`python3`. See the [README](README.md) for the full build instructions.

## Building and testing

The full package build needs the PS4 toolchain:

```sh
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
make
```

The host unit tests cover the portable core (util, SFO, and pkg parsing)
and run on your machine with no PS4 required:

```sh
make test
```

Please run `make test` before sending a pull request. CI will run it
again, together with a full PS4 build, so keeping it green locally saves
you a round trip.

## Development workflow

1. Start from an up-to-date `dev`:

   ```sh
   git checkout dev
   git pull upstream dev
   git checkout -b fix/short-description
   ```

   Branch names follow the `type/short-description` pattern, for example
   `fix/ui-jobs-off-stack` or `docs/improve-readme`.

2. Make your change. One branch, one topic: a PR that fixes a bug should
   not also reformat unrelated files.

3. Commit with a
   [conventional commit](https://www.conventionalcommits.org/) message in
   English: short, lowercase type, imperative mood, for example:

   ```
   fix(ui): move the job list snapshot off the stack
   feat(ui): human-readable lines for known error codes
   docs: record design direction in DESIGN.md
   ```

4. Push your branch and open a pull request against `dev`. Fill in the
   pull request template: what changed, why, and how you tested it.

5. Wait for CI. It runs the host tests and builds the PS4 package. If it
   fails, the log usually tells you why, and you can push fixes to the
   same branch.

6. After review, a maintainer merges your PR into `dev` with a summary
   comment, and the source branch is deleted once it is no longer needed.

Releases are handled by the maintainers: `dev` is squash-merged into
`main` as a single release commit, tagged `vX.Y.Z`, and the release
workflow attaches the built pkg automatically. You do not need to do
anything release-related in a normal contribution.

## Code style

The short version: match the code around you. The specifics that matter:

- Plain C11, no framework. 4-space indentation, no tabs.
- Opening brace on its own line for functions, same line for control
  flow. This mix is intentional, so follow it.
- `snake_case` for functions and variables. PS4 ABI types keep their
  original casing, like `SceBgftDownloadParam`.
- The build uses `-Wall -Wextra`; keep new code free of warnings.
- Comments explain non-obvious constraints and nothing else. No banner
  lines, no restating what the code already says.
- Do not delete unused imports, functions, or variables unless you are
  actively working on that code. They may be intentional, left there by
  another developer.
- Product copy (on-screen strings and docs) uses plain punctuation, no em
  dashes.

Docs changes only need Markdown that renders cleanly on GitHub.

## Need help?

If anything here is unclear, or you get stuck at any step, open an issue
or ask directly in your pull request. Asking is always welcome, and by
participating you agree to uphold our [Code of Conduct](CODE_OF_CONDUCT.md).
