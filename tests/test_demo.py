"""运行：python -m unittest discover -s tests -v。使用临时数据库，不改演示棋谱。"""
import json
import os
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import agent
import app as web
import bot


def empty_board():
    return [[None] * 19 for _ in range(19)]


class RuleTests(unittest.TestCase):
    def test_first_and_double_stone_turns(self):
        self.assertEqual([bot.current_player(i) for i in range(9)],
                         ["black", "white", "white", "black", "black", "white", "white", "black", "black"])
        self.assertEqual([bot.remaining_in_turn(i) for i in range(6)], [1, 2, 1, 2, 1, 2])

    def test_six_and_overline_all_directions(self):
        for dr, dc, start in [(1, 0, (0, 0)), (0, 1, (18, 0)), (1, 1, (0, 0)), (1, -1, (0, 18))]:
            with self.subTest(direction=(dr, dc)):
                board = empty_board()
                for count in range(1, 8):
                    row, col = start[0] + (count - 1) * dr, start[1] + (count - 1) * dc
                    board[row][col] = "black"
                    self.assertEqual(bot.has_won(board, row, col, "black"), count >= 6)

    def test_ai_wins_and_blocks_without_mutating_board(self):
        for player in ("black", "white"):
            board = empty_board()
            for col in range(5):
                board[0][col] = player
            before = [row[:] for row in board]
            self.assertEqual(bot.choose_ai_move(board, "white"), (0, 5))
            self.assertEqual(board, before)

    def test_bot_can_play_complete_game(self):
        board = empty_board()
        ended = False
        for count in range(361):
            player = bot.current_player(count)
            row, col = bot.choose_ai_move(board, player)
            self.assertIsNone(board[row][col])
            board[row][col] = player
            if bot.has_won(board, row, col, player) or count == 360:
                ended = True
                break
        self.assertTrue(ended)

    def test_platform_multiple_requests_in_one_process(self):
        messages = []
        for player in (1, 2):
            messages.append({"protocol": "arena.v1", "request_id": f"case-{player}",
                             "type": "move_request", "game": "connect6", "player": player,
                             "observation": {"board": [[0] * 19 for _ in range(19)], "current_player": player}})
        result = subprocess.run([sys.executable, "-u", "bot.py"],
                                input="\n".join(json.dumps(m) for m in messages) + "\n",
                                text=True, capture_output=True, timeout=10,
                                cwd=Path(__file__).resolve().parents[1])
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = result.stdout.splitlines()
        self.assertEqual(len(replies), 2)
        for message, raw in zip(messages, replies):
            reply = json.loads(raw)
            self.assertEqual(reply["request_id"], message["request_id"])
            self.assertEqual(reply["type"], "move_response")
            self.assertEqual(reply["protocol"], "arena.v1")
            self.assertEqual(reply["action"], {"kind": "PLACE", "row": 9, "col": 9})
            self.assertLess(len(raw.encode()), 4096)


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.original_db = web.DB_PATH
        web.DB_PATH = Path(self.temp.name) / "test.db"
        self.env = patch.dict(os.environ, {"OPENAI_API_KEY": "", "OPENAI_MODEL": ""})
        self.env.start()
        web.init_db()
        self.client = web.app.test_client()
        self.game = self.client.post("/api/games", json={}).get_json()
        self.url = "/api/games/" + self.game["id"]

    def tearDown(self):
        self.env.stop()
        web.DB_PATH = self.original_db
        self.temp.cleanup()

    def test_play_reload_history_export_and_saved_analysis(self):
        response = self.client.post(self.url + "/moves", json={"row": 9, "col": 9})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_json()["currentPlayer"], "white")
        response = self.client.post(self.url + "/ai-turn")
        state = response.get_json()
        self.assertEqual(len(state["moves"]), 3)
        self.assertEqual(state["currentPlayer"], "black")
        self.assertEqual(state["remainingStones"], 2)
        # 新建客户端、重新初始化 schema 后仍能读取同一局。
        web.init_db()
        self.assertEqual(web.app.test_client().get(self.url).get_json(), state)
        self.assertEqual(self.client.get("/api/games").get_json()["games"][0]["moveCount"], 3)
        txt = self.client.get(self.url + "/export")
        self.assertIn("001. black (10,10)", txt.get_data(as_text=True))
        self.assertIn("attachment", txt.headers["Content-Disposition"])
        exported = self.client.get(self.url + "/export?format=json").get_json()
        self.assertEqual(exported["coordinateBase"], 0)
        self.assertEqual(exported["moves"], state["moves"])
        first = self.client.post(self.url + "/analysis").get_json()["analysis"]
        self.assertEqual(first["provider"], "local-rules")
        self.assertEqual(self.client.get(self.url).get_json()["analysis"], first)
        with patch.object(web, "analyze_game", side_effect=AssertionError("must use cache")):
            self.assertEqual(self.client.post(self.url + "/analysis").status_code, 200)
        self.client.post(self.url + "/moves", json={"row": 18, "col": 18})
        self.assertIsNone(self.client.get(self.url).get_json()["analysis"])

    def test_invalid_requests_never_add_moves(self):
        for payload in ([], "text", {}, {"row": True, "col": 0}, {"row": 0.8, "col": 0},
                        {"row": "0", "col": 0}, {"row": -1, "col": 0}, {"row": 19, "col": 0}):
            with self.subTest(payload=payload):
                self.assertEqual(self.client.post(self.url + "/moves", json=payload).status_code, 400)
        self.assertEqual(len(self.client.get(self.url).get_json()["moves"]), 0)
        self.assertEqual(self.client.post(self.url + "/ai-turn").status_code, 400)
        self.client.post(self.url + "/moves", json={"row": 0, "col": 0})
        self.assertEqual(self.client.post(self.url + "/moves", json={"row": 0, "col": 1}).status_code, 400)
        self.client.post(self.url + "/ai-turn")
        self.assertEqual(self.client.post(self.url + "/moves", json={"row": 0, "col": 0}).status_code, 400)
        self.assertEqual(self.client.get("/api/games/missing").status_code, 404)

    def test_white_human_ai_opens_then_alternates_two_stone_turns(self):
        game = self.client.post("/api/games", json={"humanColor": "white"}).get_json()
        url = "/api/games/" + game["id"]
        self.assertEqual((game["humanColor"], game["aiColor"], game["currentPlayer"]), ("white", "black", "black"))
        self.assertEqual(self.client.post(url + "/moves", json={"row": 0, "col": 0}).status_code, 400)
        state = self.client.post(url + "/ai-turn").get_json()
        self.assertEqual(len(state["moves"]), 1)
        self.assertEqual(state["moves"][0]["player"], "black")
        self.assertEqual((state["currentPlayer"], state["remainingStones"]), ("white", 2))
        self.client.post(url + "/moves", json={"row": 0, "col": 0})
        self.assertEqual(self.client.post(url + "/ai-turn").status_code, 400)
        self.client.post(url + "/moves", json={"row": 0, "col": 1})
        state = self.client.post(url + "/ai-turn").get_json()
        self.assertEqual(len(state["moves"]), 5)
        self.assertEqual(state["currentPlayer"], "white")
        web.init_db()
        self.assertEqual(web.app.test_client().get(url).get_json(), state)
        self.assertEqual(self.client.get(url + "/export?format=json").get_json()["humanColor"], "white")
        self.assertIn("Human: white / AI: black", self.client.get(url + "/export").get_data(as_text=True))
        review = self.client.post(url + "/analysis").get_json()["analysis"]
        self.assertIn("白方", review["summary"])

    def test_invalid_human_color_is_rejected(self):
        for color in ("red", "", None, 1, [], {}):
            with self.subTest(color=color):
                self.assertEqual(self.client.post("/api/games", json={"humanColor": color}).status_code, 400)

    def test_black_ai_win_on_first_stone_does_not_add_second(self):
        game = self.client.post("/api/games", json={"humanColor": "white"}).get_json()
        positions = [(0, 0), (18, 18), (18, 16), (0, 1), (0, 2), (17, 18),
                     (17, 16), (0, 3), (0, 4), (16, 18), (16, 16)]
        with web.get_db() as db:
            for count, (row, col) in enumerate(positions):
                web.place_one(db, game["id"], row, col, bot.current_player(count))
        state = self.client.post("/api/games/" + game["id"] + "/ai-turn").get_json()
        self.assertEqual((state["winner"], state["status"], len(state["moves"])), ("black", "finished", 12))

    def test_win_on_first_stone_ends_turn_immediately(self):
        # 合法顺序：白方第 10 颗子直接成六，此时本回合第二颗不能再下。
        positions = [(18, 18), (0, 0), (0, 1), (18, 16), (17, 18),
                     (0, 2), (0, 3), (16, 16), (15, 18), (0, 4)]
        with web.get_db() as db:
            for count, (row, col) in enumerate(positions):
                web.place_one(db, self.game["id"], row, col, bot.current_player(count))
        response = self.client.post(self.url + "/ai-turn").get_json()
        self.assertEqual(response["winner"], "white")
        self.assertEqual(response["status"], "finished")
        self.assertEqual(len(response["moves"]), 11)
        self.assertEqual(response["remainingStones"], 0)
        self.assertEqual(self.client.post(self.url + "/moves", json={"row": 3, "col": 3}).status_code, 400)

    def test_old_schema_migrates_without_losing_games(self):
        other = Path(self.temp.name) / "old.db"
        with closing(sqlite3.connect(other)) as db:
            db.executescript("""CREATE TABLE games (id TEXT PRIMARY KEY, status TEXT, winner TEXT, created_at TEXT);
                CREATE TABLE moves (id INTEGER PRIMARY KEY, game_id TEXT, move_no INTEGER, player TEXT,
                                    row INTEGER, col INTEGER, created_at TEXT);
                INSERT INTO games VALUES ('old', 'active', NULL, '2026-11-12');
                INSERT INTO moves VALUES (1, 'old', 1, 'black', 9, 9, '2026-11-12');""")
        web.DB_PATH = other
        web.init_db()
        state = self.client.get("/api/games/old").get_json()
        self.assertEqual(state["opponent"], "local")
        self.assertEqual(state["humanColor"], "black")
        self.assertEqual(len(state["moves"]), 1)


