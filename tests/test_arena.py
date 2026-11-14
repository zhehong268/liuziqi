import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import app as web
from arena import build_request, server_guard
from agent import local_analysis
from sandbox_runner import BotFault


class ArenaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.original_db = web.DB_PATH
        web.DB_PATH = Path(self.temp.name) / "test.db"
        web.init_db()
        self.service = web.arena_service
        self.original_storage = self.service.storage
        self.service.storage = Path(self.temp.name) / "arena"
        self.client = web.app.test_client()

    def tearDown(self):
        self.service.storage = self.original_storage
        web.DB_PATH = self.original_db
        self.temp.cleanup()

    def upload(self, content=b"print('hello')", filename="bot.py"):
        response = self.client.post("/api/submissions", data={"name": "测试棋手", "file": (io.BytesIO(content), filename)})
        response.request.environ["wsgi.input"].close()
        return response

    def create_match(self, color="black"):
        submission = self.upload().get_json()
        with patch("arena.availability", return_value=(True, "ready")), patch.object(self.service.executor, "submit"):
            response = self.client.post("/api/arena/matches", json={"submissionId": submission["id"], "challengerColor": color})
        self.assertEqual(response.status_code, 202)
        self.service.pending.discard(response.get_json()["id"])
        return response.get_json()

    def test_upload_validates_files_and_preserves_versions(self):
        for content, filename in [(b"", "bot.py"), (b"exe", "bot.exe"), (b"\x00", "bot.cpp"),
                                  (b"x" * (512 * 1024 + 1), "bot.py")]:
            self.assertEqual(self.upload(content, filename).status_code, 400)
        first, second = self.upload().get_json(), self.upload().get_json()
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(first["sha256"], second["sha256"])
        self.assertEqual(len(self.client.get("/api/submissions").get_json()["submissions"]), 2)
        self.assertEqual(self.client.post("/api/submissions").status_code, 400)

    def test_missing_docker_does_not_run_or_create_game(self):
        item = self.upload().get_json()
        with patch("arena.availability", return_value=(False, "Docker unavailable")):
            response = self.client.post("/api/arena/matches", json={"submissionId": item["id"]})
        self.assertEqual(response.status_code, 503)
        self.assertEqual(self.client.get("/api/games").get_json()["games"], [])

    def test_arena_game_cannot_be_modified_by_human_routes(self):
        game = self.create_match("white")
        self.assertEqual(game["arena"]["challengerColor"], "white")
        self.assertEqual(game["arena"]["threads"], 1)
        url = "/api/games/" + game["id"]
        self.assertEqual(self.client.post(url + "/moves", json={"row": 0, "col": 0}).status_code, 409)
        self.assertEqual(self.client.post(url + "/ai-turn").status_code, 409)

    def test_timeout_records_forfeit_and_not_fake_six_in_analysis(self):
        game = self.create_match("white")
        self.service._finish(game["id"], "单次落子超过 10 秒", loser="white", elapsed=10001)
        url = "/api/games/" + game["id"]
        state = self.client.get(url).get_json()
        self.assertEqual(state["winner"], "black")
        self.assertEqual(state["arena"]["failureElapsedMs"], 10001)
        self.assertIn("超过 10 秒", state["analysis"]["summary"])
        self.assertFalse(any("已完成六连" in s for s in state["analysis"]["strengths"]))
        exported = self.client.get(url + "/export?format=json").get_json()
        self.assertEqual(exported["arena"], state["arena"])
        self.assertIn("Limits: 10000 ms", self.client.get(url + "/export").get_data(as_text=True))

    def test_cancellation_and_restart_preserve_existing_moves(self):
        for restart in (False, True):
            game = self.create_match()
            with web.get_db() as db:
                web.place_one(db, game["id"], 9, 9, "black", "challenger", elapsed_ms=42)
            if restart:
                self.service.recover()
            else:
                self.client.post("/api/arena/matches/" + game["id"] + "/cancel")
                self.service.cancelled.discard(game["id"])
            state = self.client.get("/api/games/" + game["id"]).get_json()
            self.assertEqual(state["status"], "aborted")
            self.assertEqual(state["moves"][0]["elapsedMs"], 42)
            self.assertEqual(state["arena"]["state"], "aborted")

    def test_worker_rejects_occupied_or_outside_cells_and_persists_first_stone(self):
        for action in ({"row": 19, "col": 0}, {"row": 9, "col": 9}):
            game = self.create_match()
            with web.get_db() as db:
                web.place_one(db, game["id"], 9, 9, "black", "challenger", elapsed_ms=10)
            fake = unittest.mock.MagicMock()
            fake.ask.return_value = (action, 123)
            with patch("arena.prepare", return_value=Path("unused")), patch("arena.BotProcess", return_value=fake):
                self.service._run(game["id"])
            state = self.client.get("/api/games/" + game["id"]).get_json()
            self.assertEqual(state["winner"], "black")
            self.assertEqual(state["arena"]["loser"], "white")
            self.assertEqual(len(state["moves"]), 1)
            self.assertIn("非法落子", state["arena"]["reason"])

    def test_protocol_contains_turn_context_for_official_second_stone_plan(self):
        moves = []
        expected = [(1, 0, 1), (2, 0, 2), (2, 1, 1), (1, 0, 2), (1, 1, 1)]
        for index, (player, in_turn, remaining) in enumerate(expected):
            message = build_request("case", moves)
            obs = message["observation"]
            self.assertEqual((obs["current_player"], obs["moves_in_turn"], obs["remaining_stones"]),
                             (player, in_turn, remaining))
            self.assertEqual(message["time_limit_ms"], 10000)
            self.assertEqual(obs["required_stones_this_turn"], 1)
            moves.append({"row": index, "col": index, "player": "black" if player == 1 else "white"})

    def test_white_review_checks_white_completed_turns(self):
        positions = [(0, 0), (18, 18), (18, 17), (0, 1), (0, 2), (17, 18), (17, 17),
                     (0, 3), (0, 4), (16, 18), (16, 17)]
        from bot import current_player
        moves = [{"moveNo": i + 1, "player": current_player(i), "row": r, "col": c}
                 for i, (r, c) in enumerate(positions)]
        result = local_analysis(moves, None, focus_player="white")
        self.assertTrue(any(m["moveNo"] == 11 for m in result["keyMistakes"]))
        self.assertIn("白方", result["summary"])

    def test_only_one_referee_can_hold_server_lock(self):
        path = Path(self.temp.name) / "server.lock"
        with server_guard(path):
            with self.assertRaises(RuntimeError):
                with server_guard(path):
                    self.fail("a second referee acquired the lock")
        with server_guard(path):
            pass

    def test_review_catches_two_stone_win_but_ignores_unfinished_turn(self):
        positions = [(18, 18), (0, 0), (0, 1), (17, 18), (16, 18), (0, 2), (0, 3), (15, 17)]
        from bot import current_player
        moves = [{"moveNo": i + 1, "player": current_player(i), "row": r, "col": c}
                 for i, (r, c) in enumerate(positions)]
        self.assertFalse(any(m["moveNo"] == 8 for m in local_analysis(moves, None)["keyMistakes"]))
        moves.append({"moveNo": 9, "player": "black", "row": 14, "col": 17})
        mistakes = local_analysis(moves, None)["keyMistakes"]
        self.assertTrue(any(m["moveNo"] == 9 and "2 颗子" in m["reason"] for m in mistakes))


if __name__ == "__main__":
    unittest.main()
