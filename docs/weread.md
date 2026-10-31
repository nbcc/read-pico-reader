# 微信读书传书 · 契约与状态机 / WeRead transfer source

> **状态 / Status**：v0.7，登录、书架、单本缓存与封面已实现；正文插图下一轮（见 §13）。/ v0.7: login, shelf, caching and cover implemented; inline images next.
> **上位规则 / Source of truth**：代码 `冻结 / Frozen:` 段 > [AGENTS.md](../AGENTS.md) > 本文。
> 本文只描述**行为与契约**；实现计划、调试记录留在 `docs/local/`，不入库。

---

## 0. 一句话 / Summary

把「微信读书」做成 [`read_pico_transfer`](../components/read_pico_transfer/include/read_pico_transfer.h) 的**第二个内容来源（source）**，与浏览器上传并列：设备主动**登录 → 拉书架 → 把指定书缓存成 EPUB 落到本地书库**。不新增页面，不新增网络所有者。

> Make WeRead a second **source** of the existing transfer service, parallel to browser upload: log in, fetch the shelf, cache one selected book as an EPUB into the local library. No new page, no second network owner.

---

## 1. 范围 / Scope

### 1.1 v1 必须做 / Required in v1

| # | 能力 | 说明 |
|---|---|---|
| 1 | 扫码登录 | `/r/weread-skills` → `/api/auth/getLoginUid` → 展示二维码 → 轮询 `/api/auth/getLoginInfo` |
| 2 | 会话持久化 | 仅存 `wr_vid / wr_skey / wr_rt / 昵称`，NVS 单 blob |
| 3 | 书架 | `/shelf/sync` → **在线目录**：每行一本（书名 / 作者 / 已缓存标记），分页、刷新；不缓存书架、不缓存封面 |
| 4 | 单本缓存 | 目录 → 章节 → 打包 EPUB → 提交到书库 |
| 5 | 取消与错误 | 中途取消、超时、空间不足、会话失效，均有确定出口 |
| 6 | 退出登录 | 清 NVS 会话与 cookie，**不删除**已下载 EPUB |

### 1.2 v1 明确不做 / Explicitly out of scope

| 不做 | 原因 |
|---|---|
| 正文插图 | 图片下载、OPF manifest 与失败跳过是另一条独立失败链；下一轮按"默认带图、单张失败跳过"实现（已确认） |
| 阅读进度同步 | 属阅读器侧能力，另立契约（见 §11） |
| 划线 / 书评 / 微信公众号（`MP_WXS_`） | 只读缓存与解析量都不小，且不影响"传书"闭环 |
| 书架离线缓存 | 书架只是**在线目录**：每次进页重拉，不落盘（见 §11） |
| 断点续传 | 失败即清理重来；EPUB 是整体产物，半本书没有价值 |
| 官方 Agent Gateway / Skill API Key | 只用 Web Cookie；少一套凭据与接口 |
| 按章范围缓存 | v1 只做整本（见 §11） |

> 一张"纯文字、无封面"的 EPUB 是这个版本的可交付物。图文书的阅读体验会明显受损，这是**已知且接受**的取舍。

### 1.3 用户模型 / User model

书架 = **在线文件目录**，与"传书"页的文件列表同构：列出服务端书架上的书，点一行 → 确认 → 缓存成 EPUB → 回到列表。
没有详情页、没有封面墙、没有"已下载"专区；本地已存在的书只在该行显示一个"已缓存"标记（一次 `stat`，不是第二份索引）。

> The shelf is an **online directory** that mirrors the browser file list: list, tap a row, confirm, cache as EPUB, return to the list. No detail page, no cover wall, no resume.

---

## 2. 归属与冻结 / Ownership and frozen decisions

### 2.1 为什么放进 transfer 而不是新开一个 app 页

| 理由 | 依据 |
|---|---|
| 全仓只有本组件能起 WiFi | `read_pico_transfer_start()` 独占 init/netif/event loop/射频 |
| 复用"网络页"保护 | `wifi_busy = state != STOPPED` 抑制自动锁屏；`current == &app_transfer` 抑制字体重载与空闲判断 |
| 复用书籍提交路径 | `commit_file()` 的 FatFs 覆盖处理、`file_changed_cb` 清旧进度、容量检查 |
| 复用会话停止语义 | `try_stop_if_idle` / `pause_for_sleep` / 停止顺序已调好 |

### 2.2 冻结条款（合并入 `read_pico_transfer.c` 头部冻结段）

