"""ESPHome config validation: the examples pass, wrong configs fail with a clear message.

    python3 -m pytest tests/config      (needs `pip install esphome pytest`)
"""
import pathlib
import subprocess
import textwrap

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXAMPLES = sorted((ROOT / "examples").glob("*.yaml"))

HEAD = f"""
esphome:
  name: t
esp32:
  board: esp32dev
external_components:
  - source:
      type: local
      path: {ROOT / "components"}
sensor:
  - platform: template
    id: v
    lambda: return 3.8;
  - platform: template
    id: i
    lambda: return 0.1;
"""


def esphome_config(path: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(["esphome", "config", str(path)], capture_output=True, text=True, timeout=300)


@pytest.mark.parametrize("example", EXAMPLES, ids=lambda p: p.name)
def test_examples_valid(example):
    r = esphome_config(example)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]


GOOD = {
    "voltage_only": "li_battery:\n  voltage: v\n",
    "no_input_process_voltage": "li_battery:\n  id: b\n  chemistry: liion\n",
    "current_and_power": "li_battery:\n  voltage: v\n  current: i\n  power: i\n",
    "lifepo4_4s": "li_battery:\n  voltage: v\n  chemistry: lifepo4\n  cells_series: 4\n",
    "custom_curve": textwrap.dedent("""\
        li_battery:
          voltage: v
          chemistry: custom
          curve:
            - {voltage: 3.0V, percent: 0}
            - {voltage: 4.2V, percent: 100}
        """),
}

BAD = {
    "custom_without_curve": ("li_battery:\n  voltage: v\n  chemistry: custom\n", "needs a 'curve'"),
    "curve_not_custom": (
        "li_battery:\n  voltage: v\n  curve:\n    - {voltage: 3.0V, percent: 0}\n    - {voltage: 4.2V, percent: 100}\n",
        "only used with chemistry: custom",
    ),
    "curve_not_increasing": (
        "li_battery:\n  voltage: v\n  chemistry: custom\n  curve:\n    - {voltage: 4.0V, percent: 0}\n"
        "    - {voltage: 3.0V, percent: 100}\n",
        "strictly increasing",
    ),
    "unknown_chemistry": ("li_battery:\n  voltage: v\n  chemistry: nimh\n", "nimh"),
    "zero_cells": ("li_battery:\n  voltage: v\n  cells_series: 0\n", "cells_series"),
    "voltage_not_a_sensor": ("li_battery:\n  voltage: nope\n", "nope"),
}


@pytest.mark.parametrize("name", GOOD)
def test_good(tmp_path, name):
    f = tmp_path / "c.yaml"
    f.write_text(HEAD + GOOD[name])
    r = esphome_config(f)
    assert r.returncode == 0, r.stdout[-2000:]


@pytest.mark.parametrize("name", BAD)
def test_bad(tmp_path, name):
    body, needle = BAD[name]
    f = tmp_path / "c.yaml"
    f.write_text(HEAD + body)
    r = esphome_config(f)
    assert r.returncode != 0
    assert needle in (r.stdout + r.stderr), (r.stdout + r.stderr)[-2000:]
