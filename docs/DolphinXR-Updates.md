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

## Publishing a release

1. Commit, tag and push: `git tag v0.5` then `git push origin openxr-work v0.5`.
2. Build Release from that commit, with no uncommitted changes:
   `cmake --build Build-vs2022 --config Release --target dolphin-emu winupdater`
3. Optionally, test the updater on this build:
   `python Tools\dolphinxr-update.py selftest Build-vs2022\Binaries\Dolphin.exe`
   It updates a copy of the build from a local server, as a user's copy would. Two windows open
   and close by themselves.
4. Write the release notes in Markdown, for example `notes.md` (the same text as the GitHub
   release).
5. Publish:
   ```
   python Tools\dolphinxr-update.py publish --site ..\dolphinxr-updates --binaries Build-vs2022\Binaries --name v0.5 --notes notes.md --key %USERPROFILE%\.dolphinxr\update-signing-key.pem --zip DolphinXR-v0.5-Windows.zip
   ```
   It refuses a build that has uncommitted changes, was not built from the pushed `HEAD`, or
   ships a `Sys` folder that differs from `Data/Sys`. It also refuses a key that `Updater.exe`
   would not accept.
6. Check, commit and push the site: `git add -A`, `git commit -m "v0.5"`, `git push`. GitHub
   serves it a few minutes later.
7. Create the GitHub release with the zip from step 5. It holds exactly the files of the
   manifest, so a fresh install of it updates cleanly later.

## Good to know

- The first release built with this updater (after v0.4) has to be installed by hand: v0.4 and
  earlier never check for updates.
- To publish a release again (for example after a mistake), add `--replace`.
- The site keeps the files of the two newest releases and every manifest. The site's git
  history grows by the size of each release's new files (about 10 to 35 MB). When the repository
  nears 1 GB, replace its history with a single commit (an orphan branch, then a force push).
- `Updater.exe` writes its log to `User/Logs/Updater.log`.
- The Linux AppImage and the Quest APK do not use this updater.
