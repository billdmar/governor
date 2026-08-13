"""plant_sim.py — reference plant + PID simulator for the host harness.

This is the Python reference model that mirrors, conceptually, the C plant and
PID control loop. It lets the ground station close the loop (or act as the
environment) in native_sim / emulation end-to-end tests: the host can drive a
setpoint, step the plant, and observe whether the controller converges or
diverges. It is intentionally a *reference* model — logic and structure, not a
silicon-accurate timing model. All time is virtual (``dt`` per step), matching
the labeled-emulation discipline in the project rules.

The plant is a first-order lag (single dominant pole), consistent with
TASKS.md's note that the 100 Hz control loop is "a decade above the simulated
plant's dominant pole." The PID is a plain fixed-form controller; it exists so
tests can exercise convergence (RUN) and forced divergence (F12 → SAFE_STOP).
"""

from __future__ import annotations

from dataclasses import dataclass, field

# Control loop period, from config/registry.md §1 (GOV_CTRL_PERIOD_MS = 10).
GOV_CTRL_PERIOD_MS: int = 10
_DT_S: float = GOV_CTRL_PERIOD_MS / 1000.0

# |error| beyond which the plant is considered "diverging" (F12/T5).
# Registry lists GOV_DIVERGE_LIMIT in plant-units "see control"; we pick a
# concrete reference value here for the host model and document it.
GOV_DIVERGE_LIMIT: float = 100.0


@dataclass
class FirstOrderPlant:
    """First-order lag plant: ``tau * dy/dt = -y + k * u``.

    Discretised with a simple forward-Euler step at ``dt`` seconds. ``tau`` is
    chosen so the dominant pole sits about a decade below the 100 Hz loop.

    Attributes:
        tau: Time constant (s).
        k: DC gain (output units per input unit).
        y: Current plant output (state).
    """

    tau: float = 0.1  # 1.6 Hz pole ≈ a decade below the 100 Hz loop
    k: float = 1.0
    y: float = 0.0

    def step(self, u: float, dt: float = _DT_S) -> float:
        """Advance the plant one step under actuator input ``u``.

        Args:
            u: Actuator command this step.
            dt: Virtual timestep in seconds.

        Returns:
            The new plant output.
        """
        dydt = (-self.y + self.k * u) / self.tau
        self.y += dydt * dt
        return self.y


@dataclass
class PID:
    """Reference PID controller (positional form) with output clamp.

    Attributes:
        kp, ki, kd: Gains.
        out_min, out_max: Output saturation limits (actuator range).
        setpoint: Target plant output.
    """

    kp: float = 2.0
    ki: float = 1.0
    kd: float = 0.0
    out_min: float = -1000.0
    out_max: float = 1000.0
    setpoint: float = 0.0

    _integral: float = field(default=0.0, init=False)
    _prev_err: float = field(default=0.0, init=False)

    def reset(self) -> None:
        """Clear integral/derivative history (e.g. on a state re-init)."""
        self._integral = 0.0
        self._prev_err = 0.0

    def update(self, measured: float, dt: float = _DT_S) -> float:
        """Compute the actuator command for the current measurement.

        Args:
            measured: Latest plant output.
            dt: Virtual timestep in seconds.

        Returns:
            The clamped actuator command.
        """
        err = self.setpoint - measured
        self._integral += err * dt
        deriv = (err - self._prev_err) / dt if dt > 0 else 0.0
        self._prev_err = err
        out = self.kp * err + self.ki * self._integral + self.kd * deriv
        return max(self.out_min, min(self.out_max, out))


@dataclass
class PlantSim:
    """A plant + PID pair the host can step as a closed loop.

    Attributes:
        plant: The plant model.
        pid: The controller.
        actuation_enabled: When ``False`` the actuator is forced to the safe
            value (mirrors the safety SM inhibiting actuation).
        safe_output: The value applied when actuation is inhibited
            (``GOV_SAFE_OUTPUT`` == 0).
    """

    plant: FirstOrderPlant = field(default_factory=FirstOrderPlant)
    pid: PID = field(default_factory=PID)
    actuation_enabled: bool = True
    safe_output: float = 0.0

    def set_setpoint(self, sp: float) -> None:
        """Set the controller target."""
        self.pid.setpoint = sp

    def step(self, dt: float = _DT_S) -> tuple[float, float]:
        """Run one closed-loop control step.

        Returns:
            ``(output, error)`` — the actuator command applied and the current
            tracking error (``setpoint - measured``).
        """
        if self.actuation_enabled:
            u = self.pid.update(self.plant.y, dt)
        else:
            u = self.safe_output
        self.plant.step(u, dt)
        err = self.pid.setpoint - self.plant.y
        return u, err

    def is_diverging(self) -> bool:
        """True if the current tracking error exceeds ``GOV_DIVERGE_LIMIT``."""
        return abs(self.pid.setpoint - self.plant.y) > GOV_DIVERGE_LIMIT
