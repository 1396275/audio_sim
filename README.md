# AudioSim

基于 Patchy dev 0.0.945 的独立音频仿真框架起点。源码基线为 f2517e5271af62ad3fea36d148d71a7f7c26edf9，并包含本工作区此前的 Windows 构建适配和 Light / Dark 主题改动。

## 版本管理

本目录是独立 Git 仓库，主分支为 `main`。`v0.1.0` 标记 2026-10-26 的首次源码基线，包含音频仿真、多通道路由、Welch 频域分析和启动窗口主屏定位修复。该标签表示源码快照；现有测试记录和硬件验证限制见下文。

源码、测试、构建脚本、`UI/package-lock.json` 和 `Run/Examples/` 示例纳入版本管理。`dependencies/`、`build/`、`UI/node_modules/`、`UI/dist/`、生成的 EXE/DLL 和日志由 `.gitignore` 排除；`.gitattributes` 统一文本换行。克隆源码后按“构建”一节恢复依赖并生成运行程序。依赖下载脚本中的 `pthreads4w` 当前仍使用上游 `cmake` 分支，系统工具链仍使用本机安装，因此此标签不承诺字节级可复现构建。

在本目录查看状态和保存改动：

```powershell
git status
git diff
git add Source/StandaloneApp.cpp Tests/WindowPlacementTests.cpp
git diff --cached
git commit -m "fix: describe the change"
git log --oneline --decorate -10
```

开发新功能可先运行 `git switch -c feature/功能名`，完成验证并提交后切回 `main`，使用 `git merge feature/功能名` 合并。提交信息建议用 `feat:`、`fix:`、`docs:`、`build:`、`test:`、`chore:` 前缀。

发布时使用 `v主版本.次版本.修订号`：兼容修复增加修订号，兼容功能增加次版本，不兼容变更增加主版本。先同步 `CMakeLists.txt` 中项目和 `juce_add_plugin` 的应用版本、补充变更说明并运行构建测试，再提交并创建附注标签，例如 `git tag -a v0.1.1 -m "AudioSim v0.1.1"`。`UI/package.json` 当前保留上游内部 UI 包版本，不作为应用发布版本。

使用 `git revert 提交号` 撤销已经提交的修改并保留历史；使用 `git switch -c review/v0.1.0 v0.1.0` 在新分支查看或重建基线，切换前先提交或暂存手头改动。发布标签保留原指向。

远程地址由仓库所有者提供后配置：`git remote add origin <仓库地址>`，再执行 `git push -u origin main` 和 `git push origin v0.1.0`。仅有本地提交无法在磁盘损坏后恢复，远程或独立备份用于灾难恢复。

## 启动与使用

双击 `Start-AudioSim.cmd`，运行 `Run/AudioSim.exe`。Built-in 仅提供以下节点，上游编号保持不变：

| 节点 | 类型编号 | 用途 |
| --- | --- | --- |
| Audio In Device | 3 | 音频设备输入 |
| Audio Out Device | 4 | 音频设备输出 |
| Audio Monitor | 6 | 单输入观察节点，根据实际输入通道叠加显示或分通道子图 |
| Frequency Analysis | 29 | Welch 功率谱密度、窗函数 / FFT 长度、谱线保持、频率范围 |
| Source > Audio File | 26 | 加载多通道音频文件 |
| Source > Waveform Generator | 26 | 正弦波、方波、三角波、锯齿波、白噪声 / 粉红噪声 |

启动窗口始终位于系统主显示器：保留主屏内的上次位置，旧位置在副屏或屏幕外时回到主屏居中，并按当前工作区缩小过大的窗口，避开任务栏和原生标题栏。位置在窗口显示前恢复。

通过 File > Open 打开 `Run/Examples/Gain-DLL.patchy`，播放信号并调节 Level，体验 DLL 调参和波形显示。默认附带 Level、Amp、Splitter、Spectrumyser 四个音频 DLL。齿轮 Preferences > Graph > Appearance 可切换 Light / Dark，选择会保存。

启动页使用本机字体，不再请求在线字体。声卡能力搜索在后台串行执行，扫描期间可操作画布；结果在当前界面实例内缓存，44.1/48 kHz 切换复用能力数据。接入新设备后，点击 Audio In / Out 选择框旁的 ↻ 刷新。启动不再扫描音频框架未使用的 MIDI 和串口。DLL 扫描结果与参数描述复用，避免重复加载和创建临时实例。

Monitor 环缓冲按实际路由输入通道准备并跨图重建复用，未接线时不分配环缓冲；声卡 FIFO 仅在实际打开设备后创建，重建直接接管现有队列。Monitor 仅在收到新快照或显示设置变化时重绘。`AudioSim.log` 中的 `UI timing` 记录原生 UI 初始化、添加节点和图重建耗时，便于继续定位响应问题。

## 仿真参数与声卡接入

Preferences > Audio 中只设置 Sample rate 和 Buffer size（算法帧长），不再使用全局 Channels per port。采样率允许输入 1000–192000 Hz，含 1500、2000、8000、16000 等；帧长允许 16–4096 之间的整数，含 16、24、32、64 等，支持非 2 的幂次。

