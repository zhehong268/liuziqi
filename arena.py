"""上传、串行对战队列与持久化裁判。"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import secrets
import threading

from flask import jsonify, request, send_file

from bot import current_player, remaining_in_turn
from agent import local_analysis
from sandbox_runner import BotFault, BotProcess, SandboxError, availability, prepare

BASE = Path(__file__).resolve().parent
OFFICIAL_SOURCE = BASE / "official" / "main.cpp"
OFFICIAL_NAME = "官方 AI · C6 最终版"
MAX_SOURCE = 512 * 1024


@contextmanager
def server_guard(path):
    """只允许一个裁判服务，防止第二次启动清理正在进行的对局。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        if handle.tell() == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError("已有裁判服务运行，请先停止原来的 app.py。") from error
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def build_request(game_id, moves):
    count = len(moves)
    color = current_player(count)
    board = [[0] * 19 for _ in range(19)]
    for move in moves:
        board[move["row"]][move["col"]] = 1 if move["player"] == "black" else 2
    return {"protocol": "arena.v1", "request_id": f"{game_id}-{count + 1}",
            "type": "move_request", "game": "connect6", "rule_version": "connect6-baseline-v1",
            "player": 1 if color == "black" else 2,
            "turn_index": 1 if count == 0 else (count - 1) // 2 + 2, "time_limit_ms": 10000,
            "observation": {"size": 19, "board": board, "current_player": 1 if color == "black" else 2,
                            "moves_in_turn": 0 if count == 0 else (count - 1) % 2,
                            "required_stones_this_turn": 1, "first_move_done": count > 0,
                            "move_number": count, "win_length": 6, "stones_per_turn": 2,
                            "remaining_stones": remaining_in_turn(count),
                            "decision_unit": "single_stone", "available_actions": ["PLACE"]}}


