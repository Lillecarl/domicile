"""Print the CIPD and GCS deps a Linux x64 checkout gets from DEPS, as JSON.

    python3 prebuilt_deps.py DEPOT_TOOLS DEPS_FILE

lock.py runs it: it needs depot_tools' gclient_eval, so it runs beside
nixpkgs' depot_tools.py rather than inside lock.py's interpreter.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import gclient_eval  # type: ignore[import-not-found]  # depot_tools, put on sys.path above

# What a Linux x64 gclient sync evaluates DEPS conditions with.
CHECKOUT = {
    "checkout_linux": True,
    "checkout_x64": True,
    "host_os": "linux",
    "host_cpu": "x64",
    "non_git_source": True,
    "checkout_openxr": True,
    **{
        f"checkout_{name}": False
        for name in (
            "android",
            "win",
            "mac",
            "ios",
            "fuchsia",
            "chromeos",
            "arm",
            "arm64",
            "x86",
            "mips",
            "ppc",
            "s390",
            "riscv64",
            "loong64",
        )
    },
}


class Checkout(dict[str, object]):
    """DEPS variables where a `checkout_*` that nothing set is false, as for gclient on Linux x64."""

    def __contains__(self, key: object) -> bool:
        return super().__contains__(key) or (
            isinstance(key, str) and key.startswith("checkout_")
        )

    def __missing__(self, key: str) -> object:
        if key.startswith("checkout_"):
            return False
        raise KeyError(key)


PLATFORM = {"${platform}": "linux-amd64", "${arch}": "amd64", "${os}": "linux"}

# Not part of building the engine: test data, Java tooling, updaters, CI tools.
SKIP = (
    "src/base/tracing/test/data",
    "src/components/variations/test_data/cipd",
    "src/content/test/data/gpu/meet_effects",
    "src/content/test/data/gpu/meet_effects_videos",
    "src/third_party/apache-linux",
    "src/third_party/blink/renderer/core/css/perftest_data",
    "src/third_party/checkstyle/cipd",
    "src/third_party/enterprise_companion/chromium_linux64/cipd",
    "src/third_party/google-java-format/cipd",
    "src/third_party/jdk/current",
    "src/third_party/js_code_coverage/node_modules",
    "src/third_party/ktfmt/cipd",
    "src/third_party/opus/tests/resources",
    "src/third_party/updater/chrome_linux64/cipd",
    "src/third_party/updater/chrome_linux64_sans_iid/cipd",
    "src/third_party/updater/chromium_linux64/cipd",
    "src/third_party/updater/chromium_linux64_sans_iid/cipd",
    "src/tools/luci-go",
    "src/tools/perf/page_sets/maps_perf_test/dataset",
    "src/tools/resultdb",
    "src/tools/skia_goldctl/linux",
)


def platformed(name: str) -> str:
    for template, value in PLATFORM.items():
        name = name.replace(template, value)
    return name


def main() -> None:
    scope = gclient_eval.Exec(Path(sys.argv[2]).read_text(), filename="DEPS")
    variables = Checkout({**dict(scope.get("vars", {})), **CHECKOUT})
    cipd: dict[str, list[dict[str, str]]] = {}
    gcs: dict[str, dict[str, object]] = {}
    for path, dep in scope.get("deps", {}).items():
        if not hasattr(dep, "get") or path in SKIP:
            continue
        kind = dep.get("dep_type")
        if kind not in ("cipd", "gcs"):
            continue
        condition = dep.get("condition")
        if condition and not gclient_eval.EvaluateCondition(condition, variables):
            continue
        if kind == "cipd":
            cipd[path] = [
                {"package": platformed(p["package"]), "version": p["version"]}
                for p in dep["packages"]
            ]
        else:
            gcs[path] = {
                "bucket": dep["bucket"],
                "objects": [
                    {
                        "object_name": o["object_name"],
                        "sha256sum": o["sha256sum"],
                        "output_file": o.get("output_file", ""),
                    }
                    for o in dep["objects"]
                    # Each object has its own condition: clang's lists Mac and Windows builds.
                    if not o.get("condition")
                    or gclient_eval.EvaluateCondition(o["condition"], variables)
                ],
            }
    json.dump({"cipd": cipd, "gcs": gcs}, sys.stdout, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
