"""六子棋规则、自己的简单 AI，以及平台 arena.v1 入口。只用 Python 标准库。"""
from __future__ import annotations

import json
import sys

BOARD_SIZE = 19
WIN_LENGTH = 6
DIRECTIONS = ((1, 0), (0, 1), (1, 1), (1, -1))


def current_player(move_count: int) -> str:
    if move_count == 0:
        return "black"
    return "white" if ((move_count - 1) // 2) % 2 == 0 else "black"


def remaining_in_turn(move_count: int) -> int:
    return 1 if move_count == 0 else 2 - ((move_count - 1) % 2)


def line_length(board, row, col, player, dr, dc) -> int:
    length = 1
    for sign in (1, -1):
        r, c = row + dr * sign, col + dc * sign
        while 0 <= r < BOARD_SIZE and 0 <= c < BOARD_SIZE and board[r][c] == player:
            length += 1
            r += dr * sign
            c += dc * sign
    return length


def has_won(board, row, col, player) -> bool:
    return any(line_length(board, row, col, player, dr, dc) >= WIN_LENGTH
               for dr, dc in DIRECTIONS)


def legal_cells(board) -> list[tuple[int, int]]:
    return [(r, c) for r in range(BOARD_SIZE) for c in range(BOARD_SIZE)
            if board[r][c] is None]


def winning_cells(board, player) -> list[tuple[int, int]]:
    result = []
    for row, col in legal_cells(board):
        board[row][col] = player
        if has_won(board, row, col, player):
            result.append((row, col))
        board[row][col] = None
    return result


def choose_ai_move(board, player="white") -> tuple[int, int]:
    """先直接获胜，再封堵直接威胁，否则延长棋形、接近中心。无搜索强度承诺。"""
    cells = legal_cells(board)
    if not cells:
        raise ValueError("board is full")
    opponent = "black" if player == "white" else "white"
    for color in (player, opponent):
        wins = winning_cells(board, color)
        if wins:
            return wins[0]

    def score(cell):
        row, col = cell
        board[row][col] = player
        own = max(line_length(board, row, col, player, dr, dc) for dr, dc in DIRECTIONS)
        board[row][col] = opponent
        enemy = max(line_length(board, row, col, opponent, dr, dc) for dr, dc in DIRECTIONS)
        board[row][col] = None
        neighbors = sum(1 for dr in (-1, 0, 1) for dc in (-1, 0, 1)
                        if (dr or dc) and 0 <= row + dr < BOARD_SIZE
                        and 0 <= col + dc < BOARD_SIZE and board[row + dr][col + dc])
        return own * own * 5 + enemy * enemy * 4 + neighbors, -abs(row - 9) - abs(col - 9)

    return max(cells, key=score)


def decide(message: dict) -> dict:
    """平台每次调用只返回一颗子；同回合第二颗仍由裁判再次请求。"""
    if message.get("protocol") != "arena.v1" or message.get("type") != "move_request":
        raise ValueError("expected arena.v1 move_request")
    if message.get("game") != "connect6":
        raise ValueError("this bot supports connect6 only")
    observation = message["observation"]
    raw = observation["board"]
    if len(raw) != 19 or any(len(row) != 19 for row in raw):
        raise ValueError("expected 19x19 board")
    colors = {0: None, 1: "black", 2: "white"}
    board = [[colors[value] for value in row] for row in raw]
    color = observation["current_player"]
    if color not in (1, 2):
        raise ValueError("invalid player")
    row, col = choose_ai_move(board, colors[color])
    return {"kind": "PLACE", "row": row, "col": col}


def main() -> None:
    # 常驻进程：处理多行请求；stdout 只含协议响应，日志只能写 stderr。
    for raw in sys.stdin:
        if not raw.strip():
            continue
        try:
            message = json.loads(raw)
            action = decide(message)
            print(json.dumps({"protocol": "arena.v1", "request_id": message["request_id"],
                              "type": "move_response", "action": action},
                             separators=(",", ":")), flush=True)
        except (ValueError, KeyError, TypeError) as error:
            print(f"Invalid request: {error}", file=sys.stderr, flush=True)
            raise SystemExit(1)


if __name__ == "__main__":
    main()
