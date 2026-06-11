"""Root conftest.py — adds the repo root to sys.path so `import daemon.*` resolves."""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
