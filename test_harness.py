"""
Truma iNetBox ESPHome Comprehensive End-to-End Test Harness
Connects directly to the ESP32-S3 via ESPHome native API (no HA).
Exercises every command, entity, and feature with validation.
"""

import argparse
import asyncio
import math
import sys
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple

from aioesphomeapi import (
    APIClient,
    BinarySensorInfo,
    BinarySensorState,
    ClimateFanMode,
    ClimateInfo,
    ClimateMode,
    ClimateState,
    NumberInfo,
    NumberState,
    SelectInfo,
    SelectState,
    SensorInfo,
    SensorState,
    SwitchInfo,
    SwitchState,
    TextSensorInfo,
    TextSensorState,
)

# ───────────────────────── Device Under Test ─────────────────────────
DEVICE_HOST = "YOUR_DEVICE_IP"       # e.g. "192.168.4.29"
DEVICE_PORT = 6053
NOISE_PSK = "YOUR_NOISE_PSK"         # from secrets.yaml api_encryption_key

# ───────────────────────── Expected entity counts ─────────────────────
EXPECTED_COUNTS = {
    "BinarySensor": 7,
    "Sensor": 13,
    "TextSensor": 2,
    "Climate": 3,
    "Number": 2,
    "Select": 4,
    "Switch": 4,
}
EXPECTED_TOTAL = sum(EXPECTED_COUNTS.values())  # 35

# ───────────────────────── Valid enum values for assertions ────────────
VALID_HEATING_MODES = {0, 1, 2, 3, 10, 11}         # OFF, ECO, VARIO_NIGHT, VARIO_AUTO, HIGH, BOOST
VALID_POWER_LEVELS = {0, 900, 1800}
VALID_ENERGY_MIX = {0, 1, 2, 3}                    # NONE, GAS, ELECTRICITY, MIX
VALID_OPERATING_STATUS = {0, 1, 4, 5, 6, 7, 8, 9}
VALID_ENERGY_MODE_TEXTS = {"OFF", "PROPANE", "EL_1", "EL_2", "MIX_1", "MIX_2", "EL_0", "MIX_0", "N/A"}
VALID_OP_STATUS_PREFIXES = {"OFF", "WARNING", "START_OR_COOL_DOWN", "ON (Stage", "N/A", "UNKNOWN"}


