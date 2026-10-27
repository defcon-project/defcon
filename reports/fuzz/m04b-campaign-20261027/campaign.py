#!/usr/bin/env python3
"""Bounded M04b fuzz campaign with an SSH-independent host supervisor."""
from pathlib import Path
import argparse, concurrent.futures, datetime, hashlib, json, os, platform, re
import secrets, shutil, signal, subprocess, sys, threading, time, traceback

ROOT = Path("/home/stejn/m04b-fuzz-campaign-20261027")
DATA = ROOT / "data"
SEED_CORPUS = ROOT / "seed-corpus"
CORPUS_MANIFEST = ROOT / "corpus-manifest.json"
LOCK = Path("/home/stejn/.local/state/defcon-work/m04b-campaign.lock")
NAME = "defcon-m04b-campaign-20261027"
LABEL = "org.defcon.m04b"
TOKEN_LABEL = LABEL + ".token-sha256"
# The CI host is shared with the dfcnbridge staging services since 2026-10-25. The user allowed
# the campaign on 2026-10-27 with at most 2 CPUs and 6 GiB, so two fuzz processes run at a time
# (2 x the 2300 MB RSS limit plus the 512 MB /tmp fit in 6 GiB) and each one cycles its group.
CONTAINER_CPUS = "2"
CONTAINER_MEMORY_GIB = 6
TARGETS = [["process_message_posechal", "process_message_posecommit", "process_message_poseresp"],
           ["process_message_posereport", "pose_report_wire_differential"]]
ALL = [target for group in TARGETS for target in group]
STOP = threading.Event()
STOP_REASON = {"value": None}


class CampaignFailure(RuntimeError):
    pass


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def save(path, value):
    path = Path(path)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{secrets.token_hex(6)}.tmp")
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def token_fingerprint(token):
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def redact_text(value, token):
    return value.replace(token, "<redacted>") if token else value


def parse_version(value):
    match = re.search(r"(\d+)(?:\.(\d+)(?:\.(\d+))?)?", value)
    if not match:
        raise CampaignFailure(f"Cannot parse version: {value!r}")
    return tuple(int(part or 0) for part in match.groups())


def profile_written(paths):
    return any(path.is_file() and not path.is_symlink() and path.stat().st_size > 0
               for path in paths)


def check_fresh_root():
    host_outputs = (
        "host-preflight.json", "host-runtime-check.json", "host-preflight-resources.json",
        "host-mount-preflight.json", "host-launch.json", "host-result.json",
        "host-status.json", "host-heartbeat.json", "host-supervisor.log",
        "container-created-inspect.json", "container-start-inspect.json",
        "container-final-inspect.json", "docker-command.json",
    )
    stale = [name for name in host_outputs
             if (ROOT / name).exists() or (ROOT / name).is_symlink()]
    if stale:
        raise CampaignFailure(f"Prior host campaign outputs exist: {stale!r}")
    if LOCK.exists() or LOCK.is_symlink():
        raise CampaignFailure("Campaign lock already exists; refusing launch")


def check_clean_outputs(data_root):
    data_root = Path(data_root)
    result = data_root / "inside-result.json"
    if result.exists() or result.is_symlink():
        raise CampaignFailure("Terminal inside-result.json already exists; refusing rerun")
    output_names = {"corpus", "logs", "artifacts", "profiles"}
    unexpected = {path.name for path in data_root.iterdir()} - output_names
    if unexpected:
        raise CampaignFailure(f"Unexpected stale campaign data: {sorted(unexpected)!r}")
    for name in ("corpus", "logs", "artifacts", "profiles"):
        directory = data_root / name
        if not directory.exists() and not directory.is_symlink():
            continue
        if directory.is_symlink() or not directory.is_dir():
            raise CampaignFailure(f"Output path is not a plain directory: {directory}")
        if next(directory.iterdir(), None) is not None:
            raise CampaignFailure(f"Output directory is not empty: {directory}")


