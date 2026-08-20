import subprocess, os, sys, re
GAME=os.environ.get("GAME", os.path.expanduser("~/games/netduke32_v1.2.1"))
PEDIR=os.environ.get("PEDIR", os.path.expanduser("~/dev/wine/wine-macos/lib/wine/i386-windows"))
OBJDUMP=os.environ.get("OBJDUMP", "/opt/homebrew/bin/i686-w64-mingw32-objdump")
def imports(path):
    try: out=subprocess.run([OBJDUMP,"-x",path],capture_output=True,text=True,timeout=30).stdout
    except Exception as e: return []
    return re.findall(r'DLL Name:\s*(\S+)', out)
# available PE dlls (lowercase set)
avail={f.lower():f for f in os.listdir(PEDIR) if f.lower().endswith('.dll')}
seen=set(); queue=[("netduke32.exe", os.path.join(GAME,"netduke32.exe"))]
result=set()
while queue:
    name,path=queue.pop(0)
    for dep in imports(path):
        dl=dep.lower()
        if dl in seen: continue
        seen.add(dl)
        if dl in avail:
            result.add(avail[dl])
            queue.append((avail[dl], os.path.join(PEDIR,avail[dl])))
        else:
            # api-ms-win-* redirect via apisetschema; note but don't fail
            pass
for r in sorted(result): print(r)
print("COUNT", len(result), file=sys.stderr)
