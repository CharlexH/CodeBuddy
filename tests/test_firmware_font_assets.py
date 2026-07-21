from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_HEADER = ROOT / "firmware/src/fonts/jetbrains_mono_ascii_8.h"
FONT_LICENSE = ROOT / "firmware/licenses/JetBrainsMono-OFL.txt"
MAIN = ROOT / "firmware/src/main.cpp"


def test_jetbrains_mono_asset_is_ascii_only_and_licensed():
    header = FONT_HEADER.read_text(encoding="utf-8")
    license_text = FONT_LICENSE.read_text(encoding="utf-8")

    assert "JetBrains Mono Regular and Medium v2.304" in header
    assert header.count("0x20, 0x7E, 21") == 2
    assert "JetBrainsMono_Regular8pt7b" in header
    assert "JetBrainsMono_Medium8pt7b" in header
    assert "SIL OPEN FONT LICENSE Version 1.1" in license_text


def test_firmware_uses_jetbrains_for_dashboard_and_one_chinese_font():
    main = MAIN.read_text(encoding="utf-8")

    assert '#include "fonts/jetbrains_mono_ascii_8.h"' in main
    assert "code_buddy_fonts::JetBrainsMono_Regular8pt7b" in main
    assert "code_buddy_fonts::JetBrainsMono_Medium8pt7b" in main
    assert "fonts::efontCN_10" not in main
    assert "fonts::efontCN_14" not in main
    assert "fonts::efontCN_12" in main
