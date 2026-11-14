(function () {
  "use strict";
  const engine = window.Connect6Engine;
  const opened = new Promise((resolve, reject) => {
    const request = indexedDB.open("moyi-connect6", 1);
    request.onupgradeneeded = () => request.result.createObjectStore("games", { keyPath: "id" });
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(new Error("浏览器无法保存棋局，请允许此网站使用本地存储。"));
  });
  const fail = (message, status = 400) => Object.assign(new Error(message), { status });
  async function store(mode, action) {
    const database = await opened;
    return new Promise((resolve, reject) => {
      const transaction = database.transaction("games", mode);
      let result;
      try { action(transaction.objectStore("games"), (value) => { result = value; }); }
      catch (error) { transaction.abort(); reject(error); return; }
      transaction.oncomplete = () => resolve(result);
      transaction.onerror = () => reject(transaction.error || new Error("棋局保存失败"));
      transaction.onabort = () => reject(transaction.error || new Error("棋局保存失败"));
    });
  }
  function payload(game) {
    return { ...game, currentPlayer: game.status === "active" ? engine.currentPlayer(game.moves.length) : null,
      remainingStones: game.status === "active" ? engine.remaining(game.moves.length) : 0 };
  }
  async function update(id, mutate) {
    let failure;
    try {
      return await store("readwrite", (games, finish) => {
        const request = games.get(id);
        request.onsuccess = () => {
          try {
            if (!request.result) throw fail("找不到棋局", 404);
            const game = request.result;
            mutate(game); games.put(game); finish(payload(game));
          } catch (error) { failure = error; request.transaction.abort(); }
        };
      });
    } catch (error) { throw failure || error; }
  }
  async function request(path, options = {}) {
    const method = options.method || "GET";
    const body = options.body ? JSON.parse(options.body) : {};
    if (path === "/api/health") return { ok: true, modelConfigured: false };
    if (path === "/api/arena") return { ready: false, officialName: "", message: "源码对战请使用 Windows 客户端。" };
    if (path === "/api/submissions") return { submissions: [] };
    if (path === "/api/games" && method === "GET") {
      const games = await store("readonly", (table, done) => {
        const query = table.getAll(); query.onsuccess = () => done(query.result);
      });
      return { games: games.sort((a, b) => b.createdAt.localeCompare(a.createdAt)).slice(0, 50).map((game) => ({
        id: game.id, status: game.status, winner: game.winner, opponent: "local", humanColor: game.humanColor,
        createdAt: game.createdAt, moveCount: game.moves.length })) };
    }
    if (path === "/api/games" && method === "POST") {
      if (!["black", "white"].includes(body.humanColor)) throw fail("请选择执黑或执白");
      const game = { id: crypto.randomUUID(), createdAt: new Date().toISOString(), status: "active", winner: null,
        humanColor: body.humanColor, aiColor: engine.other(body.humanColor), opponent: "local",
        board: engine.emptyBoard(), moves: [], analysis: null, arena: null };
      await store("readwrite", (table, done) => { table.add(game); done(); });
      return payload(game);
    }
    const match = path.match(/^\/api\/games\/([^/]+)(?:\/(moves|ai-turn|analysis))?$/);
    if (!match) throw fail("浏览器版不支持此操作", 404);
    const id = decodeURIComponent(match[1]), action = match[2];
    if (!action && method === "GET") {
      const game = await store("readonly", (table, done) => {
        const query = table.get(id); query.onsuccess = () => done(query.result);
      });
      if (!game) throw fail("找不到棋局", 404);
      return payload(game);
    }
    if (method !== "POST") throw fail("操作不支持", 405);
    const state = await update(id, (game) => {
      if (action === "moves") engine.place(game, body.row, body.col, game.humanColor);
      else if (action === "ai-turn") {
        if (engine.currentPlayer(game.moves.length) !== game.aiColor) throw fail("还没有轮到 AI");
        while (game.status === "active" && engine.currentPlayer(game.moves.length) === game.aiColor) {
          const [row, col] = engine.chooseMove(game.board, game.aiColor);
          engine.place(game, row, col, game.aiColor, "local");
        }
      } else if (action === "analysis") game.analysis = engine.analyze(game);
    });
    return action === "analysis" ? { gameId: id, moveCount: state.moves.length, analysis: state.analysis } : state;
  }
  const exports = new Map();
  function exportUrl(game, format) {
    const key = JSON.stringify([game.id, game.moves.length, game.status, game.analysis, format]);
    const cached = exports.get(format);
    if (cached?.key === key) return cached.url;
    if (cached) URL.revokeObjectURL(cached.url);
    const content = format === "json" ? JSON.stringify({ ...game, game: "connect6", size: 19, coordinateBase: 0,
      rule: "black first 1 stone; then 2 stones per turn; >=6 wins" }, null, 2) :
      ["Connect6 Demo Game", "Game ID: " + game.id, "Result: " + game.status + " / " + (game.winner || "-"),
        "Human: " + game.humanColor + " / AI: " + game.aiColor, "Coordinates: (row, col), 1-based.", "",
        ...game.moves.map((m) => String(m.moveNo).padStart(3, "0") + ". " + m.player + " (" + (m.row + 1) + "," + (m.col + 1) + ")"), ""].join("\n");
    const url = URL.createObjectURL(new Blob([content], { type: format === "json" ? "application/json;charset=utf-8" : "text/plain;charset=utf-8" }));
    exports.set(format, { key, url }); return url;
  }
  window.connect6BrowserApi = { request, exportUrl };
})();
