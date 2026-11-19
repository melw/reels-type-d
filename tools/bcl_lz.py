"""Novatek BCL1 container (stock-compatible: window 99999, no overlapping matches) + LZ77 (Basic Compression Library format, algorithm 9).

  python bcl_lz.py c in.rbn out.bcl     compress (header checksum left 0; run ntkcalc -cw afterwards)
  python bcl_lz.py d in.bcl out.rbn     decompress (for verification)

BCL1 header (big-endian): 'BCL1' | u16 checksum | u16 algo (9) | u32 out_size | u32 in_size
LZ stream: first byte = marker. marker,0 = literal marker; marker,VLC(len),VLC(off) = match.
VLC = 7 bits per byte, most significant group first, 0x80 = more bytes follow.
"""
import struct, sys, time


def _vlc(x):
    groups = [x & 0x7F]
    x >>= 7
    while x:
        groups.append((x & 0x7F) | 0x80)
        x >>= 7
    return bytes(reversed(groups))


def lz_compress(data, max_chain=16, window=99999):
    n = len(data)
    counts = [0] * 256
    for b in data[:min(n, 1 << 22)]:
        counts[b] += 1
    marker = min(range(256), key=lambda b: data.count(bytes([b])) if n < 1 << 16 else counts[b])
    out = bytearray([marker])
    head = {}           # 4-byte key -> list of recent positions
    lit = bytearray()
    mb = bytes([marker, 0])
    i = 0
    t0 = time.time()
    while i < n:
        best_len = best_off = 0
        if i + 4 <= n:
            key = data[i:i + 4]
            cands = head.get(key)
            if cands:
                lim = min(n - i, 0xFFFF)
                for p in reversed(cands):
                    off = i - p
                    if off > window:
                        break
                    l = 4
                    lim2 = min(lim, off)  # never overlap: stock streams always have offset >= length
                    while l < lim2 and data[p + l] == data[i + l]:
                        l += 1
                    if l > off:
                        l = off
                    if l > best_len:
                        best_len, best_off = l, off
                        if l >= 256:
                            break
            ok = (best_len >= 8 or (best_len == 4 and best_off <= 0x7F) or
                  (best_len == 5 and best_off <= 0x3FFF) or (best_len == 6 and best_off <= 0x1FFFFF) or
                  (best_len == 7 and best_off <= 0x0FFFFFFF))
        else:
            ok = False
        if ok:
            out += lit.replace(bytes([marker]), mb) if marker in lit else lit
            lit = bytearray()
            out.append(marker)
            out += _vlc(best_len)
            out += _vlc(best_off)
            end = i + best_len
            # index positions inside the match (sparsely for long runs to stay fast)
            step = 1 if best_len < 64 else 8
            for j in range(i, min(end, n - 3), step):
                lst = head.setdefault(data[j:j + 4], [])
                lst.append(j)
                if len(lst) > max_chain:
                    del lst[0]
            i = end
        else:
            lit.append(data[i])
            if i + 4 <= n:
                lst = head.setdefault(data[i:i + 4], [])
                lst.append(i)
                if len(lst) > max_chain:
                    del lst[0]
            i += 1
        if (i & 0xFFFFF) < 2 and i > 0 and sys.stderr.isatty() is False:
            pass
    out += lit.replace(bytes([marker]), mb) if marker in lit else lit
    return bytes(out)


def lz_decompress(src, out_size):
    marker = src[0]
    out = bytearray()
    i = 1
    n = len(src)

    def vlc():
        nonlocal i
        y = 0
        while True:
            b = src[i]
            i += 1
            y = (y << 7) | (b & 0x7F)
            if not b & 0x80:
                return y

    while i < n and len(out) < out_size:
        s = src[i]
        i += 1
        if s == marker:
            if src[i] == 0:
                out.append(marker)
                i += 1
            else:
                length = vlc()
                off = vlc()
                start = len(out) - off
                if off >= length:
                    out += out[start:start + length]
                else:
                    for k in range(length):
                        out.append(out[start + k])
        else:
            out.append(s)
    return bytes(out)


def main():
    mode, src, dst = sys.argv[1:4]
    data = open(src, 'rb').read()
    if mode == 'c':
        t = time.time()
        body = lz_compress(data)
        # The loader only handles word-aligned sizes (stock streams are padded too).
        # Pad with literal bytes; they decode past out_size, like the stock stream does.
        pad_byte = 0 if body[0] != 0 else 1
        body += bytes([pad_byte]) * (-len(body) % 4)
        hdr = b'BCL1' + struct.pack('>HHII', 0, 9, len(data), len(body))
        open(dst, 'wb').write(hdr + body)
        print('compressed %d -> %d bytes in %.0f s' % (len(data), len(body), time.time() - t))
    else:
        assert data[:4] == b'BCL1'
        _, algo, out_size, in_size = struct.unpack('>HHII', data[4:16])
        assert algo == 9, algo
        out = lz_decompress(data[16:16 + in_size], out_size)
        open(dst, 'wb').write(out)
        print('decompressed %d -> %d bytes (expected %d)' % (in_size, len(out), out_size))


if __name__ == '__main__':
    main()