def validate_seed_corpus(plan, root=ROOT):
    root = Path(root)
    manifest_path = root / CORPUS_MANIFEST.name
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise CampaignFailure("Corpus manifest is missing or not a regular file")
    if digest(manifest_path) != plan.get("corpus_manifest_sha256"):
        raise CampaignFailure("Corpus manifest hash differs from plan")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("targets"), dict):
        raise CampaignFailure("Corpus manifest schema is invalid")
    if set(manifest["targets"]) != set(ALL):
        raise CampaignFailure("Corpus manifest must name every fuzz target exactly once")
    seed_root = root / "seed-corpus"
    if seed_root.is_symlink() or not seed_root.is_dir():
        raise CampaignFailure("Seed corpus root is missing or not a plain directory")
    total_bytes = 0
    for target in ALL:
        directory = seed_root / target
        if directory.is_symlink() or not directory.is_dir():
            raise CampaignFailure(f"Seed corpus directory is missing or unsafe: {target}")
        entries = manifest["targets"][target]
        if not isinstance(entries, list):
            raise CampaignFailure(f"Seed corpus manifest entries are invalid: {target}")
        names = set()
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {"name", "size", "sha256"}:
                raise CampaignFailure(f"Seed corpus manifest record is invalid: {target}")
            name = entry["name"]
            if (not isinstance(name, str) or not name or name in {".", ".."}
                    or Path(name).name != name or name in names):
                raise CampaignFailure(f"Unsafe or duplicate seed name in {target}")
            names.add(name)
            path = directory / name
            if path.is_symlink() or not path.is_file():
                raise CampaignFailure(f"Seed is missing or not a regular file: {target}/{name}")
            size = path.stat().st_size
            if size != entry["size"] or digest(path) != entry["sha256"]:
                raise CampaignFailure(f"Seed hash/size differs from manifest: {target}/{name}")
            total_bytes += size
        actual = set()
        for path in directory.iterdir():
            if path.is_symlink() or not path.is_file():
                raise CampaignFailure(f"Unsafe non-file in seed corpus: {target}/{path.name}")
            actual.add(path.name)
        if actual != names:
            raise CampaignFailure(f"Unmanifested seed files exist for target {target}")
    if total_bytes > 1024**3:
        raise CampaignFailure("Initial seed corpus exceeds 1 GiB")
    return manifest


def initialize_work_data(work_root, manifest, seed_root):
    work_root = Path(work_root)
    seed_root = Path(seed_root)
    check_clean_outputs(work_root)
    for name in ("corpus", "logs", "artifacts", "profiles"):
        (work_root / name).mkdir(exist_ok=True)
    for target in ALL:
        target_corpus = work_root / "corpus" / target
        target_corpus.mkdir()
        for entry in manifest["targets"][target]:
            source = seed_root / target / entry["name"]
            copied = target_corpus / entry["name"]
            shutil.copyfile(source, copied)
            if copied.stat().st_size != entry["size"] or digest(copied) != entry["sha256"]:
                raise CampaignFailure(f"Copied seed differs from manifest: {target}/{entry['name']}")


def proc_identity(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return {"pid": int(pid), "ppid": int(fields[1]), "start_ticks": fields[19]}
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def install_signals():
    def request_stop(signum, _frame):
        STOP_REASON["value"] = signal.Signals(signum).name
        STOP.set()
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, request_stop)


def command(argv, timeout=30, check=True, redact_values=()):
    def safe(value):
        for secret in redact_values:
            value = value.replace(secret, "<redacted>")
        return value

    child = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, start_new_session=True)
    try:
        out, err = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as first:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            out, err = child.communicate(timeout=5)
        except subprocess.TimeoutExpired as second:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                out, err = child.communicate(timeout=2)
            except subprocess.TimeoutExpired as final:
                try:
                    child.kill()
                except (ProcessLookupError, PermissionError):
                    pass
                for pipe in (child.stdout, child.stderr):
                    if pipe:
                        pipe.close()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
                out = final.output or second.output or first.output or ""
                err = final.stderr or second.stderr or first.stderr or ""
        raise CampaignFailure(
            f"Timed out after {timeout}s: {safe(repr(argv))}; {safe(err[-1000:])!r}")
    result = subprocess.CompletedProcess(argv, child.returncode, out, err)
    if check and result.returncode:
        raise CampaignFailure(f"Command failed ({result.returncode}): {safe(repr(argv))}; {safe(err[-2000:])!r}")
    return result


def docker(*args, timeout=30, check=True):
    return command(["sudo", "-n", "docker", *args], timeout, check)


def is_transient(error):
    # Docker 29 lowercases its CLI error messages, so match without regard to case.
    transient = ("cannot connect to the docker daemon", "error during connect",
                 "is the docker daemon running", "context deadline exceeded",
                 "timed out after")
    error = error.lower()
    return any(word in error for word in transient)


def inspect_command(*args):
    stopping = STOP.is_set()
    attempts = 1 if stopping else 3
    timeout = 5 if stopping else 20
    for attempt in range(attempts):
        try:
            result = docker("inspect", *args, timeout=timeout, check=False)
        except CampaignFailure as exc:
            result, error = None, str(exc)
        else:
            error = result.stderr
            if result.returncode == 0 or not is_transient(error):
                return result
        if attempt + 1 == attempts or not is_transient(error):
            raise CampaignFailure(f"Docker inspect remained unavailable: {error[-1500:]!r}")
        if STOP.wait(attempt + 1):
            raise CampaignFailure(f"Stop requested during Docker inspect retry: {error[-1500:]!r}")
    raise CampaignFailure("Docker inspect retry loop ended unexpectedly")


def inspect_id(cid):
    result = inspect_command(cid)
    if result.returncode:
        raise CampaignFailure(f"Container ID inspection failed: {result.stderr[-1500:]!r}")
    rows = json.loads(result.stdout)
    if len(rows) != 1:
        raise CampaignFailure("Container ID inspection was not unique")
    return rows[0]


