#! /usr/bin/env nix-shell
#! nix-shell -i python3 -p "python3.withPackages (p: [ p.anyio ])"
"""Write chromium-info.json: the pin's DEPS in the shape nixpkgs' Chromium recipe reads.

    ./lock.py

Resolves DEPS at CHROMIUM_PIN with nixpkgs' own depot_tools.py, keeps every
hash whose url and rev are unchanged, and computes the rest by building the
recipe's own fixed-output derivations, so a hash is the one the build checks.
"""

from __future__ import annotations

import base64
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

import anyio

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parent
LOCK = HERE / "chromium-info.json"
FAKE = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
GOT = re.compile(r"got:\s+(sha256-[A-Za-z0-9+/=]+)")
DEPOT_TOOLS = {
    "rev": "96b9c092c0cdaa9f1ec87b0af777ec85977dee91",
    "hash": "sha256-XcnkXK9hxAdHISWT1h37Z+kUG0tnCuOoy7t6B02iU5g=",
}
JOBS = 6
# curl -f: the server answered with an HTTP error, such as 404.
CURL_HTTP_ERROR = 22


@dataclass(frozen=True, slots=True)
class Fod:
    """A fixed-output derivation whose hash the lock records."""

    key: str
    expr: str


async def pin() -> str:
    lines = (await anyio.Path(ENGINE / "CHROMIUM_PIN").read_text()).splitlines()
    return next(line for line in lines if line and not line.startswith("#")).strip()


async def run(*argv: str) -> tuple[int, str, str]:
    done = await anyio.run_process(list(argv), check=False)
    return done.returncode, done.stdout.decode(), done.stderr.decode()


async def nix_eval_raw(expr: str) -> str:
    rc, out, err = await run("nix", "eval", "--impure", "--raw", "--expr", expr)
    if rc != 0:
        raise RuntimeError(f"nix eval failed:\n{err}")
    return out


async def fetch_depot_tools(nixpkgs: str) -> str:
    depot_tools = await nix_eval_raw(
        f'(import {nixpkgs} {{}}).fetchgit {{ url = "https://chromium.googlesource.com/chromium/tools/depot_tools.git";'
        f' rev = "{DEPOT_TOOLS["rev"]}"; hash = "{DEPOT_TOOLS["hash"]}"; }}'
    )
    rc, _, err = await run("nix", "build", "--no-link", depot_tools)
    if rc != 0:
        raise RuntimeError(f"depot_tools fetch failed:\n{err}")
    return depot_tools


async def resolve_deps(
    nixpkgs: str, depot_tools: str, rev: str
) -> dict[str, dict[str, str]]:
    script = f"{nixpkgs}/pkgs/applications/networking/browsers/chromium/depot_tools.py"
    rc, out, err = await run("python3", script, depot_tools, rev)
    if rc != 0:
        raise RuntimeError(f"depot_tools.py failed:\n{err}")
    return json.loads(out)


async def prebuilt_in(
    depot_tools: str, deps_file: str, *child: str
) -> dict[str, Any]:  # prebuilt_deps.py's JSON, shaped there
    """`child` is a recursed dep's path and the parent DEPS file's path."""
    async with anyio.NamedTemporaryFile("w", suffix=".DEPS") as f:
        await f.write(deps_file)
        await f.flush()
        rc, out, err = await run(
            "python3",
            str(HERE / "prebuilt_deps.py"),
            depot_tools,
            str(f.name),
            *child,
        )
    if rc != 0:
        raise RuntimeError(f"prebuilt_deps.py failed on {child or 'src'}:\n{err}")
    return json.loads(out)


