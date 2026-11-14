"""固定流程 Agent：读取状态 → 调用棋形工具 → 模型/规则决策 → 校验 → 返回可保存的结果。"""
from __future__ import annotations

import json
import os
from pathlib import Path

from dotenv import load_dotenv
from bot import BOARD_SIZE, choose_ai_move, current_player, has_won, legal_cells, winning_cells

load_dotenv(Path(__file__).resolve().parent / ".env")

MOVE_SCHEMA = {"type": "object", "properties": {
    "row": {"type": "integer"}, "col": {"type": "integer"}, "reason": {"type": "string"}},
    "required": ["row", "col", "reason"], "additionalProperties": False}
ANALYSIS_SCHEMA = {"type": "object", "properties": {
    "summary": {"type": "string"},
    "keyMistakes": {"type": "array", "items": {"type": "object", "properties": {
        "moveNo": {"type": "integer"}, "reason": {"type": "string"}, "suggestion": {"type": "string"}},
        "required": ["moveNo", "reason", "suggestion"], "additionalProperties": False}},
    "strengths": {"type": "array", "items": {"type": "string"}},
    "nextPractice": {"type": "array", "items": {"type": "string"}}},
    "required": ["summary", "keyMistakes", "strengths", "nextPractice"], "additionalProperties": False}


def model_configured() -> bool:
    return bool(os.getenv("OPENAI_API_KEY", "").strip() and os.getenv("OPENAI_MODEL", "").strip())


def call_model_json(prompt: str, schema: dict, name: str) -> dict:
    """Responses 用 JSON Schema；兼容 Chat 接口用 JSON mode，业务层再次校验。"""
    if not model_configured():
        raise ValueError("缺少 API Key 或模型名")
    from openai import OpenAI
    try:
        timeout = min(60.0, max(1.0, float(os.getenv("AI_TIMEOUT_SECONDS", "20"))))
    except ValueError:
        timeout = 20.0
    with OpenAI(api_key=os.environ["OPENAI_API_KEY"],
                base_url=os.getenv("OPENAI_BASE_URL") or None,
                timeout=timeout, max_retries=0) as client:
        style = os.getenv("AI_API_STYLE", "responses").strip()
        if style == "responses":
            response = client.responses.create(
                model=os.environ["OPENAI_MODEL"], store=False,
                instructions="你是六子棋教练。只根据输入事实作答，坐标从 0 开始，输出 JSON。",
                input=prompt, max_output_tokens=2400,
                text={"format": {"type": "json_schema", "name": name, "schema": schema, "strict": True}})
            if response.status != "completed":
                raise ValueError("模型没有返回完整结果")
            raw = response.output_text
        elif style == "chat":
            response = client.chat.completions.create(
                model=os.environ["OPENAI_MODEL"],
                messages=[{"role": "system", "content": "你是六子棋教练。输出符合以下结构的 JSON：" + json.dumps(schema)},
                          {"role": "user", "content": prompt}],
                response_format={"type": "json_object"})
            if not response.choices or response.choices[0].finish_reason != "stop":
                raise ValueError("模型没有返回完整结果")
            raw = response.choices[0].message.content or ""
        else:
            raise ValueError("AI_API_STYLE 必须为 responses 或 chat")
    result = json.loads(raw)
    if not isinstance(result, dict):
        raise ValueError("模型结果必须是 JSON 对象")
    return result


def failure_notice(error: Exception) -> str:
    # 不把供应商原始异常送到浏览器，其中可能带有请求细节或认证信息。
    kind = type(error).__name__
    if kind == "AuthenticationError":
        return "模型鉴权失败，请检查服务端 API Key。已使用本地规则。"
    if kind == "RateLimitError":
        return "模型额度或请求频率受限，已使用本地规则。"
    if "Timeout" in kind:
        return "模型请求超时，已使用本地规则。"
    return "模型调用失败或返回格式/落点无效，已使用本地规则；请检查模型名、接口类型和 Base URL。"


def choose_agent_move(board, player="white", opponent="local") -> dict:
    cells = legal_cells(board)
    if opponent == "model":
        try:
            result = call_model_json(
                "规则：19×19，连成六子或以上获胜。本次只下一颗子。你执" + player +
                "。棋盘 null 为空，black 为黑，white 为白。只能从合法落点选一个，简述原因。\n" +
                json.dumps({"board": board, "legalCells": cells}, ensure_ascii=False), MOVE_SCHEMA, "connect6_move")
            row, col = result.get("row"), result.get("col")
            if type(row) is not int or type(col) is not int or (row, col) not in cells:
                raise ValueError("模型落点不合法")
            if not isinstance(result.get("reason"), str):
                raise ValueError("缺少落子原因")
            return {"row": row, "col": col, "source": "model", "reason": result["reason"][:500]}
        except Exception as error:
            notice = failure_notice(error)
    else:
        notice = "优先直接成六、封堵对手，再延长棋形并靠近中心。"
    row, col = choose_ai_move(board, player)
    return {"row": row, "col": col, "source": "local-fallback" if opponent == "model" else "local",
            "reason": notice}


