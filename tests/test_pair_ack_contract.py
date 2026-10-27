"""Guard the pairing ACK wire contract and honest transmission diagnostics.

Static checks complement hardware delivery testing; ESP_OK only confirms enqueue.
"""
from pathlib import Path
import re

source = Path("src/espnow_handler.c").read_text(encoding="utf-8")
protocol = Path("include/espnow_protocol.h").read_text(encoding="utf-8")

assert re.search(r"#define\s+ESPNOW_PAIR_ACK_PAYLOAD_LEN\s+9\b", protocol)
assert re.search(r"#define\s+ESPNOW_MSG_PAIR_ACK\s+0x02\b", protocol)
assert "uint8_t ack_payload[ESPNOW_PAIR_ACK_PAYLOAD_LEN]" in source
assert "ESPNOW_DEVICE_TYPE_BRIDGE" in source
assert "ESPNOW_PROTOCOL_VERSION" in source

send = source[source.index("static bool send_simple_message("):source.index("static void send_gpio_ack(")]
assert "esp_now_send(mac, packet, packet_len)" in send
assert "return err == ESP_OK;" in send
assert "return false;" in send

receive = source[source.index("static void on_espnow_recv_cb("):]
assert "send_simple_message(mac, ESPNOW_MSG_PAIR_ACK," in receive
assert "PAIR_ACK queued for " in receive
assert "PAIR_ACK enqueue failed for " in receive
assert "new_packet.session_id, 0," in receive
assert "ack_payload, sizeof(ack_payload)" in receive

print("PAIR_ACK contract, session echo, and enqueue telemetry guards passed")
