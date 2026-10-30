from cnc_mpg_console.devices import Device, choose_device


def dev(path, desc, serial=None):
    return Device(path, desc, "Espressif", serial, 0x303A, 0x1001, "USB")


def test_only_unambiguous_role_match_is_selected():
    s3 = dev("/dev/ttyACM0", "CircuitPython CDC", "8248583B090F")
    c3 = dev("/dev/ttyACM1", "USB Serial/JTAG debug unit", "c3-uid")
    assert choose_device([s3, c3], "s3")[0] == s3
    assert choose_device([s3, c3], "c3")[0] == c3


def test_ambiguous_or_unknown_devices_are_not_guessed():
    a = dev("/dev/a", "USB Serial/JTAG debug unit")
    b = dev("/dev/b", "USB Serial/JTAG debug unit")
    assert choose_device([a, b], "c3")[0] is None
    assert choose_device([dev("/dev/other", "USB CDC"),], "s3")[0] is None
