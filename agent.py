from __future__ import annotations

import json
import os
from typing import Any

from dotenv import load_dotenv


load_dotenv()


def local_analysis(moves: list[dict[str, Any]], winner: str | None) -> dict[str, Any]:
    """A deterministic, no-network Agent for local demos and offline judging."""
    if not moves:
        return {
            "provider": "local-rules",
            "summary": "棋局还没有落子。",
            "keyMistakes": [],
            "strengths": [],
            "nextPractice": ["先练习围绕中心区域建立连续棋形。"],
        }

    edge_moves = []
    for move in moves:
        distance = abs(move["row"] - 9) + abs(move["col"] - 9)
        if distance >= 14:
            edge_moves.append(
                {
                    "moveNo": move["moveNo"],
                    "reason": "这一步距离中心较远，短期内能参与的连线较少。",
                    "suggestion": "下一步优先检查中心附近的进攻和封堵点。",
                }
            )
        if len(edge_moves) >= 3:
            break

    winner_text = {"black": "黑方", "white": "白方"}.get(winner or "", "暂未结束")
    return {
        "provider": "local-rules",
        "summary": f"本局共记录 {len(moves)} 次落子，当前结果：{winner_text}。",
        "keyMistakes": edge_moves,
        "strengths": ["棋谱已经按顺序保存，可以继续做逐手分析。"],
        "nextPractice": [
            "每次落子前同时检查自己的连线和对手的连续威胁。",
            "练习把每回合的两颗棋放在同一个进攻计划中。",
        ],
    }


def analyze_game(moves: list[dict[str, Any]], winner: str | None) -> dict[str, Any]:
    """Ask the model for a short, reviewable analysis of a completed or ongoing game."""
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        return local_analysis(moves, winner)

    model = os.getenv("OPENAI_MODEL") or "gpt-5.5"
    move_text = "\n".join(
        f"{move['moveNo']}. {move['player']} ({move['row'] + 1},{move['col'] + 1})"
        for move in moves
    )
    prompt = f"""
你是六子棋教练。棋盘是 19×19，黑方首回合 1 子，之后每回合 2 子，连成 6 子获胜。
请根据下面的棋谱，指出最多三个关键问题，并给出适合初学者的练习建议。
当前结果：{winner or '对局进行中'}
棋谱：
{move_text or '暂无落子'}

只返回 JSON，不要 Markdown 代码围栏。JSON 格式必须是：
{{
  "summary": "一句话总结",
  "keyMistakes": [{{"moveNo": 1, "reason": "原因", "suggestion": "改进建议"}}],
  "strengths": ["做得好的地方"],
  "nextPractice": ["下一步练习"]
}}
""".strip()

    from openai import OpenAI

    client = OpenAI(api_key=api_key)
    response = client.responses.create(
        model=model,
        instructions="你是一名耐心、具体、不会编造棋谱事实的教练。只分析输入中出现的落子。",
        input=prompt,
    )
    raw_text = response.output_text.strip()
    try:
        result = json.loads(raw_text)
    except json.JSONDecodeError:
        result = {
            "provider": "openai",
            "summary": raw_text,
            "keyMistakes": [],
            "strengths": [],
            "nextPractice": [],
        }
    if not isinstance(result, dict):
        raise ValueError("AI response must be a JSON object")
    result.setdefault("provider", "openai")
    return result
