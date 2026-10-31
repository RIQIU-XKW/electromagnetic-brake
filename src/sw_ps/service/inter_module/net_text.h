/**
 *******************************************************************************
 * @file    net_text.h
 * @brief   服务层的文本拼接小工具(追加字符串/十进制/十六进制, 无堆分配)
 *
 * 不用 sprintf/snprintf: 裸机 newlib 且 configUSE_NEWLIB_REENTRANT = 0, stdio
 * 在多任务下不可重入。约定: 所有函数返回新的写入位置, 并保证 dst 以 '\0' 结尾。
 *******************************************************************************
 */

#ifndef NET_TEXT_H
#define NET_TEXT_H

#include <stdint.h>

/** @brief  追加一个字符串。 */
uint16_t Svc_Text_AppendStr(char *dst, uint16_t pos, uint16_t size, const char *str);

/** @brief  追加一个无符号十进制数。 */
uint16_t Svc_Text_AppendUint(char *dst, uint16_t pos, uint16_t size, uint32_t value);

/** @brief  追加一个字节的两位十六进制(大写)。 */
uint16_t Svc_Text_AppendHex2(char *dst, uint16_t pos, uint16_t size, uint8_t value);

/** @brief  追加一个 32 位数的八位十六进制(大写, 不带 0x 前缀)。 */
uint16_t Svc_Text_AppendHex32(char *dst, uint16_t pos, uint16_t size, uint32_t value);

#endif /* NET_TEXT_H */
