"""容器的唯一进程。exec 替换自身，避免包装器占用线程名额。"""
import json
import os
import sys

with open("/sys/fs/cgroup/pids.max", encoding="ascii") as limit:
    if limit.read().strip() != "1":
        raise SystemExit("single-thread cgroup is required")
allowed = os.sched_getaffinity(0)
os.sched_setaffinity(0, {min(allowed)})
print(json.dumps({"sandbox": "ready"}), flush=True)
target = sys.argv[1]
if target.endswith(".py"):
    os.execv("/usr/bin/python3", ["python3", "-u", target])
os.execv(target, [target])