def inspect_name_or_absent():
    result = inspect_command(NAME)
    if result.returncode == 0:
        rows = json.loads(result.stdout)
        if len(rows) != 1:
            raise CampaignFailure("Container name inspection was not unique")
        return rows[0]
    # "Error: No such object" before Docker 29, "error: no such object" from Docker 29.
    error = result.stderr.lower()
    if "no such object" in error or "no such container" in error:
        return None
    raise CampaignFailure(f"Cannot establish container-name state: {result.stderr[-1500:]!r}")


def plan_file(in_container=False):
    return Path("/runner/plan.json") if in_container else ROOT / "plan.json"


def validate_plan():
    if ROOT.resolve(strict=True) != ROOT or ROOT.is_symlink() or not DATA.is_dir() or DATA.is_symlink():
        raise CampaignFailure("Campaign root is not canonical")
    plan = json.loads(plan_file().read_text())
    if plan.get("root") != str(ROOT) or plan.get("container") != NAME:
        raise CampaignFailure("Plan root/container mismatch")
    if plan.get("duration_seconds") != 259200:
        raise CampaignFailure("Plan duration must be exactly 72 hours")
    if plan.get("uid") != os.getuid() or plan.get("gid") != os.getgid():
        raise CampaignFailure("Plan UID/GID mismatch")
    remaining = plan.get("deadline_epoch", 0) - time.time()
    hard = datetime.datetime(2026, 10, 30, 11, 55, tzinfo=datetime.timezone.utc).timestamp()
    if not 0 < remaining <= 259200 or plan["deadline_epoch"] > hard:
        raise CampaignFailure("Deadline is outside the authorized 72-hour window")
    if digest(ROOT / "campaign.py") != plan.get("runner_sha256"):
        raise CampaignFailure("Runner hash differs from frozen plan")
    if not str(plan.get("image_id", "")).startswith("sha256:"):
        raise CampaignFailure("Plan lacks immutable Docker image ID")
    if digest(ROOT / "payload" / "fuzz") != plan.get("binary_sha256"):
        raise CampaignFailure("Fuzz binary hash differs from plan")
    if digest(ROOT / "payload" / "ubsan") != plan.get("ubsan_sha256"):
        raise CampaignFailure("UBSan suppression hash differs from plan")
    validate_seed_corpus(plan)
    check_clean_outputs(DATA)
    protected = plan.get("protected_processes")
    if not isinstance(protected, list) or not protected:
        raise CampaignFailure("Protected-process inventory is empty")
    pids = [item.get("pid") for item in protected]
    if len(pids) != len(set(pids)):
        raise CampaignFailure("Protected-process inventory has duplicate PIDs")
    return plan


def check_protected(plan):
    for item in plan["protected_processes"]:
        actual = proc_identity(item["pid"])
        if not actual or actual["start_ticks"] != str(item["start_ticks"]):
            raise CampaignFailure(f"Protected process changed: {item.get('name', item['pid'])}")


def check_container(c, plan, token):
    host, config = c["HostConfig"], c["Config"]
    labels = config.get("Labels") or {}
    if c["Name"] != "/" + NAME or c["Image"] != plan["image_id"]:
        raise CampaignFailure("Container name or inspected image ID mismatch")
    if config["Image"] != plan["image_id"]:
        raise CampaignFailure("Container config did not use immutable image ID")
    if (labels.get(LABEL + ".campaign") != NAME
            or labels.get(TOKEN_LABEL) != token_fingerprint(token)):
        raise CampaignFailure("Container ownership labels mismatch")
    if (host["NetworkMode"] != "none"
            or host["NanoCpus"] != round(float(CONTAINER_CPUS) * 1_000_000_000)):
        raise CampaignFailure("Network/CPU limits mismatch")
    if host["CpuShares"] != 128 or host["Memory"] != CONTAINER_MEMORY_GIB * 1024**3:
        raise CampaignFailure("CPU-share/memory limits mismatch")
    if host["MemorySwap"] != host["Memory"] or host["PidsLimit"] != 512:
        raise CampaignFailure("Swap/process limits mismatch")
    if not host["ReadonlyRootfs"] or host["Privileged"] or "ALL" not in host["CapDrop"]:
        raise CampaignFailure("Container privilege settings mismatch")
    if not any("no-new-privileges" in x for x in (host["SecurityOpt"] or [])):
        raise CampaignFailure("no-new-privileges missing")
    if config["User"] != f"{plan['uid']}:{plan['gid']}":
        raise CampaignFailure("Container user mismatch")
    expected = sorted([(str(ROOT), "/runner", False),
                       (str(DATA), "/work", True),
                       (str(ROOT / "payload"), "/payload", False)])
    mounts = c.get("Mounts") or []
    actual_binds = sorted((m["Source"], m["Destination"], m["RW"])
                          for m in mounts if m.get("Type") == "bind")
    if actual_binds != expected:
        raise CampaignFailure(f"Unexpected container bind mounts: {actual_binds!r}")
    host_binds = {mount.get("Target"): mount
                  for mount in (host.get("Mounts") or [])
                  if mount.get("Type") == "bind"}
    for destination in ("/runner", "/payload"):
        mount = host_binds.get(destination)
        options = (mount or {}).get("BindOptions") or {}
        if (not mount or mount.get("ReadOnly") is not True
                or options.get("ReadOnlyForceRecursive") is not True
                or options.get("ReadOnlyNonRecursive") is True):
            raise CampaignFailure(
                f"Recursive read-only bind mount is not confirmed for {destination}")
    if (host.get("Init") is not True or (host.get("CapAdd") or [])
            or (host.get("Devices") or []) or host.get("OomScoreAdj") != 500):
        raise CampaignFailure("Container init/capability/device/OOM settings mismatch")
    for mount in mounts:
        if mount.get("Type") == "bind":
            continue
        if (mount.get("Type") == "tmpfs" and mount.get("Destination") == "/tmp"
                and mount.get("RW") is True):
            continue
        raise CampaignFailure(f"Unexpected non-bind mount: {mount!r}")
    if (host.get("Tmpfs") or {}).get("/tmp") != "rw,nosuid,nodev,size=512m":
        raise CampaignFailure("Temporary filesystem settings mismatch")


