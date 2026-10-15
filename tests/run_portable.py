"""使用现有 C++ 编译器运行独立协议测试，退出即清理本轮临时产物。"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="cpp-http-parser-") as folder:
    binary = Path(folder) / ("parser.exe" if os.name == "nt" else "parser")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wpedantic", "-Werror", "-I", str(ROOT),
                    str(ROOT / "tests" / "test_parser.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
