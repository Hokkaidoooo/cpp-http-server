"""用固定历史版本和真实编译器复现分段正文被提前解析的问题。"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "afa204633aa1c7df12128786e663042a9e7be37e"
SOURCE = r'''
#include "HttpRequest.h"
#include <iostream>
int main() {
    HttpRequest request;
    const bool accepted = request.parse(
        "POST /login HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 26\r\n\r\nusername=demo");
    std::cout << (accepted ? "premature_accept" : "rejected") << std::endl;
    return accepted ? 0 : 1;
}
'''


def main():
    header = subprocess.check_output(
        ["git", "show", f"{BASELINE}:HttpRequest.h"], cwd=ROOT
    )
    compiler = os.environ.get("CXX", "g++")
    with tempfile.TemporaryDirectory(prefix="cpp-http-baseline-") as folder:
        path = Path(folder)
        (path / "HttpRequest.h").write_bytes(header)
        (path / "baseline.cpp").write_text(SOURCE, encoding="utf-8")
        executable = path / ("baseline.exe" if os.name == "nt" else "baseline")
        subprocess.run(
            [compiler, "-std=c++11", str(path / "baseline.cpp"), "-o", str(executable)],
            check=True,
        )
        result = subprocess.check_output([str(executable)], text=True).strip()
        assert result == "premature_accept", result
        print(json.dumps({"baseline_commit": BASELINE, "declared_body_bytes": 26,
                          "received_body_bytes": 13, "result": result,
                          "temporary_directory_removed_on_exit": True}))


if __name__ == "__main__":
    main()
