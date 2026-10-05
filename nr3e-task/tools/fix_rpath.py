#!/usr/bin/env python3
import sys
p=sys.argv[1]
d=open(p,'rb').read()
old=b'/data/data/com.termux/files/usr/bin/../../usr/lib:/system/lib64:/vendor/lib64'
new=b'/system/lib64:/vendor/lib64:/system_ext/lib64'
if old in d:
    d=d.replace(old, new.ljust(len(old), b'\0'), 1)
    open(p,'wb').write(d)
    print('patched rpath', p)
else:
    print('no termux rpath', p)
