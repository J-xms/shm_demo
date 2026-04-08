# shm_demo — OBMM 共享内存读写示例

## 简介

本项目展示如何直接操作 `/dev/obmm_shmdev*` 设备进行共享内存读写。

实现原理参考 **openEuler ubs-mem** 项目中 `ubsmem_shmem_map` 接口的源码流程：

1. 通过 `open("/dev/obmm_shmdev<id>", O_RDWR)` 获取设备文件描述符
2. 通过 `mmap(MAP_SHARED, fd, offset)` 将设备映射到用户态虚拟地址
3. 对映射后的指针进行直接的读写操作（绕过文件系统，直接落盘到 UB 硬件内存）

## 前置条件

- 运行在 openEuler 超节点上
- UBS Agent（`ubsmd`）已安装并启动
- 已通过 `ubsmem_shmem_allocate()` 分配共享内存，获得 OBMM 设备编号
- 需使用 **root 权限**运行（OBMM 设备访问和 `/proc/self/pagemap` 读取需要）

## 编译

```bash
g++ -O2 -o obmm_test obmm_test.cpp
```

## 使用

```bash
# 查看当前系统中的 OBMM 设备
ls /dev/obmm_shmdev*

# 以 root 权限运行，假设设备编号为 0
sudo ./obmm_test 0
```

典型输出：

```
[打开] /dev/obmm_shmdev0 (fd=3)

========== 地址信息 ==========
  虚拟地址范围: 0x7f... ~ 0x7f... (共 4096 字节)
  物理地址范围: 0x... ~ 0x...     (共 4096 字节)
  maps 条目:    7f... rw-s ... /dev/obmm_shmdev0
==============================

[000001] 虚拟地址: 0x7f... | 物理地址: 0x... | 耗时: 0.12 ms | 吞吐: 8333.33 万次/s
[000010] ...
...
[100000] 虚拟地址: 0x7f... | 物理地址: 0x... | 耗时: 120.55 ms | 吞吐: 829.38 万次/s

========== 测试结果 ==========
读写次数:   100000 次
成功次数:   100000 次
总耗时:     120.55 ms
平均单次:   1.206 微秒
最终虚拟地址: 0x7f...
最终物理地址: 0x...
==============================
```

## 核心参数说明

| 宏 | 默认值 | 含义 |
|---|---|---|
| `TEST_SIZE` | 4096 | 每次 mmap 的大小（字节） |
| `ITERATIONS` | 100000 | 读写循环次数 |
| `PRINT_INTERVAL` | 10000 | 每隔多少次打印一行进度 |
| `DEV_PATH_FMT` | `/dev/obmm_shmdev%llu` | 设备路径格式 |

可在编译时传入自定义值：

```bash
g++ -O2 -DITERATIONS=500000 -DTEST_SIZE=8192 -o obmm_test obmm_test.cpp
```

## 原理概述

```
用户态
  ┌──────────────────────────────────────┐
  │  open("/dev/obmm_shmdev0")  → fd    │
  │  mmap(fd)                 → ptr     │
  │  write/read(ptr)          → 硬件直读  │
  └───────────↓──────────────────────────┘
              ↓
内核/驱动层
  ┌──────────────────────────────────────┐
  │  /dev/obmm_shmdev0   (OBMM 驱动)     │
  │  UB 硬件内存池（远端/本地）            │
  └──────────────────────────────────────┘
```

与标准 mmap 的区别：

| 维度 | 标准 mmap | OBMM mmap |
|---|---|---|
| 后端 | 文件系统或匿名页 | UB 硬件共享内存设备 |
| 跨节点 | ❌ | ✅（通过 UBS Agent 协调） |
| 需 Agent | ❌ | ✅ |
| 设备路径 | 无 | `/dev/obmm_shmdev<id>` |

## 项目结构

```
shm_demo/
├── obmm_test.cpp    # 读写压测源码
├── README.md        # 本文件
└── LICENSE          # Mulan PSL v2
```

## License

Mulan PSL v2 — 与 ubs-mem 保持一致
