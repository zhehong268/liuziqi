"""生成不包含后端、密钥或本地对局的 GitHub Pages 目录。"""
from pathlib import Path
import hashlib
import shutil

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "dist" / "pages"


def build():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / "static").iterdir():
        if source.is_file():
            shutil.copy2(source, OUTPUT / source.name)
    for name in ("engine.js", "browser-api.js"):
        shutil.copy2(ROOT / "web" / name, OUTPUT / name)
    html = (OUTPUT / "index.html").read_text(encoding="utf-8").replace("/static/", "./")
    html = html.replace('data-mode="arena"', 'data-mode="practice" data-edition="browser"')
    needle = '<script src="./app.js" defer></script>'
    assert html.count(needle) == 1
    html = html.replace(needle, '<script src="./engine.js" defer></script>\n  '
                        '<script src="./browser-api.js" defer></script>\n  ' + needle)
    html = html.replace("</nav>", '<a class="client-download" href="./downloads/MoYi-Setup-Windows-x64.exe" '
                        'download="墨弈-六子棋-Windows安装包.exe" title="Windows 10 / 11，64 位">下载安装包</a></nav>', 1)
    css = (OUTPUT / "styles.css").read_text(encoding="utf-8").replace("/static/", "./")
    css += '\nbody[data-edition="browser"] #mode-arena, body[data-edition="browser"] #arena-panel { display: none !important; }\n'
    css += '.client-download { padding: 8px 12px; color: var(--paper); background: var(--ink); border: 1px solid var(--ink); text-decoration: none; white-space: nowrap; font: 15px/1.4 var(--serif); }\n'
    css += '.client-download:hover { color: var(--paper); background: #504f4d; }\n'
    css += '@media (max-width: 480px) { body[data-edition="browser"] .masthead { gap: 14px; } body[data-edition="browser"] .mode-tabs { gap: 12px; } .client-download { padding: 8px 10px; font-size: 13px; } }\n'
    (OUTPUT / "styles.css").write_text(css, encoding="utf-8")
    for name in ("styles.css", "engine.js", "browser-api.js", "app.js", "downloads/MoYi-Setup-Windows-x64.exe"):
        if not (OUTPUT / name).is_file():
            continue
        revision = hashlib.sha256((OUTPUT / name).read_bytes()).hexdigest()[:12]
        html = html.replace('="./' + name + '"', '="./' + name + '?v=' + revision + '"')
    (OUTPUT / "index.html").write_text(html, encoding="utf-8")
    (OUTPUT / ".nojekyll").touch()
    print(OUTPUT)


if __name__ == "__main__":
    build()