def check_owned_container(c, cid, plan, token):
    config = c.get("Config") or {}
    labels = config.get("Labels") or {}
    if c.get("Id") != cid or c.get("Name") != "/" + NAME:
        raise CampaignFailure("Container ID/name does not match the owned campaign")
    if (c.get("Image") != plan["image_id"] or config.get("Image") != plan["image_id"]
            or labels.get(LABEL + ".campaign") != NAME
            or labels.get(TOKEN_LABEL) != token_fingerprint(token)):
        raise CampaignFailure("Container immutable identity/ownership does not match")


def stop_owned(cid, plan, token):
    c = inspect_id(cid)
    check_owned_container(c, cid, plan, token)
    if c["State"]["Running"]:
        docker("stop", "--time", "45", cid, timeout=60)
    c = inspect_id(cid)
    check_owned_container(c, cid, plan, token)
    if c["State"]["Running"]:
        docker("kill", cid, timeout=15)
        c = inspect_id(cid)
        check_owned_container(c, cid, plan, token)
    if c["State"]["Running"]:
        raise CampaignFailure("Owned campaign container remains running")
    return c


def directory_size(path):
    error = None
    for attempt in range(4):
        try:
            result = subprocess.run(["du", "-s", "-B1", "--", str(path)],
                                    text=True, capture_output=True, timeout=10)
        except subprocess.TimeoutExpired:
            error = "du scan attempt exceeded 10 seconds"
            if attempt < 3:
                time.sleep(0.5 * (attempt + 1))
            continue
        if result.returncode == 0:
            try:
                return int(result.stdout.split()[0])
            except (ValueError, IndexError) as exc:
                error = repr(exc)
        else:
            error = result.stderr[-1000:]
        if attempt < 3:
            time.sleep(0.5 * (attempt + 1))
    raise CampaignFailure(f"Repeated directory-size scans failed: {error!r}")


def check_host_resources(root=ROOT):
    if STOP.is_set():
        raise CampaignFailure("Supervisor stop requested before host disk scan")
    free = shutil.disk_usage(root).free
    used = directory_size(root)
    mem = {line.split(":", 1)[0]: int(line.split(":", 1)[1].split()[0])
           for line in Path("/proc/meminfo").read_text().splitlines() if ":" in line}
    available = mem.get("MemAvailable", 0)
    if free < 20 * 1024**3:
        raise CampaignFailure("Host free disk floor 20 GiB")
    if used > 8 * 1024**3:
        raise CampaignFailure("Campaign disk ceiling 8 GiB")
    if available < 3 * 1024**2:
        raise CampaignFailure("Host available memory floor 3 GiB")
    if available < (CONTAINER_MEMORY_GIB + 3) * 1024**2:   # only called before a start
        raise CampaignFailure(f"Starting needs {CONTAINER_MEMORY_GIB} GiB for the container "
                              "plus the 3 GiB host floor available")
    return {"campaign_bytes": used, "disk_free": free,
            "mem_available_kib": available}


def check_host_prerequisites(plan):
    identity = proc_identity(os.getpid())
    if not identity:
        raise CampaignFailure("Cannot read this process identity from /proc")
    kernel_release = platform.release()
    kernel_version = parse_version(kernel_release)
    if kernel_version < (5, 12, 0):
        raise CampaignFailure("Kernel 5.12 or newer is required for recursive read-only bind mounts")
    systemd_lines = command(["systemd", "--version"], timeout=10).stdout.splitlines()
    if not systemd_lines:
        raise CampaignFailure("systemd --version returned no output")
    systemd_version = parse_version(systemd_lines[0])
    if systemd_version < (240, 0, 0):
        raise CampaignFailure("systemd 240 or newer is required by the detached supervisor")
    docker_version = docker("version", "--format", "{{.Server.Version}}", timeout=10).stdout.strip()
    docker_version_tuple = parse_version(docker_version)
    if docker_version_tuple < (25, 0, 0):
        raise CampaignFailure("Docker Engine 25 or newer is required for recursive read-only bind mounts")
    image_id = docker("image", "inspect", plan["image_id"], "--format", "{{.Id}}",
                      timeout=15).stdout.strip()
    if image_id != plan["image_id"]:
        raise CampaignFailure("Pinned campaign image is not present by exact immutable ID")
    return {
        "kernel_release": kernel_release,
        "systemd_version": ".".join(map(str, systemd_version)),
        "docker_server_version": docker_version,
        "docker_server_version_tuple": docker_version_tuple,
        "image_id": image_id,
        "proc_identity": identity,
        "recursive_readonly_mount": "the exact campaign create must succeed before start",
    }


