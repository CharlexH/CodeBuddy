from pathlib import Path


def test_malformed_json_logging_uses_length_only_and_never_raw_input():
    source = (
        Path(__file__).parents[1] / "firmware" / "src" / "data.h"
    ).read_text(encoding="utf-8")
    error_branch = source.split("if (err) {", 1)[1].split("return;", 1)[0]
    assert "formatMalformedJsonLog" in error_branch
    assert "Serial.printf" not in error_branch
    assert "err.c_str" not in error_branch
    assert "head=" not in error_branch
