#!/usr/bin/env python3
"""Report the largest wasm function bodies in BYTES.
run() is register-pressure bound, so its size is the number that governs
interpreter speed (webwine/browser/README.md)."""
import sys
d=open(sys.argv[1],'rb').read()
assert d[:4]==b'\0asm'
def uleb(o):
    r=0;s=0
    while True:
        b=d[o];o+=1;r|=(b&0x7f)<<s;s+=7
        if not b&0x80: return r,o
o=8; code=None; names={}; nimp=0
while o<len(d):
    sid,o=uleb(o); sz,o=uleb(o); body=o; o+=sz
    if sid==10: code=(body,sz)
    elif sid==2:                       # imports come FIRST in the function index
        n,q=uleb(body)                 # space, so code entry i == func index nimp+i
        for _ in range(n):
            ln,q=uleb(q); q+=ln
            ln,q=uleb(q); q+=ln
            kind=d[q]; q+=1
            if kind==0: nimp+=1; _,q=uleb(q)
            elif kind==1: q+=1; lim=d[q]; q+=1; _,q=uleb(q); q=(uleb(q)[1] if lim else q)
            elif kind==2: lim=d[q]; q+=1; _,q=uleb(q); q=(uleb(q)[1] if lim else q)
            elif kind==3: q+=2
    elif sid==0:
        n,p=uleb(body)
        nm=d[p:p+n]; p+=n
        if nm==b'name':
            while p<body+sz:
                st,p2=uleb(p); ssz,p2=uleb(p2)
                if st==1:
                    cnt,q=uleb(p2)
                    for _ in range(cnt):
                        idx,q=uleb(q); ln,q=uleb(q); names[idx]=d[q:q+ln].decode('utf8','replace'); q+=ln
                p=p2+ssz
assert code, "no code section"
body,sz=code
cnt,o=uleb(body)
sizes=[]
for i in range(cnt):
    fsz,o2=uleb(o); sizes.append((fsz,i)); o=o2+fsz
sizes.sort(reverse=True)
tot=sum(s for s,_ in sizes)
print(f"{cnt} functions, {tot/1024:.0f}KB of code")
for s,i in sizes[:10]:
    print(f"  {s/1024:8.1f}KB  func#{nimp+i} {names.get(nimp+i,'?')}  ({100*s/tot:.1f}% of code)")
