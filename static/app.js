let gameState = null;

const boardElement = document.querySelector("#board");
const gameStatusElement = document.querySelector("#game-status");
const turnStatusElement = document.querySelector("#turn-status");
const gameIdElement = document.querySelector("#game-id");
const currentPlayerElement = document.querySelector("#current-player");
const remainingStonesElement = document.querySelector("#remaining-stones");
const messageElement = document.querySelector("#message");
const exportLink = document.querySelector("#export-game");
const analyzeButton = document.querySelector("#analyze-game");
const analysisElement = document.querySelector("#analysis");

function playerName(player) {
  return player === "black" ? "黑方" : player === "white" ? "白方" : "-";
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>\"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "\"": "&quot;",
    "'": "&#39;",
  }[character]));
}

function setMessage(message) {
  messageElement.textContent = message;
}

function renderBoard() {
  boardElement.replaceChildren();
  if (!gameState) return;

  for (let row = 0; row < 19; row += 1) {
    for (let col = 0; col < 19; col += 1) {
      const cell = document.createElement("button");
      const player = gameState.board[row][col];
      cell.className = "cell";
      cell.type = "button";
      cell.title = `第 ${row + 1} 行，第 ${col + 1} 列`;
      cell.dataset.row = row;
      cell.dataset.col = col;
      if (player) {
        const stone = document.createElement("i");
        stone.className = `stone ${player}`;
        cell.append(stone);
      }
      cell.addEventListener("click", () => handleCellClick(row, col));
      boardElement.append(cell);
    }
  }
}

function renderState() {
  if (!gameState) return;
  renderBoard();
  gameIdElement.textContent = gameState.id;
  currentPlayerElement.textContent = playerName(gameState.currentPlayer);
  remainingStonesElement.textContent = gameState.remainingStones;
  gameStatusElement.textContent = gameState.status === "active"
    ? "进行中"
    : gameState.status === "draw" ? "和棋" : `${playerName(gameState.winner)}获胜`;
  turnStatusElement.textContent = gameState.status === "active"
    ? `${playerName(gameState.currentPlayer)}还需落 ${gameState.remainingStones} 子`
    : "本局结束";
  exportLink.href = `/api/games/${gameState.id}/export`;
  exportLink.classList.remove("disabled");
  analyzeButton.disabled = gameState.moves.length === 0;
}

function renderAnalysis(result) {
  const analysis = result.analysis || {};
  const mistakes = (analysis.keyMistakes || [])
    .map((item) => `<li>第 ${escapeHtml(item.moveNo ?? "?")} 手：${escapeHtml(item.reason || "未提供原因")}。${escapeHtml(item.suggestion || "")}</li>`)
    .join("");
  const strengths = (analysis.strengths || []).map((item) => `<li>${escapeHtml(item)}</li>`).join("");
  const practice = (analysis.nextPractice || []).map((item) => `<li>${escapeHtml(item)}</li>`).join("");
  analysisElement.innerHTML = `
    <h3>Agent 总结</h3><p>${escapeHtml(analysis.summary || "暂无总结")}</p>
    <h3>关键问题</h3><ul>${mistakes || "<li>没有返回关键问题</li>"}</ul>
    <h3>做得好的地方</h3><ul>${strengths || "<li>没有返回优点</li>"}</ul>
    <h3>下一步练习</h3><ul>${practice || "<li>没有返回练习建议</li>"}</ul>`;
  analysisElement.hidden = false;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || "请求失败");
  return data;
}

async function newGame() {
  try {
    gameState = await api("/api/games", { method: "POST", body: "{}" });
    renderState();
    setMessage("轮到你执黑。首回合只需落 1 子。");
  } catch (error) {
    setMessage(error.message);
  }
}

async function handleCellClick(row, col) {
  if (!gameState || gameState.status !== "active" || gameState.currentPlayer !== "black") {
    return;
  }
  try {
    gameState = await api(`/api/games/${gameState.id}/moves`, {
      method: "POST",
      body: JSON.stringify({ row, col }),
    });
    renderState();
    if (gameState.status !== "active") {
      setMessage("这局已经结束，可以导出棋谱。");
      return;
    }
    if (gameState.currentPlayer === "white") {
      setMessage("基线 AI 思考中……");
      await aiTurn();
    } else {
      setMessage("同一回合还需要再下一子。");
    }
  } catch (error) {
    setMessage(error.message);
  }
}

async function aiTurn() {
  try {
    gameState = await api(`/api/games/${gameState.id}/ai-turn`, {
      method: "POST",
      body: "{}",
    });
    renderState();
    setMessage(gameState.status === "active" ? "轮到你执黑。" : "这局已经结束，可以导出棋谱。");
  } catch (error) {
    setMessage(error.message);
  }
}

document.querySelector("#new-game").addEventListener("click", newGame);
analyzeButton.addEventListener("click", async () => {
  if (!gameState) return;
  analyzeButton.disabled = true;
  setMessage("Agent 正在读取棋谱并生成分析……");
  try {
    const result = await api(`/api/games/${gameState.id}/analysis`, { method: "POST", body: "{}" });
    renderAnalysis(result);
    setMessage("分析完成。请对照棋谱检查 Agent 的结论。");
  } catch (error) {
    setMessage(error.message);
  } finally {
    analyzeButton.disabled = false;
  }
});
newGame();
