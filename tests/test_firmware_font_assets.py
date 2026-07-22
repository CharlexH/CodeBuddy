from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_HEADER = ROOT / "firmware/src/fonts/jetbrains_mono_ascii_8.h"
FONT_LICENSE = ROOT / "firmware/licenses/JetBrainsMono-OFL.txt"
FONT_GENERATOR = ROOT / "scripts/generate-jetbrains-mono-font.sh"
MAIN = ROOT / "firmware/src/main.cpp"
BUDDY = ROOT / "firmware/src/buddy.cpp"
DASHBOARD_LOGIC = ROOT / "firmware/src/landscape_dashboard_logic.h"


def test_jetbrains_mono_asset_is_ascii_only_and_licensed():
    header = FONT_HEADER.read_text(encoding="utf-8")
    license_text = FONT_LICENSE.read_text(encoding="utf-8")

    assert "JetBrains Mono Regular and Bold v2.304" in header
    assert "0x20, 0x7E, 21" in header
    assert header.count("0x2D, 0x3A") == 2
    assert "JetBrainsMono_Regular8pt7b" in header
    assert "JetBrainsMono_Bold6pt7b" in header
    assert "SIL OPEN FONT LICENSE Version 1.1" in license_text


def test_firmware_uses_jetbrains_for_dashboard_and_one_chinese_font():
    main = MAIN.read_text(encoding="utf-8")

    assert '#include "fonts/jetbrains_mono_ascii_8.h"' in main
    assert "code_buddy_fonts::JetBrainsMono_Regular8pt7b" in main
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
    assert "useDashboardStatusFont" in main
    assert "useDashboardTimeFont" in main
    assert "useDashboardSecondsFont" in main
    assert "useDashboardCardFont" in main
    status_font = main[main.index("static void useDashboardStatusFont") :]
    status_font = status_font[: status_font.index("}\n")]
    assert "JetBrainsMono_Regular7pt7b" in status_font
    card_font = main[main.index("static void useDashboardCardFont") :]
    card_font = card_font[: card_font.index("}\n")]
    assert "JetBrainsMono_Bold6pt7b" in card_font
    assert "canvas.drawString(dateLine" in main
    assert "LANDSCAPE_DASHBOARD_STATUS_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_TIME_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_SECONDS_SCALE" not in dashboard_logic
    assert "LANDSCAPE_DASHBOARD_CARD_LABEL_SCALE" not in dashboard_logic


def test_jetbrains_zero_feature_is_baked_into_generated_bitmap_fonts():
    generator = FONT_GENERATOR.read_text(encoding="utf-8")
    header = FONT_HEADER.read_text(encoding="utf-8")
    compact_header = " ".join(header.split())

    assert 'FT_Get_Name_Index(face, "zero.zero")' in generator
    assert "OpenType slashed-zero alternate" in header
    assert (
        "0x3F, 0xC0, 0x0F, 0xE0, 0x0F, 0xF0, 0x0F, 0xF8"
        in compact_header
    )


def test_ascii_buddy_restores_builtin_font_before_using_six_pixel_geometry():
    buddy = BUDDY.read_text(encoding="utf-8")
    render = buddy[buddy.index("void buddyRenderTo(") :]
    render = render[: render.index("static bool buddyAdvanceTick")]

    assert "_tgt->setFont(nullptr);" in render
    assert render.index("_tgt->setFont(nullptr);") < render.index(
        "sp->states[personaState](tickCount)"
    )


def test_portrait_seconds_use_the_same_native_font_as_hours_and_minutes():
    main = MAIN.read_text(encoding="utf-8")
    portrait_time = main[main.index("if (decision.drawTime)") :]
    portrait_time = portrait_time[: portrait_time.index("if (decision.drawDate)")]
    seconds = portrait_time[portrait_time.index("if (layout.time.showSeconds)") :]

    assert "useDashboardSecondsFont(canvas);" in seconds
    assert "useSharedFaceAsciiFont(canvas);" not in seconds