class ArenaService:
    def __init__(self, get_db, get_game, game_payload, place_one, now_iso, storage=None):
        self.db = get_db
        self.get_game = get_game
        self.payload = game_payload
        self.place_one = place_one
        self.now = now_iso
        self.storage = Path(storage) if storage else BASE / "data" / "arena"
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="arena")
        self.lock = threading.Lock()
        self.pending = set()
        self.cancelled = set()
        self.active = {}

    def recover(self):
        # 重启后的进程和棋盘无法继续保持一致，保留已有棋谱并明确标为中断。
        with self.db() as db:
            db.execute("UPDATE games SET status = 'aborted' WHERE id IN "
                       "(SELECT game_id FROM arena_matches WHERE state IN ('queued','preparing','running'))")
            db.execute("UPDATE arena_matches SET state = 'aborted', reason = '服务重启，对局中断', "
                       "finished_at = ?, thinking_player = NULL, thinking_since = NULL "
                       "WHERE state IN ('queued','preparing','running')", (self.now(),))

    def add_submission(self, upload, name):
        suffix = Path(upload.filename or "").suffix.lower()
        language = {".py": "python", ".cpp": "cpp"}.get(suffix)
        if language is None:
            raise ValueError("请上传单文件 .py 或 .cpp 源码")
        content = upload.stream.read(MAX_SOURCE + 1)
        if not content or len(content) > MAX_SOURCE:
            raise ValueError("源码不能为空，且不能超过 512 KB")
        try:
            content.decode("utf-8-sig")
        except UnicodeError as error:
            raise ValueError("源码请保存为 UTF-8 编码") from error
        if b"\x00" in content:
            raise ValueError("请上传文本源码")
        name = name.strip() or Path(upload.filename).stem
        if not 1 <= len(name) <= 40:
            raise ValueError("AI 名称请填写 1～40 个字符")
        submission_id = secrets.token_hex(12)
        directory = self.storage / "submissions" / submission_id
        directory.mkdir(parents=True)
        (directory / ("bot" + suffix)).write_bytes(content)
        (directory / ("bot" + suffix)).chmod(0o644)
        digest = hashlib.sha256(content).hexdigest()
        with self.db() as db:
            db.execute("INSERT INTO submissions (id,name,language,sha256,created_at) VALUES (?,?,?,?,?)",
                       (submission_id, name, language, digest, self.now()))
        return {"id": submission_id, "name": name, "language": language, "sha256": digest}

    def start(self, submission_id, color):
        if color not in ("black", "white"):
            raise ValueError("challengerColor 只能是 black 或 white")
        with self.db() as db:
            submission = db.execute("SELECT * FROM submissions WHERE id = ?", (submission_id,)).fetchone()
        if submission is None:
            raise LookupError("找不到已上传的 AI")
        if not OFFICIAL_SOURCE.is_file():
            raise SandboxError("缺少官方 AI 源码 official/main.cpp")
        official_bytes = OFFICIAL_SOURCE.read_bytes()
        official_hash = hashlib.sha256(official_bytes).hexdigest()
        official_dir = self.storage / "official" / official_hash / "source"
        official_dir.mkdir(parents=True, exist_ok=True)
        official_copy = official_dir / "bot.cpp"
        if not official_copy.exists():
            official_copy.write_bytes(official_bytes)
            official_copy.chmod(0o644)
        game_id = secrets.token_urlsafe(8)
        with self.lock:
            if len(self.pending) >= 8:
                raise SandboxError("队列已有 8 局，请等一局结束后再提交")
            with self.db() as db:
                db.execute("INSERT INTO games (id,status,created_at,opponent) VALUES (?,'active',?,'arena')",
                           (game_id, self.now()))
                db.execute("INSERT INTO arena_matches (game_id,submission_id,challenger_color,official_hash,"
                           "official_name,state,reason) VALUES (?,?,?,?,?,'queued','等待裁判')",
                           (game_id, submission_id, color, official_hash, OFFICIAL_NAME))
            self.pending.add(game_id)
            self.executor.submit(self._run, game_id)
        return game_id

    def cancel(self, game_id):
        with self.lock:
            if game_id in self.pending:
                self.cancelled.add(game_id)
            processes = list(self.active.get(game_id, {}).values())
        with self.db() as db:
            db.execute("UPDATE games SET status = 'aborted', winner = NULL WHERE id = ? AND status = 'active'", (game_id,))
            db.execute("UPDATE arena_matches SET state = 'aborted', reason = '已手动停止', finished_at = ?, "
                       "thinking_player = NULL, thinking_since = NULL "
                       "WHERE game_id = ? AND state IN ('queued','preparing','running')", (self.now(), game_id))
        for process in processes:
            process.close()

    def shutdown(self):
        with self.lock:
            pending = list(self.pending)
        for game_id in pending:
            self.cancel(game_id)
        self.executor.shutdown(wait=True)

    def _state(self, game_id, state, reason, thinking=None):
        with self.db() as db:
            db.execute("UPDATE arena_matches SET state=?,reason=?,thinking_player=?,thinking_since=? "
                       "WHERE game_id=? AND state != 'aborted'",
                       (state, reason, thinking, self.now() if thinking else None, game_id))

    def _finish(self, game_id, reason, loser=None, aborted=False, elapsed=0):
        with self.db() as db:
            match = db.execute("SELECT * FROM arena_matches WHERE game_id=?", (game_id,)).fetchone()
            if match["state"] == "aborted":
                return
            if aborted:
                db.execute("UPDATE games SET status='aborted',winner=NULL WHERE id=?", (game_id,))
            elif loser:
                winner = "white" if loser == "black" else "black"
                db.execute("UPDATE games SET status='finished',winner=? WHERE id=?", (winner, game_id))
            db.execute("UPDATE arena_matches SET state=?,reason=?,loser=?,failure_elapsed_ms=?,finished_at=?,"
                       "thinking_player=NULL,thinking_since=NULL WHERE game_id=?",
                       ("aborted" if aborted else "finished", reason, loser, elapsed, self.now(), game_id))
            payload = self.payload(db, self.get_game(db, game_id))
            analysis = local_analysis(payload["moves"], payload["winner"], payload["status"],
                                      focus_player=match["challenger_color"])
            analysis["summary"] = reason + "。" + analysis["summary"]
            analysis["trace"].insert(0, "读取裁判结果、逐子耗时和挑战方颜色")
            db.execute("INSERT OR REPLACE INTO analyses (game_id,move_count,result_json,created_at) VALUES (?,?,?,?)",
                       (game_id, len(payload["moves"]), json.dumps(analysis, ensure_ascii=False), self.now()))

    def _run(self, game_id):
        processes = {}
        try:
            if game_id in self.cancelled:
                return
            self._state(game_id, "preparing", "正在准备双方 AI")
            with self.lock:
                self.active[game_id] = processes
            with self.db() as db:
                match = dict(db.execute("SELECT a.*,s.language,s.id AS source_id FROM arena_matches a "
                                        "JOIN submissions s ON s.id=a.submission_id WHERE game_id=?", (game_id,)).fetchone())
            official_dir = self.storage / "official" / match["official_hash"]
            official = prepare(official_dir / "source" / "bot.cpp", "cpp", official_dir / "build")
            directory = self.storage / "submissions" / match["source_id"]
            extension = ".py" if match["language"] == "python" else ".cpp"
            try:
                challenger = prepare(directory / ("bot" + extension), match["language"], directory / "build")
            except SandboxError as error:
                self._finish(game_id, str(error), loser=match["challenger_color"])
                return
            for color in ("black", "white"):
                if game_id in self.cancelled:
                    return
                processes[color] = BotProcess(challenger if color == match["challenger_color"] else official)
            self._state(game_id, "running", "双方 AI 已就绪")
            while game_id not in self.cancelled:
                with self.db() as db:
                    snapshot = self.payload(db, self.get_game(db, game_id))
                if snapshot["status"] != "active":
                    return
                color = snapshot["currentPlayer"]
                self._state(game_id, "running", "等待 AI 落子", color)
                try:
                    action, elapsed = processes[color].ask(build_request(game_id, snapshot["moves"]))
                    if game_id in self.cancelled:
                        return
                    with self.db() as db:
                        db.execute("BEGIN IMMEDIATE")
                        if self.get_game(db, game_id)["status"] != "active":
                            return
                        self.place_one(db, game_id, action["row"], action["col"], color,
                                       "challenger" if color == match["challenger_color"] else "official",
                                       f"思考 {elapsed} ms", elapsed_ms=elapsed)
                        result = self.get_game(db, game_id)
                        done = result["status"] != "active"
                        winner = result["winner"]
                except BotFault as error:
                    self._finish(game_id, str(error), loser=color, elapsed=error.elapsed_ms)
                    return
                except ValueError as error:
                    self._finish(game_id, "非法落子：" + str(error), loser=color, elapsed=elapsed)
                    return
                if done:
                    self._finish(game_id, ("黑方" if winner == "black" else "白方") + "连成六子获胜" if winner else "棋盘已满，和棋")
                    return
        except (SandboxError, OSError) as error:
            self._finish(game_id, "裁判运行失败：" + str(error)[:3000], aborted=True)
        except Exception:
            # 保留服务端堆栈，不把服务器信息通过页面泄漏给上传者。
            import logging
            logging.exception("arena match failed: %s", game_id)
            self._finish(game_id, "裁判内部错误，请检查服务端日志", aborted=True)
        finally:
            for process in processes.values():
                process.close()
            with self.lock:
                self.pending.discard(game_id)
                self.cancelled.discard(game_id)
                self.active.pop(game_id, None)


