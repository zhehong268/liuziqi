"""源码运行与 Windows 安装版共用的资源、数据目录。"""
from pathlib import Path
import os
import sys

FROZEN = bool(getattr(sys, "frozen", False))
RESOURCE_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
if os.getenv("CONNECT6_DATA_DIR"):
    DATA_DIR = Path(os.environ["CONNECT6_DATA_DIR"]).expanduser().resolve()
elif FROZEN:
    DATA_DIR = Path(os.getenv("LOCALAPPDATA", str(Path.home()))) / "MoYi" / "data"
else:
    DATA_DIR = RESOURCE_DIR / "data"
ENV_PATH = DATA_DIR.parent / ".env" if FROZEN else RESOURCE_DIR / ".env"
