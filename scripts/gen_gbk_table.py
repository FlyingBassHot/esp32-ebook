#!/usr/bin/env python3
"""生成 GBK(含 CP936) 双字节 -> Unicode 解码表，输出 C 源文件。"""
import sys

LEAD_LO, LEAD_HI = 0x81, 0xFE          # 126
# trail 有效值: 0x40-0x7E(63) + 0x80-0xFE(127) = 190
def trail_index(t):
    if 0x40 <= t <= 0x7E: return t - 0x40
    if 0x80 <= t <= 0xFE: return t - 0x41
    return -1

rows = []
total = 0
for lead in range(LEAD_LO, LEAD_HI + 1):
    row = []
    for ti in range(190):
        # 反解 trail
        t = ti + 0x40 if ti < 63 else ti + 0x41
        cp = 0
        try:
            ch = bytes([lead, t]).decode('gbk')
            if len(ch) == 1:
                cp = ord(ch)
        except Exception:
            cp = 0
        row.append(cp)
        if cp: total += 1
    rows.append(row)

out = sys.argv[1]
with open(out, 'w') as f:
    f.write("// 自动生成：GBK 双字节 -> Unicode 解码表（勿手改）\n")
    f.write("// 生成脚本: scripts/gen_gbk_table.py\n")
    f.write('#include "gbk_table.h"\n\n')
    f.write("const uint16_t GBK_TABLE[126][190] = {\n")
    for i, row in enumerate(rows):
        vals = ",".join(str(v) for v in row)
        f.write("    {" + vals + "},  // lead 0x%02X\n" % (LEAD_LO + i))
    f.write("};\n")
print(f"OK: {total} entries mapped, written to {out}")
