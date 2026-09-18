import struct

d = open(r"C:\3DRadRTX\3DRad_res\objects\Particles\object.dll", "rb").read()
text = d[0x1000:0x18000]
print("fld/fstp 0xDD0-0xE30:")
for i in range(len(text) - 6):
    if text[i] == 0xD9 and text[i + 1] in (
        0x80, 0x81, 0x82, 0x83, 0x86, 0x87, 0x98, 0x99, 0x9A, 0x9B, 0x9E, 0x9F
    ):
        disp = struct.unpack_from("<I", text, i + 2)[0]
        if 0xDD0 <= disp <= 0xE30:
            op = "fld" if text[i + 1] < 0x90 else "fstp"
            print(hex(0x1000 + i), op, hex(disp))

print("--- GET pushes 0x400-0x430 ---")
for gid in range(0x400, 0x430):
    pat = bytes([0x68]) + struct.pack("<I", gid)
    c = d.count(pat)
    if c:
        print("push", hex(gid), c)

print("--- 0x4cde context (fld 0x57C sim) ---")
print(d[0x4cc0:0x4d40].hex())
print("--- 0x4c50 context (fld 0x5BC) ---")
print(d[0x4c50:0x4c90].hex())
