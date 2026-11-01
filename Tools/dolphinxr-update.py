#!/usr/bin/env python3
"""Publishes DolphinXR releases for the Windows auto-updater (docs/DolphinXR-Updates.md).

The update server is a static site, a clone of the GitHub Pages repository iChris4/dolphinxr-updates:
  update/v1/<track>/<platform>/latest.json  the releases of a track, newest first
  update/v1/<platform>/manifests/<hash>     the signed manifest of each release
  update/v1/content/ab/cd/<rest of hash>    the content store, gzipped files by hash

Dolphin (UICommon/AutoUpdate.cpp) reads latest.json and starts Updater.exe, which checks the
signature of the manifests against UPDATE_PUB_KEY (UpdaterCommon/UpdaterCommon.cpp) and the hash
of every file it downloads.

  keygen KEY            create the signing key: keep it secret, keep a backup
  pubkey KEY            print the public key as UPDATE_PUB_KEY
  publish ...           add a release to the site
  selftest DOLPHIN_EXE  update a copy of a build from a local server, as a user would

Signing uses the openssl command (Git for Windows has one), so nothing needs to be installed.
"""

import argparse
import base64
import datetime
import gzip
import hashlib
import html
import http.server
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Upstream's test key (Tools/test-updater.py). Updater.exe accepts it instead of UPDATE_PUB_KEY
# when DOLPHIN_UPDATE_SERVER_URL is set.
TEST_KEY_SEED = bytes.fromhex("543a581db60008bbb978a464e136d686dbc9d594119e928b5276bece3d583d81")
# PKCS#8 header of a raw Ed25519 private key (RFC 8410).
ED25519_PKCS8_PREFIX = bytes.fromhex("302e020100300506032b657004220420")

# What a build directory holds besides the release.
EXCLUDED_DIRS = {"tests", "user"}
EXCLUDED_FILES = {"portable.txt"}
EXCLUDED_SUFFIXES = {".pdb", ".ilk", ".exp", ".lib", ".bak", ".log"}

# Content referenced by this many of the newest releases of each track is kept. Older content
# is pruned: a site cached by GitHub Pages for a few minutes may still offer the previous release.
KEPT_RELEASES = 2


def fail(message):
    sys.exit(f"error: {message}")


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    for candidate in (
        r"C:\Program Files\Git\usr\bin\openssl.exe",
        r"C:\Program Files\Git\mingw64\bin\openssl.exe",
    ):
        if Path(candidate).is_file():
            return candidate
    fail("openssl not found: install Git for Windows or put openssl on PATH")


