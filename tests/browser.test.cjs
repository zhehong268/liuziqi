const test = require("node:test");
const assert = require("node:assert/strict");
const E = require("../web/engine.js");
const game = () => ({ board: E.emptyBoard(), moves: [], status: "active", winner: null, humanColor: "black" });
test("黑先一颗，此后每回合两颗", () => {
  assert.deepEqual(Array.from({ length: 7 }, (_, i) => E.currentPlayer(i)), ["black", "white", "white", "black", "black", "white", "white"]);
  assert.deepEqual(Array.from({ length: 7 }, (_, i) => E.remaining(i)), [1, 2, 1, 2, 1, 2, 1]);
  const g = game(); E.place(g, 9, 9, "black");
  assert.throws(() => E.place(g, 9, 10, "black"), /轮到/);
  assert.throws(() => E.place(g, 9, 9, "white"), /棋子/);
});
test("横竖斜线与超过六颗均能判胜", () => {
  for (const [dr, dc] of [[1, 0], [0, 1], [1, 1], [1, -1]]) {
    const board = E.emptyBoard();
    for (let i = 0; i < 7; i++) board[4 + i * dr][10 + i * dc] = "black";
    assert.ok(E.hasWon(board, 4, 10, "black"));
  }
});
test("AI 优先成六，也会封堵直接成六", () => {
  for (const color of ["black", "white"]) {
    const board = E.emptyBoard();
    for (let c = 0; c < 5; c++) board[9][c] = color;
    assert.deepEqual(E.chooseMove(board, "white"), [9, 5]);
    assert.equal(board[9][5], null);
  }
  assert.deepEqual(E.chooseMove(E.emptyBoard(), "black"), [9, 9]);
});
test("首颗成六立即终局，不补第二颗", () => {
  const g = game();
  g.moves = Array.from({ length: 3 }, (_, i) => ({ moveNo: i + 1 }));
  for (let c = 0; c < 5; c++) g.board[9][c] = "black";
  E.place(g, 9, 5, "black");
  assert.equal(g.winner, "black");
  assert.equal(g.status, "finished");
  assert.throws(() => E.place(g, 9, 6, "black"), /结束/);
});
test("复盘检查完整回合，允许同一回合第二颗完成防守", () => {
  const moves = [
    ["black", 0, 0], ["white", 9, 0], ["white", 9, 1], ["black", 0, 1],
    ["black", 0, 2], ["white", 9, 2], ["white", 9, 3], ["black", 0, 3],
    ["black", 9, 4],
  ].map(([player, row, col], i) => ({ player, row, col, moveNo: i + 1 }));
  const analysis = E.analyze({ moves, humanColor: "black", winner: null, status: "active" });
  assert.ok(!analysis.keyMistakes.some((m) => m.moveNo === 8 || m.moveNo === 9));
  moves[8] = { player: "black", row: 0, col: 4, moveNo: 9 };
  assert.ok(E.analyze({ moves, humanColor: "black", winner: null, status: "active" }).keyMistakes.some((m) => m.moveNo === 9));
});
