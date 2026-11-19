// [android] PS2 save files in and out (docs/ANDROID.md "Saves: import and export"): one memory-card save folder (its
// name and its files) read from the formats PC tools, cheat devices and PS3 use, and written as .psu or .zip.
//
//   .max  Max Drive / Action Replay Max: "Ps2PowerSave", LZARI-compressed (Lzari)
//   .cbs  CodeBreaker: "CFU\0", RC4 with a fixed key state, then zlib
//   .psu  EMS / uLaunchELF / PCSX2 / OPL: the card's own 512-byte directory entries, files padded to 1 KB clusters
//   .psv  PS3 virtual memory card export of a PS2 save ("\0VSP", type 2)
//   .zip  a save folder (any depth, as tools/backup.py or a PC zips it) or any of the above inside
//
// The container parsing is ported from mymcplus (mymc+ by Florian Märkl, based on mymc by Ross Ridge;
// GPL-3.0-or-later, https://git.sr.ht/~thestr4ng3r/mymcplus): save/format_max_drive.py, format_codebreaker.py,
// format_ems.py, format_psv.py, ps2mc_dir.py. Plain Java (no Android classes), so the JVM check
// (android/app/src/test/java/.../SaveFormatsCheck.java) runs it on a PC.
//
// Names are card names: bytes, kept 1:1 in Strings as ISO-8859-1.
package io.github.batinium.vcsrecomp;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

final class PS2Save {
    /** A file that is not a save we can read, with a message for the player */
    static final class FormatException extends Exception {
        FormatException(String message) {
            super(message);
        }
    }

    static final class File {
        final String name;
        final byte[] data;
        long modified; // ms since 1970, 0 = unknown

        File(String name, byte[] data, long modified) {
            this.name = name;
            this.data = data;
            this.modified = modified;
        }
    }

    final String dir;
    final List<File> files = new ArrayList<>();
    String format; // where it came from: "Max Drive (.max)" ...
    long modified;

    PS2Save(String dir, String format) {
        this.dir = dir;
        this.format = format;
    }

    File file(String name) {
        for (File f : files)
            if (f.name.equals(name))
                return f;
        return null;
    }

    long totalSize() {
        long n = 0;
        for (File f : files)
            n += f.data.length;
        return n;
    }

    // A memory card holds 8 MB: nothing in a save may be larger (guards against zip bombs and garbage headers)
    static final int MAX_SAVE = 8 << 20;
    static final int DIRENT = 512;
    static final int DF_FILE = 0x0010, DF_DIR = 0x0020, DF_EXISTS = 0x8000;
    // mode bits a card writes for a save folder / file: rwx, 0x0400, exists (+ 0x0080 "closed" on files)
    static final int MODE_DIR = 0x8427, MODE_FILE = 0x8497;

    // ---- reading -----------------------------------------------------------------------------------------------------

    /**
     * Every save in a file (a zip may hold several). fileName (the picked file's name) only names the format in
     * messages; the contents decide it.
     */
    static List<PS2Save> read(byte[] b, String fileName) throws FormatException {
        List<PS2Save> out = new ArrayList<>();
        if (startsWith(b, "PK\3\4") || startsWith(b, "PK\5\6"))
            readZip(b, out);
        else
            out.add(readOne(b, fileName));
        if (out.isEmpty())
            throw new FormatException("There is no PS2 save in " + fileName + ".");
        return out;
    }

    /** One save container (not a zip) */
    static PS2Save readOne(byte[] b, String fileName) throws FormatException {
        try {
            if (startsWith(b, "Ps2PowerSave"))
                return readMax(b);
            if (startsWith(b, "CFU\0"))
                return readCbs(b);
            if (startsWith(b, "\0VSP"))
                return readPsv(b);
            if (looksLikePsu(b))
                return readPsu(b);
        } catch (IndexOutOfBoundsException | java.nio.BufferUnderflowException e) {
            throw new FormatException(fileName + " is cut short or damaged.");
        }
        throw new FormatException(fileName + " is not a save this app reads (.max, .cbs, .psu, .psv, or a .zip of a "
                + "save folder).");
    }

    private static boolean startsWith(byte[] b, String magic) {
        if (b.length < magic.length())
            return false;
        for (int i = 0; i < magic.length(); i++)
            if (b[i] != (byte) magic.charAt(i))
                return false;
        return true;
    }