def openssl(*args):
    result = subprocess.run([find_openssl(), *args], capture_output=True)
    if result.returncode != 0:
        fail(f"openssl {args[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


class SigningKey:
    """An Ed25519 private key in a PEM file; "test" is upstream's public test key."""

    def __init__(self, spec):
        self.is_test = spec == "test"
        self._temp_dir = None
        if self.is_test:
            self._temp_dir = tempfile.TemporaryDirectory()
            self.path = Path(self._temp_dir.name) / "test-key.pem"
            der = ED25519_PKCS8_PREFIX + TEST_KEY_SEED
            self.path.write_bytes(
                b"-----BEGIN PRIVATE KEY-----\n"
                + base64.b64encode(der)
                + b"\n-----END PRIVATE KEY-----\n"
            )
        else:
            self.path = Path(spec).expanduser()
            if not self.path.is_file():
                fail(f"no signing key at {self.path}")

    def public_key(self):
        return openssl("pkey", "-in", str(self.path), "-pubout", "-outform", "DER")[-32:]

    def sign(self, data):
        with tempfile.TemporaryDirectory() as temp_dir:
            data_path = Path(temp_dir) / "data"
            signature_path = Path(temp_dir) / "signature"
            data_path.write_bytes(data)
            openssl(
                "pkeyutl", "-sign", "-rawin", "-inkey", str(self.path),
                "-in", str(data_path), "-out", str(signature_path),
            )
            signature = signature_path.read_bytes()
        if len(signature) != 64:
            fail("openssl did not make an Ed25519 signature")
        return signature


def c_array(key):
    rows = [", ".join(f"0x{b:02x}" for b in key[i : i + 16]) for i in (0, 16)]
    return "const std::array<u8, 32> UPDATE_PUB_KEY = {\n    " + ",\n    ".join(rows) + "};"


def git(*args):
    result = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"git {args[0]} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def file_hash(data):
    # Updater.exe's Manifest::Hash: the first 16 bytes of the SHA-256.
    return hashlib.sha256(data).digest()[:16].hex()


def collect_files(binaries):
    # The build links Sys and qt.conf when Windows allows symbolic links: follow them.
    paths = [
        Path(directory) / file_name
        for directory, _, file_names in os.walk(binaries, followlinks=True)
        for file_name in file_names
    ]
    files = {}
    for path in sorted(paths):
        relative = path.relative_to(binaries)
        if (
            relative.parts[0].lower() in EXCLUDED_DIRS
            or relative.name.lower() in EXCLUDED_FILES
            or path.suffix.lower() in EXCLUDED_SUFFIXES
        ):
            continue
        name = relative.as_posix()
        if "\t" in name or "\n" in name:
            fail(f"a manifest cannot hold the file name {name!r}")
        files[name] = path
    return files


def check_build(binaries, build_hash):
    """Refuses a build that does not match a pushed commit, like the first DolphinXR release,
    which carried an upstream hash and was replaced by upstream's updater."""
    if git("status", "--porcelain", "--untracked-files=no"):
        fail("the working tree has uncommitted changes: commit them and build again")
    if not git("branch", "-r", "--contains", build_hash):
        fail(f"{build_hash} is on no remote branch: push it first")
    exe = (binaries / "Dolphin.exe").read_bytes()
    if build_hash.encode() not in exe:
        fail(f"Dolphin.exe was not built from HEAD ({build_hash}): build again")
    if b"-dirty\0" in exe:
        fail("Dolphin.exe was built with uncommitted changes: build again")

    # Without symbolic links the build copies Data/Sys once and never refreshes the copy.
    source = collect_files(REPO_ROOT / "Data" / "Sys")
    built = collect_files(binaries / "Sys")
    stale = sorted(
        name
        for name in source.keys() | built.keys()
        if name not in source
        or name not in built
        or source[name].read_bytes() != built[name].read_bytes()
    )
    if stale:
        fail(
            f"{binaries / 'Sys'} differs from Data/Sys ({len(stale)} files, such as "
            f"{', '.join(stale[:3])}): replace it with a copy of Data/Sys"
        )


def markdown_to_html(text):
    """Converts what release notes use (headings, lists, paragraphs, **bold**, *italics*, `code`,
    links) to the HTML that Qt shows. The bundled Qt cannot read Markdown."""

    def inline(s):
        s = html.escape(s, quote=True)
        s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
        s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
        s = re.sub(r"(?<![*\w])\*([^*]+)\*(?![*\w])", r"<i>\1</i>", s)
        return re.sub(r"\[([^\]]+)\]\((https?://[^)\s]+)\)", r'<a href="\2">\1</a>', s)

    blocks, paragraph, items = [], [], []

    def close():
        if paragraph:
            blocks.append("<p>" + inline(" ".join(paragraph)) + "</p>")
            paragraph.clear()
        if items:
            blocks.append("<ul>" + "".join(f"<li>{inline(i)}</li>" for i in items) + "</ul>")
            items.clear()

    for line in text.splitlines():
        stripped = line.strip()
        heading = re.fullmatch(r"(#{1,6})\s+(.*)", stripped)
        item = re.fullmatch(r"[-*+]\s+(.*)", stripped)
        if not stripped:
            close()
        elif heading:
            close()
            # Under the release name, which the dialog shows in bold.
            level = min(len(heading.group(1)) + 2, 6)
            blocks.append(f"<h{level}>{inline(heading.group(2))}</h{level}>")
        elif item:
            if paragraph:
                close()
            items.append(item.group(1))
        elif items:
            items[-1] += " " + stripped
        else:
            paragraph.append(stripped)
    close()
    return "".join(blocks)


def read_manifest_hashes(path):
    text = gzip.decompress(path.read_bytes()).decode("utf-8")
    entries = text[: text.rfind("\n\n") + 1]
    return {line.split("\t")[1] for line in entries.splitlines() if "\t" in line}


def prune_content(site):
    root = site / "update" / "v1"
    kept = set()
    for latest_path in root.glob("*/*/latest.json"):
        platform = latest_path.parent.name
        for release in json.loads(latest_path.read_text("utf-8"))["releases"][:KEPT_RELEASES]:
            manifest = root / platform / "manifests" / release["hash"]
            if manifest.is_file():
                kept |= read_manifest_hashes(manifest)
    pruned = 0
    for path in (root / "content").glob("*/*/*"):
        if path.parent.parent.name + path.parent.name + path.name not in kept:
            path.unlink()
            pruned += 1
    for directory in sorted((root / "content").glob("*/*"), reverse=True) + sorted(
        (root / "content").glob("*")
    ):
        if directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()
    return pruned


def publish_release(
    site, binaries, key, name, build_hash, notes_html, track, platform, replace=False, zip_path=None
):
    files = collect_files(binaries)
    if "Dolphin.exe" not in files or "Updater.exe" not in files:
        fail(f"{binaries} holds no Dolphin.exe and Updater.exe")
    if key.public_key() not in files["Updater.exe"].read_bytes():
        fail(
            "this key is not the one Updater.exe checks (UPDATE_PUB_KEY in UpdaterCommon.cpp): "
            "installed copies would refuse the update"
        )

    root = site / "update" / "v1"
    latest_path = root / track / platform / "latest.json"
    manifest_path = root / platform / "manifests" / build_hash
    releases = []
    if latest_path.is_file():
        releases = json.loads(latest_path.read_text("utf-8"))["releases"]
    for release in releases:
        if (release["hash"] == build_hash or release["name"] == name) and not replace:
            fail(f"{release['name']} ({release['hash']}) is already published on {track}")
    releases = [r for r in releases if r["hash"] != build_hash and r["name"] != name]

    # Content first and latest.json last, so the site never points at missing files.
    entries = {}
    new_bytes = 0
    for file_name, path in files.items():
        data = path.read_bytes()
        digest = file_hash(data)
        entries[file_name] = digest
        content_path = root / "content" / digest[:2] / digest[2:4] / digest[4:]
        if not content_path.is_file():
            content_path.parent.mkdir(parents=True, exist_ok=True)
            compressed = gzip.compress(data, mtime=0)
            content_path.write_bytes(compressed)
            new_bytes += len(compressed)

    manifest = "".join(f"{n}\t{entries[n]}\n" for n in sorted(entries)).encode("utf-8")
    signed = manifest + b"\n" + base64.b64encode(key.sign(manifest)) + b"\n"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_bytes(gzip.compress(signed, mtime=0))

    releases.insert(
        0,
        {
            "name": name,
            "hash": build_hash,
            "date": datetime.date.today().isoformat(),
            "notes_html": notes_html,
        },
    )
    latest_path.parent.mkdir(parents=True, exist_ok=True)
    latest_path.write_text(
        json.dumps({"releases": releases}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    # Without it, GitHub Pages runs Jekyll over the site.
    (site / ".nojekyll").touch()
    pruned = prune_content(site)

    if zip_path:
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for file_name, path in files.items():
                archive.write(path, file_name)

    print(f"Published {name} ({build_hash}) on {track}/{platform}: {len(files)} files, "
          f"{new_bytes / 1e6:.1f} MB of new content, {pruned} old content files pruned.")
    return files


def cmd_keygen(args):
    path = Path(args.key).expanduser()
    if path.exists():
        fail(f"{path} exists: a new key would make the published releases unverifiable")
    path.parent.mkdir(parents=True, exist_ok=True)
    openssl("genpkey", "-algorithm", "ed25519", "-out", str(path))
    print(f"Created {path}. Keep a backup somewhere safe and never commit it.")
    print("Put this in Source/Core/UpdaterCommon/UpdaterCommon.cpp:\n")
    print(c_array(SigningKey(str(path)).public_key()))


def cmd_pubkey(args):
    print(c_array(SigningKey(args.key).public_key()))


def cmd_publish(args):
    site = Path(args.site)
    binaries = Path(args.binaries)
    if not (site / ".git").exists() and not args.hash:
        fail(f"{site} is not a clone of the update site repository")
    if args.hash:
        build_hash = args.hash.lower()
    else:
        build_hash = git("rev-parse", "HEAD")
        check_build(binaries, build_hash)

    notes_path = Path(args.notes)
    notes = notes_path.read_text("utf-8")
    notes_html = notes if notes_path.suffix.lower() in (".html", ".htm") else markdown_to_html(notes)

    publish_release(
        site, binaries, SigningKey(args.key), args.name, build_hash, notes_html,
        args.track, args.platform, args.replace, args.zip,
    )
    print(f"Check the changes, then commit and push {site}. The site serves them a few minutes "
          "after the push.")


class TestServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def cmd_selftest(args):
    """Updates a copy of a build from a local server, the way a user's copy updates: Dolphin reads
    latest.json and starts Updater.exe, which updates the files and starts Dolphin again. The
    restarted Dolphin reports to the server, which ends it. It runs in Updater.exe's test mode,
    signed with the test key."""
    exe = Path(args.dolphin_exe).resolve()
    binaries = exe.parent
    old_hash = "0" * 39 + "1"
    new_hash = "0" * 39 + "2"

    temp_root = Path(tempfile.mkdtemp(suffix=" \u00bf \U0001f42c"))
    old_dir = temp_root / "dolphin"
    next_dir = temp_root / "dolphin_next"
    site = temp_root / "site"
    print(f"Testing in {temp_root}")

    for file_name, path in collect_files(binaries).items():
        for target in (old_dir, next_dir):
            (target / file_name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target / file_name)
    # A file the update deletes, and files it adds or changes. build_info.txt stays the same:
    # a changed one makes Updater.exe install the VC++ runtime, quietly in test mode.
    (old_dir / "updater-test-old-file").write_text("old")
    (next_dir / "updater-test-file").write_text("test")
    (next_dir / "updater-test-fil\u1f51").write_text("test")
    (next_dir / "Sys" / "updater-test-sys-file").write_text("test")
    for path in [*next_dir.glob("*.exe"), *next_dir.glob("*.dll")]:
        data = bytearray(path.read_bytes())
        rich = data[:0x200].find(b"Rich")
        if rich >= 0:
            data[rich : rich + 4] = b"DOLP"
            path.write_bytes(data)

    key = SigningKey("test")
    publish_release(site, old_dir, key, "old", old_hash, "<p>old</p>", "updater-test", "win")
    publish_release(site, next_dir, key, "next", new_hash, "<p>next</p>", "updater-test", "win")
    expected = collect_files(next_dir)

    (old_dir / "portable.txt").touch()
    config = old_dir / "User" / "Config" / "Dolphin.ini"
    config.parent.mkdir(parents=True)
    config.write_text(
        "[Analytics]\nEnabled = False\nPermissionAsked = True\n"
        f"[AutoUpdate]\nDolphinXRTrack = updater-test\nHashOverride = {old_hash}\n"
    )

    done = threading.Event()
    reported = {}
    requests = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *handler_args, **handler_kwargs):
            super().__init__(*handler_args, directory=str(site), **handler_kwargs)

        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith("/update-test-done/"):
                reported["pid"] = int(self.path.rsplit("/", 1)[1])
                self.send_response(200)
                self.end_headers()
                done.set()
            else:
                super().do_GET()

        def log_message(self, *log_args):
            pass

    server = TestServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, DOLPHIN_UPDATE_SERVER_URL=f"http://127.0.0.1:{server.server_port}")

    failed = []
    dolphin = subprocess.Popen([str(old_dir / exe.name)], env=env)
    try:
        if not done.wait(args.timeout):
            failed.append(f"the updated Dolphin did not report within {args.timeout} s")
    finally:
        if "pid" in reported:
            try:
                os.kill(reported["pid"], signal.SIGTERM)
            except OSError:
                pass
        if dolphin.poll() is None:
            dolphin.kill()
        server.shutdown()
    time.sleep(2)

    if (old_dir / "updater-test-old-file").exists():
        failed.append("updater-test-old-file was not deleted")
    for file_name, path in expected.items():
        installed = old_dir / file_name
        if not installed.is_file():
            failed.append(f"missing: {file_name}")
        elif installed.read_bytes() != path.read_bytes():
            failed.append(f"not updated: {file_name}")

    log = old_dir / "User" / "Logs" / "Updater.log"
    if args.verbose or failed:
        print("Requests:", *requests, sep="\n  ")
        if log.is_file():
            print(log.read_text("utf-8", errors="replace"))
    if failed:
        print("FAILED:", *failed, sep="\n  ")
        print(f"Kept {temp_root}")
        sys.exit(1)
    print(f"OK: the copy holds the {len(expected)} files of the new release, the obsolete file "
          "is gone, and Dolphin restarted after the update.")
    if not args.keep:
        shutil.rmtree(temp_root, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)

    keygen = commands.add_parser("keygen", help="create the signing key")
    keygen.add_argument("key", help="where to write the private key (PEM)")
    keygen.set_defaults(func=cmd_keygen)

    pubkey = commands.add_parser("pubkey", help="print the public key as UPDATE_PUB_KEY")
    pubkey.add_argument("key")
    pubkey.set_defaults(func=cmd_pubkey)

    publish = commands.add_parser("publish", help="add a release to the update site")
    publish.add_argument("--site", required=True, help="clone of the update site repository")
    publish.add_argument("--binaries", required=True, help="the build, e.g. Build-vs2022/Binaries")
    publish.add_argument("--name", required=True, help="release name, e.g. v0.5")
    publish.add_argument("--notes", required=True, help="release notes, Markdown or .html")
    publish.add_argument("--key", required=True, help="the signing key from keygen")
    publish.add_argument("--zip", help="also write the release zip for GitHub")
    publish.add_argument("--track", default="stable")
    publish.add_argument("--platform", default="win")
    publish.add_argument("--replace", action="store_true", help="publish a release again")
    publish.add_argument("--hash", help=argparse.SUPPRESS)  # Tests only: skips the build checks.
    publish.set_defaults(func=cmd_publish)

    selftest = commands.add_parser("selftest", help="update a copy of a build from a local server")
    selftest.add_argument("dolphin_exe", help="e.g. Build-vs2022/Binaries/Dolphin.exe")
    selftest.add_argument("--timeout", type=int, default=120)
    selftest.add_argument("--keep", action="store_true", help="keep the test directory")
    selftest.add_argument("--verbose", action="store_true")
    selftest.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
