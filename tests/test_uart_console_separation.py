"""Keep the C3 debug USB console separate from the Doesbot UART0 path."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
defaults = (root / "sdkconfig.defaults").read_text()
config = (root / "include/config.h").read_text()
serial = (root / "src/serial_handler.c").read_text()

assert 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' in defaults
assert "#define UART_NUM             UART_NUM_0" in config
assert "#define UART_TX_PIN          21" in config
assert "#define UART_RX_PIN          20" in config
assert "uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN" in serial
print("USB Serial/JTAG diagnostics are configured independently of Doesbot UART0 GPIO21/20")
