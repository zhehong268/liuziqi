from __future__ import annotations

import json
import os
from typing import Any

from dotenv import load_dotenv
from openai import OpenAI


load_dotenv()


def analyze_game(moves: list[dict[str, Any]], winner: str | None) -> dict[str, Any]:
    """Ask the model for a short, reviewable analysis of a completed or ongoing game."""
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not configured")

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
            "summary": raw_text,
            "keyMistakes": [],
            "strengths": [],
            "nextPractice": [],
        }
    if not isinstance(result, dict):
        raise ValueError("AI response must be a JSON object")
    return result
