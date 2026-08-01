#!/usr/bin/env python3
# Merge Unity 6 UnityFS archives (ver8, LZ4/LZ4HC, blocksInfo-at-front, combined)
# into ONE archive so all serialized files + externals resolve as siblings.
import struct, sys, os, lz4.block

def rc(b,o):
    e=b.index(b'\0',o); return b[o:e], e+1

def parse(path):
    fsz=os.path.getsize(path)
    with open(path,'rb') as src:
        d=src.read()
    o=0
    magic,o=rc(d,o); ver=struct.unpack_from('>i',d,o)[0]; o+=4
    if magic != b'UnityFS':
        raise ValueError('%s is not a UnityFS archive (magic=%r)' % (path, magic))
    uver,o=rc(d,o); rev,o=rc(d,o)
    size=struct.unpack_from('>q',d,o)[0]; o+=8
    cbi=struct.unpack_from('>i',d,o)[0]; o+=4
    ubi=struct.unpack_from('>i',d,o)[0]; o+=4
    fl=struct.unpack_from('>i',d,o)[0]; o+=4
    o=(o+15)&~15
    atEnd=fl&0x80
    bi_off=(fsz-cbi) if atEnd else o
    comp=d[bi_off:bi_off+cbi]
    ct=fl&0x3f
    raw=comp if ct==0 else lz4.block.decompress(comp,uncompressed_size=ubi)
    p=0; dhash=raw[p:p+16]; p+=16
    bc=struct.unpack_from('>i',raw,p)[0]; p+=4
    blocks=[]
    for _ in range(bc):
        u,c,bf=struct.unpack_from('>IIH',raw,p); p+=10; blocks.append((u,c,bf))
    nc=struct.unpack_from('>i',raw,p)[0]; p+=4
    nodes=[]
    for _ in range(nc):
        no,ns,nf=struct.unpack_from('>qqi',raw,p); p+=20
        npath,p=rc(raw,p); nodes.append((no,ns,nf,npath))
    data_start=(o if atEnd else bi_off+cbi)
    data_start=(data_start+15)&~15
    blockdata=d[data_start:data_start+sum(b[1] for b in blocks)]
    return dict(magic=magic,ver=ver,uver=uver,rev=rev,flags=fl,dhash=dhash,
                blocks=blocks,nodes=nodes,blockdata=blockdata,
                total_u=sum(b[0] for b in blocks),fsz=fsz)

def build_blocksinfo(dhash,blocks,nodes):
    out=bytearray(); out+=dhash
    out+=struct.pack('>i',len(blocks))
    for u,c,bf in blocks: out+=struct.pack('>IIH',u,c,bf)
    out+=struct.pack('>i',len(nodes))
    for no,ns,nf,npath in nodes:
        out+=struct.pack('>qqi',no,ns,nf); out+=npath+b'\0'
    return bytes(out)

def write_merged(archs, outpath, compress_bi=True):
    base=archs[0]
    blocks=[]; nodes=[]; blockdata=bytearray(); off_shift=0
    for a in archs:
        blocks+=a['blocks']
        for (no,ns,nf,npath) in a['nodes']:
            nodes.append((no+off_shift,ns,nf,npath))
        blockdata+=a['blockdata']
        off_shift+=a['total_u']
    raw_bi=build_blocksinfo(base['dhash'],blocks,nodes)
    ubi=len(raw_bi)
    if compress_bi:
        comp_bi=lz4.block.compress(raw_bi,mode='high_compression',store_size=False)
        ctype=3  # LZ4HC-compatible; engine decompresses LZ4 stream
    else:
        comp_bi=raw_bi; ctype=0
    flags=(base['flags'] & ~0x3f) | ctype   # keep combined(0x40)/atEnd bits, set BI comp type
    # header
    hdr=bytearray()
    hdr+=b'UnityFS\0'
    hdr+=struct.pack('>i',base['ver'])
    hdr+=base['uver']+b'\0'; hdr+=base['rev']+b'\0'
    size_pos=len(hdr); hdr+=struct.pack('>q',0)   # patch later
    hdr+=struct.pack('>i',len(comp_bi))           # compBlocksInfo
    hdr+=struct.pack('>i',ubi)                     # uncompBlocksInfo
    hdr+=struct.pack('>i',flags)
    # align16, blocksInfo, align16, blockdata
    def pad16(b):
        while len(b)&15: b+=b'\0'
        return b
    body=bytearray(hdr); body=bytearray(pad16(body))
    body+=comp_bi
    body=bytearray(pad16(body))
    body+=blockdata
    struct.pack_into('>q',body,size_pos,len(body))
    with open(outpath,'wb') as dst:
        dst.write(body)
    return dict(size=len(body),nblocks=len(blocks),nnodes=len(nodes),ubi=ubi,cbi=len(comp_bi),flags=flags)

if __name__=='__main__':
    cmd=sys.argv[1]
    if cmd=='roundtrip':
        base=parse(sys.argv[2])
        info=write_merged([base],'_rt.unity3d',compress_bi=True)
        rt=parse('_rt.unity3d')
        ok_nodes=rt['nodes']==base['nodes']
        ok_blocks=rt['blocks']==base['blocks']
        ok_bd=rt['blockdata']==base['blockdata']
        # decompress node0 first block to verify readability
        u,c,bf=base['blocks'][0]
        b0=lz4.block.decompress(base['blockdata'][:c],uncompressed_size=u)
        u2,c2,bf2=rt['blocks'][0]
        b0r=lz4.block.decompress(rt['blockdata'][:c2],uncompressed_size=u2)
        print("roundtrip: nodes=%s blocks=%s blockdata=%s node0decomp=%s"%(ok_nodes,ok_blocks,ok_bd,b0==b0r))
        print("  base fsz=%d rt size=%d"%(base['fsz'],info['size']))
        print("  info=",info)
    elif cmd=='merge':
        outp=sys.argv[2]; archs=[parse(p) for p in sys.argv[3:]]
        info=write_merged(archs,outp,compress_bi=True)
        print("merged -> %s"%outp); print(info)
        r=parse(outp)
        print("re-parse OK: nodes=%d blocks=%d total_u=%d"%(len(r['nodes']),len(r['blocks']),r['total_u']))
        print("  first node:",r['nodes'][0]," ; a datapack node:",r['nodes'][7]," ; last:",r['nodes'][-1])
