"""Reference plant + PID: convergence (RUN) and forced divergence (F12).

These validate the host reference model's *logic* (does the closed loop track a
setpoint; does an unstable/inhibited configuration behave as expected) — never
silicon timing. All time is virtual (fixed dt per step).
"""

from __future__ import annotations

from governor_gs.plant_sim import GOV_DIVERGE_LIMIT, PID, FirstOrderPlant, PlantSim


def test_plant_step_moves_toward_input() -> None:
    plant = FirstOrderPlant(tau=0.1, k=1.0, y=0.0)
    y1 = plant.step(u=10.0)
    assert 0.0 < y1 < 10.0  # first-order lag: partway to k*u


def test_closed_loop_converges_to_setpoint() -> None:
    sim = PlantSim(pid=PID(kp=2.0, ki=1.0, kd=0.0))
    sim.set_setpoint(50.0)
    err = 1e9
    for _ in range(5000):  # 5000 * 10 ms virtual = 50 s
        _, err = sim.step()
    assert abs(err) < 1.0            # tracked the setpoint
    assert not sim.is_diverging()


def test_actuation_inhibited_holds_safe_output() -> None:
    sim = PlantSim(actuation_enabled=False, safe_output=0.0)
    sim.set_setpoint(100.0)
    outs = [sim.step()[0] for _ in range(50)]
    assert all(o == 0.0 for o in outs)  # never actuates when inhibited


def test_divergence_detected_with_unstable_gain() -> None:
    # A large positive-feedback-style gain with a huge setpoint pushes the
    # tracking error beyond the diverge limit -> is_diverging() must trip.
    sim = PlantSim(pid=PID(kp=0.0, ki=0.0, kd=0.0))  # controller does nothing
    sim.set_setpoint(GOV_DIVERGE_LIMIT * 10)          # plant can't reach it
    for _ in range(10):
        sim.step()
    assert sim.is_diverging()


def test_pid_reset_clears_history() -> None:
    pid = PID(kp=0.0, ki=1.0, kd=0.0)
    pid.setpoint = 10.0
    for _ in range(100):
        pid.update(0.0)
    assert pid._integral != 0.0  # accumulated
    pid.reset()
    assert pid._integral == 0.0 and pid._prev_err == 0.0
