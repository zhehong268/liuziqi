from __future__ import annotations

import json
import secrets
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, jsonify, make_response, request, send_from_directory


BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "connect6.db"
BOARD_SIZE = 19
WIN_LENGTH = 6

app = Flask(__name__, static_folder="static")


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def get_db() -> sqlite3.Connection:
    DATA_DIR.mkdir(exist_ok=True)
    connection = sqlite3.connect(DB_PATH)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    return connection


def init_db() -> None:
    with get_db() as db:
        db.executescript(
            """
            CREATE TABLE IF NOT EXISTS games (
                id TEXT PRIMARY KEY,
                status TEXT NOT NULL DEFAULT 'active',
                winner TEXT,
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS moves (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                game_id TEXT NOT NULL,
                move_no INTEGER NOT NULL,
                player TEXT NOT NULL CHECK (player IN ('black', 'white')),
                row INTEGER NOT NULL CHECK (row BETWEEN 0 AND 18),
                col INTEGER NOT NULL CHECK (col BETWEEN 0 AND 18),
                created_at TEXT NOT NULL,
                FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE,
                UNIQUE (game_id, row, col),
                UNIQUE (game_id, move_no)
            );
            """
        )


def get_game(db: sqlite3.Connection, game_id: str) -> sqlite3.Row | None:
    return db.execute("SELECT * FROM games WHERE id = ?", (game_id,)).fetchone()


def get_moves(db: sqlite3.Connection, game_id: str) -> list[sqlite3.Row]:
    return db.execute(
        "SELECT * FROM moves WHERE game_id = ? ORDER BY move_no", (game_id,)
    ).fetchall()


def board_from_moves(moves: list[sqlite3.Row]) -> list[list[str | None]]:
    board: list[list[str | None]] = [
        [None for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)
    ]
    for move in moves:
        board[move["row"]][move["col"]] = move["player"]
    return board


def current_player(move_count: int) -> str:
    """Return whose turn it is before the next single-stone request."""
    if move_count == 0:
        return "black"
    pair_index = (move_count - 1) // 2
    return "white" if pair_index % 2 == 0 else "black"


def remaining_in_turn(move_count: int) -> int:
    if move_count == 0:
        return 1
    return 2 - ((move_count - 1) % 2)


def line_length(
    board: list[list[str | None]], row: int, col: int, player: str, dr: int, dc: int
) -> int:
    length = 1
    for direction in (1, -1):
        r, c = row + dr * direction, col + dc * direction
        while 0 <= r < BOARD_SIZE and 0 <= c < BOARD_SIZE and board[r][c] == player:
            length += 1
            r += dr * direction
            c += dc * direction
    return length


def has_won(board: list[list[str | None]], row: int, col: int, player: str) -> bool:
    return any(
        line_length(board, row, col, player, dr, dc) >= WIN_LENGTH
        for dr, dc in ((1, 0), (0, 1), (1, 1), (1, -1))
    )


def legal_cells(board: list[list[str | None]]) -> list[tuple[int, int]]:
    return [
        (row, col)
        for row in range(BOARD_SIZE)
        for col in range(BOARD_SIZE)
        if board[row][col] is None
    ]


def choose_ai_move(board: list[list[str | None]], player: str = "white") -> tuple[int, int]:
    """A deterministic teaching AI: win, block, then prefer the center and neighbors."""
    opponent = "black" if player == "white" else "white"
    cells = legal_cells(board)
    if not cells:
        raise ValueError("board is full")

    def winning_cell(candidate_player: str) -> tuple[int, int] | None:
        for row, col in cells:
            board[row][col] = candidate_player
            won = has_won(board, row, col, candidate_player)
            board[row][col] = None
            if won:
                return row, col
        return None

    immediate_win = winning_cell(player)
    if immediate_win:
        return immediate_win
    immediate_block = winning_cell(opponent)
    if immediate_block:
        return immediate_block

    occupied = [
        (row, col)
        for row in range(BOARD_SIZE)
        for col in range(BOARD_SIZE)
        if board[row][col] is not None
    ]
    center = (BOARD_SIZE - 1) / 2

    def score(cell: tuple[int, int]) -> tuple[int, float, float]:
        row, col = cell
        neighbors = sum(
            1
            for dr in (-1, 0, 1)
            for dc in (-1, 0, 1)
            if (dr or dc)
            and 0 <= row + dr < BOARD_SIZE
            and 0 <= col + dc < BOARD_SIZE
            and board[row + dr][col + dc] is not None
        )
        distance = abs(row - center) + abs(col - center)
        occupied_distance = min(
            (abs(row - r) + abs(col - c) for r, c in occupied), default=0
        )
        return neighbors, -distance, -occupied_distance

    return max(cells, key=score)


