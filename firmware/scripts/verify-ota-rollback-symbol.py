"""Fail the firmware build unless Arduino defers OTA validation to the app."""

from pathlib import Path
import re
import shlex
import subprocess


SYMBOL_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(\S+)\s*$")
VERIFY_OTA_DEFINITION = re.compile(
    r"\b(?:extern\s+\"C\"\s+)?bool\s+verifyOta\s*\([^;{}]*\)\s*"
    r"(?:__attribute__\s*\(\([^;{}]*\)\)\s*)?\{"
)


def parse_nm_output(output):
    symbols = {}
    for line in output.splitlines():
        match = SYMBOL_LINE.match(line)
        if not match:
            continue
        kind, name = match.groups()
        symbols.setdefault(name, []).append(kind)
    return symbols


def verify_nm_output(output):
    symbols = parse_nm_output(output)
    errors = []
    rollback = symbols.get("verifyRollbackLater", [])
    if rollback != ["T"]:
        errors.append(
            "verifyRollbackLater must be exactly one strong global text symbol T; "
            "found %r" % rollback
        )
    verify_ota = symbols.get("verifyOta", [])
    if any(kind != "W" for kind in verify_ota):
        errors.append(
            "verifyOta must remain absent or the Arduino weak W default; found %r"
            % verify_ota
        )
    return errors


def source_override_errors(sources):
    errors = []
    for name, text in sources.items():
        if VERIFY_OTA_DEFINITION.search(text):
            errors.append("project source overrides verifyOta: %s" % name)
    return errors


def read_project_sources(source_dir):
    source_dir = Path(source_dir)
    sources = {}
    for suffix in ("*.c", "*.cc", "*.cpp", "*.cxx"):
        for path in source_dir.rglob(suffix):
            sources[str(path)] = path.read_text(encoding="utf-8", errors="replace")
    return sources


def verify_final_elf(elf_path, nm_command, source_dir):
    command = list(nm_command) + ["-g", "--defined-only", str(elf_path)]
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(
            "OTA rollback symbol check could not run nm: %s" % result.stderr.strip()
        )
    errors = verify_nm_output(result.stdout)
    errors.extend(source_override_errors(read_project_sources(source_dir)))
    if errors:
        raise RuntimeError("OTA rollback symbol check failed: " + "; ".join(errors))
    print("OTA rollback symbol check: verifyRollbackLater=T, verifyOta=weak/absent")


def _platformio_nm_command(env):
    configured = env.subst("$NM").strip()
    if configured and configured != "$NM":
        return shlex.split(configured)
    compiler = shlex.split(env.subst("$CC"))[0]
    compiler_path = Path(compiler)
    name = compiler_path.name
    for suffix in ("gcc", "g++"):
        if name.endswith(suffix):
            return [str(compiler_path.with_name(name[:-len(suffix)] + "nm"))]
    raise RuntimeError("OTA rollback symbol check could not resolve toolchain nm")


def _platformio_post_action(target, source, env):
    del source
    elf_path = Path(str(target[0]))
    nm_command = _platformio_nm_command(env)
    verify_final_elf(elf_path, nm_command, env.subst("$PROJECT_SRC_DIR"))


# PlatformIO/SCons injects Import into the extra-script global namespace. Keep
# the pure verifier importable by its standalone regression test.
try:
    Import
except NameError:
    pass
else:
    Import("env")
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _platformio_post_action)
