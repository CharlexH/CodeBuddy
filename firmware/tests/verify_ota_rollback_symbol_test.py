#!/usr/bin/env python3
import importlib.util
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "verify-ota-rollback-symbol.py"
spec = importlib.util.spec_from_file_location("verify_ota_rollback_symbol", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def expect(value, message):
    if not value:
        raise AssertionError(message)


def main():
    valid = """
42145d98 W verifyOta
42145da0 T verifyRollbackLater
"""
    expect(module.verify_nm_output(valid) == [],
           "strong C text override plus weak core verifyOta must pass")

    weak = "42145da0 W verifyRollbackLater\n"
    expect(module.verify_nm_output(weak),
           "weak Arduino default must fail closed")
    expect(module.verify_nm_output("42145d98 W verifyOta\n"),
           "missing rollback override must fail closed")
    expect(module.verify_nm_output(
        "42145d98 T verifyOta\n42145da0 T verifyRollbackLater\n"),
        "a project verifyOta override must be rejected")
    expect(module.verify_nm_output(
        "42145da0 T verifyRollbackLater\n42145db0 T verifyRollbackLater\n"),
        "duplicate rollback definitions must fail")

    expect(module.source_override_errors({"safe.cpp": "void setup() {}"}) == [],
           "unrelated source must pass")
    expect(module.source_override_errors({
        "bad.cpp": 'extern "C" bool verifyOta() { return false; }'
    }), "source-level verifyOta override must be rejected")


if __name__ == "__main__":
    main()
