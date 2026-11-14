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
    html = html.replace("</nav>", '<a class="client-download" href="https://github.com/zhehong268/liuziqi/releases/latest" '
                        'target="_blank" rel="noopener">下载客户端</a></nav>', 1)
    css = (OUTPUT / "styles.css").read_text(encoding="utf-8").replace("/static/", "./")
    css += '\nbody[data-edition="browser"] #mode-arena, body[data-edition="browser"] #arena-panel { display: none !important; }\n'
    css += '.client-download { padding: 8px 12px; color: var(--muted); text-decoration: none; font: 15px var(--serif); }\n'
    (OUTPUT / "styles.css").write_text(css, encoding="utf-8")
    for name in ("styles.css", "engine.js", "browser-api.js", "app.js"):
        revision = hashlib.sha256((OUTPUT / name).read_bytes()).hexdigest()[:12]
        html = html.replace('="./' + name + '"', '="./' + name + '?v=' + revision + '"')
    (OUTPUT / "index.html").write_text(html, encoding="utf-8")
    (OUTPUT / ".nojekyll").touch()
    print(OUTPUT)


if __name__ == "__main__":
    build()
