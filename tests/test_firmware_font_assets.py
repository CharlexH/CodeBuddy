from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_HEADER = ROOT / "firmware/src/fonts/jetbrains_mono_ascii_8.h"
FONT_LICENSE = ROOT / "firmware/licenses/JetBrainsMono-OFL.txt"
MAIN = ROOT / "firmware/src/main.cpp"
DASHBOARD_LOGIC = ROOT / "firmware/src/landscape_dashboard_logic.h"


def test_jetbrains_mono_asset_is_ascii_only_and_licensed():
    header = FONT_HEADER.read_text(encoding="utf-8")
    license_text = FONT_LICENSE.read_text(encoding="utf-8")

    assert "JetBrains Mono Regular and Medium v2.304" in header
    assert "0x20, 0x7E, 21" in header
    assert "0x20, 0x7E, 18" in header
    assert "0x20, 0x7E, 16" in header
    assert header.count("0x2D, 0x3A") == 2
    assert "JetBrainsMono_Regular8pt7b" in header
    assert "JetBrainsMono_Medium6pt7b" in header
    assert "SIL OPEN FONT LICENSE Version 1.1" in license_text


def test_firmware_uses_jetbrains_for_dashboard_and_one_chinese_font():
    main = MAIN.read_text(encoding="utf-8")

    assert '#include "fonts/jetbrains_mono_ascii_8.h"' in main
    assert "code_buddy_fonts::JetBrainsMono_Regular8pt7b" in main
    assert "code_buddy_fonts::JetBrainsMono_Medium6pt7b" in main
    assert "fonts::efontCN_10" not in main
    assert "fonts::efontCN_14" not in main
    assert "fonts::efontCN_12" in main


def test_landscape_dashboard_uses_native_size_fonts_without_bitmap_scaling():
    header = FONT_HEADER.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    dashboard_logic = DASHBOARD_LOGIC.read_text(encoding="utf-8")

    assert "JetBrainsMono_Regular7pt7b" in header
    assert "JetBrainsMono_Regular14pt7b" in header
    assert "JetBrainsMono_Regular20pt7b" in header
    assert "JetBrainsMono_Medium6pt7b" in header
    assert "useDashboardStatusFont" in main
    assert "useDashboardTimeFont" in main
    assert "useDashboardSecondsFont" in main
    assert "useDashboardCardFont" in main
    assert "LANDSCAPE_DASHBOARD_STATUS_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_TIME_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_SECONDS_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_CARD_LABEL_SCALE" not in dashboard_logic