    private static ByteBuffer le(byte[] b, int off, int len) {
        return ByteBuffer.wrap(b, off, len).order(ByteOrder.LITTLE_ENDIAN);
    }

    /** A zero-terminated name field */
    static String name(byte[] b, int off, int len) {
        int end = off;
        while (end < off + len && b[end] != 0)
            end++;
        return new String(b, off, end - off, StandardCharsets.ISO_8859_1);
    }

    private static byte[] slice(byte[] b, int off, int len) throws FormatException {
        if (off < 0 || len < 0 || off + len > b.length || off + len < 0)
            throw new FormatException("The save is cut short or damaged.");
        byte[] r = new byte[len];
        System.arraycopy(b, off, r, 0, len);
        return r;
    }

    private static void checkName(String n) throws FormatException {
        if (n.isEmpty() || n.equals(".") || n.equals("..") || n.indexOf('/') >= 0 || n.length() > 31)
            throw new FormatException("The save holds a file with an impossible name (\"" + n + "\").");
    }

    // Max Drive: header "<12sL32s32sLLL" (magic, crc, dir name, icon.sys title, compressed length + 4, file count,
    // uncompressed length), then LZARI; inside, each file is "<L32s" (size, name) + data, padded so the next header
    // ends on 16 bytes (format_max_drive.py)
    static PS2Save readMax(byte[] b) throws FormatException {
        if (b.length < 0x5C)
            throw new FormatException("The Max Drive save is cut short.");
        ByteBuffer h = le(b, 0, 0x5C);
        String dir = name(b, 16, 32);
        long clen = h.getInt(80) & 0xFFFFFFFFL, count = h.getInt(84) & 0xFFFFFFFFL, length = h.getInt(88) & 0xFFFFFFFFL;
        if (length > MAX_SAVE || count > 4096)
            throw new FormatException("The Max Drive save's header is damaged.");
        byte[] comp;
        if (clen == length)
            comp = slice(b, 0x5C, b.length - 0x5C); // some writers put the uncompressed size here
        else
            comp = slice(b, 0x5C, (int) Math.min(clen - 4, b.length - 0x5C));
        byte[] s = Lzari.decode(comp, (int) length);
        PS2Save save = new PS2Save(dir, "Max Drive (.max)");
        int off = 0;
        for (int i = 0; i < count; i++) {
            if (s.length - off < 36)
                throw new FormatException("The Max Drive save is cut short.");
            int l = le(s, off, 4).getInt();
            String n = name(s, off + 4, 32);
            off += 36;
            checkName(n);
            save.files.add(new File(n, slice(s, off, l), 0));
            off += l;
            off = roundUp(off + 8, 16) - 8;
        }
        return save;
    }

    // CodeBreaker: "CFU\0", d04, header length, then "<LL32s8s8sLLLLLL" + title: uncompressed length, body length,
    // dir name, created, modified, ...; the body (RC4 with a fixed state, then zlib) holds "<8s8sLHHLL32s" + data per
    // file (format_codebreaker.py)
    private static final int[] CBS_RC4S = {
        0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4,
        0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26, 0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62,
        0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
        0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b,
        0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52, 0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0,
        0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
        0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70, 0x05, 0x7d,
        0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d, 0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5,
        0x54, 0xcf, 0xe7, 0x41, 0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
        0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0, 0x11, 0xc2, 0xea, 0x74,
        0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38, 0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e,
        0x9b, 0x7f, 0xd8, 0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
        0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5,
        0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d, 0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3,
        0x58, 0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
        0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5};

