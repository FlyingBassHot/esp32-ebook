#ifndef __GBK_TABLE_H__
#define __GBK_TABLE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GBK(含 CP936) 双字节解码表
// 下标 0: lead - 0x81 (0x81..0xFE)
// 下标 1: trail 索引: 0x40-0x7E -> 0-62, 0x80-0xFE -> 63-189
// 值: Unicode 码点（BMP），0 表示无效序列
extern const uint16_t GBK_TABLE[126][190];

static inline int gbk_trail_index(uint8_t t) {
    if (t >= 0x40 && t <= 0x7E) return t - 0x40;
    if (t >= 0x80 && t <= 0xFE) return t - 0x41;
    return -1;
}

// 把一个 Unicode 码点编码为 UTF-8，写入 buf（至少 3 字节），返回字节数
int unicode_to_utf8(uint32_t cp, char* buf);

#ifdef __cplusplus
}
#endif

#endif
