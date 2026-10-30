#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Private Android Vulkan recording + NVN index + verified same-device replay.

Uses an installed Pico RenderDoc layer and the real paired RenderDoc MCP server.
No game launch automation, root, package/data removal, or inferred replay results.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shlex
import subprocess
import sys
import threading
import time

SETTINGS = ("enable_gpu_debug_layers", "gpu_debug_app", "gpu_debug_layer_app", "gpu_debug_layers")
PROPERTIES = ("debug.rdoc.PICO_HOOK_PROC", "debug.rdoc.IGNORE_LAYERS", "debug.mng.capture_enable", "debug.mng.capture_frame")
SCHEMA = "mng.render-record.v1"


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    temp = Path(str(path) + ".tmp")
    temp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temp.replace(path)


class Adb:
    def __init__(self, binary, serial):
        self.command = [binary, "-s", serial]

    def run(self, *args, binary=False):
        return subprocess.check_output(self.command + list(args), text=not binary, timeout=90)

    def shell(self, *args):
        return self.run("shell", shlex.join(map(str, args))).strip()

    def private(self, package, path):
        return self.run("exec-out", "run-as", package, "cat", path, binary=True)

    def fingerprint(self):
        return self.shell("getprop", "ro.build.fingerprint")

    def package(self, package):
        paths = self.shell("pm", "path", package).splitlines()
        require(paths and paths[0].startswith("package:"), "Package not installed: " + package)
        return {"apk_path": paths[0][8:], "sha256": self.shell("sha256sum", paths[0][8:]).split()[0],
                "version": [line.strip() for line in self.shell("dumpsys", "package", package).splitlines()
                            if "versionName=" in line or "versionCode=" in line]}


def load(args):
    root = Path(args.directory).resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    require(manifest.get("schema") == SCHEMA, "Unsupported recording manifest")
    adb = Adb(args.adb, manifest["serial"])
    return root, manifest, adb


def files(adb, package):
    directory = f"/sdcard/Android/media/{package}/files/RenderDocForPico"
    result = adb.shell("sh", "-c", f"if [ -d {shlex.quote(directory)} ]; then find {shlex.quote(directory)} -maxdepth 1 -name '*.rdc'; fi")
    return result.splitlines() if result else []


def prepare(args):
    require(re.fullmatch(r"[A-Za-z0-9_.]+", args.package), "Invalid package")
    require(re.fullmatch(r"[A-Za-z0-9_.]+(?::[A-Za-z0-9_]+)?", args.process), "Invalid process")
    require(args.process == args.package or args.process.startswith(args.package + ":"), "Process must belong to target package")
    require(re.fullmatch(r"[A-Za-z0-9_-]+", args.profile), "Invalid profile")
    adb = Adb(args.adb, args.serial)
    target = adb.package(args.package)
    server = adb.package(args.server_package)
    require(adb.shell("run-as", args.package, "id"), "A debuggable target is required")
    root = Path(args.directory).resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    manifest = {"schema": SCHEMA, "serial": args.serial, "fingerprint": adb.fingerprint(), "package": args.package,
                "process": args.process, "profile": args.profile, "server_package": args.server_package,
                "target_identity": target, "capture_server_identity": server,
                "settings_before": {k: adb.shell("settings", "get", "global", k) for k in SETTINGS},
                "properties_before": {k: adb.shell("getprop", k) for k in PROPERTIES},
                "replay_verified": False, "nvn_retranslation_supported": False}
    # Persist recovery data before the first mutation.
    save(root / "manifest.json", manifest)
    try:
        for key, value in zip(SETTINGS, ("1", args.package, args.server_package, "VK_LAYER_RENDERDOC_Capture")):
            adb.shell("settings", "put", "global", key, value)
        for key, value in zip(PROPERTIES, (args.process, "0", "1", "0")):
            adb.shell("setprop", key, value)
    except Exception:
        restore(args)
        raise
    print("Prepared. Restart the target normally, enter the intended scene, then arm. Recovery manifest:", root / "manifest.json")


