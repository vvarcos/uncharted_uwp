"""Compare two BBPROBE5 images field by field: names, modules, bindings, relocations, segments."""
import struct
import sys


def parse(path):
    data = open(path, 'rb').read()
    magic, size, entry, nseg, nrel, nnames, flags = struct.unpack_from('<8s6Q', data, 0)
    assert magic == b'BBPROBE5', magic
    pos = 56
    procparam, = struct.unpack_from('<Q', data, pos); pos += 8
    tls = struct.unpack_from('<4Q', data, pos); pos += 32
    nmods, = struct.unpack_from('<Q', data, pos); pos += 8
    mods = [struct.unpack_from('<7Q', data, pos + i * 56) for i in range(nmods)]
    pos += nmods * 56
    nbind, = struct.unpack_from('<Q', data, pos); pos += 8
    binds = [struct.unpack_from('<3Q', data, pos + i * 24) for i in range(nbind)]
    pos += nbind * 24
    segs = [struct.unpack_from('<3Q', data, pos + i * 24) for i in range(nseg)]
    pos += nseg * 24
    names = [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0', 1)[0].decode() for i in range(nnames)]
    pos += nnames * 128
    rels = [struct.unpack_from('<4Q', data, pos + i * 32) for i in range(nrel)]
    pos += nrel * 32
    image = data[pos:]
    return dict(size=size, entry=entry, nseg=nseg, nrel=nrel, nnames=nnames, flags=flags,
                procparam=procparam, tls=tls, mods=mods, binds=binds, segs=segs,
                names=names, rels=rels, image=image)


a = parse(sys.argv[1])
b = parse(sys.argv[2])
print(f"C : size={a['size']} segs={a['nseg']} rels={a['nrel']} names={a['nnames']} binds={len(a['binds'])} mods={a['mods']}")
print(f"PY: size={b['size']} segs={b['nseg']} rels={b['nrel']} names={b['nnames']} binds={len(b['binds'])} mods={b['mods']}")
print('names equal:', a['names'] == b['names'])
common = min(len(a['names']), len(b['names']))
for i in range(common):
    if a['names'][i] != b['names'][i]:
        print(f'first name diff at {i}: C={a["names"][i]!r} PY={b["names"][i]!r}')
        print('C  around:', a['names'][max(0, i - 2):i + 6])
        print('PY around:', b['names'][max(0, i - 2):i + 6])
        break
else:
    print('prefix equal; C extra:', a['names'][common:common + 30])
print('segs equal:', a['segs'] == b['segs'])
print('rels equal:', a['rels'] == b['rels'])
print('binds equal:', a['binds'] == b['binds'])
print('image equal:', a['image'] == b['image'])
