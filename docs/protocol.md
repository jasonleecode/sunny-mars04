# Observed Mars04 legacy USB protocol

Scope: USB ID `040e:4d35`, firmware `M04_V2.44.00.180322`, USB 2.0 on Linux.
This describes one tested firmware, not a universal specification.
All multi-byte integers and IEEE 754 fields on the wire are little-endian.

## Control transfer

Send a 4-byte opcode using bmRequestType `0x40`, bRequest/wValue/wIndex `0`.
Read the response with bmRequestType `0xc0`, bRequest/wIndex `0`, length **512**.
The IN wValue is `0x99` for IMU opcode `0x10009`, otherwise `0`.
Changing the metadata read length to 1024 caused a pipe/stall error on hardware.
Query select and response must be serialized as a pair.

| Opcode | Meaning | Used by this driver |
| --- | --- | --- |
| `0x10050` | Device info | Yes |
| `0x10015` | ToF lens calibration | Yes |
| `0x10009` | Legacy IMU batch | Experimental |
| `0x10054` | Additional camera calibration | No |

Device-info offsets: type u32 at 0 (tested value 2); width/wire-height u16 at
8/10 (224/173); RGB width/height u16 at 14/16 (1344/760); firmware ASCII field
at 24, length 20; serial ASCII at 44, length 19. Actual depth height is
wire-height minus one metadata row. Bytes after known fields are not interpreted.

Lens response starts with nine f32 values:
`fx, fy, cx, cy, k1, k2, p1, p2, k3`.

## Depth: interface 0, bulk IN 0x82

Let W=224, H=172 and N=W×H. The tested firmware's transfer size is:

```
align1024(5*N + W + 18*W*(H+1) + 131072) = 1021952
```

| Offset | Length | Content |
| --- | --- | --- |
| 0 | 2×N | Signed int16 processed radial depth |
| 2×N | 3×N | Intensity/gray data, not decoded here |
| 5×N | W | Metadata row |
| 5×N+W | 18×W×(H+1) | Appended raw phase data, ignored |
| Following | 131072 + alignment padding | Auxiliary data, ignored |

Within metadata, u32 at +16 must equal W; +32 is an observed frame sequence,
and +36 an observed device time counter. Time units and epoch are not asserted.
All buffers are length-checked before access. The old firmware does not provide
the newer SDK's depth CRC, so integrity checks are limited to transfer size and
metadata. A short read is fatal; reopen/reset rather than interpreting a shifted
buffer as an image.

Reading only the first 193536 bytes can yield one valid image, but leaves raw
phase bytes queued and corrupts subsequent frame alignment. In a hardware probe,
the documented SDK raw-disable command acknowledged but did not remove the
appended data on this old firmware. The driver therefore reads the complete
observed frame and sends no raw-mode commands.

Depth / 5460 converts to radial metres using the matching implementation's
constant. Ray generation inverts Brown-Conrady distortion with 100 fixed-point
iterations, then normalizes `(x,y,1)`. XYZ = ray × radial range. Non-positive
depth is invalid. This matches the previous working Python implementation;
absolute accuracy remains to be verified with a measured target.

## RGB: bulk IN 0x83

Image bytes: W×H×3/2 = 1532160 for W=1344, H=760.
Transfer size: align1024(image_bytes+W) = 1533952.
Metadata follows the image, with width/height u32 at +16/+20 and counters at
+32/+36. The image is **I420**: one full-resolution Y plane followed by U and V
planes, each at half width and half height. Although a public SDK marks the
frame as YUV420SP, interpreting this firmware's output as NV12 produces large
green/magenta blocks. Planar I420 decoding removes those artifacts.

## Experimental legacy IMU

The 512-byte reply begins with one count byte and three reserved bytes.
Observed legacy records are 44 bytes each, starting at offset 4:

| Record offset | Length | Interpretation |
| --- | --- | --- |
| 0 | 4 | `49 4d 55 00` ("IMU\0") |
| 4 | 4 | Raw device time counter |
| 8 | 36 | Nine f32 channel slots, semantics unverified |

Maximum count is floor((512-4)/44)=11. Every count, record marker and float
finiteness is checked. A newer SDK interprets a different 24-byte / half-float
layout; applying it blindly to this firmware mistakes the `IMU` marker for a
timestamp and produces spurious values.

Observed replies advance at approximately 10 counter units per sample. Most
channel slots are zero, including the first six during a motion test. Some
replies contain malformed markers or apparent uninitialized values. Decoding
float slots does not establish that those slots contain real sensor data.
The public API and CSV are diagnostic only; no SI units, sensor presence,
gravity response, gyro response or magnetic-field accuracy are validated.

## Validation

Offline tests cover device compatibility rejection, truncated/corrupt packets,
signed depth endianness, analytical normalized rays, known metric ranges, I420
black/white/red vectors, legacy IMU bounds, and randomized malformed input.
Hardware tests are performed separately on N1 arm64. No private captures or
device-specific calibration fixtures are part of the test suite.
