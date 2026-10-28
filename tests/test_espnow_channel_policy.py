"""Guard the ESP-NOW APSTA channel policy against startup regressions."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/espnow_handler.c").read_text()


class EspNowChannelPolicyTests(unittest.TestCase):
    def test_peers_follow_active_wifi_channel(self):
        self.assertEqual(
            SOURCE.count(".channel = ESPNOW_PEER_CHANNEL_CURRENT_WIFI;"), 2
        )
        self.assertIn("#define ESPNOW_PEER_CHANNEL_CURRENT_WIFI 0", (
            ROOT / "include/espnow_config.h"
        ).read_text())

    def test_startup_does_not_force_channel_while_sta_may_be_connecting(self):
        self.assertNotIn("esp_wifi_set_channel(", SOURCE)


if __name__ == "__main__":
    unittest.main()
