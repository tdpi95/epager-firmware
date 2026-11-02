"""display.mask_numbers on the fake board (CONFIG_SET over the protocol): numbers of 4 or more digits on a
card are drawn as ****, the setting survives a reboot, and turning it off shows the notification again.
The masking rules are unit-tested in tests/host/test_ui.c."""
from __future__ import annotations

from fake_board import scenes


def test_mask_numbers_end_to_end(fb_libs) -> None:
    b = scenes.board()
    scenes.post(b, 1, "Bank 1.234.567", "Your code is 482913, call 112")
    assert "Bank 1.234.567" in b.screen_text() and "482913" in b.screen_text()
    scenes.set_config(b, {"display.mask_numbers": True})
    shown = b.screen_text()
    assert "Bank ****" in shown and "Your code is ****, call 112" in shown
    assert "482913" not in shown and "1.234" not in shown
    b.reboot()  # a stored setting: the new card is masked from the start
    scenes.settle(b)
    scenes.post(b, 2, "Otp", "PIN 9876")
    assert "PIN ****" in b.screen_text() and "9876" not in b.screen_text()
    scenes.set_config(b, {"display.mask_numbers": False})
    assert "PIN 9876" in b.screen_text()
