#!/usr/bin/env python3
"""Turns a device's lights.toml into the lights[] table the driver ships with.

Run from cikon_light/CMakeLists.txt at configure time. Writes light_generated.h and
light_generated.c into the component's build directory, and prints the three facts CMake needs
(HAS_PWM, HAS_ADDRESSABLE, MAX_LEDS) on stdout as KEY=VALUE lines.

What it writes describes the board and nothing else: names, channels, and the capabilities those
channels add up to. Brightness, colour and effect settings are state, not wiring - light_init()
gives them their starting values, and NVS overwrites those where a saved state applies.

A bad lights.toml exits non-zero with a message naming the light and the field, which CMake turns
into a configure error. Nothing downstream re-checks the wiring: what this writes is already a
table of numbers.
"""

import argparse
import sys
import tomllib
from pathlib import Path

# TOML key -> the role enumerator in light_internal.h. Channels are emitted in this order rather
# than the file's, so LEDC channels are handed out the same way however the keys are arranged.
PWM_ROLES = {
    "red": "CH_RED",
    "green": "CH_GREEN",
    "blue": "CH_BLUE",
    "cold_white": "CH_COLD_WHITE",
    "warm_white": "CH_WARM_WHITE",
}

COLOR_ROLES = {"red", "green", "blue"}
WHITE_ROLES = {"cold_white", "warm_white"}

# A strip format is written as its component order and pasted onto led_strip's macro name. The
# letters are the components, so a W is what a separate white element means. Only the orders
# led_strip actually defines a macro for: anything else would reach the compiler as an unknown
# identifier in generated code, which is a far worse place to find out.
STRIP_FORMAT_PREFIX = "LED_STRIP_COLOR_COMPONENT_FMT_"
STRIP_FORMATS = ("GRB", "RGB", "GRBW", "RGBW")

# The chip, which sets the bit timing the RMT encoder sends (WS2812 T1H=0.9us, SK6812 0.6us,
# WS2811 1.2us with its own reset). Stated rather than guessed from the white channel: an SK6812
# without one would otherwise be clocked as a WS2812, outside its spec. WS2816 is missing because
# it wants two bytes per colour and light_pixel_t has one.
STRIP_MODEL_PREFIX = "LED_MODEL_"
STRIP_MODELS = ("WS2812", "SK6812", "WS2811")

SCHEMA = {
    "pwm": {"required": {"name"}, "optional": set(PWM_ROLES)},
    "switch": {"required": {"name", "gpio"}, "optional": {"active_low"}},
    "addressable": {"required": {"name", "gpio", "leds", "model"}, "optional": {"format"}},
}

NAME_MAX = 15  # light_config_t.name is char[16]


class ConfigError(Exception):
    pass


def sanitize(name):
    """Lowercase, spaces to underscores - what the runtime used to do to every light name, kept
    identical so MQTT topics and Home Assistant entity ids stay where they are."""
    return "".join("_" if c == " " else c.lower() for c in name)


def where(index, light):
    name = light.get("name")
    return f"light {index}" + (f" '{name}'" if isinstance(name, str) else "")


def check_type(index, light):
    kind = light.get("type")
    if kind is None:
        raise ConfigError(f"{where(index, light)}: missing 'type' (one of {sorted(SCHEMA)})")
    if kind not in SCHEMA:
        raise ConfigError(
            f"{where(index, light)}: unknown type {kind!r}, expected one of {sorted(SCHEMA)}")

    allowed = SCHEMA[kind]["required"] | SCHEMA[kind]["optional"] | {"type"}
    for key in light:
        if key not in allowed:
            raise ConfigError(f"{where(index, light)}: key {key!r} is not valid for type "
                              f"{kind!r} (allowed: {sorted(allowed - {'type'})})")
    for key in sorted(SCHEMA[kind]["required"]):
        if key not in light:
            raise ConfigError(f"{where(index, light)}: type {kind!r} requires {key!r}")
    return kind


def check_gpio(index, light, key, value):
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ConfigError(f"{where(index, light)}: {key!r} must be a GPIO number, got {value!r}")
    return value


