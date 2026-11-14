from __future__ import annotations

import json
import os
import secrets
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, jsonify, make_response, request, send_from_directory

from bot import BOARD_SIZE, current_player, has_won, legal_cells, remaining_in_turn
from agent import analyze_game, choose_agent_move, model_configured

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "data" / "connect6.db"
app = Flask(__name__, static_folder="static")
app.json.ensure_ascii = False
app.config["MAX_CONTENT_LENGTH"] = 600 * 1024


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


@contextmanager
def get_db():
    DB_PATH.parent.mkdir(exist_ok=True, parents=True)
    connection = sqlite3.connect(DB_PATH, timeout=10)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    try:
        with connection:
            yield connection
    finally:
        connection.close()


def init_db() -> None:
    with get_db() as db:
        db.executescript("""
            CREATE TABLE IF NOT EXISTS games (
                id TEXT PRIMARY KEY, status TEXT NOT NULL DEFAULT 'active',
                winner TEXT, created_at TEXT NOT NULL,
                opponent TEXT NOT NULL DEFAULT 'local'
            );
            CREATE TABLE IF NOT EXISTS moves (
                id INTEGER PRIMARY KEY AUTOINCREMENT, game_id TEXT NOT NULL,
                move_no INTEGER NOT NULL,
                player TEXT NOT NULL CHECK (player IN ('black', 'white')),
                row INTEGER NOT NULL CHECK (row BETWEEN 0 AND 18),
                col INTEGER NOT NULL CHECK (col BETWEEN 0 AND 18),
                created_at TEXT NOT NULL, source TEXT NOT NULL DEFAULT 'unknown',
                reason TEXT NOT NULL DEFAULT '',
                FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE,
                UNIQUE (game_id, row, col), UNIQUE (game_id, move_no)
            );
            CREATE TABLE IF NOT EXISTS analyses (
                game_id TEXT NOT NULL, move_count INTEGER NOT NULL,
                result_json TEXT NOT NULL, created_at TEXT NOT NULL,
                PRIMARY KEY (game_id, move_count),
                FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS submissions (
                id TEXT PRIMARY KEY, name TEXT NOT NULL, language TEXT NOT NULL,
                sha256 TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS arena_matches (
                game_id TEXT PRIMARY KEY REFERENCES games(id),
                submission_id TEXT NOT NULL REFERENCES submissions(id),
                challenger_color TEXT NOT NULL, official_hash TEXT NOT NULL, official_name TEXT NOT NULL,
                state TEXT NOT NULL, reason TEXT NOT NULL DEFAULT '', loser TEXT,
                failure_elapsed_ms INTEGER NOT NULL DEFAULT 0,
                thinking_player TEXT, thinking_since TEXT, finished_at TEXT
            );
        """)
        # 无损兼容旧版数据库，不删除已有棋局。
        for table, column, definition in (
            ("games", "opponent", "TEXT NOT NULL DEFAULT 'local'"),
            ("games", "human_color", "TEXT NOT NULL DEFAULT 'black'"),
            ("moves", "source", "TEXT NOT NULL DEFAULT 'unknown'"),
            ("moves", "reason", "TEXT NOT NULL DEFAULT ''"),
            ("moves", "elapsed_ms", "INTEGER"),
        ):
            columns = {row["name"] for row in db.execute(f"PRAGMA table_info({table})")}
            if column not in columns:
                db.execute(f"ALTER TABLE {table} ADD COLUMN {column} {definition}")


def get_game(db, game_id):
    return db.execute("SELECT * FROM games WHERE id = ?", (game_id,)).fetchone()


def get_moves(db, game_id):
    return db.execute("SELECT * FROM moves WHERE game_id = ? ORDER BY move_no", (game_id,)).fetchall()


def board_from_moves(moves):
    board = [[None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)]
    for move in moves:
        board[move["row"]][move["col"]] = move["player"]
    return board


def game_payload(db, game):
    moves = get_moves(db, game["id"])
    analysis = db.execute("SELECT result_json FROM analyses WHERE game_id = ? AND move_count = ?",
                          (game["id"], len(moves))).fetchone()
    arena = None
    if game["opponent"] == "arena":
        row = db.execute("SELECT a.*,s.name AS challenger_name FROM arena_matches a "
                         "JOIN submissions s ON s.id=a.submission_id WHERE a.game_id=?", (game["id"],)).fetchone()
        if row:
            arena = {"state": row["state"], "reason": row["reason"], "loser": row["loser"],
                     "challengerName": row["challenger_name"], "challengerColor": row["challenger_color"],
                     "officialName": row["official_name"], "officialHash": row["official_hash"],
                     "submissionId": row["submission_id"], "thinkingPlayer": row["thinking_player"],
                     "thinkingSince": row["thinking_since"], "failureElapsedMs": row["failure_elapsed_ms"],
                     "timeLimitMs": 10000, "threads": 1}
    return {
        "id": game["id"], "status": game["status"], "winner": game["winner"],
        "opponent": game["opponent"], "createdAt": game["created_at"], "arena": arena,
        "humanColor": game["human_color"] if not arena else None,
        "aiColor": ("white" if game["human_color"] == "black" else "black") if not arena else None,
        "currentPlayer": current_player(len(moves)) if game["status"] == "active" else None,
        "remainingStones": remaining_in_turn(len(moves)) if game["status"] == "active" else 0,
        "board": board_from_moves(moves),
        "moves": [{"moveNo": m["move_no"], "player": m["player"], "row": m["row"],
                   "col": m["col"], "source": m["source"], "reason": m["reason"],
                   "elapsedMs": m["elapsed_ms"]} for m in moves],
        "analysis": json.loads(analysis["result_json"]) if analysis else None,
    }