def arm(args):
    root, manifest, adb = load(args)
    require(not manifest.get("request"), "Use a fresh output directory for another recording")
    require(1 <= args.frames <= 8 and 1 <= args.trace_mib <= 128, "Invalid frame/trace bound")
    require(adb.fingerprint() == manifest["fingerprint"], "Device build changed")
    pid = adb.shell("pidof", manifest["process"])
    require(re.fullmatch(r"[0-9]+", pid), "Expected exactly one running game process")
    maps = adb.private(manifest["package"], f"/proc/{pid}/maps").decode()
    layer = [line for line in maps.splitlines() if "renderdoc" in line.lower()]
    require(layer, "No live RenderDoc layer mapping; restart after prepare")
    require(adb.package(manifest["package"])["sha256"] == manifest["target_identity"]["sha256"], "Target APK changed after prepare")
    (root / "live-layer-maps.txt").write_text("\n".join(layer) + "\n")
    (root / "live-process-maps.txt").write_text(maps)
    driver_paths = sorted({line.split(maxsplit=5)[5] for line in maps.splitlines()
                           if len(line.split(maxsplit=5)) == 6 and line.endswith("/vulkan.ad07xx.so")})
    require(len(driver_paths) <= 1, "Multiple custom Vulkan drivers mapped")
    manifest["mapped_custom_drivers"] = [{"path": path, "sha256":
        adb.shell("run-as", manifest["package"], "sha256sum", path).split()[0]} for path in driver_paths]
    manifest["driver_status"] = adb.private(manifest["package"], f"files/{manifest['profile']}/driver-status.txt").decode()

    manifest.update(request=int(time.time_ns() // 1000000), game_pid=int(pid), frames=args.frames,
                    trace_mib=args.trace_mib, rdc_before=files(adb, manifest["package"]))
    save(root / "manifest.json", manifest)
    adb.shell("setprop", "debug.mng.capture_frame", f"{manifest['request']}:{args.frames}:{args.trace_mib}")
    print("Armed at the next successful present:", manifest["request"])


def inspect_index(path):
    require(path.stat().st_size <= 128 << 20, "NVN index exceeds bound")
    records = []
    with path.open() as stream:
        for line in stream:
            require(len(line) <= 9 << 20, "Oversized index record")
            records.append(json.loads(line))
    require(len(records) >= 2, "Incomplete NVN index")
    header, footer = records[0], records[-1]
    require(header.get("schema") == "mng.nvn-index.v1" and header.get("type") == "header", "Invalid NVN header")
    require(re.fullmatch(r"[0-9a-f]{32}", header.get("run", "")), "Invalid run identity")
    require(isinstance(header.get("request"), int) and header["request"] > 0, "Invalid capture identity")
    require(1 <= header.get("frame_count", 0) <= 8, "Invalid frame count")
    require(footer.get("type") == "footer" and footer.get("complete") is True, "NVN window incomplete: " + str(footer.get("reason")))
    count, frame, presents = 0, header["first_frame"], 0
    actions = {}
    for row in records[1:-1]:
        require(row.get("frame") == frame, "Nonconsecutive frame records")
        if row.get("type") == "action":
            count += 1
            require(row.get("event") == count, "Missing or duplicate NVN action")
            actions[count] = row
        elif row.get("type") == "present":
            frame += 1
            presents += 1
        else:
            raise RuntimeError("Unknown NVN index record")
    require(count == footer["events"] and presents == footer["completed_frames"] == header["frame_count"], "Index totals mismatch")
    return header, footer, actions


def collect(args):
    root, manifest, adb = load(args)
    require(manifest.get("request") and not manifest.get("artifacts"), "No pending recording")
    package = manifest["package"]
    paths = adb.shell("run-as", package, "find", "files/" + manifest["profile"], "-maxdepth", "2", "-name", f"capture-{manifest['request']}.nvn.jsonl").splitlines()
    require(len(paths) == 1, "Expected one completed NVN index; inspect the game log")
    attempt = root / f"index-attempt-{time.time_ns()}.jsonl"
    with attempt.open("xb") as out:
        out.write(adb.private(package, paths[0]))
    header, footer, _ = inspect_index(attempt)
    identity = json.loads(header["identity"])
    require(identity["pid"] == manifest["game_pid"] and header["request"] == manifest["request"], "Recording process/request mismatch")
    candidates = [p for p in files(adb, package) if p not in manifest["rdc_before"]]
    require(len(candidates) == 1, "Expected exactly one new RDC; preserve evidence and resolve ambiguity")
    remote = candidates[0]
    size = int(adb.shell("stat", "-c", "%s", remote))
    require(0 < size <= args.max_rdc_mib << 20, "RDC size outside pull bound")
    index = root / "commands.nvn.jsonl"
    if index.exists():
        require(digest(index) == digest(attempt), "Completed index changed between collection attempts")
    else:
        attempt.rename(index)
    local = root / "capture.rdc"
    remote_hash = adb.shell("sha256sum", remote).split()[0]
    if not local.exists():
        partial = root / f"capture-attempt-{time.time_ns()}.rdc"
        adb.run("pull", remote, str(partial))
        require(partial.stat().st_size == size and digest(partial) == remote_hash, "RDC changed during collection")
        partial.rename(local)
    require(local.stat().st_size == size and digest(local) == remote_hash, "RDC changed during collection")
    manifest.update(run=header["run"], recording_identity=identity, device_rdc=remote,
                    artifacts=[{"path": p.name, "size": p.stat().st_size, "sha256": digest(p)} for p in (index, local)],
                    trace_complete=footer["complete"], replay_verified=False)
    save(root / "manifest.json", manifest)
    print("Collected complete NVN index and RDC. Pairing and replay remain unverified until replay succeeds.")


def verify_bundle(root, manifest):
    require(manifest.get("schema") == SCHEMA, "Unknown bundle schema")
    require({a["path"] for a in manifest.get("artifacts", [])} == {"commands.nvn.jsonl", "capture.rdc"}, "Missing/unknown artifacts")
    require(len(manifest["artifacts"]) == 2, "Duplicate artifacts")
    for item in manifest["artifacts"]:
        path = root / item["path"]
        require(not path.is_symlink() and path.is_file(), "Artifact must be a regular local file")
        require(path.stat().st_size == item["size"] and digest(path) == item["sha256"], "Artifact integrity failure: " + item["path"])
    header, footer, actions = inspect_index(root / "commands.nvn.jsonl")
    require(header["run"] == manifest["run"] and header["request"] == manifest["request"], "Bundle/index identity mismatch")
    return header, footer, actions


def restore(args):
    root, manifest, adb = load(args)
    require(adb.fingerprint() == manifest["fingerprint"], "Restore target build changed")
    failures = []
    for key, value in manifest["settings_before"].items():
        try:
            adb.shell("settings", "delete" if value == "null" else "put", "global", key, *([] if value == "null" else [value]))
        except Exception as error:
            failures.append(str(error))
    for key, value in manifest["properties_before"].items():
        try:
            adb.shell("setprop", key, value)
        except Exception as error:
            failures.append(str(error))
    actual = {k: adb.shell("settings", "get", "global", k) for k in manifest["settings_before"]}
    properties = {k: adb.shell("getprop", k) for k in manifest["properties_before"]}
    manifest["restored"] = not failures and actual == manifest["settings_before"] and properties == manifest["properties_before"]
    save(root / "manifest.json", manifest)
    require(manifest["restored"], "Restore incomplete: " + str(failures))
    print("Capture settings restored. The loaded layer remains until the target process next exits.")


class Mcp:
    def __init__(self, command, environment, output):
        self.output, self.number = output, 0
        self.errors = (output / "server.log").open("w")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors,
                                        text=True, bufsize=1, env=environment)
        self.lines = queue.Queue()
        def receive():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=receive, daemon=True).start()
        try:
            self.request("initialize", {"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "mng-render-replay", "version": "1"}})
            self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            result = self.request("tools/list", {})
            self.tools = {tool["name"] for tool in result["result"]["tools"]}
        except Exception:
            self.close()
            raise

    def send(self, value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()

    def request(self, method, params):
        self.number += 1
        self.send({"jsonrpc": "2.0", "id": self.number, "method": method, "params": params})
        deadline = time.monotonic() + 180
        while True:
            line = self.lines.get(timeout=max(.01, deadline - time.monotonic()))
            require(line is not None, "MCP server exited")
            result = json.loads(line)
            if result.get("id") == self.number:
                require("error" not in result, str(result.get("error")))
                return result

    def call(self, name, arguments):
        require(name in self.tools, "MCP capability unavailable: " + name)
        result = self.request("tools/call", {"name": name, "arguments": arguments})["result"]
        save(self.output / f"{self.number:03}-{name}.json", result)
        require(not result.get("isError"), "MCP tool failed: " + name)
        parsed = None
        for i, block in enumerate(result.get("content", [])):
            if block.get("type") == "text":
                try:
                    value = json.loads(block["text"])
                    if isinstance(value, dict):
                        parsed = value
                except json.JSONDecodeError:
                    pass
            elif block.get("type") == "image":
                (self.output / f"{self.number:03}-{name}-{i}.png").write_bytes(base64.b64decode(block["data"]))
        require(parsed is not None, "MCP returned no structured evidence: " + name)
        require(parsed.get("ok") is not False, "MCP operation failed: " + str(parsed))
        return parsed

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait()
        self.errors.close()


def replay(args):
    root, manifest, adb = load(args)
    header, _, actions = verify_bundle(root, manifest)
    require(adb.fingerprint() == manifest["fingerprint"], "Replay requires the captured Android device/build")
    require(adb.package(manifest["server_package"])["sha256"] == manifest["capture_server_identity"]["sha256"], "Replay Server APK differs from capture")
    require(adb.shell("sha256sum", manifest["device_rdc"]).split()[0] == digest(root / "capture.rdc"), "Remote RDC identity differs")
    output = Path(args.output).resolve(); output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, RENDERDOC_ARTIFACT_ROOT=str(output))
    if Path(args.adb).is_file():
        environment["PATH"] = str(Path(args.adb).resolve().parent) + os.pathsep + environment.get("PATH", "")
    if args.replay_marker:
        environment["RENDERDOC_REPLAY_MARKER_PATH"] = str(Path(args.replay_marker).resolve())
    command = [str(Path(args.mcp_server).resolve()), "--renderdoc-library=" + str(Path(args.library).resolve()),
               "--abi-manifest=" + str(Path(args.abi).resolve()), "--android-device=" + manifest["serial"],
               "--android-capture-path=" + manifest["device_rdc"]]
    runtime = {"library_sha256": digest(args.library), "server_sha256": digest(args.mcp_server),
               "abi_sha256": digest(args.abi), "device_fingerprint": adb.fingerprint(), "paired_server": manifest["capture_server_identity"],
               "source_rdc_sha256": digest(root / "capture.rdc"), "replay_verified": False}
    loader_before = adb.shell("getprop", "debug.rdoc.vulkan_loader")
    custom_drivers = manifest.get("mapped_custom_drivers", [])
    require(not custom_drivers or args.android_vulkan_loader,
            "Captured custom driver requires an explicit same-driver Android replay loader")
    if args.android_vulkan_loader:
        loader = Path(args.android_vulkan_loader)
        require(str(loader).startswith(f"/data/user/0/{manifest['server_package']}/files/")
                and loader.name == "librenderdoc_turnip.so" and ".." not in loader.parts,
                "Replay loader must be in the Server private files directory")
        driver_sha = adb.shell("run-as", manifest["server_package"], "sha256sum", str(loader.parent / "vulkan.ad07xx.so")).split()[0]
        require(len(custom_drivers) == 1 and driver_sha == custom_drivers[0]["sha256"],
                "Replay Turnip bytes differ from the actual capture driver")
        runtime["replay_loader"] = {"path": str(loader), "driver_sha256": driver_sha,
            "loader_sha256": adb.shell("run-as", manifest["server_package"], "sha256sum", str(loader)).split()[0]}
    save(output / "replay.json", runtime)
    # The real Android replay adapter can forward sockets, start its Server and
    # copy the host RenderDoc configuration. Preserve exactly this operation's
    # pre-state, including failures during initialization/open.
    forwards_before = set(adb.run("forward", "--list").splitlines())
    server_running = bool(adb.shell("sh", "-c", "pidof " + shlex.quote(manifest["server_package"]) + " || true"))
    config = f"/sdcard/Android/media/{manifest['server_package']}/files/renderdoc.conf"
    config_exists = adb.shell("sh", "-c", "test -f " + shlex.quote(config) + " && echo present || true") == "present"
    backup = output / "device-renderdoc.conf.before"
    if config_exists:
        backup.write_bytes(adb.run("exec-out", "cat", config, binary=True))
    save(output / "replay-environment-before.json", {"forwards": sorted(forwards_before), "server_running": server_running,
         "device_config": config, "device_config_existed": config_exists, "vulkan_loader_before": loader_before})
    client = None
    opened = False
    try:
        if args.android_vulkan_loader:
            adb.shell("setprop", "debug.rdoc.vulkan_loader", args.android_vulkan_loader)
            adb.shell("am", "force-stop", manifest["server_package"])
        client = Mcp(command, environment, output)
        client.call("capture_open", {"path": str(root / "capture.rdc")}); opened = True
        status = client.call("capture_status", {})
        require(status.get("isOpen") is True, "Replay controller is not open")
        overview = client.call("frame_overview", {})
        schema = client.call("schema_describe", {})
        tables = {row["name"]: row for row in schema["data"]["tables"]}
        require({"event_id", "name"} <= set(tables["event"]["columns"]), "Unsupported event schema")
        cursor, joined, seen = 0, [], set()
        prefix = f"macnative.nvn run={header['run']} capture={header['request']} "
        while True:
            result = client.call("query_execute", {"sql": "SELECT event_id, name FROM event WHERE event_id > " + str(cursor) +
                " AND name LIKE '" + prefix + "%' ORDER BY event_id", "limit": 5000})["data"]
            for event, name in result["rows"]:
                match = re.search(r" frame=(\d+) submission=(\d+) event=(\d+) op=(\d+) ", name)
                require(match, "Malformed NVN marker")
                frame, submission, action, opcode = map(int, match.groups())
                require(action in actions and all(actions[action][k] == v for k, v in
                        (("frame", frame), ("submission", submission), ("opcode", opcode))), "RDC / NVN event identity mismatch")
                joined.append({"rdc_event": event, "nvn_event": action, "frame": frame, "submission": submission})
                seen.add(action); cursor = max(cursor, int(event))
            if not result.get("truncated"):
                break
            require(result["rows"], "Truncated marker query made no progress")
        require(joined, "RDC has no matching run/capture markers; pairing is unverified")
        save(output / "nvn-marker-map.json", {"schema": "mng.nvn-rdc-map.v1", "run": header["run"], "request": header["request"],
            "markers": joined, "indexed_actions": len(actions), "marked_actions": len(seen),
            "precision": "command_marker", "guest_pc": "unknown", "full_resource_provenance": "not_implemented"})
        if args.event:
            client.call("event_select", {"eventId": args.event})
        if args.resource:
            require(args.event, "Texture review requires an explicit event")
            client.call("review_texture", {"request": {"eventId": args.event, "resourceId": args.resource,
                "mip": args.mip, "slice": args.slice, "sample": 0}, "options": {"maxEdge": 768}})
        runtime.update(replay_verified=True, marker_count=len(joined), status=status, overview=overview)
        save(output / "replay.json", runtime)
        manifest["replay_verified"] = True
        manifest["replay_report"] = str(output / "replay.json")
        save(root / "manifest.json", manifest)
        print("Same-device replay and NVN marker pairing verified:", len(joined), "markers")
    finally:
        cleanup_errors = []
        original_error = sys.exc_info()[0] is not None
        try:
            if opened and client:
                client.call("capture_close", {})
        except Exception as error:
            cleanup_errors.append("capture_close: " + str(error))
        if client:
            client.close()
        try:
            if args.android_vulkan_loader:
                adb.shell("am", "force-stop", manifest["server_package"])
                adb.shell("setprop", "debug.rdoc.vulkan_loader", loader_before)
            if config_exists:
                adb.run("push", str(backup), config)
            else:
                adb.shell("rm", "-f", config)
            for line in set(adb.run("forward", "--list").splitlines()) - forwards_before:
                serial, local, remote = line.split()
                if serial == manifest["serial"] and remote.startswith("localabstract:renderdoc_"):
                    adb.run("forward", "--remove", local)
            if not server_running:
                adb.shell("am", "force-stop", manifest["server_package"])
        except Exception as error:
            cleanup_errors.append("environment_restore: " + str(error))
        save(output / "cleanup.json", {"complete": not cleanup_errors, "errors": cleanup_errors})
        if cleanup_errors and not original_error:
            raise RuntimeError("Replay cleanup incomplete: " + str(cleanup_errors))


def pack(args):
    root, manifest, _ = load(args)
    verify_bundle(root, manifest)
    tool = Path(args.foundation).resolve() / "tools/gpu-snapshot/snapshot.py"
    require(tool.is_file(), "Foundation gpu-snapshot tool not found")
    subprocess.run([sys.executable, str(tool), "create", "--output", args.output,
        "--artifact", "native.nvn-index=" + str(root / "commands.nvn.jsonl"),
        "--artifact", "renderdoc.capture=" + str(root / "capture.rdc"),
        "--artifact", "native.record-manifest=" + str(root / "manifest.json")], check=True)
    subprocess.run([sys.executable, str(tool), "verify", args.output], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "arm", "collect", "restore", "verify", "replay", "pack"):
        sub = commands.add_parser(name); sub.add_argument("directory")
        if name == "prepare":
            sub.add_argument("--serial", required=True); sub.add_argument("--package", required=True)
            sub.add_argument("--process", required=True); sub.add_argument("--profile", required=True)
            sub.add_argument("--server-package", default="com.picoxr.renderdoccmd.arm64")
        if name == "arm":
            sub.add_argument("--frames", type=int, default=1); sub.add_argument("--trace-mib", type=int, default=64)
        if name == "collect":
            sub.add_argument("--max-rdc-mib", type=int, default=2048)
        if name == "pack":
            sub.add_argument("--foundation", required=True); sub.add_argument("--output", required=True)
        if name == "replay":
            for option in ("output", "mcp-server", "library", "abi"):
                sub.add_argument("--" + option, required=True)
            sub.add_argument("--replay-marker")
            sub.add_argument("--android-vulkan-loader")
            sub.add_argument("--event"); sub.add_argument("--resource")
            sub.add_argument("--mip", type=int, default=0); sub.add_argument("--slice", type=int, default=0)
    args = parser.parse_args()
    if args.command == "verify":
        root, manifest, _ = load(args); header, footer, _ = verify_bundle(root, manifest)
        print(json.dumps({"integrity": "verified", "run": header["run"], "events": footer["events"],
                          "replay_verified": manifest.get("replay_verified", False)}))
    else:
        globals()[args.command](args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.SubprocessError, queue.Empty) as error:
        print("render-record:", str(error), file=sys.stderr)
        sys.exit(1)