def inside():
    plan = json.loads(plan_file(True).read_text())
    if digest(Path("/runner/campaign.py")) != plan.get("runner_sha256"):
        raise CampaignFailure("Read-only runner does not match plan")
    if digest(Path("/payload/fuzz")) != plan.get("binary_sha256"):
        raise CampaignFailure("Mounted fuzz binary does not match plan")
    if digest(Path("/payload/ubsan")) != plan.get("ubsan_sha256"):
        raise CampaignFailure("Mounted UBSan suppression does not match plan")
    work = Path("/work")
    manifest = validate_seed_corpus(plan, Path("/runner"))
    initialize_work_data(work, manifest, Path("/runner/seed-corpus"))

    install_signals()
    end = plan["deadline_epoch"] - 300
    mutex, children, records, failures = threading.Lock(), {}, [], []

    def terminate(process):
        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()

    def worker(slot, group):
        cycle = 0
        try:
            while time.time() < end and not STOP.is_set():
                target = group[cycle % len(group)]
                name = f"{slot}-{cycle:04d}-{target}"
                artifacts = Path("/work/artifacts") / name
                artifacts.mkdir()
                log = Path("/work/logs") / (name + ".log")
                profile = (str(Path("/work/profiles") / (target + "-%p.profraw"))
                           if cycle < len(group) else "/dev/null")
                env = dict(os.environ, HOME="/tmp", FUZZ=target,
                    ASAN_SYMBOLIZER_PATH="/usr/bin/llvm-symbolizer-18",
                    UBSAN_OPTIONS="suppressions=/payload/ubsan:print_stacktrace=1:halt_on_error=1:report_error_type=1",
                    ASAN_OPTIONS="detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1",
                    LLVM_PROFILE_FILE=profile)
                seconds = min(3600, max(1, int(end - time.time())))
                argv = ["/payload/fuzz", f"-max_total_time={seconds}", "-timeout=10",
                    "-rss_limit_mb=2300", "-max_len=16384",
                    f"-seed={20261027 + slot * 10000 + cycle}", "-print_final_stats=1",
                    f"-artifact_prefix={artifacts}/", str(Path("/work/corpus") / target)]
                save(Path("/work/logs") / (name + ".command.json"), {
                    "command": argv, "environment": {k: env[k] for k in
                    ("FUZZ", "ASAN_SYMBOLIZER_PATH", "UBSAN_OPTIONS", "ASAN_OPTIONS", "LLVM_PROFILE_FILE")},
                    "started_at_utc": utc()})
                with log.open("xb") as stream:
                    process = subprocess.Popen(argv, env=env, stdout=stream,
                        stderr=subprocess.STDOUT, start_new_session=True)
                    with mutex:
                        children[slot] = {"pid": process.pid, "target": target, "cycle": cycle}
                    while process.poll() is None:
                        if STOP.wait(1):
                            terminate(process)
                            break
                output = log.read_text(errors="replace")
                hits = re.findall(r"stat::number_of_executed_units:\s*(\d+)", output)
                units = int(hits[-1]) if hits else 0
                bad = any(x in output for x in ("ERROR: AddressSanitizer", "runtime error:",
                    "ERROR: libFuzzer:", "LeakSanitizer", "SUMMARY: UndefinedBehaviorSanitizer"))
                profile_paths = sorted(p for p in Path("/work/profiles").glob(target + "-*.profraw")
                                       if p.is_file() and not p.is_symlink())
                profile_files = [p.name for p in profile_paths]
                profile_nonempty = profile_written(profile_paths)
                profile_required = cycle < len(group)
                hashes = [{"name": p.name, "sha256": digest(p)}
                          for p in sorted(artifacts.iterdir()) if p.is_file()]
                record = {"slot": slot, "cycle": cycle, "target": target,
                    "exit": process.returncode, "log": str(log), "log_sha256": digest(log),
                    "finished_at_utc": utc(), "sanitizer_or_fuzzer_error": bad,
                    "terminal_stats": bool(hits), "executed_units": units,
                    "profiles": profile_files, "profile_nonempty": profile_nonempty,
                    "profile_required": profile_required, "artifacts": hashes,
                    "interrupted": STOP.is_set()}
                with mutex:
                    children.pop(slot, None)
                    records.append(record)
                    save(Path("/work/chunks.json"), records)
                if (process.returncode or bad or not hits or units <= 0
                        or (profile_required and not profile_nonempty)):
                    failures.append(record)
                    STOP_REASON["value"] = f"Chunk failed: {target} cycle={cycle} exit={process.returncode} units={units}"
                    STOP.set()
                    break
                cycle += 1
        except BaseException as exc:
            with mutex:
                failures.append({"slot": slot, "error": repr(exc),
                    "traceback": traceback.format_exc(), "at_utc": utc()})
                save(Path("/work/chunks.json"), records)
            STOP_REASON["value"] = repr(exc)
            STOP.set()

    started, driver_error = utc(), None
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(TARGETS)) as pool:
            futures = [pool.submit(worker, slot, group) for slot, group in enumerate(TARGETS)]
            while not all(f.done() for f in futures):
                with mutex:
                    save(Path("/work/heartbeat.json"), {"at_utc": utc(),
                        "epoch": time.time(), "children": list(children.values()),
                        "completed_chunks": len(records), "failures": len(failures),
                        "stop_reason": STOP_REASON["value"]})
                time.sleep(10)
            for future in futures:
                try:
                    future.result()
                except BaseException as exc:
                    failures.append({"error": repr(exc), "traceback": traceback.format_exc()})
                    STOP.set()
    except BaseException as exc:
        driver_error = {"error": repr(exc), "traceback": traceback.format_exc()}
        STOP.set()

    units = {t: sum(r.get("executed_units", 0) for r in records
                    if r.get("target") == t and r.get("exit") == 0) for t in ALL}
    profiles = {t: sorted(p.name for p in Path("/work/profiles").glob(t + "-*.profraw")
                           if p.is_file() and p.stat().st_size > 0) for t in ALL}
    reached = time.time() >= end
    state = "PASS" if (not failures and not driver_error and not STOP.is_set() and reached
                       and all(units[t] > 0 and profiles[t] for t in ALL)) else "FAIL"
    save(Path("/work/inside-result.json"), {
        "state": state, "started_at_utc": started, "finished_at_utc": utc(),
        "chunks": len(records), "targets": ALL, "target_executed_units": units,
        "profiles": profiles,
        "profile_scope": "Only the first profile-writing chunk for each target; not cumulative campaign coverage.",
        "failures": failures, "driver_error": driver_error,
        "stop_reason": STOP_REASON["value"], "deadline_reached": reached,
        "interrupted_chunks": [r for r in records if r.get("interrupted")]})
    return 0 if state == "PASS" else 1


