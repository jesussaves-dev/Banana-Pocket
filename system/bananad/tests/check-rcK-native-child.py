#!/usr/bin/env python3
import pathlib
import signal
import subprocess
import sys
import time


helper = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
ready = work / "ready"
child_code = "import signal,sys,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); " \
	"open(sys.argv[1], 'w').close(); time.sleep(30)"
child = subprocess.Popen([sys.executable, "-c", child_code, str(ready)])

try:
    deadline = time.monotonic() + 2
    while not ready.exists() and child.poll() is None and time.monotonic() < deadline:
        time.sleep(0.01)
    assert ready.exists(), "test child did not install its SIGTERM handler"
    stat = pathlib.Path(f"/proc/{child.pid}/stat").read_text()
    rest = stat[stat.rfind(")") + 2 :].split()
    start = int(rest[19])  # Linux stat field 22, relative to field 3.
    record = work / "native-session"

    def publish(start_time: int) -> None:
        record.write_text(
            f"v1 0123456789abcdef0123456789abcdef {child.pid} {start_time} "
            "core 1 ready game.rom\n"
        )

    publish(start + 1)
    subprocess.run([str(helper), str(record), "/proc", "1", "1"], check=True)
    assert child.poll() is None, "stale start time signaled a reused PID"

    publish(start)
    subprocess.run([str(helper), str(record), "/proc", "2", "2"], check=True)
    assert child.wait(timeout=1) == -signal.SIGKILL, "bounded KILL escalation did not reap child"
    print("rcK native child PID validation and bounded reap: passed")
finally:
    if child.poll() is None:
        child.kill()
        child.wait()