def game_payload(db: sqlite3.Connection, game: sqlite3.Row) -> dict:
    moves = get_moves(db, game["id"])
    board = board_from_moves(moves)
    move_count = len(moves)
    return {
        "id": game["id"],
        "status": game["status"],
        "winner": game["winner"],
        "currentPlayer": None if game["status"] != "active" else current_player(move_count),
        "remainingStones": 0
        if game["status"] != "active"
        else remaining_in_turn(move_count),
        "board": board,
        "moves": [
            {
                "moveNo": move["move_no"],
                "player": move["player"],
                "row": move["row"],
                "col": move["col"],
            }
            for move in moves
        ],
    }


def place_one(db: sqlite3.Connection, game_id: str, row: int, col: int, player: str) -> dict:
    game = get_game(db, game_id)
    if game is None:
        raise LookupError("game not found")
    moves = get_moves(db, game_id)
    if game["status"] != "active":
        raise ValueError("game is already over")
    if player != current_player(len(moves)):
        raise ValueError(f"it is {current_player(len(moves))}'s turn")
    if not (0 <= row < BOARD_SIZE and 0 <= col < BOARD_SIZE):
        raise ValueError("row and col must be between 0 and 18")
    board = board_from_moves(moves)
    if board[row][col] is not None:
        raise ValueError("cell is already occupied")

    move_no = len(moves) + 1
    db.execute(
        "INSERT INTO moves (game_id, move_no, player, row, col, created_at) VALUES (?, ?, ?, ?, ?, ?)",
        (game_id, move_no, player, row, col, now_iso()),
    )
    board[row][col] = player
    if has_won(board, row, col, player):
        db.execute("UPDATE games SET status = 'finished', winner = ? WHERE id = ?", (player, game_id))
    elif not legal_cells(board):
        db.execute("UPDATE games SET status = 'draw', winner = NULL WHERE id = ?", (game_id,))
    return game_payload(db, get_game(db, game_id))


@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.get("/api/health")
def health():
    return jsonify({"ok": True, "service": "connect6-demo"})


@app.post("/api/games")
def create_game():
    game_id = secrets.token_urlsafe(8)
    with get_db() as db:
        db.execute(
            "INSERT INTO games (id, status, created_at) VALUES (?, 'active', ?)",
            (game_id, now_iso()),
        )
        return jsonify(game_payload(db, get_game(db, game_id))), 201


@app.get("/api/games/<game_id>")
def read_game(game_id: str):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "game not found"}), 404
        return jsonify(game_payload(db, game))


@app.post("/api/games/<game_id>/moves")
def create_move(game_id: str):
    data = request.get_json(silent=True) or {}
    try:
        row = int(data["row"])
        col = int(data["col"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "row and col are required integers"}), 400

    with get_db() as db:
        try:
            payload = place_one(db, game_id, row, col, "black")
        except LookupError as error:
            return jsonify({"error": str(error)}), 404
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        return jsonify(payload)


@app.post("/api/games/<game_id>/ai-turn")
def create_ai_turn(game_id: str):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "game not found"}), 404
        moves = get_moves(db, game_id)
        if game["status"] != "active":
            return jsonify(game_payload(db, game))
        if current_player(len(moves)) != "white":
            return jsonify({"error": "AI can move only for white"}), 400

        for _ in range(remaining_in_turn(len(moves))):
            board = board_from_moves(get_moves(db, game_id))
            row, col = choose_ai_move(board, "white")
            place_one(db, game_id, row, col, "white")
            game = get_game(db, game_id)
            if game["status"] != "active":
                break
        return jsonify(game_payload(db, game))


@app.get("/api/games/<game_id>/export")
def export_game(game_id: str):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "game not found"}), 404
        moves = get_moves(db, game_id)
        lines = [
            "Connect6 Demo Game",
            f"Game ID: {game_id}",
            "Rule: 19x19, black first with 1 stone, then 2 stones per turn, six in a row wins.",
            "",
        ]
        for move in moves:
            lines.append(
                f"{move['move_no']:03d}. {move['player']:5s} ({move['row'] + 1},{move['col'] + 1})"
            )
        response = make_response("\n".join(lines) + "\n")
        response.headers["Content-Type"] = "text/plain; charset=utf-8"
        response.headers["Content-Disposition"] = f'attachment; filename="connect6-{game_id}.txt"'
        return response


@app.post("/api/games/<game_id>/analysis")
def analyze_game_route(game_id: str):
    with get_db() as db:
        game = get_game(db, game_id)
        if game is None:
            return jsonify({"error": "game not found"}), 404
        moves = get_moves(db, game_id)
        move_payload = [
            {
                "moveNo": move["move_no"],
                "player": move["player"],
                "row": move["row"],
                "col": move["col"],
            }
            for move in moves
        ]
        try:
            from agent import analyze_game

            analysis = analyze_game(move_payload, game["winner"])
        except (RuntimeError, ValueError) as error:
            return jsonify({"error": str(error)}), 503
        except Exception as error:  # keep provider errors out of the browser stack trace
            return jsonify({"error": f"AI provider request failed: {error}"}), 502
        return jsonify({"gameId": game_id, "analysis": analysis})


if __name__ == "__main__":
    init_db()
    app.run(debug=True, port=5000)