```
冻结：本组件有两个内容来源——HTTP 上传（source=HTTP）与微信读书拉取（source=WEREAD）。
冻结：WEREAD 需要联网，且只在 STA 下工作（连已保存路由器）；不拥有 WiFi——不得调用
      esp_wifi_* / esp_netif_* / esp_event_loop_*，网络一律由本组件会话提供。
冻结：WEREAD 需要存储策略（root_dir / free_bytes_cb / file_changed_cb），但不启动 HTTP 上传服务：
      方向相反，浏览器传书是设备当服务器收，微信读书是设备当客户端拉。
冻结：微信读书产出的书与浏览器上传走同一条提交路径（.part → commit_file → file_changed_cb）。
Frozen: Two sources: HTTP upload and WeRead pull. WEREAD needs the network and runs in STA only, but
        never owns Wi-Fi (no esp_wifi/netif/event-loop calls). It needs the storage policy yet starts
        no HTTP server: the browser direction is device-as-server, WeRead is device-as-client.
        Books produced by WeRead use the same commit path as uploads.
```

**同一份配置的差异对照**（给实现者）：

| | `source=HTTP`（现状） | `source=WEREAD`（新增） |
|---|---|---|
| WiFi 模式 | AP 或 STA | **只允许 STA**；传 AP 返回 `ESP_ERR_INVALID_ARG` |
| `network_only` | 可为 true（OTA 借用） | **不允许**（WeRead 需要存储） |
| `root_dir` / `free_bytes_cb` / `file_changed_cb` | 必需（AP 也必须） | 必需 |
| `httpd` | 启动（18 条路由） | **不启动** |
| 连接后做什么 | 等浏览器上传 | 跑 WeRead worker（登录 / 书架 / 缓存） |

**不变的既有冻结**：书籍上传仍只接受 TXT/EPUB；`STA` 拒绝远程改凭据；上传中拒绝停止。

---

## 3. 公共契约 / Public contract

### 3.0 网络角色 / Network role（先读这段）

要分清三件事，否则很容易把它误解成"不需要 WiFi"：

| 说法 | 含义 |
|---|---|
| **需要 WiFi** | 是。要访问 `weread.qq.com`，必须连上已保存的路由器（STA）。**AP 模式不可用**：设备自己开的热点没有外网 |
| **不拥有 WiFi** | 不自己 `esp_wifi_init/start`、不建 netif/event loop、不抢 BLE 让路。仍由本组件的会话统一提供，和"已有 WiFi 传书"共用同一套连接状态机与凭据（`rp_wifi`） |
| **不启动 HTTP 服务** | 不起 `httpd`。浏览器传书是设备当服务器收，微信读书是设备当客户端拉 |

配置示例（与浏览器上传的差别只有 `source` 与 `mode`）：

```c
read_pico_transfer_cfg_t cfg = {
    .mode = READ_PICO_TRANSFER_MODE_STA,      // 必须 STA
    .source = READ_PICO_TRANSFER_SOURCE_WEREAD,
    .root_dir = "/sdcard/books",              // 已有上传根
    .free_bytes_cb = …, .free_bytes_ctx = …,
    .file_changed_cb = …,                     // 清旧进度
};
read_pico_transfer_start(&cfg);
```

* 未配网 → `RW_ERR_NO_WIFI`，引导到本页已有的 WiFi 设置流程；不自动弹配网页。
* WeRead 生命周期内**不切换** WiFi 模式；需要换网时先 `stop()` 再重新 `start`。
* 空闲 60 s 未操作 → `stop()` 释放射频（省电）；会话与书架留在内存，下次 `start` 后直接拉书架。

### 3.1 类型扩展（只追加，不改既有字段语义）

```c
typedef enum {
    READ_PICO_TRANSFER_SOURCE_HTTP = 0,  // 浏览器/网页上传（现状）
    READ_PICO_TRANSFER_SOURCE_WEREAD,    // 微信读书拉取
} read_pico_transfer_source_t;

typedef struct {
    /* ...既有字段保持不变... */
    read_pico_transfer_source_t source;  // 末尾追加；零值等于现状
} read_pico_transfer_cfg_t;

typedef struct {
    /* ...既有字段保持不变... */
    read_pico_transfer_source_t source;  // 让页面能区分当前会话
} read_pico_transfer_status_t;
```

`read_pico_transfer_state_t` **不新增状态**：WeRead 会话只用 `STARTING / READY / ERROR / STOPPED`；`UPLOADING` 仍只表示 HTTP 上传。

### 3.2 WeRead API