    static PS2Save readCbs(byte[] b) throws FormatException {
        if (b.length < 12)
            throw new FormatException("The CodeBreaker save is cut short.");
        ByteBuffer h = le(b, 0, b.length);
        int hlen = h.getInt(8);
        if (hlen < 92 + 32 || hlen > b.length)
            throw new FormatException("The CodeBreaker save's header is damaged.");
        long dlen = h.getInt(12) & 0xFFFFFFFFL, flen = h.getInt(16) & 0xFFFFFFFFL;
        String dir = name(b, 20, 32);
        long created = tod(b, 52), modified = tod(b, 60);
        if (dlen > MAX_SAVE)
            throw new FormatException("The CodeBreaker save's header is damaged.");
        int avail = b.length - hlen;
        int clen = (int) Math.min(flen, avail);
        if (clen != flen && clen != flen - hlen)
            throw new FormatException("The CodeBreaker save is cut short.");
        byte[] body = slice(b, hlen, clen);
        // RC4 from the fixed state
        int[] s = CBS_RC4S.clone();
        int j = 0;
        for (int ii = 0; ii < body.length; ii++) {
            int i = (ii + 1) & 0xFF;
            j = (j + s[i]) & 0xFF;
            int t = s[i];
            s[i] = s[j];
            s[j] = t;
            body[ii] ^= (byte) s[(s[i] + s[j]) & 0xFF];
        }
        byte[] plain = inflate(body, (int) dlen);
        PS2Save save = new PS2Save(dir, "CodeBreaker (.cbs)");
        save.modified = modified != 0 ? modified : created;
        int off = 0;
        while (off < plain.length) {
            if (plain.length - off < 64)
                throw new FormatException("The CodeBreaker save is cut short.");
            ByteBuffer e = le(plain, off, 64);
            long fmod = tod(plain, off + 8);
            int size = e.getInt(off + 16);
            int mode = e.getShort(off + 20) & 0xFFFF;
            String n = name(plain, off + 32, 32);
            if ((mode & (DF_FILE | DF_DIR | DF_EXISTS)) != (DF_FILE | DF_EXISTS))
                throw new FormatException("The CodeBreaker save holds a folder inside the save; PS2 saves have none.");
            checkName(n);
            save.files.add(new File(n, slice(plain, off + 64, size), fmod));
            off += 64 + size;
        }
        return save;
    }

    private static byte[] inflate(byte[] in, int max) throws FormatException {
        Inflater inf = new Inflater();
        inf.setInput(in);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        try {
            while (!inf.finished() && out.size() < max) {
                int n = inf.inflate(buf, 0, Math.min(buf.length, max - out.size()));
                if (n == 0 && (inf.needsInput() || inf.needsDictionary()))
                    break;
                out.write(buf, 0, n);
            }
        } catch (DataFormatException e) {
            throw new FormatException("The CodeBreaker save's data is damaged (" + e.getMessage() + ").");
        } finally {
            inf.end();
        }
        return out.toByteArray();
    }

    // EMS .psu: directory entries as on the card, "<HHL8sLL8sL28x448s" (mode, -, length, created, cluster, parent,
    // modified, attr, name); the folder's entry, ".", "..", then each file's entry and data padded to 1024 (format_ems.py)
    static boolean looksLikePsu(byte[] b) {
        if (b.length < DIRENT * 3)
            return false;
        ByteBuffer h = le(b, 0, b.length);
        return isDir(h.getShort(0)) && isDir(h.getShort(DIRENT)) && isDir(h.getShort(2 * DIRENT))
                && (h.getInt(4) & 0xFFFFFFFFL) >= 2 && name(b, DIRENT + 64, 32).equals(".")
                && name(b, 2 * DIRENT + 64, 32).equals("..");
    }

    private static boolean isDir(short mode) {
        return (mode & (DF_FILE | DF_DIR | DF_EXISTS)) == (DF_DIR | DF_EXISTS);
    }

    static PS2Save readPsu(byte[] b) throws FormatException {
        ByteBuffer h = le(b, 0, b.length);
        long count = (h.getInt(4) & 0xFFFFFFFFL) - 2;
        if (count > 4096)
            throw new FormatException("The .psu file's header is damaged.");
        PS2Save save = new PS2Save(name(b, 64, 32), "EMS / uLaunchELF (.psu)");
        save.modified = tod(b, 24);
        int off = 3 * DIRENT;
        for (int i = 0; i < count; i++) {
            if (off + DIRENT > b.length)
                throw new FormatException("The .psu file is cut short.");
            int mode = h.getShort(off) & 0xFFFF;
            int len = h.getInt(off + 4);
            long fmod = tod(b, off + 24);
            String n = name(b, off + 64, 32);
            if ((mode & (DF_FILE | DF_DIR | DF_EXISTS)) != (DF_FILE | DF_EXISTS))
                throw new FormatException("The .psu file holds a folder inside the save; PS2 saves have none.");
            checkName(n);
            off += DIRENT;
            save.files.add(new File(n, slice(b, off, len), fmod));
            off += roundUp(len, 1024);
        }
        return save;
    }

