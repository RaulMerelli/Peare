"""Independent metadata vectors; these do not call the internal C++ parser."""

import pytest


def _reference(data, handled=None):
    fields = {}
    pos = 0
    previous = -1
    while pos < len(data):
        if len(data) - pos < 3:
            raise ValueError("header")
        tag = int.from_bytes(data[pos:pos + 2], "little")
        lead = data[pos + 2]
        pos += 3
        if tag <= previous:
            raise ValueError("order")
        previous = tag
        length = lead
        if lead & 128:
            if pos == len(data):
                raise ValueError("length")
            length = ((lead & 127) << 8) | data[pos]
            pos += 1
        if length > len(data) - pos:
            raise ValueError("truncated")
        value = data[pos:pos + length]
        pos += length
        fields[tag] = value
    if handled is not None:
        if any(not tag & 0x8000 and tag not in handled for tag in fields):
            raise ValueError("required")
    return fields


def _get(fields, tag, width):
    if tag not in fields:
        return None
    value = fields[tag]
    if len(value) > width:
        raise ValueError("width")
    return value + bytes(width - len(value))


def _field(tag, value):
    length = len(value)
    assert length <= 32767
    encoded = bytes([length]) if length < 128 else bytes([0x80 | (length >> 8), length & 255])
    return tag.to_bytes(2, "little") + encoded + value


def test_synthetic_metadata_reference_lengths_padding_and_optional_tags():
    data = _field(2, b"\0\2") + _field(17, b"\x34\x12") + _field(0x8001, bytes(range(128)))
    fields = _reference(data, {2, 17})
    assert fields[2] == b"\0\2"
    assert _get(fields, 2, 4) == b"\0\2\0\0"
    assert _get(fields, 17, 8) == b"\x34\x12" + bytes(6)
    assert _get(fields, 18, 8) is None
    assert fields[0x8001] == bytes(range(128))


def test_synthetic_metadata_reference_width_checked_only_on_get():
    fields = _reference(_field(2, bytes(5)), {2})
    assert fields[2] == bytes(5)
    with pytest.raises(ValueError, match="width"):
        _get(fields, 2, 4)


def test_synthetic_metadata_reference_noncanonical_length_is_accepted():
    # The original reader's handling of this encoding is not yet verified.
    assert _reference(b"\x02\0\x80\x01\xff", {2})[2] == b"\xff"


@pytest.mark.parametrize("data,handled", [
    (_field(2, b"") + _field(2, b""), {2}),
    (b"\x02\0\x80", {2}),
    (b"\x02\0\x02\xff", {2}),
    (_field(7, b""), set()),
])
def test_synthetic_metadata_reference_rejects_invalid(data, handled):
    with pytest.raises(ValueError):
        _reference(data, handled)