async def prebuilt(
    depot_tools: str, deps_file: str, resolved: dict[str, dict[str, str]]
) -> dict[str, dict[str, list[dict[str, str]]]]:
    """The CIPD and GCS deps, each with the url and sha256 a fetchurl pins.

    As gclient, it reads each recursed DEPS file at its locked revision:
    devtools-frontend's holds its node_modules.
    """
    found = await prebuilt_in(depot_tools, deps_file)
    pending = list(found["recursedeps"])
    async with anyio.NamedTemporaryFile("w", suffix=".DEPS") as parent:
        await parent.write(deps_file)
        await parent.flush()
        while pending:
            path = pending.pop()
            dep = resolved.get(path)
            if dep is None:
                continue  # not in a Linux checkout: clank, ios_internal and the like
            rc, encoded, err = await run(
                "curl", "-sf", f"{dep['url']}/+/{dep['rev']}/DEPS?format=TEXT"
            )
            if rc == CURL_HTTP_ERROR:
                continue  # gitiles has no DEPS file in that dep
            if rc != 0:
                raise RuntimeError(f"DEPS fetch for {path} failed:\n{err}")
            more = await prebuilt_in(
                depot_tools, base64.b64decode(encoded).decode(), path, str(parent.name)
            )
            found["cipd"].update(more["cipd"])
            found["gcs"].update(more["gcs"])
            pending.extend(more["recursedeps"])
    gcs = {
        path: [
            {
                "url": f"https://storage.googleapis.com/{dep['bucket']}/{o['object_name']}",
                "sha256": o["sha256sum"],
                "file": o["output_file"] or o["object_name"],
            }
            for o in dep["objects"]
        ]
        for path, dep in found["gcs"].items()
    }
    cipd: dict[str, list[dict[str, str]]] = {}
    limit = anyio.Semaphore(JOBS)

    async def one(path: str, package: str, version: str) -> None:
        url = f"https://chrome-infra-packages.appspot.com/dl/{package}/+/{version}"
        async with limit:
            # CIPD refuses HEAD; a GET that does not follow shows the redirect.
            rc, out, err = await run("curl", "-s", "-o", "/dev/null", "-D", "-", url)
        # CIPD redirects to its content-addressed store: the archive's sha256.
        found_hash = re.search(r"(?im)^location: .*/SHA256/([0-9a-f]{64})", out)
        if rc != 0 or found_hash is None:
            raise RuntimeError(f"CIPD did not resolve {package}@{version}:\n{out}{err}")
        cipd.setdefault(path, []).append(
            {
                "package": package,
                "version": version,
                "url": url,
                "sha256": found_hash.group(1),
            }
        )

    async with anyio.create_task_group() as tg:
        for path, packages in found["cipd"].items():
            for p in packages:
                tg.start_soon(one, path, p["package"], p["version"])
    return {"cipd": dict(sorted(cipd.items())), "gcs": gcs}


async def gn_lock(rev: str, deps_file: str) -> dict[str, str]:
    found = re.search(r"'gn_version': 'git_revision:([0-9a-f]+)'", deps_file)
    if found is None:
        raise RuntimeError("DEPS names no gn_version")
    gn_rev = found.group(1)
    rc, out, err = await run(
        "curl", "-sf", f"https://gn.googlesource.com/gn/+/{gn_rev}?format=JSON"
    )
    if rc != 0:
        raise RuntimeError(f"gn commit lookup failed:\n{err}")
    when = json.loads(out.split("\n", 1)[1])["committer"]["time"]
    date = datetime.strptime(when, "%a %b %d %H:%M:%S %Y %z").date().isoformat()
    return {"version": f"0-unstable-{date}", "rev": gn_rev, "hash": FAKE}


async def prefetch(fod: Fod) -> str:
    rc, _, err = await run("nix", "build", "--impure", "--no-link", "--expr", fod.expr)
    if rc == 0:
        raise RuntimeError(f"{fod.key} built with the placeholder hash")
    found = GOT.search(err)
    if found is None:
        raise RuntimeError(f"{fod.key}: no hash in the failure:\n{err[-3000:]}")
    return found.group(1)


