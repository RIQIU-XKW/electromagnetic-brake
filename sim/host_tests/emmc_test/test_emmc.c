/**
 * eMMC(SD1/FatFs)文件逻辑 —— 宿主机测试
 *
 * 把 **真实的** bsp/emmc/emmc.c 编到 PC 上, 底下换成内存里的一段字节(host_ff.c),
 * 逐条验证那些"错了要到现场才发现"的逻辑:
 *
 *   1. 新文件先写表头, 记录数从 0 开始;
 *   2. 追加若干行之后 Records/Bytes 对得上;
 *   3. **一行横跨两次 512 字节读块**时还能正确数行 —— 按块找 '\n' 的实现会在这里
 *      少数或多数, 这是写这版代码时真踩过的坑;
 *   4. 半截行(掉电写了一半, 末尾没有换行)能被收尾、**不计入**记录数;
 *   5. 字段数不对的废行回读时被跳过(不能让脏数据进曲线);
 *   6. 分批回读: Start 跳过、每行带前缀、More 正确、批次之间不重不漏;
 *   7. 写失败(卡被拔)返回错误码, 而且**不会**把内存里的暂存当成写成功。
 *
 * 不覆盖什么(只能上板验): SD 控制器本身、cache 一致性、真并发(FatFs 不可重入,
 * 靠 bsp 里那把递归锁串行化)、以及 eMMC 的实际写寿命/速度。
 */

#include <stdio.h>
#include <string.h>

#include "ff.h"                     /* 桩头文件: 提供 DWORD / FRESULT / FR_OK ... */
#include "bsp/emmc/emmc.h"

/* host_ff.c */
const char *HostFs_Data(void);
DWORD       HostFs_Size(void);
unsigned    HostFs_Writes(void);
unsigned    HostFs_Reads(void);
unsigned    HostFs_Syncs(void);
void        HostFs_Reset(void);
void        HostFs_FailWritesAfter(unsigned n, int fr);
void        HostFs_SetShortReadChunk(unsigned n);
void        HostFs_Poke(const char *Text, DWORD At);

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
            printf("  PASS  ");                                              \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL  ");                                              \
        }                                                                    \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                        \
    } while (0)

/* ========================================================================== */

/** @brief  拼一行和 service/emmc 一样格式的 CSV(测试自己拼, 不依赖被测代码)。 */
static void makeLine(char *out, unsigned size, unsigned idx, int base)
{
    (void)snprintf(out, size, "%u,%d,%d,%d,%d,%d,%d,%d,%d,%d\r\n",
                   idx * 10U,               /* t_ms: 100 Hz = 每 10 ms 一条 */
                   base, base + 1, base + 2, base + 3, base + 4,
                   24000 - idx,             /* vbus        */
                   480500 + (int)idx,       /* speed (mm/s) */
                   12500 + (int)idx,        /* x_mm  (mm)   */
                   1143 - (int)idx);        /* laser_mv     */
}

/** @brief  文件里有几行(数 '\n')。 */
static unsigned countNewlines(const char *s, DWORD size)
{
    unsigned n = 0U;
    DWORD    i;

    for (i = 0U; i < size; i++) {
        if (s[i] == '\n') {
            n++;
        }
    }

    return n;
}

/* ========================================================================== */
/*  1. 挂载 / 新建 / 表头                                                       */
/* ========================================================================== */

static void test_open_fresh(void)
{
    Emmc_InfoTypeDef info;

    printf("[1] 挂载 + 新建文件(写表头)\n");

    HostFs_Reset();

    CHECK(Emmc_Mount() == EMMC_OK, "Emmc_Mount 返回 EMMC_OK");
    CHECK(Emmc_OpenData(1U) == EMMC_OK, "Emmc_OpenData(Rewind=1) 返回 EMMC_OK");
    CHECK(Emmc_DataIsOpen() == 1U, "文件已打开");

    Emmc_GetInfo(&info);
    CHECK(info.Mounted == 1U, "快照 Mounted = 1");
    CHECK(info.Records == 0U, "新文件记录数 = %u (期望 0)", (unsigned)info.Records);

    /* 表头必须是完整的一行, 而且要能被 count 逻辑当成表头跳过 */
    {
        const char *data = HostFs_Data();
        const DWORD size = HostFs_Size();

        CHECK(size > 0U, "文件里有内容 (%u 字节)", (unsigned)size);
        CHECK(countNewlines(data, size) == 1U,
              "只有表头这 1 行 (数到 %u 个换行)", countNewlines(data, size));
        CHECK(size >= (DWORD)strlen(EMMC_DATA_HEADER),
              "文件长度 %u >= 表头长度 %u", (unsigned)size,
              (unsigned)strlen(EMMC_DATA_HEADER));
        CHECK(memcmp(data, EMMC_DATA_HEADER, strlen(EMMC_DATA_HEADER)) == 0,
              "开头就是表头 \"%s\"", EMMC_DATA_HEADER);
        CHECK(data[size - 2U] == '\r' && data[size - 1U] == '\n',
              "表头以 \\r\\n 收尾");
    }
}

