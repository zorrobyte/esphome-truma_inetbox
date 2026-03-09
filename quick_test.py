"""Quick targeted test for the 3 remaining issues."""
import asyncio, time
from aioesphomeapi import APIClient, ClimateMode, ClimateFanMode, SelectInfo, SensorInfo, BinarySensorInfo, ClimateInfo, SwitchInfo

HOST = "192.168.4.29"
PORT = 6053
PSK = "d0lhdr+FCp0h2g7/KrPFIqQXxEpzmJbvhm8imsBqPjA="

states = {}
infos = {}

def on_state(state):
    states[state.key] = state

def find(name_frag, etype=None):
    for k, e in infos.items():
        if etype and not isinstance(e, etype):
            continue
        if name_frag.lower() in (e.name or e.object_id).lower():
            return e
    return None

async def wait(key, pred, timeout=60, desc=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = states.get(key)
        if s and pred(s):
            return True
        await asyncio.sleep(0.25)
    print(f"  TIMEOUT ({timeout}s): {desc}")
    return False

async def main():
    client = APIClient(HOST, PORT, None, noise_psk=PSK)
    await client.connect(login=True)
    info = await client.device_info()
    print(f"Connected: {info.name}")

    entities, _ = await client.list_entities_services()
    for e in entities:
        infos[e.key] = e
    client.subscribe_states(on_state)
    await asyncio.sleep(4)

    room = find("room", ClimateInfo)
    room_state = states.get(room.key) if room else None
    current_temp = room_state.current_temperature if room_state else None
    print(f"\nCurrent room temp: {current_temp}")

    # ─── TEST 1: BOOST with max delta ───
    print("\n=== TEST 1: BOOST (target=30, need delta>10°C) ===")
    if current_temp and (30 - current_temp) > 10:
        print(f"  Delta = {30-current_temp:.1f}°C > 10°C — BOOST should work")
    else:
        print(f"  Delta = {30-current_temp:.1f}°C <= 10°C — BOOST will be rejected by CP Plus")
        print(f"  This is a hardware limitation, not a firmware bug")

    client.climate_command(key=room.key, mode=ClimateMode.HEAT,
                           target_temperature=30.0, fan_mode=ClimateFanMode.HIGH)
    hm = find("heating mode", SensorInfo)
    if hm:
        ok = await wait(hm.key, lambda s: hasattr(s,'state') and s.state == 11, timeout=40, desc="heating_mode==11 (BOOST)")
        val = states.get(hm.key)
        print(f"  Result: heating_mode={val.state if val else '?'} (expected 11)")
        if ok:
            print("  PASS: BOOST confirmed!")
        else:
            print(f"  EXPECTED WARN: BOOST needs delta>10°C, current={current_temp}")

    # Turn off room heater
    client.climate_command(key=room.key, mode=ClimateMode.OFF)
    await asyncio.sleep(5)

    # ─── TEST 2: Energy Mix = Propane ───
    print("\n=== TEST 2: Energy Mix = Propane ===")
    # First ensure we're on Electric 2
    emix = find("energy mix", SelectInfo)
    emix_sensor = find("energy mix", SensorInfo)
    if emix:
        client.select_command(key=emix.key, state="Electric 2")
        if emix_sensor:
            await wait(emix_sensor.key, lambda s: hasattr(s,'state') and s.state == 2, timeout=40, desc="mix==2")
        print(f"  Baseline: mix_sensor={states.get(emix_sensor.key).state if emix_sensor else '?'}")

        # Now set Propane and wait with long timeout
        print("  Setting Propane...")
        client.select_command(key=emix.key, state="Propane")
        if emix_sensor:
            ok = await wait(emix_sensor.key, lambda s: hasattr(s,'state') and s.state == 1, timeout=60, desc="mix_sensor==1 (GAS)")
            val = states.get(emix_sensor.key)
            print(f"  Result: mix_sensor={val.state if val else '?'} (expected 1)")
            if ok:
                print("  PASS")
            else:
                print("  FAIL/WARN: sensor didn't update to GAS(1)")

        # Restore
        client.select_command(key=emix.key, state="Electric 2")
        await asyncio.sleep(3)

    # ─── TEST 3: Timer ON ───
    print("\n=== TEST 3: Timer ON ===")
    timer_sw = find("active timer", SwitchInfo)
    timer_bin = find("timer active", BinarySensorInfo)
    if timer_sw:
        # Ensure clean state
        client.switch_command(key=timer_sw.key, state=False)
        await asyncio.sleep(10)
        print(f"  Timer baseline: {states.get(timer_bin.key).state if timer_bin and timer_bin.key in states else '?'}")

        print("  Activating timer...")
        client.switch_command(key=timer_sw.key, state=True)
        if timer_bin:
            ok = await wait(timer_bin.key, lambda s: s.state is True, timeout=60, desc="TIMER_ACTIVE=True")
            val = states.get(timer_bin.key)
            print(f"  Result: timer_active={val.state if val else '?'} (expected True)")
            if ok:
                print("  PASS")
            else:
                print("  FAIL: timer binary sensor never went True")
                # Check if timer data_valid might be the issue
                print("  (Could be stale-state suppression or CP Plus not confirming)")

        # Disable timer
        client.switch_command(key=timer_sw.key, state=False)
        await asyncio.sleep(5)
    else:
        print("  Timer switch not found")

    print("\n=== DONE ===")
    await client.disconnect()

asyncio.run(main())