class AgentTests(unittest.TestCase):
    def test_responses_and_chat_request_wiring(self):
        from types import SimpleNamespace
        value = {"row": 9, "col": 9, "reason": "center"}
        for style in ("responses", "chat"):
            with self.subTest(style=style), patch.dict(os.environ, {
                "OPENAI_API_KEY": "test-only", "OPENAI_MODEL": "test-model",
                "AI_API_STYLE": style, "OPENAI_BASE_URL": "https://example.invalid/v1"
            }), patch("openai.OpenAI") as constructor:
                client = constructor.return_value.__enter__.return_value
                client.responses.create.return_value = SimpleNamespace(status="completed", output_text=json.dumps(value))
                client.chat.completions.create.return_value = SimpleNamespace(choices=[
                    SimpleNamespace(finish_reason="stop", message=SimpleNamespace(content=json.dumps(value)))])
                self.assertEqual(agent.call_model_json("输出 JSON", agent.MOVE_SCHEMA, "test"), value)
                self.assertEqual(constructor.call_args.kwargs["max_retries"], 0)
                if style == "responses":
                    self.assertTrue(client.responses.create.call_args.kwargs["text"]["format"]["strict"])
                else:
                    self.assertEqual(client.chat.completions.create.call_args.kwargs["response_format"], {"type": "json_object"})

    def test_model_move_validated_and_falls_back(self):
        board = empty_board()
        board[9][9] = "black"
        for invalid in ({"row": 9, "col": 9, "reason": "occupied"},
                        {"row": 99, "col": 0, "reason": "outside"},
                        {"row": True, "col": 0, "reason": "boolean"}):
            with patch.object(agent, "call_model_json", return_value=invalid):
                result = agent.choose_agent_move(board, "white", "model")
                self.assertEqual(result["source"], "local-fallback")
                self.assertIsNone(board[result["row"]][result["col"]])
        with patch.object(agent, "call_model_json", return_value={"row": 0, "col": 0, "reason": "测试"}):
            self.assertEqual(agent.choose_agent_move(board, "white", "model")["source"], "model")

    def test_provider_error_does_not_leak_secret(self):
        with patch.object(agent, "call_model_json", side_effect=RuntimeError("secret-token-do-not-show")):
            result = agent.choose_agent_move(empty_board(), "white", "model")
            self.assertNotIn("secret-token", json.dumps(result))
            self.assertEqual(result["source"], "local-fallback")

    def test_model_review_invalid_move_number_uses_local_evidence(self):
        moves = [{"moveNo": 1, "player": "black", "row": 9, "col": 9}]
        invalid = {"summary": "test", "strengths": [], "nextPractice": [],
                   "keyMistakes": [{"moveNo": 999, "reason": "test", "suggestion": "test"}]}
        with patch.object(agent, "model_configured", return_value=True), patch.object(agent, "call_model_json", return_value=invalid):
            result = agent.analyze_game(moves, None)
            self.assertEqual(result["provider"], "local-rules")
            self.assertIn("notice", result)

    def test_review_checks_completed_turn_not_first_stone(self):
        positions = [(18, 18), (0, 0), (0, 1), (18, 16), (17, 18),
                     (0, 2), (0, 3), (16, 16), (15, 18), (0, 4), (8, 8), (14, 17)]
        moves = [{"moveNo": i + 1, "player": bot.current_player(i), "row": r, "col": c}
                 for i, (r, c) in enumerate(positions)]
        middle = agent.local_analysis(moves, None)
        self.assertFalse(any(m["moveNo"] == 12 for m in middle["keyMistakes"]))
        moves.append({"moveNo": 13, "player": "black", "row": 13, "col": 18})
        end = agent.local_analysis(moves, None)
        self.assertTrue(any(m["moveNo"] == 13 for m in end["keyMistakes"]))


if __name__ == "__main__":
    unittest.main()