# ═══════════════════════ Result Types ═══════════════════════
class TestResult(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    WARN = "WARN"
    SKIP = "SKIP"


@dataclass
class TestOutcome:
    name: str
    result: TestResult
    message: str
    duration_seconds: float = 0.0


# ═══════════════════════ Logging ═══════════════════════
_log_lines: List[str] = []


_live_log = open("test_live.log", "w", buffering=1)  # line-buffered

def log(msg: str):
    ts = time.strftime("%H:%M:%S")
    ms = f"{time.time() % 1:.3f}"[1:]
    line = f"[{ts}{ms}] {msg}"
    print(line, flush=True)
    _log_lines.append(line)
    _live_log.write(line + "\n")


# ═══════════════════════ StateTracker ═══════════════════════
@dataclass
class StateChange:
    timestamp: float
    state: Any


def _ckey(key: int, etype) -> Tuple[int, str]:
    """Composite key: (entity_key, type_name) to handle key collisions across types."""
    return (key, type(etype).__name__)


def _state_type_name(state) -> str:
    """Map state class to info class name for composite key lookup."""
    mapping = {
        ClimateState: "ClimateInfo",
        SensorState: "SensorInfo",
        BinarySensorState: "BinarySensorInfo",
        NumberState: "NumberInfo",
        SelectState: "SelectInfo",
        SwitchState: "SwitchInfo",
        TextSensorState: "TextSensorInfo",
    }
    return mapping.get(type(state), type(state).__name__)


class StateTracker:
    """Stores full typed state objects, tracks change history with timestamps.
    Uses composite keys (entity_key, type_name) to handle key collisions
    (e.g. SensorInfo and SelectInfo sharing the same numeric key)."""

    def __init__(self):
        self._entities: Dict[Tuple[int, str], Any] = {}       # (key,type) -> EntityInfo
        self._entity_names: Dict[Tuple[int, str], str] = {}   # (key,type) -> name
        self._states: Dict[Tuple[int, str], Any] = {}          # (key,type) -> state
        self._history: Dict[Tuple[int, str], List[StateChange]] = {}
        # Also keep a flat list for count_by_type and find operations
        self._all_entities: List[Any] = []

    def register_entities(self, entities: list):
        for e in entities:
            ck = (e.key, type(e).__name__)
            self._entities[ck] = e
            self._entity_names[ck] = e.name or e.object_id
            self._all_entities.append(e)

    def _ck_for_state(self, state) -> Tuple[int, str]:
        return (state.key, _state_type_name(state))

    def on_state(self, state):
        ck = self._ck_for_state(state)
        name = self._entity_names.get(ck, f"unknown:{state.key}")
        old = self._states.get(ck)
        self._states[ck] = state
        if ck not in self._history:
            self._history[ck] = []

        # Detect actual value change
        changed = False
        if old is None:
            changed = True
        elif isinstance(state, ClimateState):
            changed = (
                getattr(old, 'mode', None) != state.mode
                or getattr(old, 'target_temperature', None) != state.target_temperature
                or getattr(old, 'current_temperature', None) != state.current_temperature
                or getattr(old, 'fan_mode', None) != state.fan_mode
            )
        elif isinstance(state, (SensorState, NumberState)):
            changed = getattr(old, 'state', None) != state.state
        elif isinstance(state, BinarySensorState):
            changed = getattr(old, 'state', None) != state.state
        elif isinstance(state, SelectState):
            changed = getattr(old, 'state', None) != state.state
        elif isinstance(state, SwitchState):
            changed = getattr(old, 'state', None) != state.state
        elif isinstance(state, TextSensorState):
            changed = getattr(old, 'state', None) != state.state
        else:
            changed = True

        if changed:
            self._history[ck].append(StateChange(time.time(), state))
            if isinstance(state, ClimateState):
                val = f"mode={state.mode}, target={state.target_temperature}, fan={getattr(state, 'fan_mode', '?')}"
            elif isinstance(state, TextSensorState):
                val = state.state
            elif hasattr(state, 'state'):
                val = state.state
            else:
                val = str(state)
            log(f"  STATE: {name} = {val}")

    def find(self, name_fragment: str, etype=None):
        """Find an EntityInfo by name fragment and optional type."""
        for e in self._all_entities:
            if etype and not isinstance(e, etype):
                continue
            if name_fragment.lower() in (e.name or e.object_id).lower():
                return e
        return None

    def find_all(self, etype=None) -> list:
        """Find all entities of a given type."""
        return [e for e in self._all_entities if etype is None or isinstance(e, etype)]

    def get(self, key: int, etype=None):
        """Get current state object by entity key. If etype is provided, use composite key."""
        if etype is not None:
            # Map EntityInfo type to State type name
            info_to_state = {
                ClimateInfo: "ClimateInfo",
                SensorInfo: "SensorInfo",
                BinarySensorInfo: "BinarySensorInfo",
                NumberInfo: "NumberInfo",
                SelectInfo: "SelectInfo",
                SwitchInfo: "SwitchInfo",
                TextSensorInfo: "TextSensorInfo",
            }
            ck = (key, info_to_state.get(etype, etype.__name__))
            return self._states.get(ck)
        # Fallback: search all composite keys with this numeric key
        for ck, state in self._states.items():
            if ck[0] == key:
                return state
        return None

    def get_val(self, key: int, etype=None):
        """Get the scalar .state value."""
        s = self.get(key, etype)
        if s is None:
            return None
        return getattr(s, 'state', s)

    def changes_since(self, key: int, timestamp: float, etype=None) -> List[StateChange]:
        """Return list of state changes after a given timestamp."""
        if etype is not None:
            info_to_state = {
                ClimateInfo: "ClimateInfo",
                SensorInfo: "SensorInfo",
                BinarySensorInfo: "BinarySensorInfo",
                NumberInfo: "NumberInfo",
                SelectInfo: "SelectInfo",
                SwitchInfo: "SwitchInfo",
                TextSensorInfo: "TextSensorInfo",
            }
            ck = (key, info_to_state.get(etype, etype.__name__))
            return [c for c in self._history.get(ck, []) if c.timestamp > timestamp]
        # Fallback: merge all histories for this numeric key
        result = []
        for ck, hist in self._history.items():
            if ck[0] == key:
                result.extend(c for c in hist if c.timestamp > timestamp)
        return sorted(result, key=lambda c: c.timestamp)

    def entity_name(self, key: int) -> str:
        for ck, name in self._entity_names.items():
            if ck[0] == key:
                return name
        return f"key:{key}"

    def count_by_type(self) -> Dict[str, int]:
        counts: Dict[str, int] = {}
        for e in self._all_entities:
            tname = type(e).__name__.replace("Info", "")
            counts[tname] = counts.get(tname, 0) + 1
        return counts


# ═══════════════════════ Helpers ═══════════════════════
async def wait_for_state(
    tracker: StateTracker,
    key: int,
    predicate: Callable[[Any], bool],
    timeout: float = 15.0,
    desc: str = "",
    etype=None,
) -> bool:
    """Poll every 0.25s until predicate(state) is True or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        state = tracker.get(key, etype)
        if state is not None and predicate(state):
            return True
        await asyncio.sleep(0.25)
    name = tracker.entity_name(key)
    state = tracker.get(key, etype)
    log(f"  TIMEOUT waiting for {name}: {desc} (last={state})")
    return False


def is_nan(v) -> bool:
    try:
        return math.isnan(v)
    except (TypeError, ValueError):
        return False


# ═══════════════════════ Test Runner ═══════════════════════
outcomes: List[TestOutcome] = []


def record(name: str, result: TestResult, message: str, duration: float = 0.0):
    outcomes.append(TestOutcome(name, result, message, duration))
    symbol = {"PASS": "+", "FAIL": "!", "WARN": "~", "SKIP": "-"}[result.value]
    log(f"  [{symbol}] {name}: {message}")


# ═══════════════════════════════════════════════════════════
#                        TESTS
# ═══════════════════════════════════════════════════════════

async def test_01_connectivity(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 1: Connectivity & Discovery"""
    log("\n" + "=" * 60)
    log("TEST 1: Connectivity & Discovery")
    log("=" * 60)
    t0 = time.time()

    info = await client.device_info()
    log(f"  Device: {info.name}, ESPHome {info.esphome_version}, model={info.model}")
    record("1.1 Connection", TestResult.PASS, f"Connected to {info.name}", time.time() - t0)

    # Count entities by type
    counts = tracker.count_by_type()
    total = sum(counts.values())
    log(f"  Entity counts: {counts} (total={total})")

    if total == EXPECTED_TOTAL:
        record("1.2 Entity Count", TestResult.PASS, f"{total} entities found")
    else:
        record("1.2 Entity Count", TestResult.WARN,
               f"Expected {EXPECTED_TOTAL}, got {total}: {counts}")

    for tname, expected in EXPECTED_COUNTS.items():
        actual = counts.get(tname, 0)
        if actual != expected:
            record(f"1.2b {tname} count", TestResult.WARN,
                   f"Expected {expected}, got {actual}")

    # Wait for initial states
    await asyncio.sleep(3)

    # CP Plus alive
    cp = tracker.find("cp plus", BinarySensorInfo)
    if cp:
        ok = await wait_for_state(tracker, cp.key, lambda s: s.state is True,
                                  timeout=timeout, desc="CP Plus alive=True")
        if ok:
            record("1.3 CP Plus Alive", TestResult.PASS, "CP Plus is alive")
        else:
            record("1.3 CP Plus Alive", TestResult.FAIL, "CP Plus not alive")
    else:
        record("1.3 CP Plus Alive", TestResult.FAIL, "Entity not found")

    # Heater has error
    err = tracker.find("heater has error", BinarySensorInfo)
    if err:
        ok = await wait_for_state(tracker, err.key, lambda s: s.state is False,
                                  timeout=timeout, desc="has_error=False")
        if ok:
            record("1.4 No Heater Error", TestResult.PASS, "No error reported")
        else:
            record("1.4 No Heater Error", TestResult.FAIL, "Heater reports error")
    else:
        record("1.4 No Heater Error", TestResult.FAIL, "Entity not found")

    # Error code = 0
    ec = tracker.find("error code", SensorInfo)
    if ec:
        val = tracker.get_val(ec.key)
        if val is not None and not is_nan(val) and val == 0:
            record("1.5 Error Code Zero", TestResult.PASS, f"Error code = {val}")
        else:
            record("1.5 Error Code Zero", TestResult.WARN, f"Error code = {val}")


async def test_02_sensors(tracker: StateTracker, timeout: float):
    """Test 2: Sensor Validation (read-only)"""
    log("\n" + "=" * 60)
    log("TEST 2: Sensor Validation")
    log("=" * 60)
    t0 = time.time()

    sensors = tracker.find_all(SensorInfo)
    log(f"  Found {len(sensors)} sensors")

    aircon_names = {"aircon target", "aircon current", "aircon mode", "aircon vent"}

    for s in sensors:
        name = (s.name or s.object_id).lower()
        val = tracker.get_val(s.key)
        unit = getattr(s, 'unit_of_measurement', '') or ''
        is_aircon = any(a in name for a in aircon_names)

        if val is None:
            record(f"2 {s.name}", TestResult.FAIL, "No state received")
            continue

        if is_nan(val):
            if is_aircon or "target" in name:
                record(f"2 {s.name}", TestResult.PASS, f"NaN (acceptable when OFF)")
            else:
                record(f"2 {s.name}", TestResult.WARN, f"NaN value")
            continue

        # Range checks based on sensor name
        ok = True
        msg = f"{val} {unit}"
        if "current room temp" in name:
            ok = 0 <= val <= 50
            if not ok:
                msg = f"{val} out of range [0,50]"
        elif "current water temp" in name:
            ok = 0 <= val <= 100
            if not ok:
                msg = f"{val} out of range [0,100]"
        elif "heating mode" in name:
            ok = int(val) in VALID_HEATING_MODES
            if not ok:
                msg = f"{val} not in {VALID_HEATING_MODES}"
        elif "electric power level" in name:
            ok = int(val) in VALID_POWER_LEVELS
            if not ok:
                msg = f"{val} not in {VALID_POWER_LEVELS}"
        elif "energy mix" in name:
            ok = int(val) in VALID_ENERGY_MIX
            if not ok:
                msg = f"{val} not in {VALID_ENERGY_MIX}"
        elif "operating status" in name:
            ok = int(val) in VALID_OPERATING_STATUS
            if not ok:
                msg = f"{val} not in {VALID_OPERATING_STATUS}"
        elif "error code" in name:
            ok = val >= 0
            if not ok:
                msg = f"{val} < 0"

        record(f"2 {s.name}", TestResult.PASS if ok else TestResult.FAIL, msg)

    log(f"  Sensor validation took {time.time()-t0:.1f}s")


async def test_03_binary_sensors(tracker: StateTracker, timeout: float):
    """Test 3: Binary Sensor Validation (read-only)"""
    log("\n" + "=" * 60)
    log("TEST 3: Binary Sensor Validation")
    log("=" * 60)

    bsensors = tracker.find_all(BinarySensorInfo)
    log(f"  Found {len(bsensors)} binary sensors")

    for bs in bsensors:
        val = tracker.get_val(bs.key)
        name = bs.name or bs.object_id

        if val is None:
            record(f"3 {name}", TestResult.FAIL, "No state received")
            continue

        if not isinstance(val, bool):
            record(f"3 {name}", TestResult.FAIL, f"Not boolean: {val}")
            continue

        # Special checks
        if "cp plus" in name.lower():
            if val:
                record(f"3 {name}", TestResult.PASS, "True (alive)")
            else:
                record(f"3 {name}", TestResult.FAIL, "False (not alive)")
        elif "has error" in name.lower():
            if not val:
                record(f"3 {name}", TestResult.PASS, "False (no error)")
            else:
                record(f"3 {name}", TestResult.FAIL, "True (error!)")
        else:
            record(f"3 {name}", TestResult.PASS, f"{val}")


async def test_04_text_sensors(tracker: StateTracker, timeout: float):
    """Test 4: Text Sensor Validation (read-only)"""
    log("\n" + "=" * 60)
    log("TEST 4: Text Sensor Validation")
    log("=" * 60)

    tsensors = tracker.find_all(TextSensorInfo)
    log(f"  Found {len(tsensors)} text sensors")

    for ts in tsensors:
        val = tracker.get_val(ts.key)
        name = ts.name or ts.object_id

        if val is None:
            record(f"4 {name}", TestResult.FAIL, "No state received")
            continue

        if "energy mode" in name.lower():
            if val in VALID_ENERGY_MODE_TEXTS:
                record(f"4 {name}", TestResult.PASS, f'"{val}"')
            else:
                record(f"4 {name}", TestResult.FAIL, f'"{val}" not in valid set')
        elif "operating status" in name.lower():
            valid = any(val.startswith(p) for p in VALID_OP_STATUS_PREFIXES)
            if valid:
                record(f"4 {name}", TestResult.PASS, f'"{val}"')
            else:
                record(f"4 {name}", TestResult.FAIL, f'"{val}" not matching valid prefixes')
        else:
            record(f"4 {name}", TestResult.PASS, f'"{val}"')


async def test_05_room_climate(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 5: Room Climate Control"""
    log("\n" + "=" * 60)
    log("TEST 5: Room Climate Control")
    log("=" * 60)

    room = tracker.find("room", ClimateInfo)
    if not room:
        record("5 Room Climate", TestResult.SKIP, "Entity not found")
        return

    target_sensor = tracker.find("target room temperature (sensor)", SensorInfo)

    # 5a — Temperature sweep
    for temp in [5, 15, 20, 25, 30]:
        log(f"  Setting HEAT @ {temp}C...")
        client.climate_command(key=room.key, mode=ClimateMode.HEAT, target_temperature=float(temp))
        ok = await wait_for_state(
            tracker, room.key,
            lambda s: s.target_temperature == temp,
            timeout=timeout,
            desc=f"target=={temp}"
        )
        if ok:
            record(f"5a Room temp={temp}C", TestResult.PASS, f"Target confirmed at {temp}C")
        else:
            state = tracker.get(room.key)
            actual = state.target_temperature if state else "?"
            record(f"5a Room temp={temp}C", TestResult.FAIL, f"Target={actual}, expected {temp}")

        # Cross-check with sensor (wait for LIN bus round-trip)
        if target_sensor and ok:
            _t = temp  # capture for lambda
            sok = await wait_for_state(
                tracker, target_sensor.key,
                lambda s: s.state is not None and not is_nan(s.state) and abs(s.state - _t) < 0.5,
                timeout=timeout, desc=f"sensor~={_t}"
            )
            sval = tracker.get_val(target_sensor.key)
            if sok:
                record(f"5a Room sensor@{temp}", TestResult.PASS, f"Sensor={sval}")
            else:
                record(f"5a Room sensor@{temp}", TestResult.WARN, f"Sensor={sval}, expected ~{temp}")

    # 5b — OFF
    log("  Setting room OFF...")
    client.climate_command(key=room.key, mode=ClimateMode.OFF)
    ok = await wait_for_state(
        tracker, room.key,
        lambda s: is_nan(s.target_temperature) or s.mode == ClimateMode.OFF,
        timeout=timeout,
        desc="mode=OFF"
    )
    if ok:
        record("5b Room OFF", TestResult.PASS, "Room heater off")
    else:
        record("5b Room OFF", TestResult.FAIL, "Failed to turn off")

    # Check HEATER_ROOM binary sensor
    hr = tracker.find("room heater active", BinarySensorInfo)
    if hr:
        hrok = await wait_for_state(
            tracker, hr.key, lambda s: s.state is False,
            timeout=timeout, desc="HEATER_ROOM=False"
        )
        val = tracker.get_val(hr.key)
        if hrok:
            record("5b HEATER_ROOM=False", TestResult.PASS, "Confirmed off")
        else:
            record("5b HEATER_ROOM=False", TestResult.WARN, f"HEATER_ROOM={val}")

    # 5c — Fan modes: LOW=ECO(1), MEDIUM=HIGH(10), HIGH=BOOST(11)
    # Per Truma manual: BOOST is only available when (target - current) > 10°C.
    # We use target=20 for ECO/HIGH, and target=30 for BOOST to ensure the delta.
    # If current room temp is too high for BOOST (delta<=10), we WARN instead of FAIL.
    heating_mode_sensor = tracker.find("heating mode", SensorInfo)
    room_state = tracker.get(room.key)
    current_room = room_state.current_temperature if room_state else None

    fan_tests = [
        (ClimateFanMode.LOW, "ECO", 1, 20.0),
        (ClimateFanMode.MEDIUM, "HIGH", 10, 20.0),
        (ClimateFanMode.HIGH, "BOOST", 11, 30.0),  # max temp to maximize delta
    ]

    for fan_mode, label, expected_hm, target_temp in fan_tests:
        log(f"  Setting fan={fan_mode.name} @ {target_temp}C...")
        client.climate_command(key=room.key, mode=ClimateMode.HEAT,
                               target_temperature=target_temp, fan_mode=fan_mode)
        ok = await wait_for_state(
            tracker, room.key,
            lambda s: s.fan_mode == fan_mode,
            timeout=timeout,
            desc=f"fan_mode=={fan_mode.name}"
        )
        if ok:
            record(f"5c Fan {fan_mode.name}", TestResult.PASS, f"Fan={fan_mode.name} ({label})")
        else:
            record(f"5c Fan {fan_mode.name}", TestResult.FAIL, f"Fan mode not confirmed")

        # Cross-check heating_mode sensor (wait for LIN bus confirmation)
        if heating_mode_sensor and ok:
            _ehm = expected_hm  # capture for lambda
            hmok = await wait_for_state(
                tracker, heating_mode_sensor.key,
                lambda s: s.state is not None and not is_nan(s.state) and int(s.state) == _ehm,
                timeout=timeout, desc=f"heating_mode=={_ehm}"
            )
            hm_val = tracker.get_val(heating_mode_sensor.key)
            if hmok:
                record(f"5c HM sensor {label}", TestResult.PASS, f"heating_mode={int(hm_val)}")
            else:
                # BOOST requires target-current > 10°C per Truma manual
                if expected_hm == 11 and current_room is not None and (target_temp - current_room) <= 10:
                    record(f"5c HM sensor {label}", TestResult.WARN,
                           f"heating_mode={hm_val}, expected {expected_hm} "
                           f"(BOOST needs delta>10°C, current={current_room}°C)")
                else:
                    record(f"5c HM sensor {label}", TestResult.WARN,
                           f"heating_mode={hm_val}, expected {expected_hm}")

    # 5d — Debounce: set temp, count state changes over 10s, expect <=2
    log("  Debounce check: setting temp=22C...")
    t_mark = time.time()
    client.climate_command(key=room.key, mode=ClimateMode.HEAT, target_temperature=22.0)
    await asyncio.sleep(10)
    changes = tracker.changes_since(room.key, t_mark)
    n = len(changes)
    if n <= 2:
        record("5d Debounce", TestResult.PASS, f"{n} state changes in 10s")
    else:
        record("5d Debounce", TestResult.WARN, f"{n} state changes in 10s (expected <=2)")


async def test_06_water_climate(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 6: Water Climate Control"""
    log("\n" + "=" * 60)
    log("TEST 6: Water Climate Control")
    log("=" * 60)

    water = tracker.find("water", ClimateInfo)
    if not water:
        record("6 Water Climate", TestResult.SKIP, "Entity not found")
        return

    hw = tracker.find("water heater active", BinarySensorInfo)

    # HEAT @ 40
    log("  Setting water HEAT @ 40C...")
    client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=40.0)
    ok = await wait_for_state(
        tracker, water.key,
        lambda s: s.target_temperature == 40,
        timeout=timeout, desc="target==40"
    )
    record("6a Water 40C", TestResult.PASS if ok else TestResult.FAIL,
           f"target={'40' if ok else tracker.get(water.key)}")

    if hw:
        hwok = await wait_for_state(
            tracker, hw.key, lambda s: s.state is True,
            timeout=timeout, desc="HEATER_WATER=True"
        )
        val = tracker.get_val(hw.key)
        if hwok:
            record("6a HEATER_WATER=True", TestResult.PASS, "Water heater active")
        else:
            record("6a HEATER_WATER=True", TestResult.WARN, f"HEATER_WATER={val}")

    # Wait for LIN bus to settle before next command
    log("  Waiting for LIN bus to confirm 40C before sending 60C...")
    await asyncio.sleep(5)

    # HEAT @ 60
    log("  Setting water HEAT @ 60C...")
    client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=60.0)
    ok = await wait_for_state(
        tracker, water.key,
        lambda s: s.target_temperature == 60,
        timeout=timeout, desc="target==60"
    )
    record("6b Water 60C", TestResult.PASS if ok else TestResult.FAIL,
           f"target={'60' if ok else tracker.get(water.key)}")

    # Wait for LIN bus to settle before next command
    log("  Waiting for LIN bus to confirm 60C before sending 80C...")
    await asyncio.sleep(5)

    # HEAT @ 80 (internal 200 mapped back to 80)
    log("  Setting water HEAT @ 80C (tests 200->80 fix)...")
    client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=80.0)
    ok = await wait_for_state(
        tracker, water.key,
        lambda s: s.target_temperature == 80,
        timeout=timeout, desc="target==80 (200->80 fix)"
    )
    if ok:
        record("6c Water 80C (200 fix)", TestResult.PASS, "target=80 (200 mapped to 80)")
    else:
        state = tracker.get(water.key)
        actual = state.target_temperature if state else "?"
        record("6c Water 80C (200 fix)", TestResult.FAIL, f"target={actual}, expected 80")

    # OFF
    log("  Setting water OFF...")
    client.climate_command(key=water.key, mode=ClimateMode.OFF)
    ok = await wait_for_state(
        tracker, water.key,
        lambda s: s.mode == ClimateMode.OFF or is_nan(s.target_temperature),
        timeout=timeout, desc="mode=OFF"
    )
    record("6d Water OFF", TestResult.PASS if ok else TestResult.FAIL,
           "Water heater off" if ok else "Failed to turn off")

    if hw:
        hwok = await wait_for_state(
            tracker, hw.key, lambda s: s.state is False,
            timeout=timeout, desc="HEATER_WATER=False"
        )
        val = tracker.get_val(hw.key)
        if hwok:
            record("6d HEATER_WATER=False", TestResult.PASS, "Confirmed off")
        else:
            record("6d HEATER_WATER=False", TestResult.WARN, f"HEATER_WATER={val}")

    # Debounce: toggle OFF->HEAT@40, count changes, expect <=4
    log("  Debounce check: toggling water OFF->HEAT@40...")
    t_mark = time.time()
    client.climate_command(key=water.key, mode=ClimateMode.OFF)
    await asyncio.sleep(1)
    client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=40.0)
    await asyncio.sleep(10)
    changes = tracker.changes_since(water.key, t_mark)
    n = len(changes)
    if n <= 4:
        record("6e Water Debounce", TestResult.PASS, f"{n} changes for toggle (<=4)")
    else:
        record("6e Water Debounce", TestResult.WARN, f"{n} changes for toggle (expected <=4)")


