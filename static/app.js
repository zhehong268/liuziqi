let gameState = null;
let busy = true;
let replayIndex = 0;
const $ = (selector) => document.querySelector(selector);
const boardElement = $("#board");
const storageKey = "connect6.currentGame";
const names = { black: "黑方", white: "白方" };
const sources = { model: "大模型", local: "我的规则 AI", "local-fallback": "本地 AI 接管" };

function savedGameId() {
  try { return localStorage.getItem(storageKey); } catch { return null; }
}
function rememberGame(id) {
  try { localStorage.setItem(storageKey, id); } catch { /* 禁用存储时仍可用 URL 恢复 */ }
  const url = new URL(location.href);
  url.searchParams.set("game", id);
  history.replaceState(null, "", url);
}
function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[c]));
}
function setMessage(message) { $("#message").textContent = message; }
function isReplaying() { return gameState && replayIndex < gameState.moves.length; }
function renderBoard() {
  boardElement.replaceChildren();
  if (!gameState) return;
  const board = Array.from({ length: 19 }, () => Array(19).fill(null));
  const visibleMoves = gameState.moves.slice(0, replayIndex);
  const last = visibleMoves.at(-1);
  for (const move of visibleMoves) board[move.row][move.col] = move;
  for (let row = 0; row < 19; row += 1) {
    for (let col = 0; col < 19; col += 1) {
      const move = board[row][col];
      const cell = document.createElement("button");
      cell.type = "button";
      cell.className = "cell" + ([3, 9, 15].includes(row) && [3, 9, 15].includes(col) ? " star" : "");
      cell.dataset.row = row; cell.dataset.col = col;
      cell.title = "第 " + (row + 1) + " 行，第 " + (col + 1) + " 列";
      cell.setAttribute("aria-label", cell.title + "，" + (move ? names[move.player] : "空位"));
      cell.disabled = busy || !!move || isReplaying() || gameState.status !== "active" || gameState.currentPlayer !== "black";
      if (move) {
        const stone = document.createElement("i");
        stone.className = "stone " + move.player + (move === last ? " last" : "");
        stone.textContent = move === last ? String(move.moveNo) : "";
        cell.append(stone);
      }
      cell.addEventListener("click", () => handleCellClick(row, col));
      boardElement.append(cell);
    }
  }
}
function renderAnalysis(analysis) {
  const container = $("#analysis");
  container.hidden = !analysis;
  if (!analysis) { container.replaceChildren(); return; }
  const list = (items, empty) => (items || []).map((s) => "<li>" + escapeHtml(s) + "</li>").join("") || "<li>" + empty + "</li>";
  const mistakes = (analysis.keyMistakes || []).map((m) =>
    "<li>第 " + escapeHtml(m.moveNo) + " 手：" + escapeHtml(m.reason) + "<br>建议：" + escapeHtml(m.suggestion) + "</li>").join("");
  container.innerHTML = "<h3>本局总结</h3><p>" + escapeHtml(analysis.summary) + "</p>" +
    '<p class="analysis-provider">分析方式：' + (analysis.provider === "model" ? "大模型 + 棋形工具" : "本地棋形工具") + "</p>" +
    (analysis.notice ? '<p class="notice">' + escapeHtml(analysis.notice) + "</p>" : "") +
    "<h3>可以改进</h3><ul>" + (mistakes || "<li>当前工具没有发现明确问题，可继续下完本局。</li>") + "</ul>" +
    "<h3>做得好的地方</h3><ul>" + list(analysis.strengths, "本次没有记录明确的优点。") + "</ul>" +
    "<h3>下一局练习</h3><ul>" + list(analysis.nextPractice, "继续对照棋谱练习。") + "</ul>" +
    "<details><summary>查看 Agent 执行过程</summary><ol>" + list(analysis.trace, "未记录执行过程。") + "</ol></details>";
}
function renderState() {
  $("#new-game").disabled = busy;
  $("#opponent").disabled = busy;
  $("#history").disabled = busy;
  $("#analyze-game").disabled = busy || !gameState?.moves.length || isReplaying();
  $("#replay-slider").disabled = busy || !gameState?.moves.length;
  $("#replay-end").disabled = busy || !isReplaying();
  if (!gameState) return;
  renderBoard();
  $("#game-id").textContent = gameState.id;
  $("#current-player").textContent = names[gameState.currentPlayer] || "已结束";
  $("#remaining-stones").textContent = gameState.remainingStones + " 子";
  $("#opponent-label").textContent = gameState.opponent === "model" ? "大模型 AI" : "规则 AI";
  $("#game-status").textContent = isReplaying() ? "棋谱回放" : gameState.status === "active" ? "对局进行中" : gameState.status === "draw" ? "和棋" : names[gameState.winner] + "获胜";
  $("#turn-status").textContent = isReplaying() ? "回到最新后可继续落子" : gameState.status === "active" ? names[gameState.currentPlayer] + "还需落 " + gameState.remainingStones + " 子" : "本局已结束 · 可以复盘";
  for (const [selector, format] of [["#export-game", "txt"], ["#export-json", "json"]]) {
    $(selector).href = "/api/games/" + encodeURIComponent(gameState.id) + "/export?format=" + format;
    $(selector).classList.remove("disabled");
    $(selector).setAttribute("aria-disabled", "false");
  }
  $("#replay-slider").max = gameState.moves.length;
  $("#replay-slider").value = replayIndex;
  $("#replay-label").textContent = replayIndex + " / " + gameState.moves.length;
  $("#retry-ai").hidden = busy || isReplaying() || gameState.status !== "active" || gameState.currentPlayer !== "white";
  const decision = gameState.moves.slice(0, replayIndex).filter((m) => m.player === "white").at(-1);
  $("#ai-decision").hidden = !decision?.reason;
  if (decision?.reason) {
    $("#ai-decision").innerHTML = "<strong>第 " + decision.moveNo + " 手 · " + escapeHtml(sources[decision.source] || "AI") + "</strong>" + escapeHtml(decision.reason);
  }
  renderAnalysis(gameState.analysis);
}
function setState(state) {
  gameState = state;
  replayIndex = state.moves.length;
  rememberGame(state.id);
  renderState();
}
async function api(path, options = {}) {
  const response = await fetch(path, { ...options, headers: { "Content-Type": "application/json" } });
  const data = await response.json().catch(() => ({ error: "服务器返回了异常响应，请检查后端。" }));
  if (!response.ok) {
    const error = new Error(data.error || "请求失败");
    error.status = response.status;
    throw error;
  }
  return data;
}
async function refreshHistory() {
  const { games } = await api("/api/games");
  const select = $("#history");
  select.replaceChildren();
  for (const game of games) {
    const option = document.createElement("option");
    option.value = game.id;
    const time = new Date(game.createdAt).toLocaleString("zh-CN", { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" });
    const result = game.status === "active" ? "进行中" : game.status === "draw" ? "和棋" : names[game.winner] + "胜";
    option.textContent = time + " · " + game.moveCount + " 子 · " + result;
    select.append(option);
  }
  select.value = gameState?.id || "";
}
async function analyzeCurrent() {
  if (!gameState?.moves.length) return;
  setMessage("Agent 正在读取棋谱并分析，请稍候…");
  const result = await api("/api/games/" + gameState.id + "/analysis", { method: "POST", body: "{}" });
  if (gameState.id === result.gameId && gameState.moves.length === result.moveCount) {
    gameState.analysis = result.analysis;
    renderAnalysis(result.analysis);
    setMessage(result.analysis.notice || "复盘已保存，可以对照棋谱查看关键手数。");
  }
}
async function aiTurn() {
  if (gameState.status !== "active" || gameState.currentPlayer !== "white") return;
  setMessage(gameState.opponent === "model" ? "大模型正在选择落点，请稍候…" : "你的规则 AI 正在落子…");
  setState(await api("/api/games/" + gameState.id + "/ai-turn", { method: "POST", body: "{}" }));
  const fallback = gameState.moves.slice(-2).find((m) => m.source === "local-fallback");
  setMessage(fallback ? fallback.reason : "轮到你了，每回合连续落两颗子。");
}
async function afterMove() {
  if (gameState.status !== "active") {
    await refreshHistory();
    await analyzeCurrent();
  } else if (gameState.currentPlayer === "white") {
    await aiTurn();
    await refreshHistory();
    if (gameState.status !== "active") await analyzeCurrent();
  } else {
    setMessage(gameState.remainingStones === 1 ? "同一回合，还需要再落一颗子。" : "轮到你执黑。");
  }
}
async function runBusy(action) {
  if (busy) return;
  busy = true; renderState();
  try { await action(); }
  catch (error) { setMessage(error.message); }
  finally { busy = false; renderState(); }
}
async function handleCellClick(row, col) {
  if (busy || !gameState || isReplaying() || gameState.currentPlayer !== "black" || gameState.status !== "active") return;
  await runBusy(async () => {
    setState(await api("/api/games/" + gameState.id + "/moves", { method: "POST", body: JSON.stringify({ row, col }) }));
    await afterMove();
  });
}
async function newGame() {
  setState(await api("/api/games", { method: "POST", body: JSON.stringify({ opponent: $("#opponent").value }) }));
  setMessage("你执黑。首回合先落一颗子。");
  await refreshHistory();
}
$("#new-game").addEventListener("click", () => runBusy(newGame));
$("#analyze-game").addEventListener("click", () => runBusy(analyzeCurrent));
$("#retry-ai").addEventListener("click", () => runBusy(afterMove));
$("#history").addEventListener("change", () => runBusy(async () => {
  const id = $("#history").value;
  if (!id) return;
  setState(await api("/api/games/" + encodeURIComponent(id)));
  setMessage("已读取历史对局。");
  await afterMove();
}));
$("#replay-slider").addEventListener("input", (event) => {
  replayIndex = Number(event.target.value); renderState();
});
$("#replay-end").addEventListener("click", () => {
  replayIndex = gameState.moves.length; renderState();
});
async function initialize() {
  try {
    const health = await api("/api/health");
    const modelOption = $("#opponent").querySelector('[value="model"]');
    modelOption.disabled = !health.modelConfigured;
    modelOption.textContent = health.modelConfigured ? "大模型 AI · 已配置" : "大模型 AI · 需配置";
    const saved = new URL(location.href).searchParams.get("game") || savedGameId();
    if (saved) {
      try {
        setState(await api("/api/games/" + encodeURIComponent(saved)));
        setMessage("已恢复上次棋局。");
      } catch (error) { if (error.status !== 404) throw error; }
    }
    if (!gameState) await newGame();
    else {
      await refreshHistory();
      if (gameState.currentPlayer === "white") await afterMove();
    }
  } catch (error) { setMessage("初始化失败：" + error.message + "。可点击新建棋局重试。"); }
  finally { busy = false; renderState(); }
}
initialize();