def place_one(db, game_id, row, col, player, source="human", reason="", elapsed_ms=None):
    game = get_game(db, game_id)
    if game is None:
        raise LookupError("找不到棋局")
    moves = get_moves(db, game_id)
    if game["status"] != "active":
        raise ValueError("本局已经结束")
    if player != current_player(len(moves)):
        raise ValueError("还没有轮到你落子")
    if type(row) is not int or type(col) is not int or not (0 <= row < 19 and 0 <= col < 19):
        raise ValueError("row 和 col 必须是 0～18 的整数")
    board = board_from_moves(moves)
    if board[row][col] is not None:
        raise ValueError("这里已经有棋子了")
    db.execute("""INSERT INTO moves (game_id, move_no, player, row, col, created_at, source, reason, elapsed_ms)
                  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
               (game_id, len(moves) + 1, player, row, col, now_iso(), source, reason, elapsed_ms))
    board[row][col] = player
    if has_won(board, row, col, player):
        db.execute("UPDATE games SET status = 'finished', winner = ? WHERE id = ?", (player, game_id))
    elif not legal_cells(board):
        db.execute("UPDATE games SET status = 'draw', winner = NULL WHERE id = ?", (game_id,))


@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.errorhandler(413)
def upload_too_large(error):
    return jsonify({"error": "文件过大，源码不能超过 512 KB"}), 413


@app.get("/api/health")
def health():
    return jsonify({"ok": True, "service": "connect6-demo", "modelConfigured": model_configured()})


@app.get("/api/games")
def list_games():
    with get_db() as db:
        rows = db.execute("""SELECT g.*, (SELECT COUNT(*) FROM moves m WHERE m.game_id = g.id) AS move_count
                             FROM games g ORDER BY created_at DESC LIMIT 50""").fetchall()
        return jsonify({"games": [{"id": g["id"], "status": g["status"], "winner": g["winner"],
                                   "opponent": g["opponent"], "createdAt": g["created_at"],
                                   "humanColor": g["human_color"] if g["opponent"] != "arena" else None,
                                   "moveCount": g["move_count"]} for g in rows]})


@app.post("/api/games")
def create_game():
    data = request.get_json(silent=True)
    if data is None:
        data = {}
    if not isinstance(data, dict) or data.get("opponent", "local") not in ("local", "model"):
        return jsonify({"error": "opponent 只能是 local 或 model"}), 400
    opponent = data.get("opponent", "local")
    human_color = data.get("humanColor", "black")
    if human_color not in ("black", "white"):
        return jsonify({"error": "humanColor 只能是 black 或 white"}), 400
    if opponent == "model" and not model_configured():
        return jsonify({"error": "请先在 .env 配置 API Key 和模型名，再重启后端"}), 400
    game_id = secrets.token_urlsafe(8)
    with get_db() as db:
        db.execute("INSERT INTO games (id, status, created_at, opponent, human_color) VALUES (?, 'active', ?, ?, ?)",
                   (game_id, now_iso(), opponent, human_color))
        return jsonify(game_payload(db, get_game(db, game_id))), 201


@app.get("/api/games/<game_id>")
def read_game(game_id):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "找不到棋局"}), 404
        return jsonify(game_payload(db, game))


@app.post("/api/games/<game_id>/moves")
def create_move(game_id):
    data = request.get_json(silent=True)
    if not isinstance(data, dict) or type(data.get("row")) is not int or type(data.get("col")) is not int:
        return jsonify({"error": "请提供整数 row、col"}), 400
    with get_db() as db:
        db.execute("BEGIN IMMEDIATE")
        game = get_game(db, game_id)
        if game is not None and game["opponent"] == "arena":
            return jsonify({"error": "挑战对局由裁判落子，不能手动修改"}), 409
        try:
            place_one(db, game_id, data["row"], data["col"], game["human_color"] if game else "black")
        except LookupError as error:
            return jsonify({"error": str(error)}), 404
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        return jsonify(game_payload(db, get_game(db, game_id)))


@app.post("/api/games/<game_id>/ai-turn")
def create_ai_turn(game_id):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "找不到棋局"}), 404
        if game["opponent"] == "arena":
            return jsonify({"error": "挑战对局由后台裁判运行"}), 409
        snapshot = game_payload(db, game)
    if snapshot["status"] != "active":
        return jsonify(snapshot)
    ai_color = snapshot["aiColor"]
    if snapshot["currentPlayer"] != ai_color:
        return jsonify({"error": "当前不是 AI 的回合"}), 400

    # 网络请求在数据库事务外完成，避免等待模型时锁住 SQLite。
    board = snapshot["board"]
    decisions = []
    for _ in range(snapshot["remainingStones"]):
        decision = choose_agent_move(board, ai_color, snapshot["opponent"])
        decisions.append(decision)
        row, col = decision["row"], decision["col"]
        board[row][col] = ai_color
        if has_won(board, row, col, ai_color) or not legal_cells(board):
            break
    with get_db() as db:
        db.execute("BEGIN IMMEDIATE")
        if len(get_moves(db, game_id)) != len(snapshot["moves"]):
            return jsonify({"error": "棋局已改变，请刷新后继续"}), 409
        for decision in decisions:
            place_one(db, game_id, decision["row"], decision["col"], ai_color,
                      decision["source"], decision["reason"])
        return jsonify(game_payload(db, get_game(db, game_id)))


@app.post("/api/games/<game_id>/analysis")
def analyze_game_route(game_id):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "找不到棋局"}), 404
        snapshot = game_payload(db, game)
    # 同一棋谱复用已保存结果，刷新不反复调用收费 API。
    if snapshot["arena"] and snapshot["arena"]["state"] not in ("finished", "aborted"):
        return jsonify({"error": "挑战结束后会自动生成复盘"}), 409
    focus_player = snapshot["arena"]["challengerColor"] if snapshot["arena"] else snapshot["humanColor"]
    analysis = snapshot["analysis"] or analyze_game(snapshot["moves"], snapshot["winner"], snapshot["status"],
                                                   focus_player=focus_player)
    with get_db() as db:
        db.execute("""INSERT INTO analyses (game_id, move_count, result_json, created_at) VALUES (?, ?, ?, ?)
                      ON CONFLICT(game_id, move_count) DO UPDATE SET result_json = excluded.result_json,
                      created_at = excluded.created_at""",
                   (game_id, len(snapshot["moves"]), json.dumps(analysis, ensure_ascii=False), now_iso()))
    return jsonify({"gameId": game_id, "moveCount": len(snapshot["moves"]), "analysis": analysis})


@app.get("/api/games/<game_id>/export")
def export_game(game_id):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "找不到棋局"}), 404
        payload = game_payload(db, game)
    export_format = request.args.get("format", "txt")
    if export_format not in ("txt", "json"):
        return jsonify({"error": "导出格式只能是 txt 或 json"}), 400
    if export_format == "json":
        payload.update({"game": "connect6", "coordinateBase": 0, "size": 19,
                        "rule": "black first 1 stone; then 2 stones per turn; >=6 wins"})
        content, mime = json.dumps(payload, ensure_ascii=False, indent=2), "application/json"
    else:
        lines = ["Connect6 Demo Game", f"Game ID: {game_id}",
                 f"Result: {payload['status']} / {payload['winner'] or '-'}",
                 "Rule: 19x19, black first 1 stone, then 2 stones per turn, >=6 wins.",
                 "Coordinates: (row, col), 1-based.", ""]
        if payload["arena"]:
            match = payload["arena"]
            lines += [f"Challenger: {match['challengerName']} ({match['challengerColor']})",
                      f"Official: {match['officialName']} / {match['officialHash']}",
                      "Limits: 10000 ms per stone / 1 thread", f"Reason: {match['reason']}", ""]
        else:
            lines += [f"Human: {payload['humanColor']} / AI: {payload['aiColor']}", ""]
        lines += [f"{m['moveNo']:03d}. {m['player']:5s} ({m['row'] + 1},{m['col'] + 1})"
                  + (f" [{m['elapsedMs']} ms]" if m['elapsedMs'] is not None else "")
                  for m in payload["moves"]]
        content, mime = "\n".join(lines) + "\n", "text/plain"
    response = make_response(content)
    response.headers["Content-Type"] = f"{mime}; charset=utf-8"
    response.headers["Content-Disposition"] = f'attachment; filename="connect6-{game_id}.{export_format}"'
    return response


init_db()

from arena import ArenaService, register_routes, server_guard

arena_service = ArenaService(get_db, get_game, game_payload, place_one, now_iso)
register_routes(app, arena_service)

if __name__ == "__main__":
    from sandbox_runner import remove_stale_containers

    with server_guard(BASE_DIR / "data" / "arena" / "server.lock"):
        remove_stale_containers()
        arena_service.recover()
        try:
            app.run(host=os.getenv("APP_HOST", "127.0.0.1"), port=int(os.getenv("APP_PORT", "5000")), debug=False)
        finally:
            arena_service.shutdown()
