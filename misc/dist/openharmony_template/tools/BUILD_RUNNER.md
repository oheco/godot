# oo 离线构建 runner（schemaVersion 1）

## 定位与支持边界

`build.cjs` 随生成的工程一起复制，供 C++ 导出插件用已选 Node 直接执行。它只构建
OpenHarmony Stage 工程的单一 `entry@default` 模块与 `default` product；不下载依赖、
不运行 npm/npx/ohpm、shell、Java、安装或 publish，也不修改官方 Hvigor、插件或 SDK。
不负责修改 SDK/API、`deviceTypes` 或 Godot 运行时；生成器应预先选好 SDK26 /
`runtimeOS: OpenHarmony` / `deviceTypes: ["default"]`，并准备所有本地构建输入。
不支持 C# 源码 debug；此 runner 不提供任何托管源码调试接口。

入口固定校验 `@oheco/hvigor 6.26.4-ohos.1`，官方 Hvigor/插件 6.26.4，
OpenHarmony arm64 host 和 `pinnedInterfacesUnmodified: true`。仅存在适配文件、
`hvigor --version` 成功、其他适配修订版均不够。更换版本必须显式更新并重新验收。

## C++ 调用接口

父进程先在**权限正常的应用私有缓存文件系统**创建一个本次导出独占的绝对路径
`workDir`（有效权限 0700），以及私有 JSON 请求文件（有效权限 0600、单硬链接）。
请求的直接父目录也须 0700。不要把密码请求放 HOME/hmdfs，也不要依赖 chmod
返回成功：该文件系统的有效 0660/02770 模式会被拒绝。runner 不自行 chmod。
请求文件由调用方在执行完成后删除；runner 不删除调用方的请求/签名材料。

用参数数组调用，不拼 shell 命令：

```text
<chosen-node> <absolute-project>/tools/build.cjs --request <absolute-private-json>
```

`node` 的查找、签名/执行权限与 `exec` 能力由 C++ 调用方负责；runner 的所有
子进程严格使用自身 `process.execPath`。`hvigorEntry` 必须是实际 `.cjs` 文件，
不是全局 `bin/hvigor` 软链接，也不是官方 CLI。当前机器的安装布局示例：

```text
<node-global-prefix>/lib/node_modules/@oheco/hvigor/bin/hvigor.cjs
```

当前 Node24 prefix：
`/storage/Users/currentUser/.oheco/packages/nodejs/24.21.0-ohos.1`。
SDK26 视图：`/storage/Users/currentUser/.oheco/sdk/26.0.0.35.Beta/root`，
不是其子目录 `26.0.0`。native 工具位于
`<sdkRoot>/26.0.0/toolchains/lib`，由适配器解析，runner 不另造 Java/原生工具路径。
不要把这些主机路径硬编码为所有安装的默认值。

C++ 可以在导出前额外用同一 Node 执行：

```text
<chosen-node> <absolute-hvigorEntry> --adapter-info
```

解析退出码 0 的 JSON，必须检查：

```json
{
  "adapter": "6.26.4-ohos.1",
  "host": "openharmony",
  "arch": "arm64",
  "upstream": "6.26.4",
  "pinnedInterfacesUnmodified": true
}
```

runner 在真正构建之前会再次执行同样的 preflight（15 秒上限、1 MiB 输出上限）。
这不是任意 `.cjs` 的安全认证：调用方须提供受信任的 oo 安装入口，不能将用户工程
附带的伪装 CLI 当成工具。adapter 内部还会核对固定官方接口文件摘要。

## 精确请求 schema

只接受下面七个必需字段和可选 `signing`；未知/缺失字段、错误类型均拒绝。
请求是普通 JSON，不支持注释。所有路径须绝对、可访问，不含 NUL。
`projectDir`、`sdkRoot`、`workDir` 不能通过软链接；`workDir` 须位于生成工程外。

```json
{
  "schemaVersion": 1,
  "projectDir": "/absolute/generated-project",
  "sdkRoot": "/absolute/sdk-view/root",
  "hvigorEntry": "/absolute/node-prefix/lib/node_modules/@oheco/hvigor/bin/hvigor.cjs",
  "buildMode": "debug",
  "format": "hap",
  "workDir": "/absolute/app-private-cache/export-unique-id"
}
```

