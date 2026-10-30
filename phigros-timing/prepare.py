from pathlib import Path
from zipfile import ZipFile
import argparse, hashlib, json, struct

parser=argparse.ArgumentParser()
parser.add_argument('apk',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
work=root/'.tools/phigros-inspect';work.mkdir(exist_ok=True)
build=root/'.tools/phigros-timing-build';build.mkdir(exist_ok=True)
with ZipFile(args.apk) as z:
    core=z.read('lib/arm64-v8a/libil2cpp.so')
    metadata=z.read('assets/bin/Data/Managed/Metadata/global-metadata.dat')
assert hashlib.sha256(core).hexdigest()=='c1a2b1fbc40b4ba93306eccaba90ac13983e15964726904aec039aff0cb657da','Unsupported game core'
assert hashlib.sha256(metadata).hexdigest()=='082f932994e454d3b7de9a90587fb6a76865103228ae3379e1a4701235197733','Unsupported metadata'
p=work/'inspection.json'
report=json.loads(p.read_text(encoding='utf8')) if p.exists() else {}
report['source']=str(args.apk.resolve())
p.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
phoff=struct.unpack_from('<Q',core,32)[0];size,count=struct.unpack_from('<HH',core,54)
def offset(va):
    for i in range(count):
        kind,_,off,start,_,length,_,_=struct.unpack_from('<II6Q',core,phoff+size*i)
        if kind==1 and start<=va<start+length:return off+va-start
    raise ValueError(hex(va))
addresses=[0x1d85950,0x1d85b5c,0x1d82114,0x1d83b50,0x1d87864,0x1d87680,0x1d8758c,0x1d874ac,
           0x1cb7c6c,0x1cbae10,0x1ca669c,0x1cc311c,0x1cc31d0,0x1cc3458,0x1cb9d34,0x1cb959c,0x1cb6850,0x3c53f20,
           0x1cc33dc,0x1cc42c8,0x1d5766c,
           0x3c567a4,0x3c66894,0x3c1b130,0x3c24f6c,0x3c24f94,0x1d74334,0x1cb97f0,0x1db5b90,0x1db5668,0x3c567e0,0x3c5e324,0x1d7ac10,0x3c5d7e8,0x1d859d0,0x3c597b0,0x3c55d24,0x1db5500,0x1db57c0,0x1db53e0,0x1db5d00,0x1db5aa4,0x3c0f8b0,0x3c11498]
lines=['#pragma once','#include <cstdint>','struct CoreGuard { uintptr_t offset; unsigned char bytes[32]; };','constexpr CoreGuard CoreGuards[]{']
for a in addresses:lines.append('{'+hex(a)+', {'+','.join(hex(b) for b in core[offset(a):offset(a)+32])+'}},')
lines.append('};')
(build/'core_guard.h').write_text('\n'.join(lines),encoding='utf8')
print('Exact supported 4.0.1 core verified; runtime guards generated.')