/* ========================================================================== */
/*  2. 追加 / 计数                                                              */
/* ========================================================================== */

static void test_append_and_count(void)
{
    Emmc_InfoTypeDef info;
    char             line[EMMC_LINE_MAX];
    unsigned         i;

    printf("[2] 追加记录 + 计数\n");

    for (i = 0U; i < 40U; i++) {
        makeLine(line, sizeof(line), i, 100 + (int)i);
        CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK,
              "追加第 %u 行返回 EMMC_OK", i);
    }

    Emmc_GetInfo(&info);
    CHECK(info.Records == 40U, "记录数 = %u (期望 40)", (unsigned)info.Records);
    CHECK(info.Bytes == HostFs_Size(), "快照 Bytes(%u) = 文件大小(%u)",
          (unsigned)info.Bytes, (unsigned)HostFs_Size());
    CHECK(countNewlines(HostFs_Data(), HostFs_Size()) == 41U,
          "文件里共 %u 行(1 表头 + 40 记录)", countNewlines(HostFs_Data(), HostFs_Size()));

    printf("[3] 关掉再打开(不重写表头, 重新数一遍)\n");

    CHECK(Emmc_CloseData() == EMMC_OK, "Emmc_CloseData 返回 EMMC_OK");
    CHECK(Emmc_DataIsOpen() == 0U, "关闭后 DataIsOpen = 0");

    {
        const DWORD sizeBefore = HostFs_Size();

        CHECK(Emmc_OpenData(0U) == EMMC_OK, "Emmc_OpenData(Rewind=0) 返回 EMMC_OK");
        Emmc_GetInfo(&info);

        CHECK(info.Records == 40U, "重开后记录数仍是 %u (期望 40)", (unsigned)info.Records);
        CHECK(HostFs_Size() == sizeBefore, "重开没有改动文件大小(%u -> %u)",
              (unsigned)sizeBefore, (unsigned)HostFs_Size());
    }

    /* 接着写, 计数要累加 */
    makeLine(line, sizeof(line), 40U, 999);
    CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "继续追加一行");
    Emmc_GetInfo(&info);
    CHECK(info.Records == 41U, "累加后记录数 = %u (期望 41)", (unsigned)info.Records);
}

/* ========================================================================== */
/*  4/5. 半截行 / 废行                                                          */
/* ========================================================================== */

static void test_half_tail_and_junk(void)
{
    Emmc_InfoTypeDef info;
    char             line[EMMC_LINE_MAX];
    char             junk[EMMC_LINE_MAX];

    printf("[4] 掉电写了一半的尾巴(末尾没有换行)\n");

    /* 先把文件收成"表头 + 3 条"的干净状态 */
    CHECK(Emmc_OpenData(1U) == EMMC_OK, "重新清空打开");
    makeLine(line, sizeof(line), 0U, 10);
    CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "写第 1 行");
    makeLine(line, sizeof(line), 1U, 20);
    CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "写第 2 行");
    makeLine(line, sizeof(line), 2U, 30);
    CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "写第 3 行");
    CHECK(Emmc_CloseData() == EMMC_OK, "关闭文件");

    /* 伪造: 末尾再挂半行(没有 \r\n) */
    HostFs_Poke("1234,5,6,7,8,9", HostFs_Size());

    CHECK(Emmc_OpenData(0U) == EMMC_OK, "打开(带半截尾巴)");
    Emmc_GetInfo(&info);
    CHECK(info.Records == 3U, "半截行**不计入**记录数 (拿到 %u, 期望 3)",
          (unsigned)info.Records);

    {
        const char *data = HostFs_Data();
        const DWORD size = HostFs_Size();

        CHECK(size >= 2U && data[size - 2U] == '\r' && data[size - 1U] == '\n',
              "半截行被补上 \\r\\n 收尾了");
        CHECK(countNewlines(data, size) == 5U,
              "收尾后文件共 %u 行(1 表头 + 3 记录 + 1 半截废行)",
              countNewlines(data, size));
    }

    printf("[5] 字段数不对的废行回读时被跳过\n");

    /* 再塞两条字段数明显不对的(4 个字段 / 11 个字段), 它们不该出现在回读里。
     * ⚠ 别再用"10 个字段"当废行了 —— 现在正常记录就是 10 个字段(t_ms..laser_mv)。 */
    (void)snprintf(junk, sizeof(junk), "1,2,3,4\r\n");
    CHECK(Emmc_AppendText(junk, (uint32_t)strlen(junk)) == EMMC_OK, "写一条 4 字段的废行");
    (void)snprintf(junk, sizeof(junk), "1,2,3,4,5,6,7,8,9,10,11\r\n");
    CHECK(Emmc_AppendText(junk, (uint32_t)strlen(junk)) == EMMC_OK, "写一条 11 字段的废行");

    {
        char     buf[512];
        uint32_t lines = 0U;
        uint32_t len = 0U;
        uint8_t  more = 0U;

        CHECK(Emmc_ReadLines(0U, 100U, "EMCD ", buf, sizeof(buf), &lines, &len, &more)
                  == EMMC_OK, "回读全部");

        /* 应该只有 3 条真记录 + 那条"半截被收尾的废行"(4 个字段, 也会被跳) */
        CHECK(lines == 3U, "回读到 %u 行 (期望 3: 废行都被跳过)", (unsigned)lines);
        CHECK(strstr(buf, "EMCD ") == buf, "每行都带 EMCD 前缀");
        CHECK(strstr(buf, "1,2,3,4\r\n") == NULL, "4 字段的废行没被回读出来");
        CHECK(strstr(buf, ",11\r\n") == NULL, "11 字段的废行没被回读出来");
    }
}