```c
typedef enum {
    RW_IDLE = 0, RW_QR, RW_WAIT_SCAN, RW_SHELF, RW_CACHING, RW_DONE, RW_ERROR
} read_pico_weread_state_t;

typedef enum {
    RW_ERR_NONE = 0, RW_ERR_NO_WIFI, RW_ERR_LOGIN_FAILED, RW_ERR_LOGIN_TIMEOUT,
    RW_ERR_OTP_REQUIRED, RW_ERR_SESSION_EXPIRED, RW_ERR_PROTOCOL, RW_ERR_INTEGRITY,
    RW_ERR_SD, RW_ERR_NO_SPACE, RW_ERR_CANCELLED, RW_ERR_BUSY, RW_ERR_OTA_BUSY,
} read_pico_weread_error_t;

typedef struct {
    read_pico_weread_state_t state;
    unsigned revision;              // 单调递增；页面据此判断是否重绘
    char qr_url[192];               // RW_QR / RW_WAIT_SCAN 有效
    char account[64];               // 昵称，可为空
    unsigned shelf_count;
    char title[96];                 // 正在缓存的书名
    char book_id[48];               // 正在缓存的 bookId（行内进度按它匹配）
    uint8_t stage;                  // 0 目录 1 正文 2 打包 3 提交
    uint8_t percent;                // 0..100，仅 CACHING 有意义
    unsigned chapter_done, chapter_total;
    bool loading;                   // 正在拉书架
    read_pico_weread_error_t error;
    char message[96];               // 空串或面向用户的短句
} read_pico_weread_status_t;

typedef struct {
    char book_id[48];
    char title[128];
    char author[64];
    uint32_t update_time;   // 服务端最近阅读时间，用于排序
    bool cached;            // 本地已有同名 EPUB（一次 stat，不落索引）
} read_pico_weread_shelf_item_t;

// 全部只在 UI/主循环线程调用；除 get/cancel 外都是异步启动，结果经 status 发布。
esp_err_t read_pico_transfer_weread_login(void);
esp_err_t read_pico_transfer_weread_shelf(void);             // 重新拉取，异步
unsigned  read_pico_transfer_weread_shelf_count(void);
bool      read_pico_transfer_weread_shelf_item(unsigned index, read_pico_weread_shelf_item_t *out);
esp_err_t read_pico_transfer_weread_cache(unsigned index);   // index 来自当前书架
esp_err_t read_pico_transfer_weread_cancel(void);            // 只置标志，立即返回
void      read_pico_transfer_weread_get_status(read_pico_weread_status_t *out);
bool      read_pico_transfer_weread_busy(void);              // CACHING 或二维码轮询中
esp_err_t read_pico_transfer_weread_logout(void);            // 清会话，不删 EPUB
```

`cache(index)` 按调用时刻的书架快照校验：越界或书架已被 `shelf()` 替换 → 返回 `ESP_ERR_INVALID_ARG`，状态不变。

**线程规则**：worker 只有一个；UI 线程只读 `status` 快照与书架记录；书架表是双缓冲，worker 解析进备用表、成功后整表切换，读侧只会看到完整的一份。

### 3.3 会话状态机 / Session state machine

```
        start(source=WEREAD)
STOPPED ───────────────────▶ STARTING ──(WiFi ready)──▶ READY ──stop()──▶ STOPPED
                                 │                          ▲                 ▲
                                 └──(失败: NO_WIFI/…)──▶ ERROR ──stop()───────┘
```

* `STARTING` 的完成条件：`network_ready == true`（由现有连接状态机给出），不是"WiFi 驱动已启动"。
* `READY` 只表示**网络可用**；是否已登录由 `read_pico_weread_status_t` 表达。
* `ERROR` 保留到最后一次 `stop()`，页面可重试（重试 = 重新 `start`）。

### 3.4 WeRead 子状态机 / WeRead state machine

登录不是一个独立服务，而是 `WEREAD` 会话里的第一段：会话可用（`READY`）→ 无凭据则走登录，有凭据则直接拉书架。所有网络动作都由同一个 worker 串行执行。

```
IDLE ──login()──▶ RW_QR ──(建码成功)──▶ RW_WAIT_SCAN ──(succeed)──▶ RW_SHELF
IDLE ──shelf()──▶ RW_SHELF            （已有有效会话时直接进）
RW_SHELF ──cache(i)──▶ RW_CACHING ──(提交成功)──▶ RW_DONE ──▶ RW_SHELF
RW_* ──cancel()──▶ IDLE               （已提交的书不回滚）
RW_* ──(错误)──▶ RW_ERROR ──重试──▶ 上一个可重入状态
```

迁移表：

