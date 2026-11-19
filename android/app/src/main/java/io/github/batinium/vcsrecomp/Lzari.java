// [android] LZARI decompression for Max Drive (.max) saves (docs/ANDROID.md "Saves: import and export").
//
// Haruhiko Okumura's LZARI (LZSS + adaptive arithmetic coding), decoder only, ported from mymcplus's lzari.py
// (mymc+ by Florian Märkl, based on mymc by Ross Ridge; GPL-3.0-or-later, https://git.sr.ht/~thestr4ng3r/mymcplus).
// Same constants and model updates, so it decodes what Max Drive and mymc write. Plain Java: no Android classes, so the
// JVM check (android/app/src/test/java/.../SaveFormatsCheck.java) runs it on the Mac.
package io.github.batinium.vcsrecomp;

final class Lzari {
    private static final int HIST_LEN = 4096, MIN_MATCH_LEN = 3, MAX_MATCH_LEN = 60;
    private static final int ARITH_BITS = 15;
    private static final long Q1 = 1L << ARITH_BITS, Q2 = Q1 * 2, Q3 = Q1 * 3, Q4 = Q1 * 4;
    private static final int MAX_CUM = (int) Q1 - 1;
    private static final int MAX_CHAR = 256 + MAX_MATCH_LEN - MIN_MATCH_LEN + 1;

    private final byte[] src;
    private long bitPos; // next bit to read, MSB first; past the end reads 0
    private long high = Q4, low, code;
    private final int[] symCum = new int[MAX_CHAR + 1]; // reversed, as lzari.py's decoder keeps it
    private final int[] symFreq = new int[MAX_CHAR + 1];
    private final int[] symbolToChar = new int[MAX_CHAR + 1];
    private final int[] positionCum = new int[HIST_LEN + 1];

    private Lzari(byte[] src) {
        this.src = src;
        for (int i = 0; i <= MAX_CHAR; i++) {
            symCum[i] = i;
            symFreq[i] = i == 0 ? 0 : 1;
            symbolToChar[i] = i == 0 ? 0 : i - 1;
        }
        int a = 0;
        for (int i = HIST_LEN; i > 0; i--) {
            a += 10000 / (200 + i);
            positionCum[i - 1] = a;
        }
    }

    /** Decompresses src into exactly outLength bytes. */
    static byte[] decode(byte[] src, int outLength) {
        return new Lzari(src).run(outLength);
    }

    private int bit() {
        long p = bitPos++;
        long b = p >>> 3;
        if (b >= src.length)
            return 0;
        return (src[(int) b] >> (7 - (int) (p & 7))) & 1;
    }

    private byte[] run(int outLength) {
        byte[] out = new byte[outLength];
        for (int i = 0; i < ARITH_BITS + 2; i++)
            code += code + bit();
        int histPos = HIST_LEN - MAX_MATCH_LEN;
        int[] history = new int[HIST_LEN];
        for (int i = 0; i < histPos; i++)
            history[i] = 0x20;
        int outPos = 0;
        while (outPos < outLength) {
            int c = decodeChar();
            if (c >= 0x100) {
                int pos = decodePosition();
                int length = c - 0x100 + MIN_MATCH_LEN;
                int base = Math.floorMod(histPos - pos - 1, HIST_LEN);
                for (int off = 0; off < length && outPos < outLength; off++) {
                    int v = history[(base + off) % HIST_LEN];
                    out[outPos++] = (byte) v;
                    history[histPos] = v;
                    histPos = (histPos + 1) % HIST_LEN;
                }
            } else {
                out[outPos++] = (byte) c;
                history[histPos] = c;
                histPos = (histPos + 1) % HIST_LEN;
            }
        }
        return out;
    }

    private void renormalize() {
        while (true) {
            if (low < Q2) {
                if (low < Q1 || high > Q3) {
                    if (high > Q2)
                        return;
                } else {
                    low -= Q1;
                    code -= Q1;
                    high -= Q1;
                }
            } else {
                low -= Q2;
                code -= Q2;
                high -= Q2;
            }
            low *= 2;
            high *= 2;
            code = code * 2 + bit();
        }
    }

    private int decodeChar() {
        long range = high - low;
        long maxCum = symCum[MAX_CHAR];
        long n = ((code - low + 1) * maxCum - 1) / range;
        // bisect_right(symCum, n, 1): the first index >= 1 whose value is > n
        int lo = 1, hi = MAX_CHAR + 1;
        while (lo < hi) {
            int mid = (lo + hi) >>> 1;
            if (n < symCum[mid])
                hi = mid;
            else
                lo = mid + 1;
        }
        int i = Math.min(lo, MAX_CHAR); // corrupt input only
        high = low + symCum[i] * range / maxCum;
        low += symCum[i - 1] * range / maxCum;
        int symbol = MAX_CHAR + 1 - i;
        renormalize();
        int ret = symbolToChar[symbol];
        updateModel(symbol);
        return ret;
    }

    private void updateModel(int symbol) {
        if (symCum[MAX_CHAR] >= MAX_CUM) {
            int c = 0;
            for (int i = MAX_CHAR; i > 0; i--) {
                symCum[MAX_CHAR - i] = c;
                int a = (symFreq[i] + 1) / 2;
                symFreq[i] = a;
                c += a;
            }
            symCum[MAX_CHAR] = c;
        }
        int freq = symFreq[symbol];
        int newSymbol = symbol;
        while (symFreq[newSymbol - 1] == freq)
            newSymbol--;
        if (newSymbol != symbol) {
            int swap = symbolToChar[newSymbol];
            symbolToChar[newSymbol] = symbolToChar[symbol];
            symbolToChar[symbol] = swap;
        }
        symFreq[newSymbol] = freq + 1;
        for (int i = MAX_CHAR - newSymbol + 1; i <= MAX_CHAR; i++)
            symCum[i]++;
    }

    private int decodePosition() {
        long range = high - low;
        long maxCum = positionCum[0];
        long x = ((code - low + 1) * maxCum - 1) / range;
        // lzari.py's search(): binary search on the descending table
        int c = 1, s = positionCum.length - 1;
        while (true) {
            int a = (s + c) / 2;
            if (positionCum[a] <= x)
                s = a;
            else
                c = a + 1;
            if (c >= s)
                break;
        }
        int pos = c - 1;
        high = low + positionCum[pos] * range / maxCum;
        low += positionCum[pos + 1] * range / maxCum;
        renormalize();
        return pos;
    }
}
