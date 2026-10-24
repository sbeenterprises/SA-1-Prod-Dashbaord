"""Engine limits shared by the alert rules (gen_alerts.py) and the
dashboards (gen_dashboards.py). The same coolant limits are in
ingestion/cpp-common/engine_profile.hpp (CoolantLimits) for the Readiness
pre-start check; tools/smoke_test.sh checks that the two tables agree.

Coolant (warning from, overheating from), in °C, per ENGINE_TYPE
(/etc/usv/usv.env, the engine_type tag on engine points):

- yamaha_outboard: raw-water cooled, 50 °C thermostat (Yamaha F115), runs
  about 50 to 60 °C; 75 / 85 °C is well above normal and well before damage.
- mercury_diesel: closed cooling, 83 °C thermostat (Mercury Diesel 2.0L),
  runs about 83 to 90 °C; 95 / 100 °C.
- any other or unset type: 95 / 100 °C.

Fuel (%, all engines): warning below 20, critical below 10.
"""

COOLANT_LIMITS = {
    "yamaha_outboard": (75.0, 85.0),
    "mercury_diesel": (95.0, 100.0),
}
COOLANT_DEFAULT = (95.0, 100.0)

FUEL_WARN_PCT = 20.0
FUEL_CRIT_PCT = 10.0


def coolant(engine_type):
    return COOLANT_LIMITS.get(engine_type, COOLANT_DEFAULT)


def flux_coolant_band(value="r._value"):
    """Flux expression: 0 normal, 1 high (warning), 2 overheating, from the
    row's engine_type tag (missing tag: the default limits)."""
    def pick(i):
        expr = f"{COOLANT_DEFAULT[i]:.1f}"
        for t, lim in reversed(list(COOLANT_LIMITS.items())):
            expr = (f'(if exists r.engine_type and r.engine_type == "{t}" '
                    f'then {lim[i]:.1f} else {expr})')
        return expr
    return (f"(if {value} >= {pick(1)} then 2.0 "
            f"else if {value} >= {pick(0)} then 1.0 else 0.0)")


def coolant_text(i):
    """i = 0 warning, 1 overheating: 'from 75 °C on a Yamaha outboard, 95 °C
    on a Mercury diesel, 95 °C on other engines'."""
    names = {"yamaha_outboard": "a Yamaha outboard", "mercury_diesel": "a Mercury diesel"}
    parts = [f"{lim[i]:g} °C on {names.get(t, t)}" for t, lim in COOLANT_LIMITS.items()]
    parts.append(f"{COOLANT_DEFAULT[i]:g} °C on other engines")
    return "from " + ", ".join(parts)