    // PS3 .psv: "<4sI40s8xII" (magic, version 0, signature, -, type: 2 = PS2), then "<IIIIIIIIII" (.., file count last),
    // the folder's "<8s8sII32s" (created, modified, size, mode, name), each file's "<8s8sII32s" + offset (format_psv.py)
    static PS2Save readPsv(byte[] b) throws FormatException {
        ByteBuffer h = le(b, 0, b.length);
        if (h.getInt(4) != 0)
            throw new FormatException("This .psv file has an unknown version.");
        int type = h.getInt(60);
        if (type == 1)
            throw new FormatException("This .psv file holds a PS1 save, not a PS2 one.");
        if (type != 2)
            throw new FormatException("This .psv file holds a save type this app doesn't know (" + type + ").");
        int p = 64;
        long count = h.getInt(p + 36) & 0xFFFFFFFFL;
        p += 40;
        int dmode = h.getInt(p + 20);
        if (!isDir((short) dmode))
            throw new FormatException("The .psv file's folder entry is damaged.");
        PS2Save save = new PS2Save(name(b, p + 24, 32), "PS3 (.psv)");
        save.modified = tod(b, p + 8);
        p += 56;
        if (count > 4096)
            throw new FormatException("The .psv file's header is damaged.");
        for (int i = 0; i < count; i++) {
            long fmod = tod(b, p + 8);
            int size = h.getInt(p + 16);
            int mode = h.getInt(p + 20);
            String n = name(b, p + 24, 32);
            int offset = h.getInt(p + 56);
            p += 60;
            if (isDir((short) mode))
                throw new FormatException("The .psv file holds a folder inside the save; PS2 saves have none.");
            checkName(n);
            save.files.add(new File(n, slice(b, offset, size), fmod));
        }
        return save;
    }