async def test_07_number_entities(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 7: Number Entity Control"""
    log("\n" + "=" * 60)
    log("TEST 7: Number Entity Control")
    log("=" * 60)

    room_num = tracker.find("target room temperature", NumberInfo)
    water_num = tracker.find("target water temperature", NumberInfo)
    room_climate = tracker.find("room", ClimateInfo)
    water_climate = tracker.find("water", ClimateInfo)

    # Room number tests
    if room_num:
        for val in [0, 5, 20, 30]:
            log(f"  Setting room number = {val}...")
            client.number_command(key=room_num.key, state=float(val))

            if room_climate and val > 0:
                ok = await wait_for_state(
                    tracker, room_climate.key,
                    lambda s: s.target_temperature == val,
                    timeout=timeout, desc=f"climate target=={val}"
                )
                record(f"7a Room num={val}", TestResult.PASS if ok else TestResult.FAIL,
                       f"Climate target={'matched' if ok else 'mismatch'}")
            elif val == 0:
                await asyncio.sleep(5)
                record(f"7a Room num=0 (OFF)", TestResult.PASS, "Set to OFF")
            else:
                await asyncio.sleep(5)
                record(f"7a Room num={val}", TestResult.PASS, "Set (no climate cross-check)")

        # Restore
        log("  Restoring room number to 0...")
        client.number_command(key=room_num.key, state=0.0)
        await asyncio.sleep(3)
    else:
        record("7a Room Number", TestResult.SKIP, "Entity not found")

    # Water number tests
    if water_num:
        for val in [0, 40, 60, 80]:
            log(f"  Setting water number = {val}...")
            client.number_command(key=water_num.key, state=float(val))

            if water_climate and val > 0:
                ok = await wait_for_state(
                    tracker, water_climate.key,
                    lambda s: s.target_temperature == val,
                    timeout=timeout, desc=f"climate target=={val}"
                )
                record(f"7b Water num={val}", TestResult.PASS if ok else TestResult.FAIL,
                       f"Climate target={'matched' if ok else 'mismatch'}")
            elif val == 0:
                await asyncio.sleep(5)
                record(f"7b Water num=0 (OFF)", TestResult.PASS, "Set to OFF")
            else:
                await asyncio.sleep(5)
                record(f"7b Water num={val}", TestResult.PASS, "Set (no climate cross-check)")

        # Restore
        log("  Restoring water number to 0...")
        client.number_command(key=water_num.key, state=0.0)
        await asyncio.sleep(3)
    else:
        record("7b Water Number", TestResult.SKIP, "Entity not found")


async def test_08_energy_mix(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 8: Energy Mix Select"""
    log("\n" + "=" * 60)
    log("TEST 8: Energy Mix Select")
    log("=" * 60)

    # Allow previous heater commands to clear the LIN bus
    await asyncio.sleep(5)

    emix = tracker.find("energy mix", SelectInfo)
    if not emix:
        record("8 Energy Mix", TestResult.SKIP, "Select entity not found")
        return

    emix_sensor = tracker.find("energy mix", SensorInfo)
    epower_sensor = tracker.find("electric power level", SensorInfo)

    # Save original
    orig_val = tracker.get_val(emix.key, SelectInfo)
    log(f"  Original energy mix: {orig_val}")

    # Options from YAML: Propane, Mix 1, Mix 2, Electric 1, Electric 2
    tests = [
        ("Propane",    {"mix": 1}),
        ("Mix 1",      {"mix": 3, "power": 900}),
        ("Mix 2",      {"mix": 3, "power": 1800}),
        ("Electric 1", {"mix": 2, "power": 900}),
        ("Electric 2", {"mix": 2, "power": 1800}),
    ]

    for option, expected in tests:
        log(f"  Setting energy mix = '{option}'...")
        client.select_command(key=emix.key, state=option)

        # Verify select state
        ok = await wait_for_state(
            tracker, emix.key,
            lambda s: s.state == option,
            timeout=timeout, desc=f"select=={option}",
            etype=SelectInfo,
        )

        if not ok:
            record(f"8 EMix={option}", TestResult.FAIL, "Select state not confirmed")
            continue

        # Cross-check sensors (wait for LIN bus confirmation)
        all_ok = True
        msg_parts = [f"select={option}"]

        if emix_sensor:
            _em = expected["mix"]  # capture for lambda
            emok = await wait_for_state(
                tracker, emix_sensor.key,
                lambda s: s.state is not None and not is_nan(s.state) and int(s.state) == _em,
                timeout=timeout, desc=f"mix_sensor=={_em}",
                etype=SensorInfo,
            )
            mval = tracker.get_val(emix_sensor.key, SensorInfo)
            if emok:
                msg_parts.append(f"mix_sensor={int(mval)}")
            else:
                msg_parts.append(f"mix_sensor={mval}!={expected['mix']}")
                all_ok = False

        if "power" in expected and epower_sensor:
            _ep = expected["power"]  # capture for lambda
            epok = await wait_for_state(
                tracker, epower_sensor.key,
                lambda s: s.state is not None and not is_nan(s.state) and int(s.state) == _ep,
                timeout=timeout, desc=f"power_sensor=={_ep}",
                etype=SensorInfo,
            )
            pval = tracker.get_val(epower_sensor.key, SensorInfo)
            if epok:
                msg_parts.append(f"power={int(pval)}")
            else:
                msg_parts.append(f"power={pval}!={expected['power']}")
                all_ok = False

        record(f"8 EMix={option}", TestResult.PASS if all_ok else TestResult.WARN,
               ", ".join(msg_parts))

    # Restore
    if orig_val:
        log(f"  Restoring energy mix to '{orig_val}'...")
        client.select_command(key=emix.key, state=orig_val)
        await asyncio.sleep(3)


async def test_09_electric_power(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 9: Electric Power Level Select"""
    log("\n" + "=" * 60)
    log("TEST 9: Electric Power Level Select")
    log("=" * 60)

    epower = tracker.find("electric power level", SelectInfo)
    if not epower:
        record("9 Electric Power", TestResult.SKIP, "Select entity not found")
        return

    epower_sensor = tracker.find("electric power level", SensorInfo)

    # Save original
    orig_val = tracker.get_val(epower.key)
    log(f"  Original power level: {orig_val}")

    tests = [
        ("0 W", 0),
        ("900 W", 900),
        ("1800 W", 1800),
    ]

    for option, expected_sensor in tests:
        log(f"  Setting power level = '{option}'...")
        client.select_command(key=epower.key, state=option)

        ok = await wait_for_state(
            tracker, epower.key,
            lambda s: s.state == option,
            timeout=timeout, desc=f"select=={option}"
        )

        if not ok:
            record(f"9 Power={option}", TestResult.FAIL, "Select state not confirmed")
            continue

        # Cross-check sensor (wait for LIN bus confirmation)
        if epower_sensor:
            _es = expected_sensor  # capture for lambda
            esok = await wait_for_state(
                tracker, epower_sensor.key,
                lambda s: s.state is not None and not is_nan(s.state) and int(s.state) == _es,
                timeout=timeout, desc=f"power_sensor=={_es}",
            )
            sval = tracker.get_val(epower_sensor.key)
            if esok:
                record(f"9 Power={option}", TestResult.PASS,
                       f"select={option}, sensor={int(sval)}")
            else:
                record(f"9 Power={option}", TestResult.WARN,
                       f"select={option}, sensor={sval}, expected {expected_sensor}")
        else:
            record(f"9 Power={option}", TestResult.PASS, f"select={option}")

    # Restore
    if orig_val:
        log(f"  Restoring power level to '{orig_val}'...")
        client.select_command(key=epower.key, state=orig_val)
        await asyncio.sleep(3)


async def test_10_switches(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 10: Switch Control (--full only)"""
    log("\n" + "=" * 60)
    log("TEST 10: Switch Control")
    log("=" * 60)

    hr_bin = tracker.find("room heater active", BinarySensorInfo)
    hw_bin = tracker.find("water heater active", BinarySensorInfo)
    timer_bin = tracker.find("timer active", BinarySensorInfo)

    # Activate Room Heater
    room_sw = tracker.find("activate room heater", SwitchInfo)
    if room_sw:
        # ON
        log("  Activate Room Heater ON...")
        client.switch_command(key=room_sw.key, state=True)
        if hr_bin:
            ok = await wait_for_state(tracker, hr_bin.key, lambda s: s.state is True,
                                      timeout=timeout, desc="HEATER_ROOM=True")
            if ok:
                record("10a Room SW ON", TestResult.PASS, "HEATER_ROOM=True")
                # Check defaults: target=16, heating_mode=ECO(1)
                room_climate = tracker.find("room", ClimateInfo)
                if room_climate:
                    await asyncio.sleep(3)
                    state = tracker.get(room_climate.key)
                    if state and state.target_temperature == 16:
                        record("10a Room default temp", TestResult.PASS, "target=16C")
                    else:
                        t = state.target_temperature if state else "?"
                        record("10a Room default temp", TestResult.WARN, f"target={t}, expected 16")
            else:
                record("10a Room SW ON", TestResult.FAIL, "HEATER_ROOM not True")
        else:
            await asyncio.sleep(5)
            record("10a Room SW ON", TestResult.PASS, "Command sent (no binary cross-check)")

        # OFF
        log("  Activate Room Heater OFF...")
        client.switch_command(key=room_sw.key, state=False)
        if hr_bin:
            ok = await wait_for_state(tracker, hr_bin.key, lambda s: s.state is False,
                                      timeout=timeout, desc="HEATER_ROOM=False")
            record("10b Room SW OFF", TestResult.PASS if ok else TestResult.FAIL,
                   f"HEATER_ROOM={'False' if ok else 'not False'}")
        else:
            await asyncio.sleep(5)
            record("10b Room SW OFF", TestResult.PASS, "Command sent")
    else:
        record("10a/b Room Switch", TestResult.SKIP, "Entity not found")

    # Activate Water Heater
    water_sw = tracker.find("activate water heater", SwitchInfo)
    # Filter out the (enum) variant
    water_switches = [e for e in tracker.find_all(SwitchInfo) if "activate water heater" in (e.name or "").lower()]
    water_sw = None
    water_enum_sw = None
    for ws in water_switches:
        if "(enum)" in (ws.name or "").lower():
            water_enum_sw = ws
        else:
            water_sw = ws

    if water_sw:
        log("  Activate Water Heater ON...")
        client.switch_command(key=water_sw.key, state=True)
        if hw_bin:
            ok = await wait_for_state(tracker, hw_bin.key, lambda s: s.state is True,
                                      timeout=timeout, desc="HEATER_WATER=True")
            record("10c Water SW ON", TestResult.PASS if ok else TestResult.FAIL,
                   f"HEATER_WATER={'True' if ok else 'not True'}")
            # Check default target=40
            water_climate = tracker.find("water", ClimateInfo)
            if water_climate and ok:
                await asyncio.sleep(3)
                state = tracker.get(water_climate.key)
                if state and state.target_temperature == 40:
                    record("10c Water default temp", TestResult.PASS, "target=40C")
                else:
                    t = state.target_temperature if state else "?"
                    record("10c Water default temp", TestResult.WARN, f"target={t}, expected 40")
        else:
            await asyncio.sleep(5)
            record("10c Water SW ON", TestResult.PASS, "Command sent")

        log("  Activate Water Heater OFF...")
        client.switch_command(key=water_sw.key, state=False)
        if hw_bin:
            ok = await wait_for_state(tracker, hw_bin.key, lambda s: s.state is False,
                                      timeout=timeout, desc="HEATER_WATER=False")
            record("10d Water SW OFF", TestResult.PASS if ok else TestResult.FAIL,
                   f"HEATER_WATER={'False' if ok else 'not False'}")
        else:
            await asyncio.sleep(5)
            record("10d Water SW OFF", TestResult.PASS, "Command sent")
    else:
        record("10c/d Water Switch", TestResult.SKIP, "Entity not found")

    # Activate Water Heater (enum)
    if water_enum_sw:
        log("  Activate Water Heater (enum) ON...")
        client.switch_command(key=water_enum_sw.key, state=True)
        if hw_bin:
            ok = await wait_for_state(tracker, hw_bin.key, lambda s: s.state is True,
                                      timeout=timeout, desc="HEATER_WATER=True (enum)")
            record("10e Water Enum ON", TestResult.PASS if ok else TestResult.FAIL,
                   f"HEATER_WATER={'True' if ok else 'not True'}")
        else:
            await asyncio.sleep(5)
            record("10e Water Enum ON", TestResult.PASS, "Command sent")

        log("  Activate Water Heater (enum) OFF...")
        client.switch_command(key=water_enum_sw.key, state=False)
        if hw_bin:
            ok = await wait_for_state(tracker, hw_bin.key, lambda s: s.state is False,
                                      timeout=timeout, desc="HEATER_WATER=False (enum)")
            record("10f Water Enum OFF", TestResult.PASS if ok else TestResult.FAIL,
                   f"HEATER_WATER={'False' if ok else 'not False'}")
        else:
            await asyncio.sleep(5)
            record("10f Water Enum OFF", TestResult.PASS, "Command sent")
    else:
        record("10e/f Water Enum Switch", TestResult.SKIP, "Entity not found")

    # Active Timer — allow previous heater commands to clear the LIN bus first
    await asyncio.sleep(5)
    timer_sw = tracker.find("active timer", SwitchInfo)
    if timer_sw:
        log("  Active Timer ON...")
        client.switch_command(key=timer_sw.key, state=True)
        if timer_bin:
            ok = await wait_for_state(tracker, timer_bin.key, lambda s: s.state is True,
                                      timeout=timeout, desc="TIMER_ACTIVE=True")
            record("10g Timer ON", TestResult.PASS if ok else TestResult.FAIL,
                   f"TIMER_ACTIVE={'True' if ok else 'not True'}")
        else:
            await asyncio.sleep(5)
            record("10g Timer ON", TestResult.PASS, "Command sent")

        log("  Active Timer OFF...")
        client.switch_command(key=timer_sw.key, state=False)
        if timer_bin:
            ok = await wait_for_state(tracker, timer_bin.key, lambda s: s.state is False,
                                      timeout=timeout, desc="TIMER_ACTIVE=False")
            record("10h Timer OFF", TestResult.PASS if ok else TestResult.FAIL,
                   f"TIMER_ACTIVE={'False' if ok else 'not False'}")
        else:
            await asyncio.sleep(5)
            record("10h Timer OFF", TestResult.PASS, "Command sent")
    else:
        record("10g/h Timer Switch", TestResult.SKIP, "Entity not found")


async def test_11_aircon(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 11: Aircon Control (--full only, WARN not FAIL if no physical aircon)"""
    log("\n" + "=" * 60)
    log("TEST 11: Aircon Control")
    log("=" * 60)

    aircon = tracker.find("aircon", ClimateInfo)
    if not aircon:
        record("11 Aircon Climate", TestResult.SKIP, "Entity not found")
        return

    # Climate modes — just verify no crash
    for mode in [ClimateMode.OFF, ClimateMode.COOL, ClimateMode.HEAT,
                 ClimateMode.FAN_ONLY, ClimateMode.HEAT_COOL]:
        log(f"  Setting aircon mode={mode.name}...")
        try:
            client.climate_command(key=aircon.key, mode=mode)
            await asyncio.sleep(3)
            record(f"11a Aircon {mode.name}", TestResult.PASS, "No crash")
        except Exception as e:
            record(f"11a Aircon {mode.name}", TestResult.WARN, f"Error: {e}")

    # Aircon Operating Mode select
    ac_mode = tracker.find("aircon operating mode", SelectInfo)
    if ac_mode:
        for opt in ["Off", "Ventilation", "Cooling", "Heating", "Auto"]:
            log(f"  Setting aircon op mode = '{opt}'...")
            try:
                client.select_command(key=ac_mode.key, state=opt)
                await asyncio.sleep(3)
                record(f"11b AC OpMode={opt}", TestResult.PASS, "No crash")
            except Exception as e:
                record(f"11b AC OpMode={opt}", TestResult.WARN, f"Error: {e}")
    else:
        record("11b Aircon OpMode", TestResult.SKIP, "Select not found")

    # Aircon Fan Mode select
    ac_fan = tracker.find("aircon fan mode", SelectInfo)
    if ac_fan:
        for opt in ["Low", "Mid", "High", "Night", "Auto"]:
            log(f"  Setting aircon fan mode = '{opt}'...")
            try:
                client.select_command(key=ac_fan.key, state=opt)
                await asyncio.sleep(3)
                record(f"11c AC FanMode={opt}", TestResult.PASS, "No crash")
            except Exception as e:
                record(f"11c AC FanMode={opt}", TestResult.WARN, f"Error: {e}")
    else:
        record("11c Aircon FanMode", TestResult.SKIP, "Select not found")

    # Set temperature
    log("  Setting aircon temp=20C...")
    try:
        client.climate_command(key=aircon.key, mode=ClimateMode.COOL, target_temperature=20.0)
        await asyncio.sleep(3)
        record("11d Aircon temp=20", TestResult.PASS, "No crash")
    except Exception as e:
        record("11d Aircon temp=20", TestResult.WARN, f"Error: {e}")

    # Turn off
    client.climate_command(key=aircon.key, mode=ClimateMode.OFF)
    await asyncio.sleep(2)


async def test_12_debounce_stress(client: APIClient, tracker: StateTracker, timeout: float):
    """Test 12: Debounce & Stress (--stress only)"""
    log("\n" + "=" * 60)
    log("TEST 12: Debounce & Stress")
    log("=" * 60)

    room = tracker.find("room", ClimateInfo)
    water = tracker.find("water", ClimateInfo)

    # 12a — Rapid-fire 5 room temp changes with 200ms spacing
    if room:
        log("  Rapid-fire room temp changes: 18,22,15,25,20...")
        t_mark = time.time()
        for temp in [18, 22, 15, 25, 20]:
            client.climate_command(key=room.key, mode=ClimateMode.HEAT,
                                   target_temperature=float(temp))
            await asyncio.sleep(0.2)

        log("  Waiting 15s for settle...")
        await asyncio.sleep(15)
        changes = tracker.changes_since(room.key, t_mark)
        n = len(changes)
        log(f"  {n} state changes from 5 rapid commands")
        if n <= 10:
            record("12a Rapid Room", TestResult.PASS, f"{n} changes (<=10)")
        else:
            record("12a Rapid Room", TestResult.WARN, f"{n} changes (>10)")

        # Check for bounce pattern: state flips back to old value before settling
        bounce_detected = False
        if len(changes) >= 3:
            for i in range(2, len(changes)):
                c_prev = changes[i-2]
                c_cur = changes[i]
                if isinstance(c_prev.state, ClimateState) and isinstance(c_cur.state, ClimateState):
                    if (c_prev.state.target_temperature == c_cur.state.target_temperature
                            and c_prev.state.target_temperature != changes[i-1].state.target_temperature):
                        bounce_detected = True
                        break
        if bounce_detected:
            record("12a Bounce Pattern", TestResult.WARN, "Bounce detected (old->new->old)")
        else:
            record("12a Bounce Pattern", TestResult.PASS, "No bounce pattern")
    else:
        record("12a Rapid Room", TestResult.SKIP, "Room climate not found")

    # 12b — Toggle water ON/OFF/ON rapidly
    if water:
        log("  Rapid water toggle: ON/OFF/ON...")
        t_mark = time.time()
        client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=40.0)
        await asyncio.sleep(0.3)
        client.climate_command(key=water.key, mode=ClimateMode.OFF)
        await asyncio.sleep(0.3)
        client.climate_command(key=water.key, mode=ClimateMode.HEAT, target_temperature=40.0)

        log("  Waiting 15s for settle...")
        await asyncio.sleep(15)
        changes = tracker.changes_since(water.key, t_mark)
        n = len(changes)

        # Should converge to HEAT@40
        final = tracker.get(water.key)
        converged = (final and final.mode == ClimateMode.HEAT and final.target_temperature == 40)
        if converged:
            record("12b Water Toggle", TestResult.PASS, f"{n} changes, converged to HEAT@40")
        else:
            record("12b Water Toggle", TestResult.WARN,
                   f"{n} changes, final={final.mode if final else '?'}@{final.target_temperature if final else '?'}")
    else:
        record("12b Water Toggle", TestResult.SKIP, "Water climate not found")


async def test_13_operating_status(tracker: StateTracker, timeout: float):
    """Test 13: Operating Status Check"""
    log("\n" + "=" * 60)
    log("TEST 13: Operating Status Check")
    log("=" * 60)

    raw_sensor = tracker.find("operating status (raw)", SensorInfo)
    text_sensor = tracker.find("operating status", TextSensorInfo)

    if not raw_sensor:
        record("13 OpStatus Raw", TestResult.SKIP, "Sensor not found")
        return
    if not text_sensor:
        record("13 OpStatus Text", TestResult.SKIP, "Text sensor not found")
        return

    raw_val = tracker.get_val(raw_sensor.key)
    text_val = tracker.get_val(text_sensor.key)

    log(f"  Operating status: raw={raw_val}, text='{text_val}'")

    if raw_val is None or is_nan(raw_val):
        if text_val == "N/A":
            record("13 OpStatus Consistency", TestResult.PASS, "Both NaN/N/A")
        else:
            record("13 OpStatus Consistency", TestResult.WARN,
                   f"raw=NaN but text='{text_val}'")
        return

    raw_int = int(raw_val)
    # Build expected text from raw
    expected_map = {
        0: "OFF",
        1: "WARNING",
        4: "START_OR_COOL_DOWN",
        5: "ON (Stage 5)",
        6: "ON (Stage 6)",
        7: "ON (Stage 7)",
        8: "ON (Stage 8)",
        9: "ON (Stage 9)",
    }

    expected_text = expected_map.get(raw_int)
    if expected_text is None:
        # Unknown status — text should start with "UNKNOWN"
        if text_val and text_val.startswith("UNKNOWN"):
            record("13 OpStatus Consistency", TestResult.PASS,
                   f"raw={raw_int}, text='{text_val}' (unknown status)")
        else:
            record("13 OpStatus Consistency", TestResult.WARN,
                   f"raw={raw_int}, text='{text_val}', expected UNKNOWN(*)")
    elif text_val == expected_text:
        record("13 OpStatus Consistency", TestResult.PASS,
               f"raw={raw_int} <-> text='{text_val}'")
    else:
        record("13 OpStatus Consistency", TestResult.FAIL,
               f"raw={raw_int}, text='{text_val}', expected '{expected_text}'")


# ═══════════════════════ State Save/Restore ═══════════════════════

@dataclass
class SavedState:
    room_mode: Optional[int] = None
    room_target: Optional[float] = None
    room_fan: Optional[int] = None
    water_mode: Optional[int] = None
    water_target: Optional[float] = None
    energy_mix: Optional[str] = None
    electric_power: Optional[str] = None


def save_state(tracker: StateTracker) -> SavedState:
    saved = SavedState()
    room = tracker.find("room", ClimateInfo)
    if room:
        state = tracker.get(room.key)
        if state:
            saved.room_mode = state.mode
            saved.room_target = state.target_temperature
            saved.room_fan = getattr(state, 'fan_mode', None)

    water = tracker.find("water", ClimateInfo)
    if water:
        state = tracker.get(water.key)
        if state:
            saved.water_mode = state.mode
            saved.water_target = state.target_temperature

    emix = tracker.find("energy mix", SelectInfo)
    if emix:
        saved.energy_mix = tracker.get_val(emix.key, SelectInfo)

    epower = tracker.find("electric power level", SelectInfo)
    if epower:
        saved.electric_power = tracker.get_val(epower.key, SelectInfo)

    log(f"  Saved state: room={saved.room_mode}@{saved.room_target} fan={saved.room_fan}, "
        f"water={saved.water_mode}@{saved.water_target}, "
        f"emix={saved.energy_mix}, epower={saved.electric_power}")
    return saved


async def restore_state(client: APIClient, tracker: StateTracker, saved: SavedState):
    log("\n" + "=" * 60)
    log("RESTORING ORIGINAL STATE")
    log("=" * 60)

    room = tracker.find("room", ClimateInfo)
    if room and saved.room_mode is not None:
        if saved.room_mode == ClimateMode.OFF:
            log("  Restoring room OFF...")
            client.climate_command(key=room.key, mode=ClimateMode.OFF)
        elif saved.room_target and not is_nan(saved.room_target):
            log(f"  Restoring room HEAT@{saved.room_target}...")
            kwargs = {"key": room.key, "mode": ClimateMode.HEAT,
                      "target_temperature": saved.room_target}
            if saved.room_fan is not None:
                kwargs["fan_mode"] = saved.room_fan
            client.climate_command(**kwargs)

    water = tracker.find("water", ClimateInfo)
    if water and saved.water_mode is not None:
        if saved.water_mode == ClimateMode.OFF:
            log("  Restoring water OFF...")
            client.climate_command(key=water.key, mode=ClimateMode.OFF)
        elif saved.water_target and not is_nan(saved.water_target):
            log(f"  Restoring water HEAT@{saved.water_target}...")
            client.climate_command(key=water.key, mode=ClimateMode.HEAT,
                                   target_temperature=saved.water_target)

    emix = tracker.find("energy mix", SelectInfo)
    if emix and saved.energy_mix:
        log(f"  Restoring energy mix = '{saved.energy_mix}'...")
        client.select_command(key=emix.key, state=saved.energy_mix)

    epower = tracker.find("electric power level", SelectInfo)
    if epower and saved.electric_power:
        log(f"  Restoring electric power = '{saved.electric_power}'...")
        client.select_command(key=epower.key, state=saved.electric_power)

    await asyncio.sleep(5)
    log("  State restored.")


# ═══════════════════════ Main ═══════════════════════

async def main():
    parser = argparse.ArgumentParser(description="Truma iNetBox End-to-End Test Harness")
    parser.add_argument("--no-control", action="store_true",
                        help="Read-only tests only (1-4, 13)")
    parser.add_argument("--full", action="store_true",
                        help="All tests including switches and aircon (1-13)")
    parser.add_argument("--stress", action="store_true",
                        help="Include debounce stress tests (adds test 12)")
    parser.add_argument("--test", type=str, default=None,
                        help="Run single test by name (e.g., '5' or 'room')")
    parser.add_argument("--timeout", type=int, default=15,
                        help="Per-command timeout in seconds (default 15)")
    args = parser.parse_args()
    timeout = args.timeout

    log("Truma iNetBox Comprehensive Test Harness")
    log(f"Target: {DEVICE_HOST}:{DEVICE_PORT}")
    log(f"Mode: {'no-control' if args.no_control else 'full' if args.full else 'default'}"
        f"{' +stress' if args.stress else ''}")
    log(f"Timeout: {timeout}s")

    # Connect
    client = APIClient(DEVICE_HOST, DEVICE_PORT, password="", noise_psk=NOISE_PSK)
    log("Connecting...")
    await client.connect(login=True)
    info = await client.device_info()
    log(f"Connected: {info.name} (ESPHome {info.esphome_version})")

    tracker = StateTracker()

    try:
        # Discover entities
        entities, services = await client.list_entities_services()
        tracker.register_entities(entities)
        log(f"Discovered {len(entities)} entities, {len(services)} services")

        # Subscribe to states
        client.subscribe_states(tracker.on_state)
        log("Subscribed to state updates, waiting for initial states...")
        await asyncio.sleep(4)

        # Save state before control tests
        saved = save_state(tracker)

        # Determine which tests to run
        run_readonly = True
        run_control = not args.no_control
        run_switches = args.full
        run_aircon = args.full
        run_stress = args.stress
        single = args.test

        # Test execution
        t_start = time.time()

        if single:
            # Single test mode
            test_map = {
                "1": ("connectivity", lambda: test_01_connectivity(client, tracker, timeout)),
                "2": ("sensors", lambda: test_02_sensors(tracker, timeout)),
                "3": ("binary", lambda: test_03_binary_sensors(tracker, timeout)),
                "4": ("text", lambda: test_04_text_sensors(tracker, timeout)),
                "5": ("room", lambda: test_05_room_climate(client, tracker, timeout)),
                "6": ("water", lambda: test_06_water_climate(client, tracker, timeout)),
                "7": ("number", lambda: test_07_number_entities(client, tracker, timeout)),
                "8": ("energy", lambda: test_08_energy_mix(client, tracker, timeout)),
                "9": ("power", lambda: test_09_electric_power(client, tracker, timeout)),
                "10": ("switch", lambda: test_10_switches(client, tracker, timeout)),
                "11": ("aircon", lambda: test_11_aircon(client, tracker, timeout)),
                "12": ("debounce", lambda: test_12_debounce_stress(client, tracker, timeout)),
                "13": ("status", lambda: test_13_operating_status(tracker, timeout)),
            }
            found = False
            for key, (label, fn) in test_map.items():
                if single == key or single.lower() in label:
                    await fn()
                    found = True
                    break
            if not found:
                log(f"Unknown test: {single}")
                log(f"Valid: {', '.join(f'{k}({v[0]})' for k, v in test_map.items())}")
        else:
            # Read-only tests (always run)
            if run_readonly:
                await test_01_connectivity(client, tracker, timeout)
                await test_02_sensors(tracker, timeout)
                await test_03_binary_sensors(tracker, timeout)
                await test_04_text_sensors(tracker, timeout)

            # Control tests
            if run_control:
                await test_05_room_climate(client, tracker, timeout)
                await test_06_water_climate(client, tracker, timeout)
                await test_07_number_entities(client, tracker, timeout)
                await test_08_energy_mix(client, tracker, timeout)
                await test_09_electric_power(client, tracker, timeout)

            # Full tests
            if run_switches:
                await test_10_switches(client, tracker, timeout)

            if run_aircon:
                await test_11_aircon(client, tracker, timeout)

            # Stress tests
            if run_stress:
                await test_12_debounce_stress(client, tracker, timeout)

            # Operating status (always run)
            if run_readonly:
                await test_13_operating_status(tracker, timeout)

            # Restore state after control tests
            if run_control:
                await restore_state(client, tracker, saved)

        total_time = time.time() - t_start

        # ─────────── Summary ───────────
        log("\n" + "=" * 70)
        log("TEST SUMMARY")
        log("=" * 70)
        log(f"{'Test':<40} {'Result':<6} {'Time':>6}  Message")
        log("-" * 70)

        n_pass = n_fail = n_warn = n_skip = 0
        for o in outcomes:
            symbol = {"PASS": "PASS", "FAIL": "FAIL", "WARN": "WARN", "SKIP": "SKIP"}[o.result.value]
            log(f"  {o.name:<38} {symbol:<6} {o.duration_seconds:>5.1f}s  {o.message}")
            if o.result == TestResult.PASS:
                n_pass += 1
            elif o.result == TestResult.FAIL:
                n_fail += 1
            elif o.result == TestResult.WARN:
                n_warn += 1
            else:
                n_skip += 1

        log("-" * 70)
        log(f"  TOTAL: {n_pass} passed, {n_fail} failed, {n_warn} warnings, {n_skip} skipped")
        log(f"  Duration: {total_time:.1f}s")
        log("=" * 70)

        # Write log to file
        log_path = "test_results.log"
        with open(log_path, "w", encoding="utf-8") as f:
            for line in _log_lines:
                f.write(line + "\n")
        log(f"Full log written to {log_path}")

        # Exit code
        if n_fail > 0:
            log(f"RESULT: FAIL ({n_fail} failures)")
            return 1
        else:
            log("RESULT: ALL PASSED")
            return 0

    finally:
        await client.disconnect()
        log("Disconnected.")


if __name__ == "__main__":
    print("Truma iNetBox Comprehensive End-to-End Test Harness")
    print("Usage: python test_harness.py [--no-control] [--full] [--stress] [--test N] [--timeout N]")
    print()
    exit_code = asyncio.run(main())
    sys.exit(exit_code)