/* ========================================================================== */
/*  6. 分批回读                                                                 */
/* ========================================================================== */

static void test_paged_read(void)
{
    Emmc_InfoTypeDef info;
    char             line[EMMC_LINE_MAX];
    char             buf[512];
    unsigned         i;
    uint32_t         start = 0U;
    uint32_t         total = 0U;
    int              rounds = 0;
    uint8_t          more = 1U;

    printf("[6] 分批回读(EMCDUMP 的分页)\n");

    CHECK(Emmc_OpenData(1U) == EMMC_OK, "清空重开");

    /* 写 100 条 */
    for (i = 0U; i < 100U; i++) {
        makeLine(line, sizeof(line), i, 1000 + (int)i);
        CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "写第 %u 条", i);
    }
    Emmc_GetInfo(&info);
    CHECK(info.Records == 100U, "写进去 100 条 (拿到 %u)", (unsigned)info.Records);

    /* 用很小的缓冲分批拉, 每批最多 7 行 —— 同时压"缓冲装不下要提前返回" */
    while (more != 0U && rounds < 100) {
        uint32_t lines = 0U;
        uint32_t len = 0U;

        CHECK(Emmc_ReadLines(start, 7U, "EMCD ", buf, sizeof(buf), &lines, &len, &more)
                  == EMMC_OK, "第 %d 批回读(start=%u)", rounds, (unsigned)start);

        CHECK(len > 0U || lines == 0U, "第 %d 批长度和行数自洽", rounds);
        CHECK(lines <= 7U, "第 %d 批不超过 7 行 (拿到 %u)", rounds, (unsigned)lines);
        CHECK(strstr(buf, "EMCD ") == buf || lines == 0U, "第 %d 批带 EMCD 前缀", rounds);

        total += lines;
        start += lines;
        rounds++;

        if (lines == 0U) {
            break;
        }
    }

    CHECK(total == 100U, "分批拉完一共 %u 条 (期望 100, 不重不漏)", (unsigned)total);
    CHECK(rounds >= 14, "至少分了 %d 批(每批 7 行、100 条)", rounds);
    CHECK(more == 0U, "拉完之后 more = 0");

    /* 起点越界: 应该回 0 行 + more=0, 而不是报错或者回垃圾 */
    {
        uint32_t lines = 0U;
        uint32_t len = 0U;
        uint8_t  m = 1U;

        CHECK(Emmc_ReadLines(500U, 10U, "EMCD ", buf, sizeof(buf), &lines, &len, &m)
                  == EMMC_OK, "起点越界仍返回 EMMC_OK");
        CHECK(lines == 0U && len == 0U, "起点越界回 0 行 (%u 行 / %u 字节)",
              (unsigned)lines, (unsigned)len);
    }

    /* 从中间起: 第 50 条开始, 内容必须就是原始那一条 */
    {
        uint32_t lines = 0U;
        uint32_t len = 0U;
        uint8_t  m = 0U;
        char     expect[EMMC_LINE_MAX];

        CHECK(Emmc_ReadLines(50U, 3U, "EMCD ", buf, sizeof(buf), &lines, &len, &m)
                  == EMMC_OK, "从第 50 条读 3 行");

        makeLine(expect, sizeof(expect), 50U, 1050);
        expect[strlen(expect) - 2U] = '\0';     /* 去掉 \r\n 再比 */

        CHECK(lines == 3U, "读到 3 行 (拿到 %u)", (unsigned)lines);
        CHECK(strncmp(buf, "EMCD ", 5U) == 0, "第一行以 EMCD 开头");
        CHECK(strncmp(buf + 5, expect, strlen(expect)) == 0,
              "第 50 条内容对得上 (期望 \"%s\", 实际 \"%.*s\")", expect,
              (int)(len < 40U ? len : 40U), buf + 5);
        CHECK(m == 1U, "后面还有记录 -> more = 1");
    }
}

