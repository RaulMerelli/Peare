# Resource compatibility

This table preserves compatibility declarations from the C# implementation
that served as the porting baseline. It is not a test report for every entry.
The rows name container resource types, not payload decoders. Opener exposes the
type from the resource directory; Decoder interprets the raw `RT_FONT` payload.

| Mark | Meaning |
|---|---|
| Yes | The baseline implements the resource type. |
| No | The baseline does not implement the resource type. |
| - | The resource type does not apply to that module family. |
| (1) | The resource type has known limits. |
| (2) | The code exists, but no suitable sample was available. |
| (3) | The code has a known incompatibility with Windows 1.x or 2.x resources. |
| — | The resource type does not apply to that module family. |

| Resource | NE OS/2 | LX OS/2 | LE OS/2 | NE Windows | PE Windows |
|---|---:|---:|---:|---:|---:|
| `RT_POINTER` | Yes | Yes | Yes | - | - |
| `RT_MESSAGE` | Yes | Yes | Yes | - | - |
| `RT_MESSAGETABLE` | - | - | - | Yes (1)(3) | Yes |
| `RT_BITMAP` | Yes | Yes | Yes | Yes | Yes |
| `RT_STRING` | Yes | Yes | Yes | Yes (3) | Yes |
| `RT_DISPLAYINFO` | Yes | Yes | Yes | - | - |
| `RT_MENU` | Yes | Yes | Yes | Yes (3) | Yes |
| `RT_ACCELTABLE` | Yes | Yes | Yes | - | - |
| `RT_ACCELERATOR` | - | - | - | Yes | Yes |
| `RT_DLGINCLUDE` | Yes | Yes | Yes | — | — |
| `RT_HELPTABLE` | Yes (2) | Yes | Yes | — | — |
| `RT_HELPSUBTABLE` | Yes (2) | Yes | Yes | — | — |
| `RT_FONT` | Yes | Yes | Yes | Yes | Yes |
| `RT_FONTDIR` | Yes (1) | Yes (1) | Yes (1) | Yes (1) | Yes (1) |
| `RT_DIALOG` | Yes (1) | Yes (1) | Yes (1) | No | No |
| `RT_NAMETABLE` | — | — | — | Yes (3) | — |
| `RT_GROUP_ICON` | — | — | — | Yes | Yes |
| `RT_ICON` | — | — | — | Yes | Yes |
| `RT_VERSION` | — | — | — | Yes | Yes |
| `RT_GROUP_CURSOR` | — | — | — | Yes | Yes |
| `RT_CURSOR` | — | — | — | Yes | Yes |

OS/2 pointer and bitmap resources can contain `BA`, `BM`, `IC`, `CI`, `CP`, or
`PT` data. One resource can produce more than one image.

The C++ implementation continues to change. A `Yes` value does not guarantee
support for undocumented or vendor-specific variants.
