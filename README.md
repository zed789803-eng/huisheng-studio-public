<p align="center">
  <img src="ui/logo.png" width="112" alt="绘声Studio">
</p>

<h1 align="center">绘声Studio</h1>

<p align="center">
  把指定 App 的声音低延迟送进 DAW，经过 VST3 处理后，再把宿主混音送回去，<br>
  作为 OBS / 抖音直播伴侣 / 微信可选的「麦克风」。
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-2f6f4e" alt="AGPL-3.0"></a>
  <a href="https://github.com/zed789803-eng/huisheng-studio/releases/latest"><img src="https://img.shields.io/github/v/release/zed789803-eng/huisheng-studio?label=download&color=2f6f4e" alt="latest release"></a>
  <img src="https://img.shields.io/badge/macOS-12%2B-555" alt="macOS 12+">
  <img src="https://img.shields.io/badge/universal%20binary-arm64%20%7C%20x86__64-555" alt="universal binary">
</p>

## 怎么用

四步。第 1 步装一次就好，后面三步都在 DAW 和推流软件里完成。

**1. 装驱动和后台服务**

最快是直接下安装包：到 [Releases](https://github.com/zed789803-eng/huisheng-studio/releases/latest)
下载 `.pkg` 双击安装，或者用 `.dmg`。安装过程要输入管理员密码。

> 安装包是本机自签名、没有公证的，macOS 会提示「无法验证开发者」。
> 右键 → 打开，或到「系统设置 → 隐私与安全性」点「仍要打开」即可，不是文件损坏。

想从源码装也行：

```sh
sh scripts/install_driver.sh   # 需要管理员密码，会重启 coreaudiod
sh scripts/install_agent.sh
```

装完系统里会多出八台**单向**设备：输出页四台 `HsAudioSC 1/2` 到 `HsAudioSC 7/8`，
输入页四台 `HsAudioSR 1/2` 到 `HsAudioSR 7/8`。编号相同的两台是一对环回——
DAW 往 `HsAudioSC n/n` 送，OBS 从 `HsAudioSR n/n` 取。

**2. 在 DAW 里插 Capture**

插在一条立体声 Bus 或总线轨上。左边点 **选择软件** 勾选要采集的 App——可以一次勾几个，
它们相加后一起混进这条轨道，直接读这些 App 的声音，不用改它们的输出设备；或者改选
**虚拟通道**，外部播放器把输出设成同一对里的 `HsAudioSC X`。两路可以同时开，插件内部
相加，不做限幅（全程 float32）。

Capture：勾选要采集的一个或多个 App，或选虚拟通道；顶部显示后台服务与权限状态，缺权限会直接给出设置路径。

要同时采多个平台有两种做法：**一个实例里勾多个软件**（它们相加后走同一条轨），或者
**在不同轨道上各插一个 Capture**——一个实例占一路，最多八路同时跑，互不干扰，关掉其中
一个也不影响其他几个。勾过的软件按身份记住（不是按进程号），下次打开工程会自动认回来。

App 自己的采样率和工程不一致也没关系（比如酷狗 44.1 kHz、工程 48 kHz）：链路会自动重采样，
不会因为采样率不同就不出声；页脚会写出「应用 X Hz，工程 Y Hz」，一个实例里勾了多个软件时
写的是「N 个软件一起送进这条轨道」。

**3. 在 Main 输出插 Return**

选 `HsAudioSC 1/2`，宿主混音就从这个输出半台送出去（OBS 那边读 `HsAudioSR 1/2`）。
采集通道和回传通道不要选同一对，插件会检测并拒绝。

Return：选回传通道，界面底部直接提示 OBS 的麦克风该选哪个设备。

**4. OBS 麦克风选 `HsAudioSR 1/2`**

抖音直播伴侣、微信视频号同理，把麦克风或输入设备选成同一个虚拟设备。

整条链路约 30 ms，具体数字见下文「延迟」。

## 工作原理

```
伴奏 App ──process tap──▶ 绘声Studio Agent ──共享内存──▶ Capture VST3
                                                        │
人声 ──声卡输入──────────────────▶ Studio One ◀─────────┘
                                        │
                         Return VST3 ──▶ Agent ──▶ HsAudioSC 1/2 ══▶ HsAudioSR 1/2 ──▶ OBS
```

驱动只做环回，不做 DSP；跨进程的每一段用 64-tap 窗函数 sinc 插值器加 PI 控制稳住
缓冲水位，所以 DAW 的时钟和虚拟设备的时钟不需要一致，采样率不同也能走同一条链路。

## 系统要求

- **macOS 12.0 及以上**，安装包与插件均为通用二进制（Apple Silicon + Intel 同一个包）。
- **应用采集（把单个 App 的声音送进 DAW）需要 macOS 14.2+**：它用的是 CoreAudio
  process tap，这个 API 从 14.2 才有。macOS 12 / 13 上插件里的软件选择按钮会置灰并说明原因，
  虚拟通道与 Return 两条链路完全可用。
- 想在 12 / 13 上抓单个 App，只能让它输出到 `HsAudioSC n/n` 虚拟设备（App 自己支持选输出设备时才可行），
  或者另装 BlackHole / Loopback 这类虚拟声卡中转。

## 构建

需要 macOS 12 以上、Xcode 命令行工具、CMake 3.22 以上（`brew install cmake`）。

```sh
git clone https://github.com/zed789803-eng/huisheng-studio.git
cd huisheng-studio
sh scripts/build.sh
```

JUCE 固定为 8.0.15：配置时如果 `third_party/JUCE` 不存在，CMake 会自动下载对应 tag
的源码包并校验 SHA-256；如果你已经放了一份（离线构建），直接放进 `third_party/JUCE`
即可，不会再下载。

`scripts/build.sh` 优先用 `third_party/cmake-3.31.6-macos-universal/`（如果你本地放了），
找不到就用 `PATH` 里的 `cmake`；想指定别的就 `CMAKE=/path/to/cmake sh scripts/build.sh`。

产物：

- `build/agent/HuishengStudio Agent.app`
- `build/HuishengStudioCapture_artefacts/Release/VST3/HsAudio Capture.vst3`
- `build/HuishengStudioCapture_artefacts/Release/AU/HsAudio Capture.component`
- `build/HuishengStudioReturn_artefacts/Release/VST3/HsAudio Return.vst3`
- `build/HuishengStudioReturn_artefacts/Release/AU/HsAudio Return.component`
- `build/drivers/HuishengStudio.driver`
- `build/hsmon`

构建同时产出 VST3 和 AU：VST3 给 Cubase / Nuendo / Studio One / Reaper / Bitwig，
AU 给 Logic Pro / GarageBand / MainStage（这几个根本不加载 VST）。VST3 会自动复制到
`~/Library/Audio/Plug-Ins/VST3/`，`.component` 会复制到
`~/Library/Audio/Plug-Ins/Components/`。

装好后校验 AU（改了 code 或换了机器都先跑这一步）：

```sh
killall -9 AudioComponentRegistrar 2>/dev/null
auval -v aufx Cap1 Huis    # HsAudio Capture
auval -v aufx Ret1 Huis    # HsAudio Return
```

## 安装

```sh
sh scripts/install_driver.sh   # 需要管理员密码，会重启 coreaudiod
sh scripts/install_agent.sh
```

装好后是八台**单向**设备，编号相同的两台是一对：

| 设备 | 用途 |
|---|---|
| `HsAudioSC 1/2` → `HsAudioSR 1/2` | DAW 混音 → OBS 麦克风（Return 默认） |
| `HsAudioSC 3/4` → `HsAudioSR 3/4` | 外部播放器 → DAW（Capture 默认） |
| `HsAudioSC 5/6` → `HsAudioSR 5/6` | 备用 |
| `HsAudioSC 7/8` → `HsAudioSR 7/8` | 备用 |

`SC` 那台只出现在「输出」页，`SR` 那台只出现在「输入」页，所以选设备时一眼能看出方向。

纯虚拟通道路径不需要任何系统权限。只有“在插件里直接选 App”才需要
「屏幕与系统音频录制」，由 Agent 在首次使用时弹出。

## 延迟

一个 128 帧的 IO 周期在 48 kHz 是 2.67 ms。整条链路：

| 段 | 延迟 |
|---|---|
| 驱动 out→in | 1 个 IO 周期 + 64 帧 safety + 1024 帧读滞后（约 21 ms） |
| Agent / 插件之间的 ring | 128 帧目标水位，由重采样器维持 |
| App tap | 由 macOS 决定，通常 1 个 IO 周期 |

读滞后是 1.0.9 起才有的：输出半台和输入半台是两台设备，HAL 可能先跑读的周期，
所以输入半台固定落在共享时间线后面 1024 帧再读（`kPairReadLagFrames`）。

Return 全链路 = 一个 ring 缓冲（128 帧）+ 一个设备 IO 周期 + 驱动环回：

| 组成 | 128 帧 / 48 kHz |
|---|---|
| Return 插件 → Agent ring | 2.7 ms（由重采样器稳住的 128 帧水位） |
| 虚拟设备输出缓冲 | 2.7 ms |
| 驱动 out→in | 2.7～5.3 ms + 21.3 ms 读滞后 |

合计 **约 30 ms**，设备缓冲调到 512 帧时约 33 ms。
（数字来自缓冲区配置，不是估算经验值。）

## 漂移补偿

DAW 的时钟（声卡晶振）和虚拟设备的时钟独立，实测相差约 5.5 ppm，
不处理的话每小时会有几次掉音。每个跨界链路都带一个 64-tap 窗函数 sinc
插值器，由 PI 控制器把缓冲水位稳在 128 帧：

- 比率恒为 1.0 时是逐样本拷贝（bit-exact）
- 补偿范围 ±600 ppm，远大于晶振误差
- 音高偏移最大 0.1 cent，人耳不可闻
- 实测 120 秒 × 5.5 / 50 ppm 正负两个方向，缓冲稳定在 129–156 帧，0 次失步

不同采样率的 App 或设备也走同一个插值器，不再直接报错。

## 测试

```sh
sh tests/run.sh
```

五组不需要安装任何东西的测试：

- **驱动环回**：直接调用驱动的 IO 路径，验证 1 个 IO 周期延迟、逐样本一致、
  未写入的设备返回静音、上报的 0 / 64 / 1024。
- **采集槽位**：八个槽位、每个槽位最多八个应用的 ring 名字各不相同、长度在 `shm_open`
  允许范围内，往一个槽位（或同一个槽位里的另一个应用）写入的音频在别处一帧都看不到
  （一个实例偷另一个实例的帧，会表现为断续、爆音），以及每槽位/每应用的字段互不影响，
  两个应用可以各自跑在 44.1 kHz 与 48 kHz。
- **漂移长跑**：±5.5 / ±50 ppm 各跑 120 秒，验证缓冲稳定在目标水位附近。
- **块大小与采样率比**：128/256/512 帧，以及 44.1k↔48k、96k→48k。
- **注册码**：与生成器共用一组固定向量，验证生成、校验、抄错一个字必须验不过，以及激活（把码写进文件再读回来算已注册、坏文件不算、没有文件就是未激活）。

CI 里还会用 `tools/preview` 离屏跑一次行为检查（两个实例必须各占一路；同一个实例勾两个软件、
App 是 44.1 kHz、工程是 48 kHz 时输出必须是两路之和而不是静音），结论以
`slot checks: PASSED` 打在日志里。

## 排障：coreaudiod 占满 CPU、宿主打不开

如果重启宿主时 `coreaudiod` 冲到 80–100%，宿主卡在启动音频这一步，先查有没有重复的 HAL 驱动：

```sh
ls /Library/Audio/Plug-Ins/HAL/*.driver
grep -l 6E7696C7-460F-493A-941D-3733613ED3A6 /Library/Audio/Plug-Ins/HAL/*.driver/Contents/Info.plist
```

第二个命令只应命中 `HuishengStudio.driver`。命中两个就说明有旧实验版本残留在同一目录，它们共用同一个 CFPlugIn factory UUID，所有音频客户端都会报 *More than one bundle with the same factory UUID*，`coreaudiod` 会在每次属性查询时重建插件表，CPU 直接打满。删掉多余的那个，然后：

```sh
sudo killall coreaudiod
```

1.0.6 起安装包会在 preinstall 里按 UUID 清理这类残留，所以正常升级不会再留下第二份。

## 诊断

```sh
build/hsmon          # 一次快照
build/hsmon -w       # 每 0.5 s 刷新
build/hsmon -r tap   # 单个 ring 的原始计数
```

权限引导窗口可以在进程内离屏渲染成 PNG，不需要系统的“屏幕录制”授权，
用它就能检查卡片里的文字有没有被摆到卡片外面：

```sh
"/Applications/HuishengStudio Agent.app/Contents/MacOS/HuishengStudio Agent" \
    --snapshot-permissions /tmp/guide.png
```

`hsreturnsim` 假装自己是 Return 插件：先心跳若干秒，再静默若干秒。用来确认
Agent 只在插件活着时打开虚拟输出，插件退出后不会每两秒反复启停：

```sh
build-release/hsreturnsim 6 6
```

Agent 是单实例的。安装器、插件、登录项可能在同一秒里各拉起一次，只有第一个
拿到锁的进程真正工作；其余进程把“显示授权引导”转给它，然后立刻退出，
不会有两个 worker 抢同一组 ring 和 tap。

## 已知限制

- 驱动只提供 2 声道设备；多声道虚拟设备还没有做。
- 同时最多八个 Capture 实例（八路槽位）；第九个实例的界面会直接说明要先关掉一个，
  不会静默地不工作。一个实例里最多勾八个软件，它们相加后混进同一条轨道。
- 一条轨道上只插一个 Capture：这个插入件整条替换轨道输入，串两个的话只有最后一个出声。
- App 采集需要 macOS 14.2+（process tap）。
- 覆盖升级时后台服务会被新版替换：如果升级前那个旧版服务还在跑，它写的是上一版的共享内存
  布局，插件读不到心跳。1.0.12 起插件会自己识别这种情况、请旧服务退出并拉起新装的那个，
  因此升级后**不需要**为此重启电脑；真的起不来时页脚会直接提示去「应用程序」里手动打开。
- 发布用的安装包目前是本机自签名 `HuishengStudio Dev`，没有 Developer ID 公证，所以
  macOS 会要求用户手动放行；这不影响源码构建，只影响别人安装时的摩擦。
- JUCE 8 是 AGPLv3 / 商业双授权，本项目的许可选择见下一节。

## 注册

安装包带一个离线注册码：一个码对一个名字，**不绑机器、不联网、不过期**。
默认是「只提示」——没注册时插件窗口右下角显示「未注册 · 输入注册码」，采音照常工作，
不会把正在录的工程搞哑。

- 码存在 `~/Library/Application Support/huishengstudio/license.txt`，Capture、Return 与
  Agent 读的是同一个文件，在哪个插件的窗口里贴都算数。
- 生成器 `hskeygen`（源码 `tools/hskeygen.cpp`，CI 产物里也单独附一个）：

```sh
build/hskeygen 张三               # 出一个码
build/hskeygen --batch 客户 10     # 连着出 10 个
build/hskeygen --check HS1-...    # 校验一个码
```

`hskeygen` 里带着产品密钥，只自己留着，不要随安装包一起发出去。
`shared/License.h` 里的 `kEnforcement` 是硬禁用开关：改成 1，未注册就不再启动采集；
默认 0，只提示。

## 许可

本项目以 **GNU Affero 通用公共许可证第 3 版（AGPL-3.0）** 发布，全文见
[LICENSE](LICENSE)。选它的原因是 JUCE 8 的双授权：AGPLv3 或 JUCE 商业许可。

- 你可以自由使用、修改、再分发，条件是按 AGPL-3.0 提供完整对应源码。
- 如果你想把本项目的代码并入闭源产品，需要自行获得
  [JUCE 商业许可](https://juce.com/legal/juce-8-licence)；AGPL-3.0 不允许那样做。
- 第三方组件（VST3 SDK、zlib、libpng、HarfBuzz、SheenBidi 等）各自的许可与声明见
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
- VST is a trademark of Steinberg Media Technologies GmbH。本项目与 Steinberg 无隶属关系。

AGPL-3.0 只授予版权许可，不授予商标许可，详见下面的「商标」。

## 隐私

- 本程序**不发起任何网络连接**，没有遥测、没有账号、没有云端处理。
- 采集到的音频只在进程内存和匿名共享内存里流转，不落盘；进程退出即释放。
- 需要「屏幕与系统音频录制」和「麦克风」两项系统权限，用途仅为读取你选中的 App
  的声音、以及 huishengstudio 虚拟输入通道；可以在系统设置里随时撤销。
- 卸载脚本会一并清除这些权限记录，见 [scripts/uninstall.sh](scripts/uninstall.sh)。

## 商标与来源

本仓库是 [wannengya-audio](https://github.com/shangcheng8/wannengya-audio)（作者 shangcheng8）
的修改版，源码按 AGPL-3.0 取得。上游对「万能鸭」「wannengya」名称、上游 `ui/logo.png`
图标与 `com.wannengya.*` bundle identifier 保留权利，这些不随 AGPL-3.0 授权；本版本已按
上游要求把上述名称、图标与 bundle identifier 全部更换。

本版本使用「绘声Studio」名称、`ui/logo.png` 图标以及 `com.huishengstudio.*` 这几个
bundle identifier，它们同样不在 AGPL-3.0 的授权范围内，再分发时请换成你自己的名字和
bundle id。

## 贡献

欢迎 issue 和 PR。提交代码即表示你同意以 AGPL-3.0 授权你的贡献，并且保留你对该贡献的
版权（不要求转让）。如果你希望本项目将来能提供商业许可，请不要提交你无法这样授权的代码。