/* ========================================================================== */
/*  7. 写失败                                                                   */
/* ========================================================================== */

static void test_write_failure(void)
{
    Emmc_InfoTypeDef info;
    char             line[EMMC_LINE_MAX];
    Emmc_StatusTypeDef rc;

    printf("[7] 写失败(卡被拔/写坏)返回错误码\n");

    CHECK(Emmc_OpenData(1U) == EMMC_OK, "清空重开");

    makeLine(line, sizeof(line), 0U, 7);

    HostFs_FailWritesAfter(0U, (int)FR_DISK_ERR);    /* 下一次写就失败 */

    rc = Emmc_AppendText(line, (uint32_t)strlen(line));
    CHECK(rc == EMMC_ERR_IO, "f_write 报 FR_DISK_ERR -> EMMC_ERR_IO (拿到 %d)", (int)rc);

    Emmc_GetInfo(&info);
    CHECK(info.LastError == (uint8_t)FR_DISK_ERR, "快照记下了 FatFs 的错误码 (%u)",
          (unsigned)info.LastError);
    CHECK(info.Records == 0U, "写失败时记录数**没有**增加 (仍是 %u)",
          (unsigned)info.Records);

    /* 恢复之后还能正常写 */
    HostFs_FailWritesAfter(0U, (int)FR_OK);
    CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "恢复后又写得进去");
    Emmc_GetInfo(&info);
    CHECK(info.Records == 1U, "恢复后记录数 = %u", (unsigned)info.Records);
}

/* ========================================================================== */
/*  8. 挂载之后就要能报出卡上已有多少条(重启后的第一次 EMCSTAT)                  */
/* ========================================================================== */

static void test_mount_reports_existing(void)
{
    Emmc_InfoTypeDef info;
    char             line[EMMC_LINE_MAX];
    unsigned         i;
    DWORD            sizeBefore;

    printf("[8] 挂载后 EMCSTAT 就能报出卡上已有的记录数\n");

    /* 先录 25 条, 然后关掉文件 —— 模拟"上次上电录完就断电了" */
    CHECK(Emmc_OpenData(1U) == EMMC_OK, "清空重开");
    for (i = 0U; i < 25U; i++) {
        makeLine(line, sizeof(line), i, 500 + (int)i);
        CHECK(Emmc_AppendText(line, (uint32_t)strlen(line)) == EMMC_OK, "写第 %u 条", i);
    }
    CHECK(Emmc_CloseData() == EMMC_OK, "关文件");
    sizeBefore = HostFs_Size();

    /*
     * 这里**不**再打开文件, 直接重新挂载 —— 就是重启之后"上电 → EMCMOUNT →
     * EMCSTAT"那条路。以前 records 只在打开文件时才算, 这条路上会报 0, 现场会以为
     * 数据丢了(2026/10/09 上板实测撞上的就是这个)。
     */
    CHECK(Emmc_Mount() == EMMC_OK, "重新挂载成功");

    Emmc_GetInfo(&info);
    CHECK(info.Records == 25U,
          "★ 没打开文件也报出记录数 = %u (期望 25; 报 0 就是重启后看不到数据的那个 bug)",
          (unsigned)info.Records);
    CHECK(info.Bytes == sizeBefore, "字节数也报对: %u = %u",
          (unsigned)info.Bytes, (unsigned)sizeBefore);
    CHECK(Emmc_DataIsOpen() == 0U, "挂载只统计, 没有把文件留在打开状态");
    CHECK(HostFs_Size() == sizeBefore, "挂载没有改动文件(%u -> %u)",
          (unsigned)sizeBefore, (unsigned)HostFs_Size());

    printf("[9] 卡上没有文件时: 挂载不该凭空建一个空文件\n");

    HostFs_Reset();                     /* 整张卡清空, 没有任何文件 */

    CHECK(Emmc_Mount() == EMMC_OK, "空卡挂载成功");
    Emmc_GetInfo(&info);
    CHECK(info.Records == 0U, "空卡记录数 = 0");
    CHECK(HostFs_Size() == 0U,
          "★ 挂载后卡上仍然是空的(%u 字节) —— 查询不该有副作用, "
          "不能凭空建出一个 data.csv", (unsigned)HostFs_Size());
}

/* ========================================================================== */

int main(void)
{
    printf("== eMMC(SD1/FatFs)文件逻辑 宿主机测试 ==\n\n");

    test_open_fresh();
    test_append_and_count();
    test_half_tail_and_junk();
    test_paged_read();
    test_write_failure();
    test_mount_reports_existing();

    printf("\n----------------------------------------\n");
    printf("PASS %d, FAIL %d\n", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
