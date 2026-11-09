#!/usr/bin/env python3
"""Turns a defaults file and a device profile's YAML into a X-macro list of config fields.

Run from the network adapter's CMakeLists.txt at configure time. Writes <NAME>_FIELDS(STR, U8, U16, U32, U64)
into the header it is given, in the form config_manager.h already expands into config_t, the
setters and the NVS code.

Both files are <section>: {<key>: value}. The profile starts from the defaults and repeats only
what differs; it can name nothing the defaults do not. A setting is stored as <section>_<key>.
The type comes from the value: true/false is a uint8_t, a number a uint16_t, text a string. A
string's buffer is twice its value, in steps of 32 and not under 32, so setconf has room for a
longer one.

The profile also says which uplink the device has, uplink: wifi, ethernet or thread. That is no
setting: the project's CMakeLists.txt turns it into the CONFIG_NETWORK_UPLINK_* option before
sdkconfig exists, and this script only checks the word and reports it.

A bad profile exits non-zero with a message naming the section and the key, which CMake turns into
a configure error.
"""

import argparse
import sys
from pathlib import Path

import yaml

NAME_MAX = 15  # NVS key limit

# A service runs when its section is in the profile. wifi is an uplink, not a service, and the
# web server is http or https, so neither is here
SERVICES = ("mdns", "mqtt", "sntp", "ota", "monitor")
UPLINKS = ("wifi", "ethernet", "thread")
U16_MAX = 0xFFFF
STR_STEP = 32


class ConfigError(Exception):
    pass


def load(path):
    """The sections of a file, and the uplink it names (None when it does not)."""
    data = yaml.safe_load(Path(path).read_text())
    if data is None:
        return {}, None
    uplink = None
    if isinstance(data, dict) and "uplink" in data:
        uplink = data.pop("uplink")
        if uplink not in UPLINKS:
            raise ConfigError(f"{path}: uplink: {uplink!r}, expected one of {list(UPLINKS)}")
    # A section with nothing under it, like a bare https:, is None
    if not isinstance(data, dict) or not all(s is None or isinstance(s, dict)
                                             for s in data.values()):
        raise ConfigError(f"{path}: expected <section>: followed by <key>: <value> lines")
    return {section: keys or {} for section, keys in data.items()}, uplink


def kind(value):
    # bool before int: True is an int in Python
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "number"
    if isinstance(value, str):
        return "text"
    return None


def merge(defaults, profile, profile_path):
    merged = {section: dict(keys) for section, keys in defaults.items()}
    for section, keys in profile.items():
        if section not in defaults:
            raise ConfigError(f"{profile_path}: unknown section {section!r}, expected one of "
                              f"{sorted(defaults)}")
        for key, value in keys.items():
            where = f"{profile_path}: {section}.{key}"
            if key not in defaults[section]:
                raise ConfigError(f"{where}: unknown key, expected one of "
                                  f"{sorted(defaults[section])}")
            if kind(value) != kind(defaults[section][key]):
                raise ConfigError(f"{where}: expected {kind(defaults[section][key])}, "
                                  f"got {value!r}")
            if key.endswith("pass") and value != "":
                raise ConfigError(f"{where}: passwords are not set in a profile, set it at "
                                  f"runtime with setconf")
            merged[section][key] = value
    return merged


def c_string(text):
    if not all(c.isprintable() for c in text):
        raise ConfigError(f"{text!r}: not printable text")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def field_line(section, key, value):
    name = f"{section}_{key}"
    if len(name) > NAME_MAX:
        raise ConfigError(f"{section}.{key}: {name!r} is {len(name)} characters, NVS keys are "
                          f"{NAME_MAX} at most")
    what = kind(value)
    if what == "bool":
        return f"U8({name}, {int(value)})"
    if what == "number":
        if not 0 <= value <= U16_MAX:
            raise ConfigError(f"{section}.{key}: {value} does not fit in 0..{U16_MAX}")
        return f"U16({name}, {value})"
    if what == "text":
        size = max(STR_STEP, -(-(2 * len(value) + 1) // STR_STEP) * STR_STEP)
        return f"STR({name}, {size}, {c_string(value)})"
    raise ConfigError(f"{section}.{key}: {value!r} is not text, a number or true/false")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("defaults")
    parser.add_argument("profile", help="may not exist: the defaults are used as they are")
    parser.add_argument("--name", required=True)
    parser.add_argument("--out-header", required=True)
    args = parser.parse_args()

    try:
        defaults, _ = load(args.defaults)
        profile, uplink = load(args.profile) if Path(args.profile).exists() else ({}, None)
        merged = merge(defaults, profile, args.profile)
        lines = [field_line(s, k, v) for s, keys in merged.items() for k, v in keys.items()]
        # The web server is http or https, whichever section the profile has; with neither it is
        # http
        if "http" in profile and "https" in profile:
            raise ConfigError(f"{args.profile}: http and https are both there, keep one")
        lines.append(field_line("http", "secure", "https" in profile))
    except (ConfigError, yaml.YAMLError) as error:
        print(error, file=sys.stderr)
        return 1

    macro = f"{args.name.upper()}_FIELDS"
    # Without a network.yaml there is nothing to say a service is off
    enabled = {s: not Path(args.profile).exists() or s in profile for s in SERVICES}
    enabled["http"] = not Path(args.profile).exists() or "http" in profile or "https" in profile
    prefix = args.name.upper()
    flags = "".join(f"#define {prefix}_{s.upper()}_ENABLED {int(on)}\n"
                    for s, on in enabled.items())

    body = " \\\n    ".join(lines)
    Path(args.out_header).write_text(
        f"// Generated by the network adapter's tools/network_cikon_config_gen.py. Do not edit.\n"
        f"#pragma once\n\n"
        f"{flags}\n"
        f"#define {macro}(STR, U8, U16, U32, U64) \\\n    {body}\n")
    print((f"uplink: {uplink}; " if uplink else "") +
          "services on: " + (" ".join(s for s, on in enabled.items() if on) or "none") +
          "; off: " + (" ".join(s for s, on in enabled.items() if not on) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