def build_pwm(index, light, out):
    present = {role for role in PWM_ROLES if role in light}
    if not present:
        raise ConfigError(
            f"{where(index, light)}: type 'pwm' needs at least one of {sorted(PWM_ROLES)}")
    # The only rule about which channels may sit together: a color light needs all three, since
    # two of them cannot mix a hue. Whites are free to appear alone or in a cold/warm pair.
    if present & COLOR_ROLES and not COLOR_ROLES <= present:
        missing = sorted(COLOR_ROLES - present)
        raise ConfigError(f"{where(index, light)}: color needs red, green and blue together, "
                          f"missing {missing}")

    out["has_color"] = COLOR_ROLES <= present
    out["has_white"] = bool(present & WHITE_ROLES)
    out["has_cct"] = WHITE_ROLES <= present
    out["is_switch"] = False
    out["is_addressable"] = False
    for role, enumerator in PWM_ROLES.items():
        if role in present:
            out["channels"].append(
                {"role": enumerator, "gpio": check_gpio(index, light, role, light[role])})


def build_switch(index, light, out):
    out["has_color"] = out["has_white"] = out["has_cct"] = False
    out["is_switch"] = True
    out["is_addressable"] = False
    active_low = light.get("active_low", False)
    if not isinstance(active_low, bool):
        raise ConfigError(f"{where(index, light)}: 'active_low' must be true or false")
    out["channels"].append({
        "role": "CH_SWITCH",
        "gpio": check_gpio(index, light, "gpio", light["gpio"]),
        "active_level": not active_low,
    })


def build_addressable(index, light, out):
    leds = light["leds"]
    if not isinstance(leds, int) or isinstance(leds, bool) or leds < 1:
        raise ConfigError(f"{where(index, light)}: 'leds' must be a positive count, got {leds!r}")

    fmt = light.get("format", "GRB")
    if fmt not in STRIP_FORMATS:
        raise ConfigError(f"{where(index, light)}: unknown format {fmt!r}, expected one of "
                          f"{list(STRIP_FORMATS)}")

    model = light["model"]
    if model not in STRIP_MODELS:
        raise ConfigError(f"{where(index, light)}: unknown model {model!r}, expected one of "
                          f"{list(STRIP_MODELS)}")

    out["has_color"] = True
    out["has_white"] = "W" in fmt
    out["has_cct"] = False
    out["is_switch"] = False
    out["is_addressable"] = True
    out["channels"].append({
        "role": "CH_ADDRESSABLE",
        "gpio": check_gpio(index, light, "gpio", light["gpio"]),
        "leds": leds,
        "format": STRIP_FORMAT_PREFIX + fmt,
        "model": STRIP_MODEL_PREFIX + model,
        "has_white_channel": "W" in fmt,
    })


BUILDERS = {"pwm": build_pwm, "switch": build_switch, "addressable": build_addressable}


def build_light(index, light, max_channels):
    kind = check_type(index, light)

    name = light["name"]
    if not isinstance(name, str) or not name:
        raise ConfigError(f"{where(index, light)}: 'name' must be a non-empty string")
    clean = sanitize(name)
    if len(clean) > NAME_MAX:
        raise ConfigError(f"{where(index, light)}: name {clean!r} is {len(clean)} characters, "
                          f"at most {NAME_MAX} fit")

    # Both spellings are needed: the table and the MQTT command key use the sanitised one, while
    # Home Assistant shows the name as written (ha.c passes it through for display and sanitises
    # separately for the entity id).
    out = {"name": clean, "display_name": name, "kind": kind, "channels": []}
    BUILDERS[kind](index, light, out)

    if len(out["channels"]) > max_channels:
        raise ConfigError(f"{where(index, light)}: {len(out['channels'])} channels, at most "
                          f"{max_channels} fit")
    return out


def load(path, max_channels):
    try:
        with open(path, "rb") as f:
            doc = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(str(e)) from None

    for key in doc:
        if key != "light":
            raise ConfigError(f"unexpected top-level key {key!r}, only [[light]] is used")
    entries = doc.get("light")
    if not entries:
        raise ConfigError("no [[light]] entries")

    lights = [build_light(i, entry, max_channels) for i, entry in enumerate(entries)]

    owner = {}
    for light in lights:
        for channel in light["channels"]:
            gpio = channel["gpio"]
            if gpio in owner:
                raise ConfigError(
                    f"GPIO {gpio} is used by both '{owner[gpio]}' and '{light['name']}'")
            owner[gpio] = light["name"]

    seen = set()
    for light in lights:
        if light["name"] in seen:
            raise ConfigError(f"two lights end up named '{light['name']}'")
        seen.add(light["name"])

    # LEDC channels are handed out in table order, skipping lights that drive no timer - the same
    # allocation the runtime parser did.
    next_ledc = 0
    for light in lights:
        for channel in light["channels"]:
            if channel["role"] in PWM_ROLES.values():
                channel["ledc_ch"] = next_ledc
                next_ledc += 1
    return lights, next_ledc