def check_lock(token):
    owner = json.loads((LOCK / "owner.json").read_text())
    if owner.get("token") != token or owner.get("root") != str(ROOT):
        raise CampaignFailure("Lock token/root mismatch")


def host_child(token):
    outcome = {"state": "RUNNING", "started_at_utc": utc(),
               "pid": os.getpid(), "container_id": None, "unit": None}
    plan, cid, error = None, None, None
    cleanup_ok, unlock = False, False
    install_signals()
    try:
        plan = validate_plan()
        check_lock(token)
        previous_owner = json.loads((LOCK / "owner.json").read_text())
        outcome["unit"] = previous_owner.get("unit")
        owner = {"root": str(ROOT), "token": token,
            "pid": os.getpid(), "process": proc_identity(os.getpid()),
            "state": "RUNNING", "started_at_utc": utc(),
            "deadline_epoch": plan["deadline_epoch"]}
        if previous_owner.get("unit"):
            owner["unit"] = previous_owner["unit"]
        save(LOCK / "owner.json", owner)
        host_checks = check_host_prerequisites(plan)
        save(ROOT / "host-runtime-check.json", {"state": "PASS", "at_utc": utc(), **host_checks})
        if STOP.is_set():
            raise CampaignFailure(f"Supervisor received {STOP_REASON['value']} before resource check")
        check_protected(plan)
        if inspect_name_or_absent() is not None:
            raise CampaignFailure("Campaign container name exists; refusing duplicate")
        resource_check = check_host_resources()
        save(ROOT / "host-preflight-resources.json", {
            "at_utc": utc(), **resource_check})
        args = ["create", "--pull=never", "--name", NAME, "--init", "--restart=no",
            "--stop-timeout", "45", "--label", f"{LABEL}.campaign={NAME}",
            "--label", f"{TOKEN_LABEL}={token_fingerprint(token)}", "--network", "none",
            "--cpus", CONTAINER_CPUS, "--cpu-shares", "128",
            "--memory", f"{CONTAINER_MEMORY_GIB}g", "--memory-swap", f"{CONTAINER_MEMORY_GIB}g",
            "--pids-limit", "512", "--read-only",
            "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
            "--oom-score-adj", "500", "--user", f"{plan['uid']}:{plan['gid']}",
            "--tmpfs", "/tmp:rw,nosuid,nodev,size=512m",
            "--log-driver", "json-file", "--log-opt", "max-size=10m",
            "--log-opt", "max-file=3",
            "--mount", f"type=bind,src={ROOT},dst=/runner,readonly,bind-recursive=readonly",
            "--mount", f"type=bind,src={DATA},dst=/work",
            "--mount", f"type=bind,src={ROOT}/payload,dst=/payload,readonly,bind-recursive=readonly",
            "--workdir", "/work", plan["image_id"],
            "python3", "/runner/campaign.py", "--inside"]
        save(ROOT / "docker-command.json", ["sudo", "-n", "docker",
            *[arg.replace(token, "<redacted>") for arg in args]])
        created = docker(*args, timeout=60)
        cid = created.stdout.strip()
        if not re.fullmatch(r"[0-9a-f]{64}", cid):
            raise CampaignFailure("Docker create returned invalid ID")
        outcome["container_id"] = cid
        c = inspect_id(cid)
        check_container(c, plan, token)
        save(ROOT / "container-created-inspect.json", c)
        save(ROOT / "host-mount-preflight.json", {
            "state": "PASS", "at_utc": utc(),
            "method": "campaign container create with bind-recursive=readonly and exact mount inspection before start",
            "container_id": cid})
        if STOP.is_set():
            raise CampaignFailure(f"Supervisor received {STOP_REASON['value']} before container start")
        docker("start", cid)
        c = inspect_id(cid)
        check_container(c, plan, token)
        save(ROOT / "container-start-inspect.json", c)
        save(ROOT / "host-status.json", {"state": "RUNNING", "pid": os.getpid(),
            "container_id": cid, "started_at_utc": outcome["started_at_utc"],
            "deadline_epoch": plan["deadline_epoch"]})

        reason = None
        while c["State"]["Running"]:
            if STOP.is_set():
                reason = f"Supervisor received {STOP_REASON['value']}"
                raise CampaignFailure(reason)
            if time.time() >= plan["deadline_epoch"]:
                reason = "72-hour deadline reached; stopping container"
            else:
                check_protected(plan)
            free = shutil.disk_usage(ROOT).free
            used = directory_size(ROOT)
            mem = {line.split(":", 1)[0]: int(line.split(":", 1)[1].split()[0])
                   for line in Path("/proc/meminfo").read_text().splitlines() if ":" in line}
            if free < 20 * 1024**3:
                reason = reason or "Host free disk floor 20 GiB"
            if used > 8 * 1024**3:
                reason = reason or "Campaign disk ceiling 8 GiB"
            if mem.get("MemAvailable", 0) < 3 * 1024**2:
                reason = reason or "Host available memory floor 3 GiB"
            save(ROOT / "host-heartbeat.json", {"state": "RUNNING", "at_utc": utc(),
                "campaign_bytes": used, "disk_free": free,
                "mem_available_kib": mem.get("MemAvailable", 0),
                "limit_reason": reason, "container_id": cid})
            if reason:
                raise CampaignFailure(reason)
            time.sleep(15)
            c = inspect_id(cid)
            check_container(c, plan, token)

        final = inspect_id(cid)
        check_container(final, plan, token)
        save(ROOT / "container-final-inspect.json", final)
        result = json.loads((DATA / "inside-result.json").read_text())
        if final["State"]["OOMKilled"] or final["State"]["ExitCode"] != 0 or result.get("state") != "PASS":
            raise CampaignFailure(f"Campaign failed: docker={final['State'].get('ExitCode')} inside={result.get('state')}")
        outcome["state"] = "PASS"
    except BaseException as exc:
        error = {"error": redact_text(repr(exc), token),
                 "traceback": redact_text(traceback.format_exc(), token), "at_utc": utc()}
        outcome["state"] = "FAIL"

    try:
        if plan is not None:
            if cid is None:
                existing = inspect_name_or_absent()
                if existing is None:
                    cleanup_ok = unlock = True
                elif (existing.get("Config", {}).get("Labels") or {}).get(TOKEN_LABEL) == token_fingerprint(token):
                    cid = existing["Id"]
                    outcome["container_id"] = cid
                else:
                    raise CampaignFailure("Named container is not owned by this launch")
            if cid is not None:
                final = stop_owned(cid, plan, token)
                save(ROOT / "container-final-inspect.json", final)
                cleanup_ok = unlock = True
    except BaseException as exc:
        outcome["state"] = "UNCERTAIN"
        error = {"error": redact_text(repr(exc), token),
                 "traceback": redact_text(traceback.format_exc(), token),
                 "prior_error": error, "at_utc": utc()}
        unlock = False

    outcome.update({"finished_at_utc": utc(), "cleanup_confirmed": cleanup_ok,
                    "error": error, "signal": STOP_REASON["value"]})
    save(ROOT / "host-result.json", outcome)
    save(ROOT / "host-status.json", outcome)
    if unlock and outcome["state"] in ("PASS", "FAIL"):
        try:
            check_lock(token)
            (LOCK / "owner.json").unlink()
            LOCK.rmdir()
        except BaseException as exc:
            outcome["state"] = "UNCERTAIN"
            outcome["lock_cleanup_error"] = repr(exc)
            save(ROOT / "host-result.json", outcome)
            save(ROOT / "host-status.json", outcome)
    return 0 if outcome["state"] == "PASS" else 1