**端口数量由各模块独立定义。** Source / Audio In 各有一个音频输出口；Audio Out / Monitor 各有一个音频输入口。算法 DLL 的固定输入输出数量取自 `PAX_Descriptor.audioInputs/audioOutputs`；支持 `PAX_getAudioOutputCount` 的 DLL 可在参数改变后更新动态输出数量，框架同步重建端口与有效连线。路由使用模块实际提供的端口，不增加 DLL 未声明的接口。

**每条音频连线可承载多个通道。** 文件源保留文件通道数；波形源在自己的设置中选择 Signal channels（默认 1，最多 64）；声卡输入按该模块选择的物理通道数确定；算法和 Monitor 跟随上游路由，受该算法自身通道能力限制。独立分支分别推导缓冲宽度，同一输入口接多个源时按通道索引混音，宽度取最大值，缺失通道补静音。布局与路由缓存只在消息线程准备，DSP 图交换后直接使用已准备缓冲。

- **44100 / 48000 Hz**：允许搜索声卡，Audio In / Out 列出实际设备、驱动、通道数及支持的回调长度。不支持当前采样率的设备不可选；后端也拒绝自动改用其他采样率。通道设置中选择物理通道，按升序映射到该模块的信号通道；首次默认选择实际可用通道，最多 64。
- **所有其他采样率**：只运行内部仿真，不搜索、不打开声卡；切换时停止设备回调并释放已打开的声卡。使用 Source 文件或波形发生器作为输入，保存的声卡选择会在回到 44.1/48 kHz 后重新尝试接入。

算法线程独立于声卡，以所选采样率、帧长处理。44.1/48 kHz 下使用同采样率 FIFO 衔接算法帧长和声卡支持的回调长度；16/24 样本帧不会强制声卡采用相同回调长度。算法线程按系统调度运行，小帧可能成批执行，当前不是硬实时或离线加速仿真器。

Source 支持最多 64 通道的音频文件，按原通道顺序输出；超过 64 的部分不输出。文件采样率不同于仿真采样率时，在内存中转换，过程不使用声卡。波形发生器在仿真采样率下生成信号，最高频率限制为采样率的 0.49 倍（界面最高 20 kHz）；方波与锯齿波是基础波形，未作带限抗混叠。文件整体载入内存，支持格式取决于 JUCE 编译启用的解码器。

Monitor 根据其输入连接的实际信号通道显示；它只有输入，不再透传。从要观察的算法输出口分一条线接到 Monitor，算法输出也可同时连接后续模块。可勾选任意通道，Single plot 同图叠加，Subplots 每通道一个子图；All 自动跟随全部输入通道。显示选择与布局随工程保存。捕获先按当前时间窗口取原始数据；不超过 512 个样本时逐样本显示，更多样本时显示每桶 min/max 峰值包络，避免稀疏抽点形成假低频，触发对齐也在原始数据上完成。Spectrumyser 的频率范围也随采样率调整，FFT 图当前观察第一输入通道。

旧工程中的 Monitor 透传链在加载、片段导入及撤销恢复时自动转换为观察分支，保留到原下游的直接音频连接；相同端点的重汇合路径会合并为单条连接，因此依赖重复路径叠加增益的旧工程需要检查其混音结果。

本轮卡顿修复已于 2026-10-22 编译，`PatchyTests` 与 `PaxDllSmoke` 自动化测试均通过，包含后台发现取消/缓存/生命周期、Monitor 64 通道及 FIFO 重建交接检查；`Run/AudioSim.exe` 与四个音频 DLL 已更新。程序实际启动成功，用户已确认拖拽验证通过。WebView2 调试端点未连接成功，因此未取得自动化端到端绘制耗时；原生日志记录的 UI ready 处理约 3–5 ms，首次添加节点处理约 0.17 ms，这些数值不包含 WebView 启动和前端绘制。声卡实际播放/录音未在本轮验证。引擎参数目前随应用设置保存，`.patchy` 保存拓扑与节点参数。

## 频域分析

从 Built-in > Audio 添加 **Frequency Analysis**，将需要观察的源或算法输出分支连接到其 Audio In。也可打开 `Run/Examples/Welch-Analysis.patchy`，点击源节点的播放按钮，观察 1 kHz 信号。

- **Window**：Hann、Hamming、Blackman、Blackman-Harris、Flat Top、Rectangular。
- **FFT length**：256、512、1024、2048、4096、8192、16384；频率间隔为采样率 / FFT 长度，与仿真帧长独立。
- **Welch averages**：2 / 4 / 8 / 16 段，50% 重叠；周期窗、保留直流，先平均线性功率，再转换为单边 PSD（dBFS/Hz，满刻度样本值为 1）。按采样率和窗能量归一化，DC 与 Nyquist 不加倍。PSD 峰值随窗和 FFT 长度变化，不等同于正弦幅度 dBFS。
- **Channel**：选择实际输入通道。计算直接读取原始浮点采样，不使用时域波形显示的抽样、包络或整数压缩数据。
- **Hold spectrum / Replace held**：保存当前显示谱作为黄色虚线参考，绿色实时谱继续更新；Clear held 清除参考。保持谱保留原采样率、FFT 网格、窗和平均段数，连同查看设置一起随工程保存，支持撤销。
- **From / To**：选择频率范围，上限自动限制在当前采样率的一半；可选择 Linear / Log，或使用 Full range、20 Hz–20 kHz 预设。对数坐标隐藏 DC。鼠标悬停读出频率及两条谱线的 PSD；齿轮设置可调整纵轴范围。

