"""Small binary-XML editor preserving every existing manifest node/attribute."""
import struct

ANDROID = 'http://schemas.android.com/apk/res/android'
NONE = 0xffffffff
RESOURCE_IDS = {'name':0x01010003, 'label':0x01010001, 'exported':0x01010010,
                'authorities':0x01010018, 'initOrder':0x0101001a,
                'isGame':0x010103f4, 'appCategory':0x01010545}

class Manifest:
    def __init__(self, data):
        assert struct.unpack_from('<HH', data) == (3, 8)
        self.chunks = []
        self.strings = []
        self.resources = []
        pos = 8
        while pos < len(data):
            kind, header, size = struct.unpack_from('<HHI', data, pos)
            chunk = bytearray(data[pos:pos+size])
            assert len(chunk) == size and size >= 8
            self.chunks.append(chunk)
            if kind == 1:
                count, styles, flags, start, _ = struct.unpack_from('<5I', chunk, 8)
                assert styles == 0, 'Styled manifest string pool is unsupported'
                for i in range(count):
                    at = start + struct.unpack_from('<I', chunk, header+i*4)[0]
                    def length(at, wide=False):
                        if wide:
                            n = struct.unpack_from('<H', chunk, at)[0]; at += 2
                            if n & 0x8000:
                                n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', chunk, at)[0]; at += 2
                        else:
                            n = chunk[at]; at += 1
                            if n & 0x80: n = ((n & 0x7f) << 8) | chunk[at]; at += 1
                        return n, at
                    if flags & 0x100:
                        _, at = length(at); n, at = length(at)
                        self.strings.append(bytes(chunk[at:at+n]).decode('utf8'))
                    else:
                        n, at = length(at, True)
                        self.strings.append(bytes(chunk[at:at+n*2]).decode('utf-16le'))
            if kind == 0x180:
                self.resources = list(struct.unpack_from('<'+'I'*((size-8)//4), chunk, 8))
            pos += size
        assert pos == len(data)

    def intern(self, text):
        if text not in self.strings: self.strings.append(text)
        return self.strings.index(text)

    def attr_name(self, name):
        index = self.intern(name)
        self.resources.extend([0] * max(0, index + 1 - len(self.resources)))
        expected = RESOURCE_IDS[name]
        assert self.resources[index] in (0, expected)
        self.resources[index] = expected
        return index

    def nodes(self):
        for chunk in self.chunks:
            kind = struct.unpack_from('<H', chunk)[0]
            if kind not in (0x102, 0x103): continue
            name = self.strings[struct.unpack_from('<I', chunk, 20)[0]]
            attrs = []
            if kind == 0x102:
                start, size, count = struct.unpack_from('<3H', chunk, 24)
                for i in range(count):
                    at = 16 + start + i * size
                    ns, n, raw, _, _, tp, value = struct.unpack_from('<IIIHBBI', chunk, at)
                    text = self.strings[raw] if raw != NONE else self.strings[value] if tp == 3 else value
                    attrs.append((self.strings[n], text, at))
            yield kind, name, chunk, attrs

    def set_text(self, chunk, at, value):
        index = self.intern(value)
        struct.pack_into('<I', chunk, at+8, index)
        struct.pack_into('<HBBI', chunk, at+12, 8, 0, 3, index)

    def set_typed_attribute(self, element, name, value, value_type):
        index = self.attr_name(name)
        for kind, node, chunk, attrs in self.nodes():
            if kind != 0x102 or node != element: continue
            for key, _, at in attrs:
                if key == name:
                    struct.pack_into('<I', chunk, at+8, NONE)
                    struct.pack_into('<HBBI', chunk, at+12, 8, 0, value_type, value)
                    return
            start, size, count, id_index, class_index, style_index = struct.unpack_from('<6H', chunk, 24)
            assert size == 20 and not any((id_index, class_index, style_index))
            at = 16 + start
            existing = [bytes(chunk[at+i*size:at+(i+1)*size]) for i in range(count)]
            existing.append(struct.pack('<IIIHBBI',self.intern(ANDROID),index,NONE,8,0,value_type,value))
            existing.sort(key=lambda a:self.resources[struct.unpack_from('<I',a,4)[0]])
            chunk[at:at+count*size] = b''.join(existing)
            struct.pack_into('<I', chunk, 4, len(chunk))
            struct.pack_into('<H', chunk, 28, count+1)
            return
        raise ValueError('Missing element: '+element)

    def patch(self, package):
        original = next(v for k,n,c,a in self.nodes() if k==0x102 and n=='manifest' for key,v,at in a if key=='package')
        for kind, name, chunk, attrs in list(self.nodes()):
            if kind != 0x102: continue
            for key, value, at in attrs:
                if name == 'manifest' and key == 'package': self.set_text(chunk, at, package)
                elif name == 'application' and key == 'label': self.set_text(chunk, at, 'Phigros 准度测试版')
                elif key == 'name' and name in ('application','activity','activity-alias','service','receiver','provider'):
                    if isinstance(value,str) and value.startswith('.'): self.set_text(chunk, at, original+value)
                    elif isinstance(value,str) and '.' not in value: self.set_text(chunk, at, original+'.'+value)
                elif key == 'name' and name in ('permission','uses-permission') and isinstance(value,str) and value.startswith(original+'.'):
                    self.set_text(chunk, at, package+value[len(original):])
                elif key == 'authorities' and isinstance(value,str) and value.startswith(original):
                    self.set_text(chunk, at, package+value[len(original):])

        attrs = []
        for name, value, tp in [('name','com.pigeongames.timing.TimingBridge',3),
                                ('authorities',package+'.timing-init',3),('exported',0,18),('initOrder',100,16)]:
            index = self.attr_name(name)
            raw = self.intern(value) if tp == 3 else NONE
            data = raw if tp == 3 else value
            attrs.append((RESOURCE_IDS[name],struct.pack('<IIIHBBI',self.intern(ANDROID),index,raw,8,0,tp,data)))
        body = b''.join(a for _,a in sorted(attrs))
        provider = self.intern('provider')
        start = struct.pack('<HHIII',0x102,16,36+len(body),NONE,NONE)+struct.pack('<II6H',NONE,provider,20,20,len(attrs),0,0,0)+body
        end = struct.pack('<HHIII',0x103,16,24,NONE,NONE)+struct.pack('<II',NONE,provider)
        for kind,name,chunk,_ in self.nodes():
            if kind == 0x103 and name == 'application':
                where = next(i for i,c in enumerate(self.chunks) if c is chunk)
                self.chunks[where:where] = [bytearray(start),bytearray(end)]
                break
        else: raise ValueError('No application end node')
        self.set_typed_attribute('application', 'isGame', 1, 18)
        self.set_typed_attribute('application', 'appCategory', 0, 16)

    def encode(self):
        offsets, data = [], bytearray()
        def length(n):
            assert n < 0x8000
            return bytes([n]) if n < 128 else bytes([0x80 | (n>>8),n&255])
        for s in self.strings:
            raw = s.encode('utf8'); offsets.append(len(data))
            data += length(len(s.encode('utf-16le'))//2)+length(len(raw))+raw+b'\0'
        data += bytes((-len(data))%4)
        count = len(offsets); size = 28+count*4+len(data)
        pool = struct.pack('<HHI5I',1,28,size,count,0,0x100,28+count*4,0)+struct.pack('<'+'I'*count,*offsets)+data
        resource = struct.pack('<HHI',0x180,8,8+len(self.resources)*4)+struct.pack('<'+'I'*len(self.resources),*self.resources)
        chunks=[]
        for c in self.chunks:
            kind = struct.unpack_from('<H',c)[0]
            chunks.append(pool if kind==1 else resource if kind==0x180 else c)
        body=b''.join(chunks)
        return struct.pack('<HHI',3,8,len(body)+8)+body
