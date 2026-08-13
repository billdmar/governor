"""pytest bootstrap: put the host package root on sys.path."""

from __future__ import annotations

import sys
from pathlib import Path

_HOST_ROOT = Path(__file__).resolve().parents[1]
if str(_HOST_ROOT) not in sys.path:
    sys.path.insert(0, str(_HOST_ROOT))
