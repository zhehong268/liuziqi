"""Windows 客户端：启动本机网页，检测并构建 Docker 对战环境。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import traceback
import webbrowser


def self_test(report: Path) -> int:
    """打包后执行，不使用用户原有数据库或 Docker 容器。"""
    result = {"ok": False}
    try:
        with tempfile.TemporaryDirectory(prefix="moyi-check-") as directory:
            os.environ["CONNECT6_DATA_DIR"] = directory
            from app import app, arena_service, DB_PATH
            from paths import RESOURCE_DIR
            from waitress.server import create_server
            client = app.test_client()
            assert client.get("/api/health").json["ok"], "health"
            assert client.get("/").status_code == 200, "index"
            assert client.get("/static/ink-grid.svg").status_code == 200, "board asset"
            example = client.get("/api/arena/example")
            assert example.status_code == 200 and b"arena.v1" in example.data, "example download"
            example.close()
            digest = hashlib.sha256((RESOURCE_DIR / "official" / "main.cpp").read_bytes()).hexdigest()
            assert digest == "563a56aee9bf7a40d82fa2e80fdaefc008b0405cdb3c816ede723079fd681135", "official source"
            assert (RESOURCE_DIR / "sandbox" / "Dockerfile").is_file(), "sandbox resources"
            created = client.post("/api/games", json={"opponent": "local", "humanColor": "white"})
            assert created.status_code == 201, "new practice"
            game_id = created.json["id"]
            opening = client.post("/api/games/" + game_id + "/ai-turn").json
            assert len(opening["moves"]) == 1 and opening["currentPlayer"] == "white", "white opening"
            assert client.get("/api/games/" + game_id + "/export?format=json").status_code == 200, "export"
            assert DB_PATH.parent == Path(directory).resolve() and DB_PATH.is_file(), "temporary database"
            from urllib.request import ProxyHandler, build_opener
            server = create_server(app, host="127.0.0.1", port=0)
            thread = threading.Thread(target=server.run, daemon=True)
            thread.start()
            try:
                with build_opener(ProxyHandler({})).open(
                    f"http://127.0.0.1:{server.effective_port}/api/health", timeout=5
                ) as response:
                    assert json.load(response)["ok"], "HTTP service"
            finally:
                server.close()
                thread.join(5)
                server.task_dispatcher.shutdown()
                arena_service.shutdown()
            result = {"ok": True, "officialSha256": digest, "practice": True,
                      "staticAssets": True, "database": True, "export": True, "http": True}
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        result["traceback"] = traceback.format_exc()
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if result["ok"] else 1


def launch():
    import tkinter as tk
    from tkinter import ttk
    from waitress.server import create_server
    from app import app, arena_service, get_db
    from arena import server_guard
    from paths import DATA_DIR, RESOURCE_DIR
    from sandbox_runner import IMAGE, availability, docker, remove_stale_containers

    root = tk.Tk()
    root.title("墨弈 · 六子棋")
    root.geometry("520x350")
    root.resizable(False, False)
    root.configure(background="#efeeec")
    messages = queue.Queue()
    state = {"server": None, "url": None, "closing": False, "build": None, "building": False}
    status = tk.StringVar(value="正在启动棋局…")
    arena_status = tk.StringVar(value="正在检查对战环境…")
    tk.Label(root, text="墨弈", font=("KaiTi", 30), bg="#efeeec", fg="#333230").pack(pady=(24, 12))
    tk.Label(root, textvariable=status, font=("Microsoft YaHei", 10), bg="#efeeec", wraplength=465).pack(pady=5)
    tk.Label(root, textvariable=arena_status, font=("Microsoft YaHei", 10), bg="#efeeec", wraplength=465).pack(pady=5)
    controls = ttk.Frame(root)
    controls.pack(pady=15)
    open_button = ttk.Button(controls, text="打开棋局", command=lambda: state["url"] and webbrowser.open(state["url"]))
    open_button.grid(row=0, column=0, padx=6, pady=5)
    open_button.state(["disabled"])

    def check_arena():
        ready, message = availability()
        if not ready and "镜像" in message:
            message = "请点击“准备对战环境”，首次需要下载镜像。"
        messages.put(("arena", "对战环境已就绪 · 每子 10 秒 / 单线程" if ready else message))

    def build_arena():
        if state["building"]:
            return
        state["building"] = True
        build_button.state(["disabled"])
        arena_status.set("正在准备对战镜像，首次需要下载编译环境…")

        def work():
            try:
                info = docker("info", "--format", "{{.OSType}}", timeout=8)
                if info.returncode or info.stdout.strip() != "linux":
                    raise RuntimeError("请先启动 Docker Desktop，并切换到 Linux 容器。")
                DATA_DIR.mkdir(parents=True, exist_ok=True)
                with (DATA_DIR / "docker-build.log").open("w", encoding="utf-8") as log:
                    process = subprocess.Popen(
                        ["docker", "build", "-t", IMAGE, str(RESOURCE_DIR / "sandbox")],
                        stdout=log, stderr=subprocess.STDOUT,
                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                    )
                    state["build"] = process
                    code = process.wait()
                if code:
                    raise RuntimeError("镜像准备失败，可在记录目录查看 docker-build.log。")
                check_arena()
            except Exception as error:
                messages.put(("arena", str(error)))
            finally:
                state["build"] = None
                messages.put(("build-done", None))
        threading.Thread(target=work, daemon=True).start()

    build_button = ttk.Button(controls, text="准备对战环境", command=build_arena)
    build_button.grid(row=0, column=1, padx=6, pady=5)
    ttk.Button(controls, text="安装 Docker", command=lambda: webbrowser.open("https://www.docker.com/products/docker-desktop/")).grid(row=1, column=0, padx=6, pady=5)

    def open_data():
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        os.startfile(str(DATA_DIR))
    ttk.Button(controls, text="打开记录目录", command=open_data).grid(row=1, column=1, padx=6, pady=5)
    tk.Label(root, text="关闭此窗口会停止本机服务，对局记录会保留。", font=("Microsoft YaHei", 9),
             fg="#868583", bg="#efeeec").pack(pady=8)

    def backend():
        server = None
        try:
            with server_guard(DATA_DIR / "arena" / "server.lock"):
                remove_stale_containers()
                arena_service.recover()
                server = create_server(app, host="127.0.0.1", port=0, threads=8,
                                       max_request_body_size=600 * 1024, ident="Connect6")
                state["server"] = server
                url = f"http://127.0.0.1:{server.effective_port}/"
                with get_db() as db:
                    last = db.execute("SELECT id FROM games WHERE status='active' ORDER BY created_at DESC LIMIT 1").fetchone()
                if last:
                    url += "?game=" + last["id"]
                (DATA_DIR / "server.url").write_text(url, encoding="utf-8")
                messages.put(("ready", url))
                if not state["closing"]:
                    server.run()
        except Exception as error:
            messages.put(("error", str(error)))
        finally:
            if server is not None:
                server.close()
                server.task_dispatcher.shutdown()
            arena_service.shutdown()
            messages.put(("closed", None))

    def stop():
        if state["closing"]:
            return
        state["closing"] = True
        status.set("正在停止本机服务…")
        if state["build"] is not None:
            state["build"].terminate()
        if state["server"] is not None:
            state["server"].close()
        if not backend_thread.is_alive():
            root.destroy()

    def drain():
        while not messages.empty():
            kind, value = messages.get_nowait()
            if kind == "ready" and not state["closing"]:
                state["url"] = value
                status.set("棋局已启动")
                open_button.state(["!disabled"])
                webbrowser.open(value)
            elif kind == "arena":
                arena_status.set(value)
            elif kind == "error":
                status.set(value)
            elif kind == "build-done":
                state["building"] = False
                build_button.state(["!disabled"])
            elif kind == "closed" and state["closing"]:
                root.destroy()
                return
        root.after(100, drain)
    backend_thread = threading.Thread(target=backend, daemon=True)
    backend_thread.start()
    threading.Thread(target=check_arena, daemon=True).start()
    root.protocol("WM_DELETE_WINDOW", stop)
    root.after(100, drain)
    root.mainloop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", type=Path, metavar="REPORT")
    arguments = parser.parse_args()
    if arguments.self_test:
        raise SystemExit(self_test(arguments.self_test.resolve()))
    if getattr(sys, "frozen", False):
        from paths import DATA_DIR
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = (DATA_DIR / "desktop.log").open("a", encoding="utf-8", buffering=1)
    launch()
