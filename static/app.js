let gameState = null;
let busy = true;
let replayIndex = 0;
let viewMode = "arena";
let arenaInfo = null;
let polling = false;
const $ = (selector) => document.querySelector(selector);
const boardElement = $("#board");
const storageKey = "connect6.currentGame";
const names = { black: "黑方", white: "白方" };
const sources = { model: "大模型", local: "我的规则 AI", "local-fallback": "本地 AI 接管", official: "官方 AI", challenger: "挑战方 AI" };
for (const selector of ["#column-coords", "#row-coords"]) {
  for (let index = 1; index <= 19; index += 1) {
    const label = document.createElement("span");
    label.textContent = index;
    $(selector).append(label);
  }
}

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
  const board = Array.from({ length: 19 }, () => Array(19).fill(null));
  const visibleMoves = gameState?.moves.slice(0, replayIndex) || [];
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
      cell.disabled = busy || !gameState || !!move || isReplaying() || !!gameState.arena || gameState.status !== "active" || gameState.currentPlayer !== gameState.humanColor;
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
    "<h3>可以改进</h3><ul>" + (mistakes || "<li>当前工具没有发现明确问题，可以结合棋谱继续检查。</li>") + "</ul>" +
    "<h3>做得好的地方</h3><ul>" + list(analysis.strengths, "本次没有记录明确的优点。") + "</ul>" +
    "<h3>下一局练习</h3><ul>" + list(analysis.nextPractice, "继续对照棋谱练习。") + "</ul>" +
    "<details><summary>查看 Agent 执行过程</summary><ol>" + list(analysis.trace, "未记录执行过程。") + "</ol></details>";
}
function renderState() {
  $("#new-game").disabled = busy;
  $("#opponent").disabled = busy;
  for (const input of document.querySelectorAll('[name="human-color"]')) input.disabled = busy;
  for (const selector of ["#mode-arena", "#mode-practice"]) $(selector).disabled = busy;
  $("#history").disabled = busy;
  $("#analyze-game").disabled = busy || !gameState?.moves.length || isReplaying() || (!!gameState?.arena && !["finished", "aborted"].includes(gameState.arena.state));
  $("#replay-slider").disabled = busy || !gameState?.moves.length;
  $("#replay-end").disabled = busy || !isReplaying();
  $("#upload-ai").disabled = busy;
  $("#start-match").disabled = busy || !arenaInfo?.ready || !$("#submission").value;
  $("#stop-match").hidden = !gameState?.arena || gameState.status !== "active";
  $("#stop-match").disabled = busy;
  renderBoard();
  renderThinking();
  if (!gameState) {
    $("#game-status").textContent = "等待开局";
    $("#opponent-label").textContent = viewMode === "arena" ? "AI 挑战" : "人机练习";
    return;
  }
  $("#game-id").textContent = gameState.id;
  $("#current-player").textContent = gameState.status === "active" ? names[gameState.currentPlayer] : "已结束";
  $("#remaining-stones").textContent = gameState.status === "active" ? gameState.remainingStones + " 子" : "—";
  $("#opponent-label").textContent = gameState.arena ? "AI 挑战" : gameState.opponent === "model" ? "大模型 AI" : "规则 AI";
  $("#game-status").textContent = isReplaying() ? "棋谱回放" : gameState.status === "active" ? "对局进行中" : gameState.status === "aborted" ? "对局已中断" : gameState.status === "draw" ? "和棋" : names[gameState.winner] + "获胜";
  $("#turn-status").textContent = isReplaying() ? "回到最新后可继续落子" : gameState.status === "active" ? names[gameState.currentPlayer] + "还需落 " + gameState.remainingStones + " 子" : "本局已结束 · 可以复盘";
  for (const [selector, format] of [["#export-game", "txt"], ["#export-json", "json"]]) {
    $(selector).href = "/api/games/" + encodeURIComponent(gameState.id) + "/export?format=" + format;
    $(selector).classList.remove("disabled");
    $(selector).setAttribute("aria-disabled", "false");
  }
  $("#replay-slider").max = gameState.moves.length;
  $("#replay-slider").value = replayIndex;
  $("#replay-label").textContent = replayIndex + " / " + gameState.moves.length;
  $("#retry-ai").hidden = !!gameState.arena || busy || isReplaying() || gameState.status !== "active" || gameState.currentPlayer !== gameState.aiColor;
  const decision = gameState.moves.slice(0, replayIndex).filter((m) => gameState.arena || m.player === gameState.aiColor).at(-1);
  $("#ai-decision").hidden = !decision?.reason;
  if (decision?.reason) {
    $("#ai-decision").innerHTML = "<strong>第 " + decision.moveNo + " 手 · " + escapeHtml(sources[decision.source] || "AI") + "</strong>" + escapeHtml(decision.reason);
  }
  renderAnalysis(gameState.analysis);
  for (const color of ["black", "white"]) {
    $("#" + color + "-label").textContent = gameState.arena
      ? (gameState.arena.challengerColor === color ? gameState.arena.challengerName : "官方 AI")
      : (color === gameState.humanColor ? "你" : "AI") + " · " + names[color];
  }
  $("#move-times").hidden = !gameState.arena;
  if (gameState.arena) {
    $("#move-times").innerHTML = "<h3>最近落子耗时</h3>" + gameState.moves.slice(0, replayIndex).slice(-6).map((m) =>
      "<div><span>第 " + m.moveNo + " 子 · " + names[m.player] + "</span><strong>" + m.elapsedMs + " ms</strong></div>").join("");
    setMessage(gameState.arena.reason || "正在等待裁判。");
  }
}
function setState(state, follow = true) {
  const oldIndex = replayIndex;
  const changedGame = gameState?.id !== state.id;
  gameState = state;
  if (changedGame && !state.arena) {
    document.querySelector('[name="human-color"][value="' + state.humanColor + '"]').checked = true;
    $("#opponent").value = state.opponent;
  }
  replayIndex = follow ? state.moves.length : Math.min(oldIndex, state.moves.length);
  rememberGame(state.id);
  renderState();
}
async function api(path, options = {}) {
  const headers = options.body instanceof FormData ? {} : { "Content-Type": "application/json" };
  const response = await fetch(path, { ...options, headers });
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
    const result = game.status === "active" ? "进行中" : game.status === "aborted" ? "已中断" : game.status === "draw" ? "和棋" : names[game.winner] + "胜";
    option.textContent = time + " · " + (game.opponent === "arena" ? "AI挑战" : game.humanColor === "white" ? "执白" : "执黑") + " · " + game.moveCount + " 子 · " + result;
    select.append(option);
  }
  if (!games.length) select.append(new Option("暂无对局", ""));
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
  if (gameState.status !== "active" || gameState.currentPlayer !== gameState.aiColor) return;
  setMessage(gameState.opponent === "model" ? "大模型正在选择落点，请稍候…" : "AI 正在落子…");
  setState(await api("/api/games/" + gameState.id + "/ai-turn", { method: "POST", body: "{}" }));
  const fallback = gameState.moves.slice(-2).find((m) => m.source === "local-fallback");
  setMessage(fallback ? fallback.reason : "轮到你了，每回合连续落两颗子。");
}
async function afterMove() {
  if (gameState.arena) return;
  if (gameState.status !== "active") {
    await refreshHistory();
    await analyzeCurrent();
  } else if (gameState.currentPlayer === gameState.aiColor) {
    await aiTurn();
    await refreshHistory();
    if (gameState.status !== "active") await analyzeCurrent();
  } else {
    setMessage(gameState.moves.length === 0 ? "你执黑，首回合落一颗子。" : gameState.remainingStones === 1 ? "同一回合，还需要再落一颗子。" : "轮到你了，请连续落两颗子。");
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
  if (busy || !gameState || gameState.arena || isReplaying() || gameState.currentPlayer !== gameState.humanColor || gameState.status !== "active") return;
  await runBusy(async () => {
    setState(await api("/api/games/" + gameState.id + "/moves", { method: "POST", body: JSON.stringify({ row, col }) }));
    await afterMove();
  });
}
async function newGame() {
  const humanColor = document.querySelector('[name="human-color"]:checked').value;
  setState(await api("/api/games", { method: "POST", body: JSON.stringify({ opponent: $("#opponent").value, humanColor }) }));
  await refreshHistory();
  await afterMove();
}
$("#new-game").addEventListener("click", () => runBusy(newGame));
$("#analyze-game").addEventListener("click", () => runBusy(analyzeCurrent));
$("#retry-ai").addEventListener("click", () => runBusy(afterMove));
$("#history").addEventListener("change", () => runBusy(async () => {
  const id = $("#history").value;
  if (!id) return;
  setState(await api("/api/games/" + encodeURIComponent(id)));
  setMode(gameState.arena ? "arena" : "practice");
  setMessage("已读取历史对局。");
  await afterMove();
}));
function setMode(mode) {
  viewMode = mode;
  document.body.dataset.mode = mode;
  $("#arena-panel").hidden = mode !== "arena";
  $("#practice-controls").hidden = mode !== "practice";
  $("#arena-live").hidden = mode !== "arena";
  for (const item of ["arena", "practice"]) {
    $("#mode-" + item).classList.toggle("selected", mode === item);
    $("#mode-" + item).setAttribute("aria-pressed", String(mode === item));
  }
  renderState();
}
function renderThinking() {
  const match = gameState?.arena;
  if (!match) { $("#arena-live").textContent = "上传代码后即可发起挑战"; return; }
  if (match.state === "running" && match.thinkingSince) {
    const elapsed = Math.max(0, (Date.now() - Date.parse(match.thinkingSince)) / 1000);
    $("#arena-live").textContent = names[match.thinkingPlayer] + "思考中 · " + Math.min(10, elapsed).toFixed(1) + " / 10 秒";
  } else {
    const labels = { queued: "排队中", preparing: "准备 / 编译中", running: "正在对战", finished: "对战结束", aborted: "对战中断" };
    $("#arena-live").textContent = labels[match.state] || match.state;
  }
}
async function refreshSubmissions(selected) {
  const { submissions } = await api("/api/submissions");
  const select = $("#submission");
  const old = selected || select.value;
  select.replaceChildren();
  for (const item of submissions) select.append(new Option(item.name + " · " + item.language + " · " + item.sha256.slice(0, 7), item.id));
  if (!submissions.length) select.append(new Option("请先上传源码", ""));
  if (submissions.some((s) => s.id === old)) select.value = old;
}
$("#mode-arena").addEventListener("click", () => setMode("arena"));
$("#mode-practice").addEventListener("click", () => setMode("practice"));
$("#submission").addEventListener("change", renderState);
$("#upload-form").addEventListener("submit", (event) => {
  event.preventDefault();
  runBusy(async () => {
    try {
      const result = await api("/api/submissions", { method: "POST", body: new FormData(event.target) });
      await refreshSubmissions(result.id);
      $("#upload-message").textContent = result.name + " 已保存，版本 " + result.sha256.slice(0, 7) + "。选择执子后点击开始挑战。";
    } catch (error) { $("#upload-message").textContent = error.message; }
  });
});
$("#start-match").addEventListener("click", () => runBusy(async () => {
  arenaInfo = await api("/api/arena");
  $("#arena-readiness").textContent = arenaInfo.officialName + " · " + arenaInfo.message;
  if (!arenaInfo.ready) $(".protocol-note").open = true;
  if (!arenaInfo.ready) throw new Error(arenaInfo.message);
  setState(await api("/api/arena/matches", { method: "POST", body: JSON.stringify({
    submissionId: $("#submission").value, challengerColor: $("#challenger-color").value,
  }) }));
  setMode("arena");
  await refreshHistory();
}));
$("#stop-match").addEventListener("click", () => runBusy(async () => {
  setState(await api("/api/arena/matches/" + gameState.id + "/cancel", { method: "POST", body: "{}" }));
  await refreshHistory();
}));
async function pollMatch() {
  if (polling || busy || !gameState?.arena || ["finished", "aborted"].includes(gameState.arena.state)) return;
  const id = gameState.id;
  polling = true;
  try {
    const state = await api("/api/games/" + encodeURIComponent(id));
    if (gameState?.id !== id) return;
    setState(state, !isReplaying());
    if (["finished", "aborted"].includes(state.arena.state)) await refreshHistory();
  } catch (error) { setMessage("读取对局失败：" + error.message); }
  finally { polling = false; }
}
setInterval(pollMatch, 1000);
setInterval(renderThinking, 250);
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
    arenaInfo = await api("/api/arena");
    $("#arena-readiness").textContent = arenaInfo.officialName + " · " + arenaInfo.message;
    if (!arenaInfo.ready) $(".protocol-note").open = true;
    await refreshSubmissions();
    const saved = new URL(location.href).searchParams.get("game") || savedGameId();
    if (saved) {
      try {
        setState(await api("/api/games/" + encodeURIComponent(saved)));
        setMode(gameState.arena ? "arena" : "practice");
        setMessage("已恢复上次棋局。");
      } catch (error) { if (error.status !== 404) throw error; }
    }
    if (!gameState) { setMessage("上传 AI 后开始挑战，或切换到人机练习。"); await refreshHistory(); }
    else {
      await refreshHistory();
      if (!gameState.arena && gameState.currentPlayer === gameState.aiColor) await afterMove();
    }
  } catch (error) { setMessage("初始化失败：" + error.message + "。可点击新建棋局重试。"); }
  finally { busy = false; renderState(); }
}
initialize();
