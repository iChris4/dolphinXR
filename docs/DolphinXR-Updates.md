# DolphinXR updates (Windows)

DolphinXR updates itself from its own releases, not from dolphin-emu.org. Upstream's updater was
switched off in May 2026 after it replaced a VR build with official Dolphin: the first DolphinXR
release had been built on an upstream commit, so the update server recognized it as an outdated
official build.

## How it works

The update server is a static site, the GitHub Pages repository `iChris4/dolphinxr-updates`, served
at `https://ichris4.github.io/dolphinxr-updates` (`DOLPHINXR_UPDATE_SERVER_URL` in
`Source/Core/UICommon/AutoUpdate.cpp`):

| Path | Content |
| --- | --- |
| `update/v1/stable/win/latest.json` | The releases, newest first: name, commit hash, release notes |
| `update/v1/win/manifests/<hash>` | The files of each release with their hashes, signed |
| `update/v1/content/` | The files themselves, gzipped and named by hash |

1. At startup (or Help > Check for Updates) DolphinXR downloads `latest.json` and looks for the
   commit hash it was built from.
2. If that hash is the newest release, it is up to date. If it is an older release, it shows the
   release notes since that version and offers to update. If it is not listed at all (a
   development build), or the build had uncommitted changes, it never updates.
3. `Updater.exe` downloads both manifests and checks their signature against the public key built
   into it (`UPDATE_PUB_KEY` in `Source/Core/UpdaterCommon/UpdaterCommon.cpp`). It downloads the
   changed files, checks each hash, replaces them, deletes the files the new release no longer
   has, and restarts DolphinXR.

The updater only touches files listed in the manifests: the `User` folder of a portable install
is safe. A listed file that was changed by hand is renamed to `.bak` before it is replaced.

The setting is Settings > General > Auto Update ("DolphinXR Releases" or "Don't Update"),
stored as `[AutoUpdate] DolphinXRTrack` in `Dolphin.ini`. It is not upstream's `UpdateTrack`,
which an official Dolphin sharing the same user folder writes.

## One-time setup

1. **The signing key.** It was created on 2026-11-01 at
   `C:\Users\Christophe\.dolphinxr\update-signing-key.pem`. Keep a backup somewhere safe (a
   password manager, a USB key) and never commit it.
   - If it is lost, installed copies can no longer be updated: a release with a new key
     (`keygen`, then the printed `UPDATE_PUB_KEY` in `UpdaterCommon.cpp`) has to be installed by
     hand.
   - If it leaks, do the same, and tell users to reinstall by hand.
2. **The site.** Create the public repository `iChris4/dolphinxr-updates` on GitHub. In Settings >
   Pages, choose "Deploy from a branch", branch `main`, folder `/ (root)`. Clone it next to the
   source tree, for example `..\dolphinxr-updates`.
3. **The release checkout.** Releases are built in a separate clone, `..\DolphinXR-release`, so
   uncommitted work in the everyday tree never reaches a release. It was made on 2026-11-03 for
   v0.5, with its submodules cloned from the everyday tree's `.git/modules` and its `origin` on
   GitHub. Its build directory is `Build`, configured like `Build-vs2022`:
   `cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DENABLE_VR=ON`.
   - `Externals/OpenXR` is pinned in git to `a170408a4d`, a commit that exists nowhere (set by
     the "WIP Docker" commit `ca0070222d`). The everyday tree builds with `d6fe963` instead (SDK
     1.1.57 plus a fix for non-MSVC compilers, so the same as 1.1.57 on Windows), and so does the
     release checkout. Its config skips that submodule (`submodule.Externals/OpenXR.update none`
     and `ignore all`), so `git submodule update` leaves it alone.

## Publishing a release

1. Tag the release commit and push it: `git tag v0.6 <commit>` then
   `git push origin <commit>:refs/heads/openxr-work v0.6`. The tag is a plain (lightweight) tag
   like the earlier ones: an annotated tag would change the version string `git describe` gives.
2. In `..\DolphinXR-release`, check out the tag and build it:
   ```
   git fetch origin --tags
   git checkout v0.6
   git -c protocol.file.allow=always submodule update --init --recursive
   cmake --build Build --config Release --target dolphin-emu dolphin-tool winupdater --parallel
   ```
   Use Visual Studio's bundled CMake (3.31), as for `Build-vs2022`. `git describe --dirty` must
   print no `-dirty`.
3. Test the updater on this build:
   `python Tools\dolphinxr-update.py selftest Build\Binaries\Dolphin.exe`
   It updates a copy of the build from a local server, as a user's copy would. Two windows open
   and close by themselves.
4. Write the release notes in Markdown, for example `notes.md` (the same text as the GitHub
   release).
5. Publish, from `..\DolphinXR-release`:
   ```
   python Tools\dolphinxr-update.py publish --site ..\dolphinxr-updates --binaries Build\Binaries --name v0.6 --notes notes.md --key %USERPROFILE%\.dolphinxr\update-signing-key.pem --zip DolphinXR-v0.6-Windows.zip
   ```
   It refuses a build that has uncommitted changes, was not built from the pushed `HEAD`, or
   ships a `Sys` folder that differs from `Data/Sys`. It also refuses a key that `Updater.exe`
   would not accept.
6. Check, commit and push the site: `git add -A`, `git commit -m "v0.6"`, `git push`. GitHub
   serves it a few minutes later.
7. Create the GitHub release with the zip from step 5. It holds exactly the files of the
   manifest, so a fresh install of it updates cleanly later.

## Good to know

- v0.5 (`944a8da23c`, 2026-11-03) is the first release with this updater, and has to be
  installed by hand: v0.4 and earlier never check for updates.
- To publish a release again (for example after a mistake), add `--replace`.
- The site keeps the files of the two newest releases and every manifest. The site's git
  history grows by the size of each release's new files (about 10 to 35 MB). When the repository
  nears 1 GB, replace its history with a single commit (an orphan branch, then a force push).
- `Updater.exe` writes its log to `User/Logs/Updater.log`.
- The Linux AppImage and the Quest APK do not use this updater.
