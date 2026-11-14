(function (root) {
  "use strict";
  const SIZE = 19;
  const directions = [[1, 0], [0, 1], [1, 1], [1, -1]];
  const inside = (r, c) => r >= 0 && r < SIZE && c >= 0 && c < SIZE;
  const other = (color) => color === "black" ? "white" : "black";
  const currentPlayer = (count) => count === 0 ? "black" : Math.floor((count - 1) / 2) % 2 === 0 ? "white" : "black";
  const remaining = (count) => count === 0 ? 1 : 2 - (count - 1) % 2;
  const emptyBoard = () => Array.from({ length: SIZE }, () => Array(SIZE).fill(null));
  function lineLength(board, row, col, color, dr, dc) {
    let length = 1;
    for (const sign of [1, -1]) {
      let r = row + dr * sign, c = col + dc * sign;
      while (inside(r, c) && board[r][c] === color) { length++; r += dr * sign; c += dc * sign; }
    }
    return length;
  }
  const hasWon = (board, row, col, color) => directions.some(([dr, dc]) => lineLength(board, row, col, color, dr, dc) >= 6);
  function legalCells(board) {
    const cells = [];
    for (let r = 0; r < SIZE; r++) for (let c = 0; c < SIZE; c++) if (board[r][c] === null) cells.push([r, c]);
    return cells;
  }
  function chooseMove(board, color) {
    const cells = legalCells(board), enemy = other(color);
    if (!cells.length) throw new Error("棋盘已经下满");
    for (const candidate of [color, enemy]) {
      for (const [r, c] of cells) {
        board[r][c] = candidate;
        const win = hasWon(board, r, c, candidate);
        board[r][c] = null;
        if (win) return [r, c];
      }
    }
    let best = cells[0], bestScore = -Infinity, bestDistance = -Infinity;
    for (const [r, c] of cells) {
      board[r][c] = color;
      const own = Math.max(...directions.map(([dr, dc]) => lineLength(board, r, c, color, dr, dc)));
      board[r][c] = enemy;
      const theirs = Math.max(...directions.map(([dr, dc]) => lineLength(board, r, c, enemy, dr, dc)));
      board[r][c] = null;
      let neighbors = 0;
      for (let dr = -1; dr <= 1; dr++) for (let dc = -1; dc <= 1; dc++) {
        if ((dr || dc) && inside(r + dr, c + dc) && board[r + dr][c + dc]) neighbors++;
      }
      const score = own * own * 5 + theirs * theirs * 4 + neighbors;
      const distance = -Math.abs(r - 9) - Math.abs(c - 9);
      if (score > bestScore || (score === bestScore && distance > bestDistance)) {
        best = [r, c]; bestScore = score; bestDistance = distance;
      }
    }
    return best;
  }
  function place(game, row, col, color, source = "human") {
    if (game.status !== "active") throw new Error("本局已经结束");
    if (currentPlayer(game.moves.length) !== color) throw new Error("还没有轮到你落子");
    if (!Number.isInteger(row) || !Number.isInteger(col) || !inside(row, col)) throw new Error("落点超出棋盘");
    if (game.board[row][col] !== null) throw new Error("这里已经有棋子了");
    game.board[row][col] = color;
    game.moves.push({ moveNo: game.moves.length + 1, row, col, player: color, source,
      reason: source === "local" ? "优先直接成六、封堵对手，再延长棋形并靠近中心。" : "", elapsedMs: null });
    game.analysis = null;
    if (hasWon(game.board, row, col, color)) { game.status = "finished"; game.winner = color; }
    else if (game.moves.length === SIZE * SIZE) game.status = "draw";
  }
  function turnThreat(board, color) {
    for (const missing of [1, 2]) for (const [dr, dc] of directions) {
      for (let r = 0; r < SIZE; r++) for (let c = 0; c < SIZE; c++) {
        if (!inside(r + 5 * dr, c + 5 * dc)) continue;
        const blanks = [];
        let own = 0;
        for (let i = 0; i < 6; i++) {
          const rr = r + i * dr, cc = c + i * dc;
          if (board[rr][cc] === color) own++;
          else if (board[rr][cc] === null) blanks.push([rr, cc]);
        }
        if (own === 6 - missing && blanks.length === missing) return blanks;
      }
    }
    return [];
  }
  function analyze(game) {
    const board = emptyBoard(), mistakes = [];
    const focus = game.humanColor, enemy = other(focus);
    const focusName = focus === "black" ? "黑方" : "白方", enemyName = enemy === "black" ? "黑方" : "白方";
    for (let i = 0; i < game.moves.length; i++) {
      const move = game.moves[i];
      board[move.row][move.col] = move.player;
      if (move.player !== focus || mistakes.length >= 3 || hasWon(board, move.row, move.col, focus)) continue;
      if (currentPlayer(i + 1) !== focus) {
        const threat = turnThreat(board, enemy);
        if (threat.length) mistakes.push({ moveNo: move.moveNo,
          reason: focusName + "本回合结束时，" + enemyName + "可用 " + threat.length + " 颗子在" +
            threat.map(([r, c]) => "第 " + (r + 1) + " 行第 " + (c + 1) + " 列").join("、") + "连成六子。",
          suggestion: "落完两颗子前，检查对手四子、五子且未被挡住的六格连线，优先消除成六威胁。" });
      }
      if (i === 0 && Math.abs(move.row - 9) + Math.abs(move.col - 9) >= 14) mistakes.push({
        moveNo: 1, reason: "首子靠近棋盘边角，可延伸的方向较少。",
        suggestion: "可以尝试从中心区域开局；这是布局建议，不代表此手必败。" });
    }
    const result = game.winner ? (game.winner === "black" ? "黑方获胜" : "白方获胜") : game.status === "draw" ? "和棋" : "仍在进行";
    return { provider: "local-rules",
      summary: "本局 " + game.moves.length + " 颗子，" + result + "。复盘检查" + focusName + "完整回合后的单子、双子成六威胁。",
      keyMistakes: mistakes.slice(0, 3),
      strengths: game.winner === focus ? [focusName + "已完成六连或以上连线。"] :
        game.moves.length && !mistakes.length ? ["没有发现完整回合结束后的单子、双子成六威胁；其他战术还需结合棋谱检查。"] : [],
      nextPractice: ["先找自己能成六的位置，再检查对手用一颗或两颗子成六的连线。", "把两颗子作为一组规划；本版不搜索多回合战术。"],
      trace: ["读取浏览器保存的棋谱", "重建 " + game.moves.length + " 颗子的局面", "检查完整回合后的单子、双子成六威胁", "生成结构化练习建议"] };
  }
  const engine = { SIZE, other, currentPlayer, remaining, emptyBoard, hasWon, chooseMove, place, turnThreat, analyze };
  if (typeof module !== "undefined" && module.exports) module.exports = engine;
  else root.Connect6Engine = engine;
})(globalThis);
