/**
 * obmm_test.cpp — OBMM 设备读写压测工具
 *
 * 基于 ubs-mem 库中 ubsmem_shmem_map 的实现原理，
 * 直接打开 /dev/obmm_shmdev<id> 设备并进行 mmap 读写测试。
 *
 * 支持架构: x86_64, aarch64 (ARM64)
 * 编译: g++ -O2 -o obmm_test obmm_test.cpp
 * 运行: sudo ./obmm_test <obmm_dev_id>
 * 示例: sudo ./obmm_test 0
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cerrno>
#include <chrono>
#include <atomic>
#include <inttypes.h>   // PRIu64 / PRIx64

/* ──────────────────── 配置 ──────────────────── */
#define DEV_PATH_FMT       "/dev/obmm_shmdev%" PRIu64
#define DEV_PATH_LEN       64
#define TEST_SIZE          (4096)         // 每次映射 4KB
#define ITERATIONS         (100000)       // 读写 10 万次
#define PRINT_INTERVAL     (10000)        // 每 1 万次打印进度

/* ──────────────────── 物理地址查询 ──────────────────── */
/**
 * 通过 /proc/self/pagemap 将虚拟地址转换为物理地址。
 *
 * 兼容性说明:
 *   - x86_64:  PFN 在 bit[0-54]（55 位物理地址空间）
 *   - aarch64: PFN 在 bit[0-47]（4K 页，48-bit IPA）
 * 注意：普通用户可能无权限读取 pagemap，返回 0。
 */
static uint64_t virt_to_phys(void* vaddr) {
    FILE* f = fopen("/proc/self/pagemap", "rb");
    if (!f) return 0;

    uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t vpage_num = (uint64_t)vaddr / page_size;

    off_t offset = (off_t)(vpage_num * sizeof(uint64_t));
    if (fseeko(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    uint64_t entry = 0;
    if (fread(&entry, sizeof(entry), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // bit 63 = 1 表示页位于物理 RAM（x86_64 和 ARM64 均适用）
    if (!(entry & (1ULL << 63))) return 0;

#if defined(__aarch64__)
    // ARM64 4K 页: PFN 在 bit[0-47]
    uint64_t phys_page = entry & ((1ULL << 48) - 1);
#else
    // x86_64: PFN 在 bit[0-54]
    uint64_t phys_page = entry & ((1ULL << 55) - 1);
#endif

    return phys_page * page_size + ((uint64_t)vaddr % page_size);
}

/* ──────────────────── 地址信息打印 ──────────────────── */
static void print_addr_info(void* mapped, size_t size) {
    uint64_t phys_start = virt_to_phys(mapped);
    printf("  虚拟地址范围: %p ~ %p (共 %zu 字节)\n",
           mapped, (uint8_t*)mapped + size - 1, size);
    if (phys_start != 0) {
        printf("  物理地址范围: 0x%016" PRIx64 " ~ 0x%016" PRIx64 "\n",
               phys_start, phys_start + size - 1);
    } else {
        printf("  物理地址:     (普通用户无权读取 pagemap，请使用 root)\n");
    }

    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "/dev/obmm") && strstr(line, "rw-s")) {
                printf("  maps 条目:    %s", line);
                break;
            }
        }
        fclose(maps);
    }
}

/* ──────────────────── 读写操作 ──────────────────── */
static inline void write_data(uint8_t* ptr, size_t size, uint64_t iter) {
    for (size_t i = 0; i < size; ++i) {
        ptr[i] = (uint8_t)((iter + i) & 0xFF);
    }
}

static inline bool read_data(uint8_t* ptr, size_t size, uint64_t iter) {
    for (size_t i = 0; i < size; ++i) {
        uint8_t expected = (uint8_t)((iter + i) & 0xFF);
        if (ptr[i] != expected) {
            return false;
        }
    }
    return true;
}

/* ──────────────────── 主程序 ──────────────────── */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "\n用法: sudo %s <obmm_dev_id>\n", argv[0]);
        fprintf(stderr, "示例: sudo %s 0\n\n", argv[0]);
        fprintf(stderr, "提示: 通过 ubsmem_shmem_alloc() 分配内存后,\n");
        fprintf(stderr, "      查看 /dev/obmm_shmdev* 确认设备编号。\n");
        return 1;
    }

    uint64_t dev_id = strtoull(argv[1], nullptr, 0);

    /* 1. 打开 OBMM 设备 */
    char dev_path[DEV_PATH_LEN];
    snprintf(dev_path, sizeof(dev_path), DEV_PATH_FMT, dev_id);

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open 设备失败");
        fprintf(stderr, "\n请确认设备 %s 存在，且以 root 权限运行。\n", dev_path);
        return 1;
    }
    printf("[打开] %s (fd=%d)\n\n", dev_path, fd);

    /* 2. mmap 映射 */
    void* mapped = mmap(nullptr, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap 失败");
        close(fd);
        return 1;
    }

    /* 3. 打印地址信息 */
    printf("========== 地址信息 ==========\n");
    print_addr_info(mapped, TEST_SIZE);
    printf("==============================\n\n");

    /* 4. 预热：写一次建立初始数据 */
    write_data(static_cast<uint8_t*>(mapped), TEST_SIZE, 0);

    /* 5. 10 万次读写压测 */
    std::atomic<uint64_t> ok{0};
    auto start = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 1; i <= ITERATIONS; ++i) {
        write_data(static_cast<uint8_t*>(mapped), TEST_SIZE, i);
        if (!read_data(static_cast<uint8_t*>(mapped), TEST_SIZE, i)) {
            fprintf(stderr, "[错误] 第 %" PRIu64 " 次校验失败!\n", i);
            break;
        }
        ++ok;

        if (i % PRINT_INTERVAL == 0) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
            if (elapsed < 0.001) elapsed = 0.001;  // 防除零
            printf("[%06" PRIu64 "] 虚拟地址: %p | 物理地址: 0x%08" PRIx64 " | "
                   "耗时: %.2f ms | 吞吐: %.2f 万次/s\n",
                   i, mapped,
                   (uint64_t)virt_to_phys(mapped),
                   elapsed, i * 1000.0 / elapsed / 10000.0);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    printf("\n========== 测试结果 ==========\n");
    printf("读写次数:   %d 次\n", ITERATIONS);
    printf("成功次数:   %" PRIu64 " 次\n", ok.load());
    printf("总耗时:     %.2f ms\n", ms);
    printf("平均单次:   %.3f 微秒\n", ms * 1000.0 / ITERATIONS);
    printf("最终虚拟地址: %p\n", mapped);
    printf("最终物理地址: 0x%08" PRIx64 "\n", (uint64_t)virt_to_phys(mapped));
    printf("==============================\n");

    munmap(mapped, TEST_SIZE);
    close(fd);
    return 0;
}
