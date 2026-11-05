# Raw payloads

PeareDecoder can decode a payload without an executable container. Examples
include standalone OS/2 bitmap files and Windows or OS/2 FNT files.

The caller can pass the platform and resource type as text. These values help
the Decoder select a route. If the caller omits them, the Decoder examines the
payload structure and uses RawDetect as a fallback.

If no decoder accepts the payload, the caller still receives the original
bytes. The GUI then displays a hexadecimal view.