    // A zip: save folders (a folder with icon.sys in it, at any depth) and save files of the formats above
    static void readZip(byte[] b, List<PS2Save> out) throws FormatException {
        Map<String, PS2Save> folders = new LinkedHashMap<>();
        List<PS2Save> inner = new ArrayList<>();
        Map<String, List<File>> byFolder = new LinkedHashMap<>();
        long total = 0;
        String innerError = null;
        try (ZipInputStream z = new ZipInputStream(new ByteArrayInputStream(b), StandardCharsets.UTF_8)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory())
                    continue;
                String path = e.getName().replace('\\', '/');
                if (path.startsWith("__MACOSX/") || path.contains("/__MACOSX/"))
                    continue;
                byte[] data = readAll(z, MAX_SAVE - total);
                total += data.length;
                int slash = path.lastIndexOf('/');
                String leaf = path.substring(slash + 1);
                if (leaf.isEmpty() || leaf.equals(".DS_Store") || leaf.equals(SaveCard.ORDER_FILE) || leaf.equals("backup.json"))
                    continue;
                if (slash > 0) {
                    String folder = path.substring(0, slash);
                    long t = e.getTime();
                    byFolder.computeIfAbsent(folder, k -> new ArrayList<>())
                            .add(new File(SaveCard.decodeName(latin1(leaf)), data, t > 0 ? t : 0));
                }
                // a save container anywhere in the zip
                if (data.length >= 4 && (startsWith(data, "Ps2PowerSave") || startsWith(data, "CFU\0")
                        || startsWith(data, "\0VSP") || looksLikePsu(data))) {
                    try {
                        inner.add(readOne(data, leaf));
                    } catch (FormatException fe) {
                        innerError = fe.getMessage();
                    }
                }
            }
        } catch (IOException e) {
            throw new FormatException("The zip file can't be read (" + e.getMessage() + ").");
        }
        for (Map.Entry<String, List<File>> f : byFolder.entrySet()) {
            boolean hasIcon = false;
            for (File x : f.getValue())
                hasIcon |= x.name.equalsIgnoreCase("icon.sys");
            if (!hasIcon)
                continue;
            String folder = f.getKey();
            String dir = SaveCard.decodeName(latin1(folder.substring(folder.lastIndexOf('/') + 1)));
            PS2Save s = new PS2Save(dir, "a save folder in a .zip");
            for (File x : f.getValue()) {
                checkName(x.name);
                s.files.add(x);
            }
            folders.put(folder, s);
        }
        out.addAll(folders.values());
        out.addAll(inner);
        if (out.isEmpty() && innerError != null)
            throw new FormatException(innerError);
    }

    private static String latin1(String s) {
        for (int i = 0; i < s.length(); i++)
            if (s.charAt(i) > 0xFF)
                return new String(s.getBytes(StandardCharsets.UTF_8), StandardCharsets.ISO_8859_1);
        return s;
    }

    static byte[] readAll(InputStream in, long limit) throws IOException, FormatException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > limit)
                throw new FormatException("It is larger than a whole memory card (8 MB): not a PS2 save.");
        }
        return out.toByteArray();
    }

    static int roundUp(int n, int to) {
        return (n + to - 1) / to * to;
    }

    // ---- the card's time stamps: "<xBBBBBH" (-, seconds, minutes, hours, day, month, year), Japan time ------------

    private static final TimeZone JST = TimeZone.getTimeZone("GMT+09:00");

    /** ms since 1970, 0 when the field is empty or impossible */
    static long tod(byte[] b, int off) {
        if (off < 0 || off + 8 > b.length)
            return 0;
        int sec = b[off + 1] & 0xFF, min = b[off + 2] & 0xFF, hour = b[off + 3] & 0xFF, day = b[off + 4] & 0xFF,
                month = b[off + 5] & 0xFF, year = (b[off + 6] & 0xFF) | (b[off + 7] & 0xFF) << 8;
        if (year < 1990 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59)
            return 0;
        Calendar c = Calendar.getInstance(JST, Locale.ROOT);
        c.clear();
        c.set(year, month - 1, day, hour, min, sec);
        return c.getTimeInMillis();
    }

    static void putTod(ByteBuffer b, int off, long ms) {
        Calendar c = Calendar.getInstance(JST, Locale.ROOT);
        c.setTimeInMillis(ms > 0 ? ms : System.currentTimeMillis());
        b.put(off, (byte) 0);
        b.put(off + 1, (byte) c.get(Calendar.SECOND));
        b.put(off + 2, (byte) c.get(Calendar.MINUTE));
        b.put(off + 3, (byte) c.get(Calendar.HOUR_OF_DAY));
        b.put(off + 4, (byte) c.get(Calendar.DAY_OF_MONTH));
        b.put(off + 5, (byte) (c.get(Calendar.MONTH) + 1));
        b.putShort(off + 6, (short) c.get(Calendar.YEAR));
    }

    // ---- writing -----------------------------------------------------------------------------------------------------

    private static void dirent(ByteBuffer b, int off, int mode, int length, long created, long modified, String name) {
        b.putShort(off, (short) mode);
        b.putInt(off + 4, length);
        putTod(b, off + 8, created);
        putTod(b, off + 24, modified);
        byte[] n = name.getBytes(StandardCharsets.ISO_8859_1);
        for (int i = 0; i < Math.min(n.length, 32); i++)
            b.put(off + 64 + i, n[i]);
    }

    /** The save as an EMS .psu (format_ems.py's save()) */
    byte[] toPsu() {
        int size = 3 * DIRENT;
        for (File f : files)
            size += DIRENT + roundUp(f.data.length, 1024);
        byte[] out = new byte[size];
        ByteBuffer b = le(out, 0, size);
        long t = modified;
        for (File f : files)
            t = Math.max(t, f.modified);
        dirent(b, 0, MODE_DIR, files.size() + 2, t, t, dir);
        dirent(b, DIRENT, MODE_DIR, 0, t, t, ".");
        dirent(b, 2 * DIRENT, MODE_DIR, 0, t, t, "..");
        int off = 3 * DIRENT;
        for (File f : files) {
            dirent(b, off, MODE_FILE, f.data.length, f.modified, f.modified, f.name);
            off += DIRENT;
            System.arraycopy(f.data, 0, out, off, f.data.length);
            off += roundUp(f.data.length, 1024);
        }
        return out;
    }

    /**
     * The save folder as a .zip: <dir>/<file>, the names as the runtime stores them on a PC or the phone
     * (SaveCard.encodeName, so ':' etc. survive Windows), each file's time kept
     */
    byte[] toZip() throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(bytes, StandardCharsets.UTF_8)) {
            String folder = SaveCard.encodeName(dir) + "/";
            ZipEntry d = new ZipEntry(folder);
            if (modified > 0)
                d.setTime(modified);
            z.putNextEntry(d);
            z.closeEntry();
            for (File f : files) {
                ZipEntry e = new ZipEntry(folder + SaveCard.encodeName(f.name));
                if (f.modified > 0)
                    e.setTime(f.modified);
                z.putNextEntry(e);
                z.write(f.data);
                z.closeEntry();
            }
        }
        return bytes.toByteArray();
    }
}