| 当前 | 事件 | 动作 | 新状态 |
|---|---|---|---|
| IDLE | `login()` | 起 worker：GET skills 页 → getLoginUid → 发布 `qr_url` | RW_QR → RW_WAIT_SCAN |
| RW_WAIT_SCAN | 轮询 `succeed=true` | 写 NVS 会话并置 `logged_in`，随后自动拉书架 | RW_SHELF（拉取失败则 RW_ERROR） |
| RW_WAIT_SCAN | `logicCode=NEED_OTP` | 结束轮询，`error=RW_ERR_OTP_REQUIRED` | RW_ERROR |
| RW_WAIT_SCAN | 300 s 无结果 | 结束轮询，`RW_ERR_LOGIN_TIMEOUT` | RW_ERROR |
| RW_SHELF | `shelf()` | 重拉 `/shelf/sync` | RW_SHELF |
| RW_SHELF | `cache(i)` | 预检（会话、空间、正忙）→ 进入流水线 | RW_CACHING |
| RW_CACHING | 提交成功 | `file_changed_cb` → 刷新书架 `cached` 标记 | RW_DONE → RW_SHELF |
| RW_CACHING | `cancel()` | 删 workspace，清 `.part` | IDLE |
| RW_CACHING | 出错 | 删 workspace；**旧书保持** | RW_ERROR |
| 任意 | `logout()` | 清 NVS、清书架、清 cookie | IDLE |

`RW_DONE` 只作一次性完成态：页面显示"已缓存"并在 2 s 后或用户操作后回 `RW_SHELF`。

### 3.5 错误映射 / Error to user text

| 错误 | 用户文案（UI 字面量） | 可重试 |
|---|---|---|
| `RW_ERR_NO_WIFI` | 未配置网络，请先在传输页连接 WiFi | 是（配网后） |
| `RW_ERR_LOGIN_FAILED` | 登录失败，请重新扫码 | 是 |
| `RW_ERR_LOGIN_TIMEOUT` | 二维码已过期，请重新获取 | 是 |
| `RW_ERR_OTP_REQUIRED` | 该账号需要验证码（当前版本暂不支持） | 否（v1） |
| `RW_ERR_SESSION_EXPIRED` | 登录已失效，请重新扫码 | 是 |
| `RW_ERR_PROTOCOL` | 微信读书接口返回异常，请稍后重试 | 是 |
| `RW_ERR_INTEGRITY` | 下载内容校验失败，请重试 | 是 |
| `RW_ERR_SD` | 存储读写失败，请检查存储卡 | 是 |
| `RW_ERR_NO_SPACE` | 存储空间不足，至少需要 %u MB | 否 |
| `RW_ERR_CANCELLED` | 已取消 | 是 |
| `RW_ERR_BUSY` | 正在处理上一本，请稍候 | 是 |
| `RW_ERR_OTA_BUSY` | 正在升级固件，请稍后再试 | 是 |
| `RW_ERR_CLOCK` | 设备时间未校准，请先联网对时 | 是（对时后） |
| `RW_ERR_NO_MEM` | 内存不足，请重启后重试 | 是 |

### 3.6 取消语义 / Cancellation

* **取消粒度**：每个 4 KiB 网络分块之间检查 `cancel` 标志；HTTP 单请求超时 10 s（正文）/ 15 s（登录与书架）。
* **join 上限**：`stop()` / 离页前先等 15 s（覆盖单次 10 s 请求超时）；仍未退出则记 `ESP_LOGW` 后继续等待——宁可变慢也不在请求仍持有 netif 时拆射频。
* **取消后的磁盘**：删 `chapters/`、`work/`、`*.part`；`toc.bin` 可保留（下次直接用），已提交的 EPUB 不回滚。
* **取消不会**把 `RW_ERR_CANCELLED` 显示成错误页，只回 `RW_SHELF`。

---

## 4. 生命周期与并发 / Lifecycle and concurrency

### 4.1 启动序列

```
UI 线程                       worker                    备注
start(source=WEREAD) ─────▶  (任务先创建，等信号)      射频启动前预留 16 KiB 内部 RAM 栈
   │ 射频启动 / 连接
   │ network_ready
   ├─ 有会话 ─────────────────▶ shelf()                进页直接出书架
   └─ 无会话 ────login()─────▶ QR
```

* worker 栈 **16 KiB**，`MALLOC_CAP_INTERNAL`；大缓冲（响应体、章节、ZIP 目录）一律 PSRAM。
* worker 任务在 `esp_wifi_start()` **之前**创建，失败即整体启动失败——与 httpd 的预留策略一致。

### 4.2 停止序列（顺序不可调换）

1. `weread_cancel()` → 置标志；
2. `weread_join()`（≤5 s）→ 释放 `esp_http_client`；
3. `httpd_stop()`（本 source 下为空）；
4. `esp_wifi_stop()` → `esp_wifi_deinit()`；
5. 反注册事件、销毁 netif、删 event loop；
6. 释放 `s_buffer`；状态置 `STOPPED`。

> 关键：**必须先 join 再动射频**。否则 worker 正在 `esp_http_client_read()` 时射频被拆，属于 use-after-free。

