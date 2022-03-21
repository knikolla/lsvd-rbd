#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import mkdisk

nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

def startup():
    mkdisk.cleanup(img)
    sectors = 10*1024*2 # 10MB
    mkdisk.mkdisk(img, sectors)
    mkcache.mkcache(nvme)
    lsvd.init(img, 1, True)

    lsvd.cache_open(nvme)
    lsvd.wcache_init(1)
    lsvd.rcache_init(2)
    lsvd.fake_rbd_init()
    time.sleep(0.1) # let evict_thread start up in valgrind

def finish():
    lsvd.rcache_shutdown()
    lsvd.wcache_shutdown()
    lsvd.cache_close()
    lsvd.shutdown()

_img = None
def rbd_startup():
    global _img
    print('img', img)
    print('nvme', nvme)
    mkdisk.cleanup(img)
    sectors = 10*1024*2 # 10MB
    mkdisk.mkdisk(img, sectors)
    mkcache.mkcache(nvme)
    name = nvme + ',' + img
    _img = lsvd.rbd_open(name)

def rbd_finish():
    lsvd.rbd_close(_img)

class tests(unittest.TestCase):

    def test_1_wcache_holes(self):
        startup()
        lsvd.write(0, b'A'*20*1024)
        lsvd.flush()
        lsvd.wcache_write(4096, b'B'*4096)
        lsvd.wcache_write(3*4096, b'C'*4096)
        d = lsvd.fake_rbd_read(0, 20*1024)

        self.assertEqual(d, b'A'*4096 + b'B'*4096 + b'A'*4096 + b'C'*4096 + b'A'*4096)
        finish()
        
    # write cache is 125 pages = 1000 sectors
    # read cache is 16 * 64k blocks = 2048 sectors
    # volume is 10MiB = 20480 sectors = 2650 4KB pages
    def test_2_rand_write(self):
        pgs = [_*4096 for _ in [b'A', b'B', b'C', b'D', b'E', b'F', b'G', b'H']]
        rbd_startup()
        pg = 0
        for i in range(26):
            data = pgs[i % 8]
            lsvd.rbd_write(_img, pg*4096, data)
            pg = (pg + 97) % 2650         # close enough to random...
        time.sleep(0.1)
        pg = 0
        for i in range(26):
            d0 = pgs[i % 8]
            d = lsvd.rbd_read(_img, pg*4096, 4096)
            self.assertEqual(d0, d)
            pg = (pg + 97) % 2650         # close enough to random...
        rbd_finish()

        
if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

