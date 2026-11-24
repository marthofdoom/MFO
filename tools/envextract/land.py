"""LAND VHGT decoder.  VHGT = f32 offset + 33x33 signed bytes + 3 unused.  Row y (south to north), column x (west to east), vertices 128 units apart:
H[y][0] = H[y-1][0] + d[y][0] (H[-1][0] = offset), H[y][x] = H[y][x-1] + d[y][x]; the sum is in units of 8 game units."""
import struct

N = 33
SPACING = 128.0


def decode_vhgt(vh):
    off = struct.unpack_from('<f', vh, 0)[0]; d = struct.unpack_from('<%db' % (N * N), vh, 4)
    out = []; rowstart = off
    for y in range(N):
        rowstart += d[y * N]; h = rowstart; row = [h * 8.0]
        for x in range(1, N):
            h += d[y * N + x]; row.append(h * 8.0)
        out.append(row)
    return out


def height_at(grid, lx, ly):
    """bilinear height at cell-local (0..4096) position"""
    fx = min(max(lx / SPACING, 0.0), N - 1.000001); fy = min(max(ly / SPACING, 0.0), N - 1.000001)
    x0, y0 = int(fx), int(fy); tx, ty = fx - x0, fy - y0
    a = grid[y0][x0] * (1 - tx) + grid[y0][x0 + 1] * tx; b = grid[y0 + 1][x0] * (1 - tx) + grid[y0 + 1][x0 + 1] * tx
    return a * (1 - ty) + b * ty
