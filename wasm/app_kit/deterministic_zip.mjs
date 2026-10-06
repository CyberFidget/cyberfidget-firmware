// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import { deflateRawSync } from 'node:zlib';

// ZIP32, UTF-8 names, Unix regular files 0644, DOS epoch, raw DEFLATE level 9.
// No directory entries, extra fields, archive comments or platform metadata.
export function zip(files) {
    const table = Array.from({ length: 256 }, (_, n) => {
        for (let bit = 0; bit < 8; ++bit) n = (n >>> 1) ^ ((n & 1) ? 0xedb88320 : 0);
        return n >>> 0;
    });
    const locals = [], central = [];
    let offset = 0;
    for (const name of Object.keys(files).sort()) {
        const bytes = Buffer.from(files[name]), packed = deflateRawSync(bytes, { level: 9 });
        const filename = Buffer.from('cf-app-kit/' + name);
        let crc = 0xffffffff;
        for (const byte of bytes) crc = (crc >>> 8) ^ table[(crc ^ byte) & 255];
        crc = (crc ^ 0xffffffff) >>> 0;
        if (bytes.length > 0xffffffff || packed.length > 0xffffffff || offset > 0xffffffff) throw new Error('Kit exceeds ZIP32');
        const local = Buffer.alloc(30);
        local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4);
        local.writeUInt16LE(0x800, 6); local.writeUInt16LE(8, 8); local.writeUInt16LE(33, 12);
        local.writeUInt32LE(crc, 14); local.writeUInt32LE(packed.length, 18); local.writeUInt32LE(bytes.length, 22);
        local.writeUInt16LE(filename.length, 26);
        const entry = Buffer.alloc(46);
        entry.writeUInt32LE(0x02014b50, 0); entry.writeUInt16LE(0x0314, 4); entry.writeUInt16LE(20, 6);
        entry.writeUInt16LE(0x800, 8); entry.writeUInt16LE(8, 10); entry.writeUInt16LE(33, 14);
        entry.writeUInt32LE(crc, 16); entry.writeUInt32LE(packed.length, 20); entry.writeUInt32LE(bytes.length, 24);
        entry.writeUInt16LE(filename.length, 28); entry.writeUInt32LE((0o100644 * 65536) >>> 0, 38);
        entry.writeUInt32LE(offset, 42);
        locals.push(local, filename, packed); central.push(entry, filename);
        offset += local.length + filename.length + packed.length;
    }
    if (central.length / 2 > 65535) throw new Error('Too many ZIP32 entries');
    const directory = Buffer.concat(central), end = Buffer.alloc(22);
    end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(central.length / 2, 8); end.writeUInt16LE(central.length / 2, 10);
    end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
    return Buffer.concat([...locals, directory, end]);
}
