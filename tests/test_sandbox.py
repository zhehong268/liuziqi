"""真实 Docker 检查：设置 ARENA_DOCKER_TESTS=1 后执行。"""
import json
import io
import os
import stat
from pathlib import Path
import tempfile
import time
import unittest

from arena import build_request
from sandbox_runner import BotFault, BotProcess, prepare


REPLY = '''
import json, sys
for raw in sys.stdin:
    req = json.loads(raw)
    %s
    print(json.dumps({"protocol":"arena.v1", "type":"move_response", "request_id":req["request_id"],
                      "action":{"kind":"PLACE", "row":0, "col":0}}), flush=True)
'''


@unittest.skipUnless(os.getenv("ARENA_DOCKER_TESTS") == "1", "需要 Docker 和 connect6-sandbox:1 镜像")
class SandboxIntegrationTests(unittest.TestCase):
    def run_bot(self, source):
        directory = Path(self.temp.name)
        path = directory / "bot.py"
        path.write_text(source, encoding="utf-8")
        return BotProcess(prepare(path, "python", directory / "build"))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temp.cleanup()

    def test_python_and_cpp_can_reply_in_actual_container(self):
        process = self.run_bot(REPLY % "pass")
        try:
            for i in range(2):
                action, elapsed = process.ask(build_request("integration-" + str(i), []))
                self.assertEqual(action["row"], 0)
                self.assertLess(elapsed, 10000)
        finally:
            process.close()
        directory = Path(self.temp.name)
        source = directory / "bot.cpp"
        source.write_text(r'''#include <iostream>
#include <string>
int main() {
    std::string raw;
    while (std::getline(std::cin, raw)) {
        std::string key = "\"request_id\":\"";
        auto begin = raw.find(key) + key.size();
        auto id = raw.substr(begin, raw.find('"', begin) - begin);
        std::cout << "{\"protocol\":\"arena.v1\",\"type\":\"move_response\",\"request_id\":\""
                  << id << "\",\"action\":{\"kind\":\"PLACE\",\"row\":0,\"col\":0}}" << std::endl;
    }
}''', encoding="utf-8")
        directory.chmod(0o700)
        source.chmod(0o600)
        artifact = prepare(source, "cpp", directory / "cpp-build")
        self.assertTrue(artifact.exists())
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o600)
            self.assertEqual(artifact.stat().st_uid, os.getuid())
            self.assertEqual(stat.S_IMODE(artifact.stat().st_mode), 0o755)
        process = BotProcess(artifact)
        try:
            action, _ = process.ask(build_request("cpp-integration", []))
            self.assertEqual(action["col"], 0)
        finally:
            process.close()

    def test_real_ten_second_timeout_kills_ai(self):
        process = self.run_bot(REPLY % "import time; time.sleep(30)")
        start = time.monotonic()
        try:
            with self.assertRaisesRegex(BotFault, "超过 10 秒"):
                process.ask(build_request("timeout", []))
            self.assertGreaterEqual(time.monotonic() - start, 10)
            self.assertIsNotNone(process.process.poll())
        finally:
            process.close()

    def test_thread_creation_is_blocked_by_cgroup(self):
        code = '''import json,sys,threading
for raw in sys.stdin:
    req=json.loads(raw)
    try:
        thread=threading.Thread(target=lambda:None)
        thread.start()
        raise SystemExit("thread limit did not work")
    except RuntimeError:
        print(json.dumps({"protocol":"arena.v1","type":"move_response","request_id":req["request_id"],
                          "action":{"kind":"PLACE","row":0,"col":0}}),flush=True)
'''
        process = self.run_bot(code)
        try:
            action, _ = process.ask(build_request("thread", []))
            self.assertEqual(action["row"], 0)
        finally:
            process.close()

    def test_malformed_wrong_request_id_crash_and_output_flood(self):
        sources = ["print('not-json',flush=True); input(); input()",
                   REPLY.replace('req["request_id"]', '"wrong"') % "pass",
                   "raise SystemExit(1)", "print('x'*20000,flush=True); input(); input()"]
        for code in sources:
            with self.subTest(source=code[:30]):
                process = self.run_bot(code)
                try:
                    with self.assertRaises(BotFault):
                        process.ask(build_request("invalid", []))
                finally:
                    process.close()

    def test_real_timeout_persists_forfeit_analysis_and_official_opening(self):
        import app as web
        from arena import ArenaService
        from werkzeug.datastructures import FileStorage

        original_db = web.DB_PATH
        web.DB_PATH = Path(self.temp.name) / "match.db"
        web.init_db()
        service = ArenaService(web.get_db, web.get_game, web.game_payload, web.place_one, web.now_iso,
                               storage=Path(self.temp.name) / "arena")
        try:
            source = (REPLY % "import time; time.sleep(30)").encode()
            with io.BytesIO(source) as stream:
                submission = service.add_submission(FileStorage(stream=stream, filename="slow.py"), "超时检查")
            game_id = service.start(submission["id"], "white")
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                with web.get_db() as db:
                    state = web.game_payload(db, web.get_game(db, game_id))
                if state["status"] != "active":
                    break
                time.sleep(0.1)
            self.assertEqual(state["status"], "finished", state["arena"]["reason"])
            self.assertEqual(state["winner"], "black")
            self.assertEqual(state["arena"]["loser"], "white")
            self.assertGreaterEqual(state["arena"]["failureElapsedMs"], 10000)
            self.assertEqual(len(state["moves"]), 1)
            self.assertEqual(state["moves"][0]["source"], "official")
            self.assertIn("超过 10 秒", state["analysis"]["summary"])
        finally:
            service.shutdown()
            web.DB_PATH = original_db


if __name__ == "__main__":
    unittest.main()
