#!/usr/bin/python3

import sys
import os
import lsvd
import test3 as t3
import uuid
import blkdev

def prettyprint(p, insns):
    for field,fmt in insns:
        x = eval('p.' + field)
        pad = ' ' * (10 - len(field)) + ':'
        if type(fmt) == str:
            print(field, pad, fmt % x)
        elif callable(fmt):
            print(field, pad, fmt(x))
        elif type(fmt) == dict:
            print(field, pad, fmt[x])

fmt_uuid = lambda u: uuid.UUID(bytes=bytes(u[:]))

fieldnames = dict({(10, 'DATA'),(11, 'CKPT'),(12, 'PAD'),(13, 'SUPER'),
                       (14, 'W_SUPER'),(15, 'R_SUPER')})
magic = lambda x: 'ok' if x == lsvd.LSVD_MAGIC else '**BAD**'
blk_fmt = lambda x: '%d%s' % (x, '' if x < npages else ' *INVALID*')

hdr_pp = [['magic', magic], ['type', fieldnames], ['vol_uuid', fmt_uuid],
              ['write_super', blk_fmt], ['read_super', blk_fmt]]

wsup_pp = [['magic', magic], ['type', fieldnames], ['vol_uuid', fmt_uuid],
               ['seq', '%d'], ['base', '%d'], ['limit', '%d'],
               ['next', '%d'], ['oldest', '%d']]

rsup_pp = [["magic", magic], ["type", fieldnames], ['vol_uuid', fmt_uuid], ["unit_size", '%d'], 
               ["base", '%d'], ["units", '%d'], ["map_start", '%d'],["map_blocks", '%d'],
               ["bitmap_start", '%d'], ["bitmap_blocks", '%d'], ["evict_type", '%d'],
               ["evict_start", '%d'], ["evict_blocks", '%d']]

fd = os.open(sys.argv[1], os.O_RDONLY)
sb = os.fstat(fd)
if blkdev.S_ISBLK(sb.st_mode):
    npages = blkdev.dev_get_size(fd)
else:
    npages = sb.st_size // 4096

super = t3.c_super(fd)
print('superblock: (0)')
prettyprint(super, hdr_pp)
print('\nwrite superblock: (%s)' % blk_fmt(super.write_super))
if super.write_super < npages:
    w_super = t3.c_w_super(fd, super.write_super)
    prettyprint(w_super, wsup_pp)

print('\nread superblock: (%s)' % blk_fmt(super.read_super))
if super.read_super < npages:
    r_super = t3.c_r_super(fd, super.read_super)
    prettyprint(r_super, rsup_pp)

def read_exts(b, npgs, n):
    buf = os.pread(fd, npgs*4096, b*4096)
    bytes = n*lsvd.sizeof_j_map_extent
    e = (lsvd.j_map_extent*n).from_buffer(bytearray(buf[0:bytes]))
    return [(_.lba, _.len, _.page) for _ in e]

if super.write_super < npages:
    b = w_super.oldest
    while True:
        [j,e] = t3.c_hdr(fd, b)
    #    if j.type != lsvd.LSVD_J_DATA:
    #        break
        if j.magic != lsvd.LSVD_MAGIC:
            break
        if j.type == lsvd.LSVD_J_CKPT:
            print('\ncheckpoint:', b)
            e = read_exts(b+1, j.len - 1, w_super.map_entries)
            print(' '.join(['(%d+%d->%d)' % _ for _ in e]))
        else:
            h_pp = [["magic", magic], ["type", fieldnames], ["seq", '%d'], ["len", '%d']]
            print('\ndata: (%d)' % b)
            prettyprint(j, h_pp)
            print('extents    :', ' '.join(['%d+%d' % (_.lba,_.len) for _ in e]))
        b = b + j.len