def register_routes(app, service):
    @app.get("/api/arena")
    def arena_status():
        ready, reason = availability()
        source_exists = OFFICIAL_SOURCE.is_file()
        return jsonify({"ready": ready and source_exists, "message": reason if source_exists else "缺少官方 AI 源码",
                        "officialName": OFFICIAL_NAME,
                        "officialHash": hashlib.sha256(OFFICIAL_SOURCE.read_bytes()).hexdigest() if source_exists else None,
                        "timeLimitMs": 10000, "threads": 1, "languages": ["python", "cpp"]})

    @app.get("/api/arena/example")
    def arena_example():
        return send_file(BASE / "bot.py", as_attachment=True, download_name="connect6_example.py")

    @app.get("/api/submissions")
    def submissions():
        with service.db() as db:
            rows = db.execute("SELECT * FROM submissions ORDER BY created_at DESC LIMIT 50").fetchall()
        return jsonify({"submissions": [dict(row) for row in rows]})

    @app.post("/api/submissions")
    def upload_ai():
        upload = request.files.get("file")
        if upload is None:
            return jsonify({"error": "请选择 AI 源码文件"}), 400
        try:
            result = service.add_submission(upload, request.form.get("name", ""))
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        return jsonify(result), 201

    @app.post("/api/arena/matches")
    def start_match():
        data = request.get_json(silent=True)
        if not isinstance(data, dict) or not isinstance(data.get("submissionId"), str):
            return jsonify({"error": "请先上传 AI，并提供 submissionId"}), 400
        ready, reason = availability()
        if not ready:
            return jsonify({"error": reason}), 503
        try:
            game_id = service.start(data["submissionId"], data.get("challengerColor", "black"))
        except LookupError as error:
            return jsonify({"error": str(error)}), 404
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        except SandboxError as error:
            return jsonify({"error": str(error)}), 409
        with service.db() as db:
            return jsonify(service.payload(db, service.get_game(db, game_id))), 202

    @app.post("/api/arena/matches/<game_id>/cancel")
    def cancel_match(game_id):
        with service.db() as db:
            game = service.get_game(db, game_id)
            if game is None or game["opponent"] != "arena":
                return jsonify({"error": "找不到挑战对局"}), 404
            if game["status"] != "active":
                return jsonify(service.payload(db, game))
        service.cancel(game_id)
        with service.db() as db:
            return jsonify(service.payload(db, service.get_game(db, game_id)))
