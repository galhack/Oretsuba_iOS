# 我们没有翅膀 · Apple 平台移植

《俺たちに翼はない》（Navel，2009）的 iPhone / iPad / Apple Vision Pro / Mac 原生移植。

> **v0.1.1 预发布。** 中文版启动时直接进入游戏标题主界面，无需点击三张补丁提示图。 iPhone、iPad、Vision Pro 已实机确认中文正文、声音与推进；iPad 普通／快速存读档、Vision Pro 普通存读档、原生大按钮、回想切换和章节文字均已通过。iPhone／Vision Pro 第一段片头、iPad 第二段片头的连续画面、音乐和结束返回已通过实机确认。Mac 已验证中文开篇与发布构建的目录选择。完整角色路线尚未逐一通关，部分视觉效果仍有逆向开放问题，详见[验证记录与覆盖边界](docs/VALIDATION.md)。安装包见 [v0.1.1 Release](https://github.com/galhack/Oretsuba_iOS/releases/tag/v0.1.1)。

本项目从零重写了游戏所用的 Lucifen 引擎（脚本虚拟机、图层合成、过渡与特效、文本、界面、音频、
片头动画、存档），在 Apple 设备上原生运行，并支持「中文三号补丁」汉化。

**本仓库不包含任何游戏内容。** 你需要自己购买游戏，并使用你自己的游戏文件。

## 支持的设备

| 设备 | 方式 | 构建最低系统要求 |
|---|---|---|
| iPhone、iPad | 侧载（Sideloadly 或 Xcode） | iOS / iPadOS 17 及以上 |
| Apple Vision Pro | Xcode 签名安装原生 visionOS 应用 | visionOS 1 及以上 |
| Mac（Apple 芯片或 Intel） | 直接运行 | macOS 14 及以上 |

## 准备游戏文件

1. 游戏光盘（或光盘镜像）中的 **`SET_UP` 文件夹**，其中应包含
   `SCRIPT.LPK`、`SYS.LPK`、`CHR.LPK`、`PIC.LPK`、`BGM.LPK`、`SE.LPK`、`VOICE.LPK`、`DATA.LPK`。
   无需在 Windows 上安装游戏。
2. （可选）**中文三号补丁**文件夹，其中应包含 `SCRIPT_CHS`、`SYS`、`DATA` 文件夹以及
   `ORE_TUBA_CHS.dll`。有补丁时可在启动器中选择「简体中文」。

## 安装

### iPhone / iPad（侧载）

1. 在本仓库的 [Releases](../../releases) 页面下载最新的 `Oretsuba-iOS-unsigned.ipa`。
2. 用数据线连接设备，打开 [Sideloadly](https://sideloadly.io/)，选择设备与 IPA，
   填入你的 Apple ID 后点击 Start。免费 Apple ID 签名的应用 7 天后需要重新签名，
   且同一设备最多同时安装 3 个免费签名的应用。
3. 首次打开前，在设备「设置 › 通用 › VPN 与设备管理」中信任你的开发者证书。
4. 如果系统要求开发者模式，在「设置 › 隐私与安全性 › 开发者模式」中启用，并按提示重启、确认。

免费账号的有效期与应用数量限制见 [Apple 个人团队说明](https://developer.apple.com/help/account/basics/about-your-developer-account)。
更新或续签时，用同一个 Apple ID 和 Bundle ID 覆盖安装，保留应用及本地存档；不要先卸载。
Sideloadly 的覆盖安装和自动续签方法见 [官方 FAQ](https://sideloadly.io/faq.html)。

### Apple Vision Pro

本项目在第一代 Vision Pro 上使用原生 visionOS 目标，安装方式如下：

1. 在 Mac 安装带 visionOS 平台组件的 Xcode，在 Settings › Accounts 登录自己的 Apple Account。
2. 按下方「从源码构建」生成并打开工程。
3. 在 Xcode 的设备管理中将 Vision Pro 与 Mac 配对；Xcode 27 使用 Device Hub。保持头显解锁，并按系统提示开启开发者模式。
4. 在工程的 Signing & Capabilities 中选择自己的 Team，保持 Automatically manage signing 开启。
5. 选择已配对的 Vision Pro 为运行目标，点击 Run。应用安装后，再按下方方法导入游戏文件。

可参考 [Apple 的实机运行说明](https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices)
和 [开发者模式说明](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)。
Release 中的 `Oretsuba-visionOS-unsigned.ipa` 需要另行签名，不能直接安装；目前已验证的安装方式是 Xcode，尚未验证 Sideloadly 对原生 visionOS 包的支持。

### Mac

下载 `Oretsuba-macOS.zip`，解压后把 `Oretsuba.app` 拖入「应用程序」。
首次打开时如被系统拦截，请在「系统设置 › 隐私与安全性」中点击「仍要打开」。

## 导入游戏文件

* **iPhone / iPad**：用数据线连接 Mac，在访达侧栏选中设备 › 「文件」标签页 ›「我们没有翅膀」，
  把 `SET_UP` 文件夹（以及汉化补丁文件夹）拖进去。也可以先把文件夹存入「文件」App，
  再在启动器中点「从『文件』导入文件夹…」。
* **Vision Pro**：用隔空投送把文件夹发到 Vision Pro 并存入「文件」，再在启动器中导入。
* **Mac**：在启动器中直接选择游戏目录与汉化补丁目录，无需复制。

启动器检测到文件后即可选择语言并开始游戏。

## 操作

**Vision Pro**：使用游戏窗口下方的原生「保存／读取／快存／快读／回想／菜单」按钮。再次点击「回想」可关闭文本记录，正文可注视后捏合推进。

| 操作 | 触控 | 鼠标 / 键盘 |
|---|---|---|
| 前进 / 确定 | 轻点 | 左键、Enter |
| 菜单 / 隐藏文本框 | 双指轻点 | 右键 |
| 回想（文本记录） | 下拉 | 滚轮向上 |
| 快进 | 长按 | 按住 Ctrl（或在设置中改为 Shift） |

也支持游戏手柄：A 确定、B 菜单、右扳机快进、十字键上下滚动。

## 从源码构建

本轮验证使用 Xcode 27.1。若系统默认指向 Command Line Tools，先在当前终端设置：

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
```

```sh
ORE_TEAM=YOUR_TEAM_ID tools/apple/gen_project.sh   # 把 YOUR_TEAM_ID 换成自己的开发团队 ID
open app/Oretsuba.xcodeproj
```

团队 ID 可在 Xcode 的签名设置中确认。只构建 Mac 时可直接运行 `tools/apple/gen_project.sh`，无需开发团队。
工程生成脚本使用 PATH 中的 XcodeGen；未安装时会下载固定版本到本项目忽略的 `build/tools` 目录。

命令行测试宿主（macOS，无窗口运行并输出截图）：

```sh
make -C engine
build/host/orehost --data <SET_UP 目录> --frames 1200 --shot 600,1200 --out /tmp/ore
```

引擎结构与逆向笔记见 `docs/ENGINE_GUIDE.md` 与 `docs/re/`。

## 致谢与许可

* 游戏《俺たちに翼はない》© Navel。本项目与 Navel 无关，仅供已购买正版的玩家使用。
* 中文汉化：中文三号补丁制作组（汉化工程 [HIllya51/ORE_TUBA_Chinese](https://github.com/HIllya51/ORE_TUBA_Chinese)）。
* 第三方库：stb_vorbis、stb_image、stb_image_write（public domain / MIT），pl_mpeg（MIT），minimp3（CC0）。
  LPK / ELG 格式参考了 GARbro（MIT）。
  相关许可和署名见 [ThirdPartyNotices.txt](app/Resources/ThirdPartyNotices.txt)，也包含在应用包内。

## 本地验证与测试包

```sh
make -C engine -j8 all tests
build/host/save_roundtrip /path/to/SET_UP
build/host/quick_save /path/to/SET_UP
build/host/command_input /path/to/SET_UP
build/host/native_coverage /path/to/SET_UP
build/host/native_coverage /path/to/SET_UP /path/to/中文三号补丁
build/host/image_blend
build/host/movie_flow /path/to/SET_UP /path/to/中文三号补丁
python3 tools/test_chinese_save.py /path/to/SET_UP /path/to/中文三号补丁
python3 tools/test_quick_save.py /path/to/SET_UP /path/to/中文三号补丁
python3 tools/test_history_toggle.py /path/to/SET_UP /path/to/中文三号补丁
xcrun swiftc -module-cache-path build/swift-module-cache \
  app/Sources/App/FolderImport.swift app/Tests/FolderImportTests.swift \
  -o build/folder-import-tests
build/folder-import-tests
tools/apple/package.sh ios       # 未签名 IPA，供侧载工具重新签名
tools/apple/package.sh visionos  # 未签名原生 visionOS IPA
tools/apple/package.sh macos     # 本地 ad-hoc 签名的 Mac Catalyst 包
```

产物在 `build/packages/`，不包含游戏数据。未签名 IPA 不能直接通过设备安装工具安装；需由用户自己的账号签名。Mac 包尚未公证。

维护者：galhack <galhack@protonmail.com>。
