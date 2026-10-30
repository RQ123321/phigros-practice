"""Build an independent test APK; original game code/assets remain byte-identical."""
from pathlib import Path
import sys, json, zipfile, shutil, hashlib, struct, copy
sys.path.insert(0, str(Path(__file__).resolve().parent))
from manifest import Manifest

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / '.tools/phigros-inspect'
source = Path(json.loads((WORK/'inspection.json').read_text(encoding='utf8'))['source'])
library = ROOT/'.tools/phigros-timing-build/libphigrostiming.so'
dex = ROOT/'.tools/phigros-timing-dex/classes.dex'
target = WORK/'timing-unsigned.apk'
package = 'com.PigeonGames.Phigros.timing'
def rename_resource_package(data):
    data=bytearray(data)
    kind,header,size=struct.unpack_from('<HHI',data)
    assert kind==2 and size==len(data)
    at=header; changed=0
    while at<len(data):
        kind,_,size=struct.unpack_from('<HHI',data,at)
        assert size>=8 and at+size<=len(data)
        if kind==0x200:
            name=bytes(data[at+12:at+268]).decode('utf-16le').split('\0')[0]
            if name=='com.PigeonGames.Phigros':
                encoded=package.encode('utf-16le')
                assert len(encoded)<256
                data[at+12:at+268]=encoded+bytes(256-len(encoded));changed+=1
        at+=size
    assert changed==1, 'Expected exactly one application resource package'
    return bytes(data)
assert library.is_file() and dex.is_file()
assert dex.read_bytes().startswith(b'dex\n')
# Android APK ZIP32 supports unsigned 32-bit offsets. Python's conservative
# default switches to ZIP64 at 2 GiB, which Android's signer does not accept.
zipfile.ZIP64_LIMIT = (1 << 32) - 1
assert source.stat().st_size + library.stat().st_size < zipfile.ZIP64_LIMIT - 10000000

with zipfile.ZipFile(source) as original:
    assert 'classes2.dex' not in original.namelist()
    m = Manifest(original.read('AndroidManifest.xml'))
    original_nodes = sum(1 for kind,_,_,_ in m.nodes() if kind==0x102)
    m.patch(package)
    for kind, name, chunk, attrs in list(m.nodes()):
        if kind==0x102 and name=='manifest':
            for key,value,at in attrs:
                if key=='versionName':m.set_text(chunk,at,'4.0.1-timing.17')
                elif key=='versionCode':struct.pack_into('<I',chunk,at+16,173)
    manifest = m.encode()
    check=Manifest(manifest)
    assert sum(1 for kind,_,_,_ in check.nodes() if kind==0x102)==original_nodes+1
    assert any(n=='manifest' and any(k=='package' and v==package for k,v,_ in a) for kind,n,c,a in check.nodes())
    application = next(a for kind,n,c,a in check.nodes() if kind==0x102 and n=='application')
    assert any(k=='isGame' and v==1 for k,v,_ in application)
    assert any(k=='appCategory' and v==0 for k,v,_ in application)
    (WORK/'timing-manifest.xml').write_bytes(manifest)
    copied, removed = 0, []
    with zipfile.ZipFile(target,'w',allowZip64=False) as out:
        for item in original.infolist():
            name=item.filename
            if (name.startswith('META-INF/') and name.upper().endswith(('.RSA','.DSA','.EC','.SF','.MF'))) or name.startswith('lib/armeabi-v7a/') or name.startswith('assets/dexopt/'):
                removed.append(name);continue
            entry=copy.copy(item)
            if name=='AndroidManifest.xml':out.writestr(entry,manifest)
            elif name=='resources.arsc':out.writestr(entry,rename_resource_package(original.read(name)))
            else:
                with original.open(item) as src,out.open(entry,'w') as dst:shutil.copyfileobj(src,dst,4*1024*1024)
            copied+=1
            if copied%500==0:print(f'Copied {copied} entries',flush=True)
        out.writestr('classes2.dex',dex.read_bytes(),compress_type=zipfile.ZIP_DEFLATED)
        out.writestr('lib/arm64-v8a/libphigrostiming.so',library.read_bytes(),compress_type=zipfile.ZIP_STORED)
    with zipfile.ZipFile(target) as result:
        assert len(result.namelist())==len(set(result.namelist()))
        for name in original.namelist():
            if name in ('AndroidManifest.xml','resources.arsc') or name in removed:continue
            # Streaming copy preserves original ZIP CRC; spot-check executable code bytes.
            assert result.getinfo(name).CRC==original.getinfo(name).CRC,name
        for name in ['classes.dex','lib/arm64-v8a/libil2cpp.so','lib/arm64-v8a/libunity.so']:
            assert hashlib.sha256(result.read(name)).digest()==hashlib.sha256(original.read(name)).digest(),name
        assert result.read('classes2.dex')==dex.read_bytes()
        assert result.read('lib/arm64-v8a/libphigrostiming.so')==library.read_bytes()
        assert result.read('resources.arsc')==rename_resource_package(original.read('resources.arsc'))
    report={'source':str(source),'package':package,'version':'4.0.1-timing.17','copiedEntries':copied,
        'removedEntries':removed,'coreUnchanged':True,'originalDexUnchanged':True,'assetCrcsMatch':True,
        'resourceTablePackageRenamed':True,'isGame':True,'appCategory':'game',
        'captureLibrarySha256':hashlib.sha256(library.read_bytes()).hexdigest(),
        'arm64SelfTests':'compiled; device execution must be confirmed separately for this build'}
    (WORK/'package-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print('Unsigned independent APK built:',target.stat().st_size,flush=True)
