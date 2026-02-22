#!/usr/bin/env python3

"""Compatibility wrapper.

The implementation moved to scripts/models/generate_mock_model_onnx.py.
"""

from __future__ import annotations

import pathlib
import runpy


def main() -> None:
  script = pathlib.Path(__file__).resolve().parent / "models" / "generate_mock_model_onnx.py"
  runpy.run_path(str(script), run_name="__main__")


if __name__ == "__main__":
  main()