- `schemaVersion` 必须是数字 1。
- `buildMode` 只能为 `debug` / `release`，`format` 只能为 `hap` / `app`。
- 签名省略时强制本次构建无签名，不沿用原 profile 的签名设置。
- 若传 `signing`，下面七个字段全部必需、须为非空字符串，不接受额外字段；材料
  路径必须为现有普通文件。密码可以含空格、Unicode、引号等，只禁止 NUL。

```json
{
  "certificate": "/absolute/private-config/application.cer",
  "profile": "/absolute/private-config/application.p7b",
  "storeFile": "/absolute/private-config/application.p12",
  "keyAlias": "application",
  "signAlg": "SHA256withECDSA",
  "keyPassword": "<supplied-only-in-private-request>",
  "storePassword": "<supplied-only-in-private-request>"
}
```

签名材料与密码必须由调用方显式提供；runner 不读取 HOME 密码文件、不使用个人
签名、不生成证书、不直接执行签名工具。HAP/APP 签名只走已经原生适配的 Hvigor。
签名配置 `type: OpenHarmony`；`certificate` 映射到 `material.certpath`，不是
`material.certificate`。生成 profile 仅保存环境引用：

```json
{
  "keyPassword": "env:OHECO_HVIGOR_KEY_PASSWORD",
  "storePassword": "env:OHECO_HVIGOR_STORE_PASSWORD"
}
```

真实值仅在私有请求和构建子进程环境中，绝不写入生成 profile 或子进程 argv。
stdout/stderr 分别流式透传；密码、材料路径和 alias 按完整值脱敏，支持跨块与
UTF-8 边界。退出错误也不输出请求、JSON 解析详情或原始文件系统异常。
不能保证任意第三方脚本编码/变换密码后的输出安全；不要添加不受信任的插件。

## 子进程命令与环境

HAP：

```text
<process.execPath> <hvigorEntry> --mode module -p module=entry@default -p product=default -p buildMode=<debug|release> assembleHap --no-daemon --no-incremental
```

APP：

```text
<process.execPath> <hvigorEntry> --mode project -p product=default -p buildMode=<debug|release> assembleApp --no-daemon --no-incremental
```

仅签名 APP 额外追加 `--parallel`，让内部签名实际分派到 worker。固定官方
6.26.4 的 SignPackagesFromApp 在 worker 拒绝时，main-thread 成功回退漏掉 Promise
resolve，会永久等待；这里采用已被适配器原生矩阵验证的 worker 调度，不修改官方包。
HAP / unsigned APP 不追加该开关，所有格式仍为 no-daemon / no-incremental。

cwd 为 `projectDir`；无 shell/npx，也不使用 daemon/增量构建。只在子进程设置：

- `OHOS_SDK_HOME`、`OHOS_BASE_SDK_HOME` = 请求 `sdkRoot`。
- `DEVECO_SDK_HOME` 若真实路径等于 sdkRoot 可保留，否则 unset；`JAVA_HOME` unset。
- `HVIGOR_USER_HOME` = 私有 workDir；`TMPDIR` / `TMP` / `TEMP` = workDir/tmp。
- 子进程 `HOME` = workDir/home。父进程 HOME 不变，`process.execPath`、`NODE_PATH`
  保持不变；适配入口仍从自身安装目录解析齐备的官方依赖。
- 设置 npm offline 环境开关；不会运行 npm。hvigor 配置依赖必须为空，工程 package
  依赖只允许已存在的 `file:` 输入。缺失/registry 依赖明确失败，不尝试补装。

这是依赖齐备、受信任的生成工程的离线构建接口，不是操作系统网络/Java安全沙箱；
任意工程插件仍可自行联网/执行其他程序。官方适配器提供 Java 回退 guard。

## Profile 事务、结果与崩溃恢复