def launch_host():
    install_signals()
    plan = validate_plan()
    check_fresh_root()
    if (ROOT / "host-result.json").exists():
        raise CampaignFailure("Terminal result exists; refusing rerun")
    user = str(os.getuid())
    linger = command(["loginctl", "show-user", user, "--property=Linger", "--value"],
                     timeout=10).stdout.strip()
    if linger != "yes":
        raise CampaignFailure("User lingering is disabled; refusing an SSH-session-bound launch")
    host_checks = check_host_prerequisites(plan)
    resource_check = check_host_resources()
    if STOP.is_set():
        raise CampaignFailure(f"Supervisor received {STOP_REASON['value']} before lock acquisition")
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    LOCK.mkdir()
    token = secrets.token_hex(24)
    unit = f"m04b-fuzz-campaign-20261027-{secrets.token_hex(4)}"
    log = ROOT / "host-supervisor.log"
    launch_attempted = False
    try:
        save(LOCK / "owner.json", {"root": str(ROOT), "token": token,
            "pid": None, "unit": unit, "state": "LAUNCHING", "started_at_utc": utc(),
            "deadline_epoch": plan["deadline_epoch"]})
        save(ROOT / "host-preflight.json", {
            "state": "PASS", "at_utc": utc(), "host": host_checks, **resource_check})
        log.touch(mode=0o600, exist_ok=True)
        argv = ["systemd-run", "--user", "--quiet", "--no-block", f"--unit={unit}",
            "--property=Type=exec", "--property=KillMode=control-group",
            "--property=Restart=no", "--property=TimeoutStopSec=300",
            f"--property=StandardOutput=append:{log}",
            f"--property=StandardError=append:{log}", "--",
            sys.executable, str(ROOT / "campaign.py"), "--daemon-child", "--token", token]
        if STOP.is_set():
            raise CampaignFailure(f"Supervisor received {STOP_REASON['value']} before systemd-run")
        launch_attempted = True
        command(argv, timeout=30, redact_values=(token,))
        details = None
        for attempt in range(8):
            result = command(["systemctl", "--user", "show", unit,
                "--property=ActiveState", "--property=MainPID"], timeout=10)
            values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
            pid = int(values.get("MainPID", "0"))
            if values.get("ActiveState") == "active" and pid > 0:
                details = {"pid": pid, "process": proc_identity(pid)}
                break
            if (ROOT / "host-result.json").exists():
                break
            time.sleep(1)
        if details is None:
            raise CampaignFailure(f"systemd unit did not become active: {unit}")
        launch = {"state": "RUNNING", "unit": unit, **details,
            "started_at_utc": utc(), "log": str(log),
            "host_result": str(ROOT / "host-result.json")}
        save(ROOT / "host-launch.json", launch)
        print(json.dumps(launch), flush=True)
    except BaseException as exc:
        launch = {"state": "UNCERTAIN" if launch_attempted else "NOT_LAUNCHED",
            "at_utc": utc(), "error": redact_text(repr(exc), token),
            "traceback": redact_text(traceback.format_exc(), token), "unit": unit,
            "host_result": str(ROOT / "host-result.json"), "cleanup_confirmed": False}
        save(ROOT / "host-launch.json", launch)
        if not launch_attempted:
            try:
                if not LOCK.is_dir() or LOCK.is_symlink():
                    raise CampaignFailure("Launch lock path changed before cleanup")
                owner_path = LOCK / "owner.json"
                entries = {entry.name for entry in LOCK.iterdir()}
                if entries - {"owner.json"}:
                    raise CampaignFailure("Unexpected files in launch lock")
                if owner_path.exists() or owner_path.is_symlink():
                    if not owner_path.is_file() or owner_path.is_symlink():
                        raise CampaignFailure("Unsafe owner path in launch lock")
                    owner = json.loads(owner_path.read_text())
                    if owner.get("token") != token or owner.get("root") != str(ROOT):
                        raise CampaignFailure("Launch lock ownership changed")
                    owner_path.unlink()
                LOCK.rmdir()
                launch["cleanup_confirmed"] = True
                save(ROOT / "host-launch.json", launch)
            except BaseException as cleanup_exc:
                launch["state"] = "UNCERTAIN"
                launch["cleanup_error"] = redact_text(repr(cleanup_exc), token)
                save(ROOT / "host-launch.json", launch)
        raise CampaignFailure(redact_text(str(exc), token)) from None
    return 0


def main():
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--inside", action="store_true")
    group.add_argument("--host", action="store_true")
    group.add_argument("--daemon-child", action="store_true")
    parser.add_argument("--token")
    args = parser.parse_args()
    if args.inside:
        return inside()
    if args.host:
        if args.token:
            raise CampaignFailure("--token is reserved for detached supervisor")
        return launch_host()
    if not args.token:
        raise CampaignFailure("Detached supervisor requires its token")
    return host_child(args.token)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BaseException as exc:
        print(f"FATAL: {exc!r}", file=sys.stderr, flush=True)
        traceback.print_exc()
        sys.exit(1)
