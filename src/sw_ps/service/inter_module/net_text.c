/**
 *******************************************************************************
 * @file    net_text.c
 * @brief   服务层的文本拼接小工具 (见 net_text.h 的说明)
 *******************************************************************************
 */

#include "service/inter_module/net_text.h"

#include <stddef.h>             /* NULL */

uint16_t Svc_Text_AppendStr(char *dst, uint16_t pos, uint16_t size, const char *str)
{
    if ((dst == NULL) || (size == 0U)) {
        return 0U;
    }

    if (str != NULL) {
        while ((*str != '\0') && ((uint16_t)(pos + 1U) < size)) {
            dst[pos++] = *str++;
        }
    }

    dst[pos] = '\0';

    return pos;
}

uint16_t Svc_Text_AppendUint(char *dst, uint16_t pos, uint16_t size, uint32_t value)
{
    char     digits[10];
    uint16_t count = 0U;

    if ((dst == NULL) || (size == 0U)) {
        return 0U;
    }

    if (value == 0U) {
        digits[count++] = '0';
    }

    while ((value > 0U) && (count < (uint16_t)sizeof(digits))) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while ((count > 0U) && ((uint16_t)(pos + 1U) < size)) {
        dst[pos++] = digits[--count];
    }

    dst[pos] = '\0';

    return pos;
}

uint16_t Svc_Text_AppendHex2(char *dst, uint16_t pos, uint16_t size, uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";

    if ((dst == NULL) || (size == 0U)) {
        return 0U;
    }

    if ((uint16_t)(pos + 3U) <= size) {
        dst[pos++] = hex[(value >> 4) & 0x0FU];
        dst[pos++] = hex[value & 0x0FU];
    }

    dst[pos] = '\0';

    return pos;
}

uint16_t Svc_Text_AppendHex32(char *dst, uint16_t pos, uint16_t size, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int32_t          shift;

    if ((dst == NULL) || (size == 0U)) {
        return 0U;
    }

    for (shift = 28; shift >= 0; shift -= 4) {
        if ((uint16_t)(pos + 1U) >= size) {
            break;
        }
        dst[pos++] = hex[(value >> (uint32_t)shift) & 0x0FU];
    }

    dst[pos] = '\0';

    return pos;
}