def local_analysis(moves: list[dict], winner: str | None, status="active") -> dict:
    """重建棋谱，检测黑方完整回合结束后的直接漏防，不把第一颗子的中间状态判成失误。"""
    board = [[None] * BOARD_SIZE for _ in range(BOARD_SIZE)]
    mistakes = []
    for index, move in enumerate(moves):
        row, col, player = move["row"], move["col"], move["player"]
        board[row][col] = player
        if player != "black" or len(mistakes) >= 3 or has_won(board, row, col, player):
            continue
        if current_player(index + 1) != "black":
            threats = winning_cells(board, "white")
            if threats:
                r, c = threats[0]
                mistakes.append({"moveNo": move["moveNo"],
                    "reason": f"黑方本回合结束时，白方可在第 {r+1} 行第 {c+1} 列直接连成六子。",
                    "suggestion": "落完本回合两颗子前，检查四个方向，优先消除直接成六威胁。"})
        if index == 0 and abs(row - 9) + abs(col - 9) >= 14:
            mistakes.append({"moveNo": move["moveNo"], "reason": "首子靠近棋盘边角，可延伸的方向较少。",
                             "suggestion": "可以尝试从中心区域开局；这是布局建议，不代表此手必败。"})
    result_text = {"black": "黑方获胜", "white": "白方获胜"}.get(winner, "和棋" if status == "draw" else "仍在进行")
    return {"provider": "local-rules", "summary": f"本局 {len(moves)} 颗子，{result_text}。本地复盘重点检查黑方的直接漏防与开局位置。",
            "keyMistakes": mistakes[:3],
            "strengths": (["黑方已完成六连或以上连线。"] if winner == "black" else
                          ["尚未检测到完整回合结束后的直接漏防；这不代表没有其他战术问题。"] if moves and not mistakes else []),
            "nextPractice": ["先找自己能直接成六的位置，再检查对手下一颗能成六的位置。",
                             "把每回合两颗子作为一组规划；本版不搜索多回合战术。"],
            "trace": ["读取已保存棋谱", f"重建 {len(moves)} 颗子的局面", "检查完整回合后的直接威胁", "生成结构化练习建议"]}


def validate_analysis(result: dict, moves: list[dict]) -> None:
    if not isinstance(result.get("summary"), str):
        raise ValueError("缺少总结")
    for field in ("strengths", "nextPractice"):
        if not isinstance(result.get(field), list) or not all(isinstance(s, str) for s in result[field]):
            raise ValueError("建议格式无效")
    if not isinstance(result.get("keyMistakes"), list):
        raise ValueError("问题格式无效")
    for item in result["keyMistakes"]:
        if not isinstance(item, dict) or type(item.get("moveNo")) is not int or not 1 <= item["moveNo"] <= len(moves):
            raise ValueError("模型引用了不存在的手数")
        if not all(isinstance(item.get(key), str) for key in ("reason", "suggestion")):
            raise ValueError("问题格式无效")


def analyze_game(moves: list[dict], winner: str | None, status="active") -> dict:
    evidence = local_analysis(moves, winner, status)
    if not model_configured() or not moves:
        return evidence
    try:
        result = call_model_json(
            "规则：19×19，黑方首回合一颗子，此后每回合两颗，六连或以上获胜。分析黑方的不足，最多三个关键问题；"
            "标出实际手数并给出可操作建议。局面未结束时不能宣称胜负。区分工具已验证的直接威胁与布局判断，"
            "不要把同一回合两颗子之间的状态当成最终漏防。\n" +
            json.dumps({"moves": moves, "status": status, "winner": winner, "toolEvidence": evidence}, ensure_ascii=False),
            ANALYSIS_SCHEMA, "connect6_review")
        validate_analysis(result, moves)
        result.update({"provider": "model", "trace": evidence["trace"][:3] + ["调用大模型解释棋谱", "校验结果结构和引用手数"]})
        return result
    except Exception as error:
        evidence["notice"] = failure_notice(error)
        evidence["trace"].append("模型失败，保留本地工具分析")
        return evidence