async def main() -> int:
    nixpkgs = await nix_eval_raw(f"import {HERE / 'nixpkgs.nix'}")
    rev = await pin()
    lock_path = anyio.Path(LOCK)
    old = (
        json.loads(await lock_path.read_text())
        if await lock_path.exists()
        else {"DEPS": {}, "deps": {}}
    )
    depot_tools = await fetch_depot_tools(nixpkgs)
    resolved = await resolve_deps(nixpkgs, depot_tools, rev)
    rc, deps_file, err = await run(
        "sh",
        "-c",
        f"curl -sf 'https://chromium.googlesource.com/chromium/src/+/{rev}/DEPS?format=TEXT' | base64 -d",
    )
    if rc != 0:
        raise RuntimeError(f"DEPS fetch failed:\n{err}")

    lock: dict = {
        "version": (await anyio.Path(ENGINE / "CHROMIUM_VERSION").read_text()).strip(),
        "DEPS": {},
        "deps": {
            "depot_tools": DEPOT_TOOLS,
            "gn": await gn_lock(rev, deps_file),
            "npmHash": FAKE,
        },
        **(await prebuilt(depot_tools, deps_file, resolved)),
    }
    for path, dep in resolved.items():
        entry: dict[str, str | bool] = {
            "url": dep["url"],
            "rev": dep["rev"],
            "hash": FAKE,
        }
        if path == "src":
            entry["recompress"] = True
        before = old["DEPS"].get(path)
        if before and before["url"] == entry["url"] and before["rev"] == entry["rev"]:
            entry["hash"] = before["hash"]
        lock["DEPS"][path] = entry
    old_gn = old["deps"].get("gn")
    if old_gn and old_gn["rev"] == lock["deps"]["gn"]["rev"]:
        lock["deps"]["gn"] = old_gn
    # npm's lock lives in src: the same src rev keeps the same npm deps.
    old_src = old["DEPS"].get("src")
    if old_src and old_src["rev"] == lock["DEPS"]["src"]["rev"]:
        lock["deps"]["npmHash"] = old["deps"].get("npmHash", FAKE)

    async def write() -> None:
        await lock_path.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")

    await write()
    recipe = (
        f"(let pkgs = import {nixpkgs} {{}}; info = pkgs.lib.importJSON {LOCK};"
        f" in (pkgs.chromium.override {{ upstream-info = info; }}).browser.passthru"
    )
    fods = [
        Fod(f"DEPS:{path}", f'{recipe}.chromiumDeps."{path}")')
        for path, entry in lock["DEPS"].items()
        if entry["hash"] == FAKE
    ]
    if lock["deps"]["gn"]["hash"] == FAKE:
        gn = lock["deps"]["gn"]
        fods.append(
            Fod(
                "gn",
                f'(import {nixpkgs} {{}}).gn.override {{ version = "{gn["version"]}"; rev = "{gn["rev"]}"; hash = ""; }}',
            )
        )
    limit = anyio.Semaphore(JOBS)
    failures: list[str] = []

    async def one(fod: Fod) -> None:
        async with limit:
            try:
                got = await prefetch(fod)
            except RuntimeError as error:
                failures.append(str(error))
                return
            kind, _, path = fod.key.partition(":")
            if kind == "DEPS":
                lock["DEPS"][path]["hash"] = got
            else:
                lock["deps"]["gn"]["hash"] = got
            await write()
            print(f"{fod.key} {got}", file=sys.stderr)

    async with anyio.create_task_group() as tg:
        for fod in fods:
            tg.start_soon(one, fod)

    # npm's lock lives in src, so its hash comes last.
    if not failures and lock["deps"]["npmHash"] == FAKE:
        try:
            lock["deps"]["npmHash"] = await prefetch(Fod("npm", f"{recipe}.npmDeps)"))
            await write()
        except RuntimeError as error:
            failures.append(str(error))
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(anyio.run(main))