构建前读取根 build-profile（支持 JSON 加注释/尾逗号，不支持任意 JSON5），临时
替换 `app.signingConfigs` 和 default product 的 `signingConfig`。原 profile 同一文件
系统 rename 为 `.godot-build-runner-original.json5`；成功、失败、输出不匹配或正常
信号中止后，在 `finally` 通过 rename 精确恢复原始字节/格式/模式，恢复成功后才
写 `result.json`。不修改 entry profile、SDK、官方包或任何运行时源文件。

workDir 和工程分别用 `.runner-lock`、`.godot-build-runner.lock` 防并发。拒绝调用
不会移除别人已有的锁。SIGINT/SIGTERM 转发给子进程；不强杀。SIGKILL、断电或
系统杀死无法运行 finally：需确认自有构建进程全部退出后，将工程保留的原 profile
备份 rename 回 build-profile，再删除自有 project lock。不能随意删除或覆盖该备份。
如果恢复本身失败，保留备份与 project lock，以便人工恢复，不产生成功结果。

在指定平面目录选择当前产物，**不递归搜索、不挑 first match**：

| 格式 | 唯一查找目录 | 名称结尾 |
| --- | --- | --- |
| HAP | `entry/build/default/outputs/default` | `-unsigned.hap` / `-signed.hap` |
| APP | `build/outputs/default` | `-unsigned.app` / `-all-signed.app` |

签名 APP 的产品合同是外层 APP 与内部 HAP 都走签名流程，不能把只有外层签名的
普通 `-signed.app` 当作成功导出。runner 只在本次临时选定 product 的
`buildOption.packOptions` 中强制 `appWithSignedPkg: true` 和
`buildAppSkipSignHap: false`，选择 `-all-signed.app`。原 false/missing/true 选项均会
在 finally 精确恢复为原始字节；不永久更改模板/用户 profile。HAP 和 unsigned
导出不改这些打包选项。原生签名 gate 另行验证外 APP 与每个内部 HAP。
本次构建前只清掉对应目录中的该格式旧产物，防止把旧文件当成当前成功结果。
不支持 custom build cache 目录；匹配零个/多个、空文件或软链接均失败。
`signed` 表示 Hvigor 对所选外层产物成功运行了请求的签名流程；runner 不做额外
密码/证书信任判断或独立验签，也不声明产物获得真实设备安装信任。

退出码 0 时，`<workDir>/result.json` 是私有 0600 文件，精确五个字段：

```json
{
  "schemaVersion": 1,
  "outputPath": "/absolute/generated-project/entry/build/default/outputs/default/entry-default-unsigned.hap",
  "mode": "debug",
  "format": "hap",
  "signed": false
}
```

构建产物保留在生成工程中；C++ 在检查退出码 0、结果 schema/预期 mode/format/
signed 和普通非空文件存在之后，将 `outputPath` 复制到用户所选目标。stdout 仅是
构建日志，不是 JSON 协议。非零退出必须视作失败，不能接受残留结果；正常有效
请求开始时会移除先前 result。推荐每次分配新的独占 workDir，并由调用方统一清理。

## 回归测试与尚未验收项

在仓库根目录执行（仅 Node 内置库，无 npm install）：

```sh
node --check misc/dist/openharmony_template/tools/build.cjs
node --test platform/openharmony/tests/test_build_runner.cjs
```

当前 46 项合成回归全部通过。测试生成合成材料和合成 package，覆盖 argv/env、
HAP/APP debug/release 签名矩阵、跨块错误密码脱敏、profile 原字节恢复、SIGTERM
中止恢复、失败/缺失/歧义/软链接/陈旧产物、精确 schema、私有 FS、锁、错误适配
版本/平台/固定接口，以及离线依赖边界。不会签真实 HAP。

本机已验证 oo 适配器在 child-only 私有 HOME/TMPDIR 下的 adapter-info 仍能解析
完整官方依赖，CLI --help 退出 0 并确认 --no-incremental/--no-daemon 支持。
这不等于 Godot Editor 应用沙箱内完整原生导出验收。还需集成方验证：所选 Node/SDK 与原生工具在应用 UID 下
可访问且可执行、SDK26 生成配置、真实项目的无 Java 离线导出，以及正常用户的运行
调试流程。设备安装、个人签名、publish 和 C# 源码 debug 不属于此 runner 验收范围。
