"""What the fixture builder and the scenarios share: a client for mirad's socket, starting mirad in a
fixture HOME (plainly or under a valgrind tool), and reading its memory and threads from /proc."""

import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import tarfile
import time
from pathlib import Path


# Mira stores absolute paths, so a fixture is built and its scenarios run in one HOME. Base and branch
# running side by side each have their own, named alike so paths are the same length.
HOME = Path(os.environ.get("MIRA_BENCH_HOME", "/tmp/mira-bench-home"))


class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout=60):
        super().__init__("mirad", timeout=timeout)
        self.unix_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(str(self.unix_path))


class Client:
    def __init__(self, sock):
        self.sock = sock

    def request(self, method, path, body=None, raw=False, headers=None):
        conn = UnixHTTPConnection(self.sock)
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        conn.request(method, path, body=data, headers=headers or ({"Content-Type": "application/json"} if data else {}))
        response = conn.getresponse()
        payload = response.read()
        conn.close()
        if raw:
            return response.status, payload
        return response.status, (json.loads(payload) if payload else None)

    def get(self, path):
        return self.request("GET", path)

    def wait_job(self, reply, timeout=600):
        """Follows a {"job": id} reply to its end; returns the job's final JSON."""
        status, body = reply
        if not isinstance(body, dict) or "job" not in body:
            return body
        job = body["job"]
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, state = self.get(f"/v1/jobs/{job}")
            if state.get("state") != "running":
                return state
            time.sleep(0.005)
        raise TimeoutError(f"job {job} still running after {timeout}s")


class Mirad:
    """One mirad in `home`, optionally under valgrind (`tool` = callgrind, massif, ...)."""

    def __init__(self, binary, home, tool=None, out_dir=None, extra_valgrind=()):
        self.binary = Path(binary)
        self.home = Path(home)
        self.sock = self.home / "run" / "mirad.sock"
        self.tool = tool
        self.out_dir = Path(out_dir) if out_dir else self.home
        self.extra_valgrind = list(extra_valgrind)
        self.proc = None
        self.log = None

    def env(self):
        env = dict(os.environ)
        home = str(self.home)
        env.update({
            "HOME": home,
            "XDG_CONFIG_HOME": f"{home}/.config",
            "XDG_DATA_HOME": f"{home}/.local/share",
            "XDG_CACHE_HOME": f"{home}/.cache",
            "XDG_RUNTIME_DIR": f"{home}/run",
            "TMPDIR": f"{home}/tmp",
            "MIRA_SOCKET": str(self.sock),
            "MIRA_NO_POWER": "1",
        })
        return env

    def start(self, wait=True, timeout=300):
        for sub in ("run", "tmp"):
            (self.home / sub).mkdir(parents=True, exist_ok=True)
        os.chmod(self.home / "run", 0o700)
        if self.sock.exists():
            self.sock.unlink()
        argv = [str(self.binary), "--socket", str(self.sock)]
        if self.tool:
            self.out_file = self.out_dir / f"{self.tool}.out"
            argv = ["valgrind", f"--tool={self.tool}", f"--{self.tool}-out-file={self.out_file}"
                    if self.tool in ("callgrind", "massif") else f"--log-file={self.out_file}"] + self.extra_valgrind + argv
        self.log = open(self.home / "mirad.log", "w")
        self.started = time.monotonic()
        self.proc = subprocess.Popen(argv, env=self.env(), stdout=self.log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        if wait:
            self.wait_ready(timeout)
        return self

    def wait_ready(self, timeout=300):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"mirad exited with {self.proc.returncode}; see {self.home / 'mirad.log'}")
            if self.sock.exists():
                try:
                    status, _ = self.client().get("/v1/config")
                    if status == 200:
                        self.ready_after = time.monotonic() - self.started
                        return self.ready_after
                except OSError:
                    pass
            time.sleep(0.01)
        raise TimeoutError("mirad never answered")

    def wait_log(self, text, timeout=600):
        """Waits for `text` in mirad's log, e.g. the startup scan's summary line."""
        deadline = time.monotonic() + timeout
        path = self.home / "mirad.log"
        while time.monotonic() < deadline:
            if text in path.read_text(errors="replace"):
                return time.monotonic() - self.started
            time.sleep(0.02)
        raise TimeoutError(f"no '{text}' in mirad's log")

    def client(self):
        return Client(self.sock)

    def pid(self):
        """mirad's own pid, under valgrind too (valgrind execs the tool in the same process)."""
        return self.proc.pid

    def proc_status(self):
        return proc_status(self.pid())

    def callgrind(self, command):
        """callgrind_control on this mirad: "zero", "dump", "instr on" ... (needs tool=callgrind)."""
        args = {"zero": ["-z"], "dump": ["-d"], "on": ["-i", "on"], "off": ["-i", "off"]}[command]
        # valgrind's control pipes are in mirad's TMPDIR, so callgrind_control looks there too.
        done = subprocess.run(["callgrind_control", *args, str(self.pid())], env=self.env(), capture_output=True, text=True)
        if done.returncode != 0 or "Error" in done.stdout:
            raise RuntimeError(f"callgrind_control {command}: {done.stdout.strip()}")

    def stop(self, timeout=60):
        if self.proc is None or self.proc.poll() is not None:
            return
        os.killpg(self.proc.pid, signal.SIGTERM)
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
        self.log.close()


def proc_status(pid):
    """A process's threads, resident memory and its own (anonymous) memory."""
    fields = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        key, _, value = line.partition(":")
        fields[key] = value.strip()
    rollup = {}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()[1:]:
        key, _, value = line.partition(":")
        rollup[key] = value.strip()
    return {
        "threads": int(fields["Threads"]),
        "rss_kb": int(fields["VmRSS"].split()[0]),
        "pss_anon_kb": int(rollup.get("Pss_Anon", "0 kB").split()[0]),
    }


def cpu_ticks(pid):
    """CPU time a process has used, in clock ticks (1/100 s)."""
    stat = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return int(stat[11]) + int(stat[12])


def unpack(fixture_tar, dest=HOME):
    """A fresh copy of the fixture HOME, so every scenario starts from the same library."""
    dest = Path(dest)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with tarfile.open(fixture_tar) as tar:
        tar.extractall(dest, filter="tar")
    return dest


def callgrind_total(path):
    """Instructions executed (Ir) in one callgrind dump: its totals line (summary is 0 after a zero)."""
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.startswith("totals:"):
            return int(line.split()[1])
    return 0