分析约每秒刷新 10 次，在原生消息线程执行，不在音频回调中计算 FFT。历史缓冲为 65536 个采样；启动时以及大 FFT 设置下，实际平均段数可能小于所选值，图下显示实际 / 请求段数。样本不足一个完整 FFT 时等待采集；采样率或输入通道数改变后重新积累历史。

## 构建

在此目录执行：

```powershell
pwsh -NoProfile -File .\tools\Build-AudioSim.ps1
```

脚本配置 CMake、打包 React UI、构建应用和四个音频 DLL、运行检查，并将运行文件更新到 `Run/`。更新前请保存工程并退出 AudioSim。日志位于 `build/windows-release/`，可用 `-ConfigureOnly` 仅配置、`-SkipDeploy` 编译验证但暂不替换运行文件，或 `-Clean` 清理本目录生成的构建产物并完整重编译。脚本自动探测 MSVC 的实际头文件提示前缀，保证中文环境下的 Ninja 头文件依赖跟踪。

本目录已复制所需第三方依赖和 npm 缓存，构建不依赖旁边的 `Patchy/`、`PatchyTrial/` 或旧 `dependencies/`。系统工具仍使用本机的 MSVC 14.51、CMake/Ninja、Node.js 和 PowerShell；更换开发机时请调整构建脚本中的工具路径。缺少依赖时可运行 `tools/Get-AudioSimDependencies.ps1`。

在 VS Code 中打开 `AudioSim.code-workspace`。Ctrl+Shift+B 构建，F5 构建后调试程序；需要 Microsoft C/C++ 扩展。

## 后续开发入口

| 路径 | 内容 |
| --- | --- |
| `Source/AudioSimConfig.h` | 程序标识、允许启用的内置节点编号 |
| `UI/src/BuiltinNodes.ts` | Built-in 音频节点目录 |
| `Source/ProcessingGraph.cpp` | 音频图调度与节点实例化 |
| `Source/GraphModel.*` | 拓扑、连接、参数、序列化与撤销 |
| `Source/WebBridge*`、`UI/src/Bridge.ts` | C++ / React 消息交互 |
| `Pax/PaxAPI.h` | 算法 DLL 的 C ABI 接口，保留 API v4 |
| `Pax/LevelPax/LevelPax.cpp` | 最小音频 DLL 与参数示例 |
| `Pax/PaxScanner.cpp`、`Pax/PaxRegistry.cpp` | 插件扫描、加载与实例管理 |
| `UI/src/Theme.ts`、`UI/src/index.css` | 主题逻辑与颜色 |

接入已有算法时，先做一层符合 Pax API v4 的适配 DLL，随后复制到 `Run/AudioSimPax/` 并重新启动。每个音频端口最多支持 64 通道。DLL 不需要更改 v4 结构体；新适配层可导出 `int PAX_getMaxAudioChannels(void)` 声明最大通道能力，处理时使用 `ctx->numChannels`，按 `audioIn[port * numChannels + channel]` / `audioOut[...]` 访问。没有该导出的旧 DLL 保留双声道布局，高于其能力的通道不会传入该 DLL。DLL 本身也需要支持所选采样率和帧长。

添加新的内置节点时，请同步更新 `AudioSimConfig.h`、`BuiltinNodes.ts`、端口定义和处理工厂；添加算法 DLL 通常只需扩展 `Pax/` 中的目标。

## 独立性与保留范围

本工程使用 AudioSim 名称、独立的应用设置与 WebView 数据、独立的插件目录。Windows 设置/日志位于 `%APPDATA%\AudioSim`，本地插件位于 `Run/AudioSimPax`，用户插件目录为 `%APPDATA%\AudioSim\Pax`，默认 Wrap 库位于文档目录的 `AudioSim/Wraps`。

新建入口和内置处理工厂只启用上表中的 Audio / Source 节点。上游 MIDI、网络、灯光等实现仍保留在源码中供重构参考；默认不构建这些额外 Pax 示例。需要构建它们时可启用 CMake 选项 `AUDIOSIM_BUILD_EXTRA_PAX=ON`。

为方便对照，上游的 `Patchy*` 类名和部分 CMake 内部目标名、`.patchy` 工程格式保留。工程保存拓扑和参数，连续 WAV/CSV 数据记录尚待扩展。

## 来源

上游项目：[eLeMenCy/Patchy](https://github.com/eLeMenCy/Patchy)，分支 dev。上游说明保存在 `UPSTREAM-README.md`，原许可证和版权声明保留在 `LICENSE`、`Pax/LICENSE` 及各源码中；依赖许可证随程序放入 `Run/Licenses/`。