### 4.3 与其它子系统的互斥

| 场景 | 规则 |
|---|---|
| 进入 WeRead | `pico_online_busy()` 为真 → `RW_ERR_OTA_BUSY`，不启动会话 |
| WeRead 会话中进入 OTA | 现有 OTA 入口只检查 `pico_online_busy()` 与 `UPLOADING`；**需补一条**：`read_pico_transfer_weread_busy()` 为真时拒绝，或弹确认后 `stop()` 再升级 |
| 缓存中按电源键锁屏 | `pause_for_sleep` 返回 false → 拒绝锁屏并提示（与上传一致） |
| 缓存中自动锁屏 | 已由 `wifi_busy` 抑制，无需额外代码 |
| 缓存中拔 TF 卡 | 现有 `transfer_on_media_lost` 会调 `stop_session()` → `read_pico_transfer_stop()` → 内部 cancel+join；页面回错误页 |
| 空闲省电 | `READY` 且 60 s 无操作 → 自动 `stop()`；WeRead 会话与书架保留，下次重新 `start` |
| 后台对时 | `app_time_service_poll` 在 transfer 非 `STOPPED` 时本就不动作 |

`try_stop_if_idle()`：`weread_busy()` 时必须返回 false（等价于 UPLOADING）。

---

## 5. 数据与存储 / Data and storage

### 5.1 SD 布局

```
/sdcard/books/WeRead/<书名>-<bookId>.epub         最终书（书库可见）
/sdcard/.readpico/weread/<bookId>/chapters/NNNN.xhtml   章节 XHTML（工作区）
/sdcard/.readpico/weread/<bookId>/work/book.epub.part   打包临时区
/sdcard/.readpico/weread/<bookId>/work/central.tmp      ZIP 中央目录暂存
```

* `root_dir` 来自 `read_pico_transfer_cfg_t.root_dir`（`book_store_upload_root()`，即 `/sdcard/books`）；`WeRead/` 子目录由本功能创建。
* **只支持 SD**：卡未挂载直接 `RW_ERR_SD`，不落到内置 `/flash/books`（1 MiB 单文件上限放不下一本书）。
* **工作区不持久化**：成功或失败都删除 `<bookId>/`。没有 `toc.bin`、没有图片目录、没有断点文件。
* 章节与书的关系放在书里：每个章节 XHTML 的 `<head>` 内写一行 `<!--weread-uid:<chapterUid>-->`，将来做进度同步时从 EPUB 里读，不额外落盘。
* 生成时包含封面 `cover-image`（取书架 `cover` URL，魔数校验后嵌入）；正文 `<img>` 仍在消毒阶段丢弃。

### 5.2 NVS

| 键 | 内容 | 说明 |
|---|---|---|
| `rp_weread/session` | `{magic, version, vid[64], skey[384], rt[384], name[64]}` | 单 blob，长度与终止符校验，失败视为无会话 |
| `rp_weread/disclaimer` | `{magic, version, accepted}` | 首次进入必须确认 |

* 写入顺序：登录成功 → 先写 session → 再刷新 UI；退出登录 → 先删 session → 再清 cookie 与书架，失败可重试。
* 明文存储与 `rp_wifi` 同级（当前威胁模型：设备为个人所有）。**不得**打印 `wr_skey`/`wr_rt`，日志只打长度。

### 5.3 原子性

| 产物 | 写入方式 |
|---|---|
| EPUB | 写 `work/book.epub.part` → `zip_open` 自检 → `commit_file()` → `file_changed_cb(path)` |
| 章节 XHTML | `.part` + rename（单章；工作区，不是最终产物） |

`commit_file()` 已处理 FatFs 不能直接覆盖：先备份旧书、rename 新书、失败回滚。**旧书在新书提交成功前始终可读。**
整本成功后才删工作区；中途失败保留不了任何可用产物，直接删。

### 5.4 空间预检

* 进入 `RW_CACHING` 前：`free_bytes_cb() >= 估算 + 8 MiB`。
* 估算 = `Σ wordCount × 6`（UTF-8 中文 3 字节 + 标记与冗余），无 `wordCount` 时按每章 32 KiB 估。
* 不足 → `RW_ERR_NO_SPACE`，提示所需 MB，**不进入**流水线。
* 单章写失败按 `RW_ERR_SD` 中止整本。

---

## 6. 缓存流水线 / Cache pipeline

### 6.1 阶段与进度权重

| 阶段 | 内容 | 进度 |
|---|---|---|
| `TOC` | 阅读页 psvts + `chapterInfos` | 2% |
| `TEXT` | 逐章下载 / 解码 / 消毒 / 落盘 | 5–85% |
| `PACKAGE` | 下载封面（失败跳过）+ 组 OPF/nav/ncx + ZIP | 88% |
| `COMMIT` | 提交 + 回调 | 96% → 100% |

