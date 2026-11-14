"""只通过 Docker 运行参赛代码；不提供宿主机执行的降级路径。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time
import uuid

IMAGE = os.getenv("ARENA_IMAGE", "connect6-sandbox:1")
MOVE_SECONDS = 10.0
MAX_LINE = 16 * 1024


class SandboxError(Exception):
    pass


class BotFault(Exception):
    def __init__(self, reason, elapsed_ms=0):
        super().__init__(reason)
        self.elapsed_ms = elapsed_ms


def docker(*args, timeout=10):
    try:
        return subprocess.run(["docker", *args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SandboxError("Docker 无法连接或操作超时，请启动 Docker Desktop（Linux 容器）。") from error


def availability():
    try:
        info = docker("info", "--format", "{{.OSType}}", timeout=5)
        if info.returncode or info.stdout.strip() != "linux":
            return False, "请启动 Docker Desktop，并使用 Linux 容器。"
        image = docker("image", "inspect", IMAGE, timeout=5)
        if image.returncode:
            return False, "对战镜像还没构建，请先运行 docker build -t connect6-sandbox:1 sandbox。"
        return True, "裁判已就绪"
    except SandboxError as error:
        return False, str(error)


def limits(name, *, compile_mode=False):
    return ["--name", name, "--rm", "--network", "none", "--read-only",
            "--label", "connect6-demo=arena",
            "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
            "--user", "65534:65534", "--cpus", "1", "--pids-limit", "64" if compile_mode else "1",
            "--memory", "512m", "--memory-swap", "512m", "--log-driver", "none",
            "--ulimit", "nofile=64:64", "--ulimit", "fsize=33554432:33554432",
            "--tmpfs", "/tmp:rw,noexec,nosuid,size=128m"]


def remove_container(name):
    try:
        docker("rm", "-f", name, timeout=10)
    except SandboxError:
        pass


def remove_stale_containers():
    """仅在唯一后端启动时清理本 Demo 崩溃遗留的容器。"""
    try:
        result = docker("ps", "-aq", "--filter", "label=connect6-demo=arena", timeout=5)
        for container in result.stdout.splitlines():
            remove_container(container)
    except SandboxError:
        pass


def prepare(source: Path, language: str, destination: Path) -> Path:
    """编译阶段单独放宽进程数，让 g++ 能启动 cc1plus 和链接器。"""
    destination.mkdir(parents=True, exist_ok=True)
    if language == "python":
        output = destination / "bot.py"
        shutil.copyfile(source, output)
        output.chmod(0o644)
        return output
    output = destination / "bot"
    stamp = destination / "compiled.ok"
    if output.exists() and stamp.exists():
        return output
    destination.chmod(0o777)
    name = "c6-build-" + uuid.uuid4().hex
    command = ["docker", "run", *limits(name, compile_mode=True),
               "--mount", f"type=bind,source={source.parent.resolve()},target=/source,readonly",
               "--mount", f"type=bind,source={destination.resolve()},target=/output",
               IMAGE, "g++", "-std=c++17", "-O2", "-pipe", "/source/" + source.name,
               "-o", "/output/bot"]
    tail = bytearray()
    process = None
    reader = None
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

        def drain():
            while chunk := process.stdout.read(1024):
                tail.extend(chunk)
                if len(tail) > 8192:
                    del tail[:-8192]

        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        try:
            code = process.wait(timeout=60)
        except subprocess.TimeoutExpired as error:
            raise SandboxError("编译超过 60 秒，已终止。") from error
        reader.join(timeout=2)
        if code or not output.exists():
            detail = tail.decode("utf-8", errors="replace")[-3000:]
            raise SandboxError("C++ 编译失败：\n" + detail)
        output.chmod(0o755)
        stamp.write_text("ok", encoding="ascii")
        return output
    finally:
        remove_container(name)
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            if reader:
                reader.join(timeout=2)
            process.stdout.close()
        destination.chmod(0o755)


class BotProcess:
    def __init__(self, artifact: Path):
        self.name = "c6-play-" + uuid.uuid4().hex
        self.lines = queue.Queue(maxsize=4)
        self.closed = threading.Event()
        self.close_lock = threading.Lock()
        self.failed = threading.Event()
        self.failure = "AI 进程已退出"
        self.process = subprocess.Popen(
            ["docker", "run", "-i", *limits(self.name),
             "--mount", f"type=bind,source={artifact.parent.resolve()},target=/bot,readonly",
             IMAGE, "python3", "/opt/arena-entry.py", "/bot/" + artifact.name],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.readers = [threading.Thread(target=self._read, daemon=True),
                        threading.Thread(target=self._stderr, daemon=True)]
        for reader in self.readers:
            reader.start()
        try:
            ready = self.lines.get(timeout=30)
            if json.loads(ready) != {"sandbox": "ready"}:
                raise ValueError("no sandbox handshake")
        except (queue.Empty, ValueError, TypeError) as error:
            self.close()
            raise SandboxError("运行容器启动失败，无法确认单线程限制。") from error

    def _read(self):
        while not self.closed.is_set():
            line = self.process.stdout.readline(MAX_LINE + 1)
            if not line:
                self.failed.set()
                break
            if len(line) > MAX_LINE:
                self.failure = "响应超过 16 KB"
                self.failed.set()
                break
            try:
                self.lines.put_nowait(line)
            except queue.Full:
                self.failure = "AI 在没有请求时连续输出"
                self.failed.set()
                break

    def _stderr(self):
        # 持续排空，日志不进入内存或数据库，避免堵塞和无限日志。
        while self.process.stderr.read(4096):
            pass

    def ask(self, message, timeout=MOVE_SECONDS):
        if self.failed.is_set() or not self.lines.empty():
            raise BotFault(self.failure if self.failed.is_set() else "AI 提前输出了响应")
        start = time.monotonic()
        written = threading.Event()
        write_error = []

        def write():
            try:
                self.process.stdin.write((json.dumps(message, separators=(",", ":")) + "\n").encode())
                self.process.stdin.flush()
            except (OSError, ValueError):
                write_error.append(True)
            finally:
                written.set()

        threading.Thread(target=write, daemon=True).start()
        deadline = start + timeout
        try:
            while time.monotonic() < deadline:
                if write_error:
                    raise BotFault("AI 输入管道已关闭")
                if self.failed.is_set() and self.lines.empty():
                    raise BotFault(self.failure)
                if self.closed.is_set():
                    raise BotFault("对局已停止")
                if not written.is_set():
                    written.wait(min(0.02, max(0, deadline - time.monotonic())))
                    continue
                try:
                    raw = self.lines.get(timeout=min(0.05, max(0, deadline - time.monotonic())))
                except queue.Empty:
                    continue
                elapsed = (time.monotonic() - start) * 1000
                if elapsed > timeout * 1000:
                    break
                try:
                    reply = json.loads(raw)
                except (ValueError, UnicodeError) as error:
                    raise BotFault("AI 返回的内容不是合法 JSON", round(elapsed)) from error
                if (not isinstance(reply, dict) or reply.get("protocol") != "arena.v1"
                        or reply.get("type") != "move_response"
                        or reply.get("request_id") != message["request_id"]):
                    raise BotFault("响应协议或 request_id 不匹配", round(elapsed))
                action = reply.get("action")
                if (not isinstance(action, dict) or action.get("kind") != "PLACE"
                        or type(action.get("row")) is not int or type(action.get("col")) is not int):
                    raise BotFault("action 必须是含整数 row、col 的 PLACE", round(elapsed))
                return action, round(elapsed)
            raise BotFault("单次落子超过 10 秒", round((time.monotonic() - start) * 1000))
        except BotFault as error:
            if not error.elapsed_ms:
                error.elapsed_ms = round((time.monotonic() - start) * 1000)
            self.close()
            raise

    def close(self):
        with self.close_lock:
            if self.closed.is_set():
                return
            self.closed.set()
        remove_container(self.name)
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        for reader in self.readers:
            reader.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except (OSError, ValueError):
                pass