def emit_header(lights, has_addressable, max_leds, source):
    lines = [
        f"// Generated from {source} by lights_gen.py. Do not edit.",
        "#pragma once",
        "",
        f"#define LIGHT_COUNT {len(lights)}",
    ]
    if has_addressable:
        lines += ["", f"#define LIGHT_EFFECTS_MAX_LEDS {max_leds}"]

    # The adapter registers one command and one Home Assistant entity per light. A command
    # handler takes only its JSON argument (command_handler_t in cmnd.h), so each light needs a
    # function of its own to carry the index, and the preprocessor cannot count - hence a list to
    # expand. It comes from here so that the Nth command is the Nth light by construction.
    commands = " ".join(f"X({i})" for i in range(len(lights)))
    entities = " ".join(f'HA_ENTITY_ENTRY("{light["display_name"]}")' for light in lights)
    lines += ["",
              f"#define LIGHT_CMND_LIST {commands}",
              f"#define HA_ENTITY_LIST {entities}"]
    return "\n".join(lines) + "\n"


def emit_source(lights, has_pwm, has_addressable, ledc_used, source, header):
    assert_ledc = has_pwm and ledc_used

    out = [f"// Generated from {source} by lights_gen.py. Do not edit.", ""]
    if assert_ledc:
        out += ['#include "soc/soc_caps.h"', ""]
    out += [f'#include "{header}"',  # LIGHT_COUNT
            '#include "light_internal.h"',
            "",
            "light_config_t lights[LIGHT_COUNT] = {"]

    for light in lights:
        out.append("    {")
        out.append(f'        .name = "{light["name"]}",')
        out.append(f'        .channel_count = {len(light["channels"])},')
        for flag in ("has_color", "has_white", "has_cct", "is_switch"):
            out.append(f'        .{flag} = {str(light[flag]).lower()},')
        if has_addressable:
            out.append(f'        .is_addressable = {str(light["is_addressable"]).lower()},')
        out.append("        .channels = {")
        for channel in light["channels"]:
            fields = [f'.role = {channel["role"]}', f'.gpio = {channel["gpio"]}']
            if "active_level" in channel:
                fields.append(f'.active_level = {str(channel["active_level"]).lower()}')
            if "ledc_ch" in channel:  # only the roles that occupy a timer
                fields.append(f'.ledc_ch = {channel["ledc_ch"]}')
            if has_addressable and "leds" in channel:
                fields.append(f'.led_count = {channel["leds"]}')
                fields.append(f'.led_model = {channel["model"]}')
                fields.append(f'.led_color_format = {channel["format"]}')
                fields.append(f'.has_white_channel = {str(channel["has_white_channel"]).lower()}')
            out.append("            {" + ", ".join(fields) + "},")
        out.append("        },")
        out.append("    },")
    out.append("};")

    # How many channels a chip has is the one limit the generator cannot know and the runtime
    # reports too late: over budget, a light is dropped at start-up and only says so on the
    # console. A bad GPIO is left to gpio_config()/ledc_channel_config() - they refuse it just as
    # loudly, and the masks that would answer it here differ in width between chips.
    if assert_ledc:
        out += ["",
                f"_Static_assert({ledc_used} <= SOC_LEDC_CHANNEL_NUM,",
                f'               "the lights need {ledc_used} LEDC channels, more than this chip '
                f'has");']
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("toml", type=Path)
    parser.add_argument("--out-header", type=Path, required=True)
    parser.add_argument("--out-source", type=Path, required=True)
    parser.add_argument("--max-channels", type=int, default=5)
    args = parser.parse_args()

    try:
        lights, ledc_used = load(args.toml, args.max_channels)
    except ConfigError as e:
        print(f"{args.toml}: {e}", file=sys.stderr)
        return 1

    has_pwm = any(c["role"] in PWM_ROLES.values() for l in lights for c in l["channels"])
    has_addressable = any(l["is_addressable"] for l in lights)
    max_leds = max((c["leds"] for l in lights for c in l["channels"] if "leds" in c), default=0)

    args.out_header.write_text(emit_header(lights, has_addressable, max_leds, args.toml.name))
    args.out_source.write_text(
        emit_source(lights, has_pwm, has_addressable, ledc_used, args.toml.name,
                    args.out_header.name))

    print(f"HAS_PWM={int(has_pwm)}")
    print(f"HAS_ADDRESSABLE={int(has_addressable)}")
    print(f"MAX_LEDS={max_leds}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