封面在打包阶段完成：`GET <shelf.cover>`（**不带 cookie**；会话只允许发往 weread.qq.com 及其子域）→ 魔数校验 JPEG/PNG → 写入 `OEBPS/cover.jpg|png`，OPF 同时写 `properties="cover-image"` 与 `<meta name="cover">`。书架缩略图由阅读器的 `book_epub_cover` 自动生成，不需要 crossmux 那种 BMP 转换。

界面按 **5% 桶与阶段变化**节流；进度只刷新该行右侧区块（`APP_REDRAW_AREA` + DU），每 20% 或阶段切换用 GL16 定稿，不整页刷新。

### 6.2 单章子步骤

```
e_0 ─┐
e_1 ─┼─▶ 拼接（跳过首字符）─▶ 反交换 ─▶ Base64 解码 ─▶ XHTML 消毒 ─▶ chapters/NNNNNN.xhtml
e_3 ─┘   ↑ 每片先校验 [32 字节大写 MD5 == md5(剩余体)]
```

* **不请求 `e_2`（CSS 分片）**：阅读器只解释有界 CSS 子集，样式由固定 stylesheet 承担。
* 消毒白名单以阅读器 `html_text` 支持集为准：保留 `p/h1-h3/div/br/strong/em/blockquote`，剥掉 `class/id/style/epub:type/script/iframe/svg`。
* **`<img>` 整体丢弃**：v1 不打包图片，保留标签只会在 EPUB 里留下指向网络地址的坏引用。
* 脚注（`qqreader-footnote` 等）取 `alt` 文本**内联**为 `（注：…）`，图片本身丢弃——本阅读器没有脚注弹窗。
* 每章在 `<head>` 内写 `<!--weread-uid:<chapterUid>-->`（见 §5.1），阅读器会跳过注释，不影响排版。
* TXT 格式的书：`e_0` 返回带 `bookId` 的 JSON 时改用 `t_0` / `t_1`，解码成纯文本后按空行切段并转义。

### 6.3 重试与超时

| 情形 | 首版策略 |
|---|---|
| 网络错误 / 5xx | **不自动重试**：整本中止、清工作区，保留书架；重试由用户再点该行 |
| 会话失效（`-2012` 或 401） | 清本地会话 → `RW_ERR_SESSION_EXPIRED`，提示重新扫码（不自动重登） |
| 完整性失败（MD5/Base64/ZIP） | 中止，提示重试 |
| 整本上限 | 15 min，超时中止并清理工作区 |

---

## 7. 界面与交互 / UI

### 7.1 入口

传输页「选择方式」卡片由 2 行改为 4 行：`WiFi 传书 / 热点传书 / USB 传输 / 微信读书`。
文件首页的 3 张卡保持不变；从文件首页直达 USB 时返回仍回文件页，从本列表进入 USB 时返回回到本列表。
USB 目前没有 Lucide 掩模（生成器需 Pillow ≥ 11.3），图标位先用文字徽标占位。

### 7.2 视图

| 视图 | 元素 | 刷新 |
|---|---|---|
| 免责 | 说明文字 + 同意 / 返回 | 整页 |
| 二维码 | QR（`ui_wifi_qr_prepare_url`）+ 状态行 + 返回 | 建码整页；轮询只刷状态行（`APP_REDRAW_AREA`） |
| 书架 | **在线目录**，每页 7 行：书名 / 作者 / 已缓存标记；点行开始缓存 | 翻页整页；缓存按 5% 与阶段节流 |
| 缓存中 | 正在缓存那一行右侧显示 `缓存 42%`（打包阶段显示 `打包 88%`），按钮变为「取消缓存」 | 只推该行右侧进度块（DU）；每 20% / 阶段切换 GL16 定稿 |
| 进度 | 书名 + 阶段 + 百分比 + 取消 | 阶段切换整页；百分比 5% 桶局部 |
| 完成 | "已缓存" + 打开 / 返回 | 整页 |
| 错误 | 文案 + 重试 / 返回 | 整页 |

* 书架**没有封面位**：行高与文件页一致，左侧不放缩略图。
* "已缓存"只是对 `root_dir/WeRead/<书名>-<bookId>.epub` 做一次 `stat`，不是第二份索引。
* 书架记录只存在 PSRAM（最多 1024 本，超出截断并提示），不落盘；进页重拉。
* 点行开始缓存该本；缓存期间其余行保持可读，只有进度行变化。
* 缓存完成后不强制打开，停在列表并刷新"已缓存"标记。

### 7.3 离页规则

| 当前状态 | 返回键行为 |
|---|---|
| `RW_IDLE` / `RW_SHELF` / `RW_ERROR` | 直接离开；会话保持到 60 s 空闲 |
| `RW_CACHING` | 返回键**只取消本次缓存并停在书架**，不离开页面 |
| `RW_WAIT_SCAN` | 直接离开并取消轮询 |

> 被动离页（拔卡、异常、其他入口抢焦点）不依赖拦截：`transfer_on_exit()` / `on_media_lost()` 会调 `stop_session()`，最终由 `read_pico_transfer_stop()` 取消并 join worker。此时按"已取消"处理，不留半本书。

---

## 8. 安全与合规 / Security and compliance

* **只走 Web Cookie**，不请求、不保存官方 `wrk-` API Key。
* **TLS 校验证书**（`esp_crt_bundle_attach`），不使用 `setInsecure`；请求前必须保证时钟可信（`time(NULL) >= 1704067200`，否则先 `read_pico_transfer_sync_time_online()`）。
* 若 `weread.qq.com` 的根不在默认 bundle：走仓库已有的证书补丁机制（`tools/prepare_tls_bundle.py` + `tls_bundle_host_test.py`），并在本文记录补入日期与根指纹。
* **Cookie 只发往 `weread.qq.com` 及其子域**：封面走腾讯 CDN，`wr_skey` 不得带到第三方主机；响应里的 `Set-Cookie` 同样只在该域吸收。
* 凭据不入日志、不上传、不在 UI 展示；退出登录必须清除。
* 首次进入显示免责声明与非官方声明；用户拒绝则不发起任何网络请求。
* 协议实现参考 [crossmux](https://github.com/0x1abin/crossmux)（MIT），在 `THIRD_PARTY_NOTICES.md` 登记；**不得**引入 AGPL 代码。

---

## 9. 限制与已知风险 / Limits and risks

| 项 | 限制 |
|---|---|
| 单章解码 | ≤ 4 MiB；超出 → `RW_ERR_INTEGRITY` |
| 章节数 | ≤ 8192（由阅读器 `book_epub` 上限决定） |
| ZIP | ZIP32、store-only；条目名 ≤ 96 字节 |
| 书名 | 文件名清洗后 ≤ 96 字节，重名以 `<书名>-<bookId>.epub` 区分 |
| 图文 | v1 无图；`<img>` 丢弃，纯图片书基本不可读 |
| 封面 | v1 不生成 `cover-image`；书架显示阅读器的默认封面 |
| 公众号 | 不支持 `MP_WXS_` |
| 协议 | 非公开接口，可能随服务端变化失效；接口集中在 `weread_protocol.c` / `weread_http.c` |
| Flash | 估算 +120~200 KiB；合入前记录 Pico 目标 `idf.py size` 前后值 |

---

## 10. 验收 / Acceptance

### 10.1 主机侧（`tools/`）

| 测试 | 内容 |
|---|---|
| `weread_protocol_host_test.c` | `encodeId` / `signQuery` / `urlEncode` 固定向量；UTF-8 截断 |
| `weread_shelf_host_test.c` | JSON 转义与代理对、书架解析、容量截断、稳定排序、文件名清洗与存在性 |
| `weread_content_host_test.c` | 固定签名向量、分片 MD5、反交换/Base64 往返、psvts 提取、XHTML 消毒、目录解析、ZIP 回读校验 |
| `weread_decode_host_test.c` | MD5 校验、反交换、Base64、消毒白名单、脚注内联 |
| `weread_zip_host_test.c` | store ZIP 目录、CRC、`mimetype` 首条；用 `zip_open()` 回读自检 |
| `weread_state_host_test.c` | 状态迁移、取消、错误映射（HTTP 用桩） |

固定向量（与公开协议研究一致，已用 Python 参考实现核对）：

```
encodeId("43208843") == "c9c321c07293508bc9c79df"
encodeId("2")        == "c81322c012c81e728d9d180"
signQuery("a=1&b=2") == "2a0a2922"
```

### 10.2 真机清单

1. 首次进入：免责 → 二维码 → 扫码 → 书架出现。
2. 断网：错误页 + 重试可用；恢复后重试成功。
3. 缓存一本：进度四阶段可见；完成后停在列表并标记"已缓存"，回本地书架能打开、翻页、记住进度。
4. 同书重缓存：旧书在提交前仍可打开；提交后进度不串书。
5. 取消：中途取消后无残留 `.part`，书架无半本书。
6. 空间不足：预检拦截并提示所需空间。
7. 锁屏：缓存中按电源键被拒绝并提示；空闲时正常锁屏。
8. 退出登录：会话清除，已下载书保留，再进需重新扫码。
9. OTA 互斥：缓存中进入升级被拒绝；升级中进入微信读书被拒绝。
10. 拔卡：缓存中拔 TF → 错误页，无崩溃。

---

## 11. 决策状态 / Decision status

### 11.1 已确认 / Confirmed (v0.2)

| 项 | 结论 |
|---|---|
| 联网 | 需要 WiFi，且只走 STA（连已保存路由器）；网络由本组件会话提供，WeRead 不自建 |
| 插图 | 不做；`<img>` 在消毒阶段丢弃 |
| 封面 | 不做；EPUB 无 `cover-image`，书架用默认封面 |
| 进度同步 | 本期不做，另立契约 |
| 划线 / 书评 / 公众号 | 本期不做 |
| 断点续传 | 不做；失败即清理，重来 |
| 书架形态 | **在线文件目录**：进页重拉，不落盘、无封面位、点一行缓存一行 |
| 书架缓存 | 不做 |
| 按章范围缓存 | 不做，只整本 |
| 退出登录 | 保留已下载 EPUB |
| 工作区 | 不持久化；成功/失败都删；章节 UID 写进章节 XHTML 注释 |

### 11.2 已确认（2026-10-31） / Confirmed

| # | 问题 | 结论 |
|---|---|---|
| Q1 | 入口位置 | 传输页「选择方式」第 4 行：WiFi / 热点 / USB / 微信读书 |
| Q2 | 书放哪里 | `/sdcard/books/WeRead/` 子目录 |
| Q3 | 免责声明频率 | 装一次确认一次（NVS 标记），退出登录不清除 |

---

## 12. 变更记录 / Revision log

| 版本 | 日期 | 内容 |
|---|---|---|
| v0.1 | 2026-10-31 | 初稿：归属、公共 API、两层状态机、存储与流水线、验收清单 |
| v0.2 | 2026-10-31 | 澄清 WEREAD 需要 WiFi 但只走 STA、不自建网络（§2.2/§3.0）；确认无图/无封面/无进度/无划线/无断点，书架改为在线目录（§1.2/§1.3/§5/§6.2/§7.2）；取消 `toc.bin` 持久化，章节 UID 写入 XHTML 注释；决策状态改为"已确认/仍待确认"（§11） |
| v0.3 | 2026-10-31 | Q1–Q3 确认；登录链路落地（§13）；join 上限改为 15 s；新增 `RW_ERR_CLOCK` |
| v0.4 | 2026-10-31 | 传书方式改为 4 行（补回 USB）；USB 从本列表进入时返回列表；登录链路实现进度更新 |
| v0.5 | 2026-10-31 | 书架落地：流式 JSON 扫描、书架解析/排序、PSRAM 记录表、列表与翻页；新增 `RW_ERR_NO_MEM` |
| v0.6 | 2026-10-31 | 单本缓存：签名请求、分片校验/反交换/Base64、XHTML 消毒、store-only ZIP、原子提交；行内进度与取消 |
| v0.7 | 2026-10-31 | 封面：书架 `cover` 采集、CDN 下载、魔数校验、OPF `cover-image`；cookie 仅限 weread 子域 |

---

## 13. 实现进度 / Implementation status

| 模块 | 状态 |
|---|---|
| `source=WEREAD` 生命周期（STA、不起 httpd、停止前 join） | 已实现 |
| 协议纯函数（MD5 / `_e` / `_s` / URL 编码 / UTF-8 截断） | 已实现，主机回归通过 |
| 会话结构（校验、Cookie 吸收、请求头、NVS） | 已实现，主机回归通过 |
| HTTPS 请求（证书包、Set-Cookie、取消、响应上限） | 已实现，未在真机编译验证 |
| 登录 worker（skills → getLoginUid → 轮询 → 保存会话） | 已实现，未在真机编译验证 |
| 传输页第 4 行入口与免责/二维码/状态视图 | 已实现，主机 UI 回归通过；真机未验证 |
| 书架拉取与列表 | 已实现，主机回归通过；真机未验证 |
| 章节下载、EPUB 打包、提交到书库 | 已实现（EPUB/TXT 两种正文、行内进度、取消），主机回归通过；真机未验证 |
| 封面 | 已实现（魔数校验、失败跳过、OPF `cover-image`）；真机未验证 |
| 行内进度 | 缓存行的右侧显示 `缓存 42%` / `打包 88%`；5% 桶触发局部刷新，不整页刷 |
| 进度同步、划线书评 | 未实现（不在本期） |

构建验证要求：`idf.py build`（ESP-IDF v6.1）与真机扫码；本机没有该工具链时不得把主机测试当作固件验证。
Build verification requires `idf.py build` (ESP-IDF v6.1) plus a real scan-code run; host tests are not firmware validation.
