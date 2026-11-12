#include "CaptureEditor.h"

#include "SystemCompat.h"

#include "AgentLauncher.h"
#include "LicensePrompt.h"

#include <BinaryData.h>

using namespace hs_assets;

#include <cmath>

using namespace hs;
using namespace hs::theme;

namespace {

juce::Image toImage(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
        return {};

    return juce::ImageFileFormat::loadFrom(bytes.data(), bytes.size());
}

// One column grid, no nested boxes: every control starts at kFieldX and ends at
// kWidth - kMargin, and the meters use the same two edges, so nothing can drift
// out of line when a row is added.
constexpr int kWidth = 520;
constexpr int kHeight = 420;

constexpr int kMargin = 24;
constexpr int kLabelColumn = 100;
constexpr int kLabelGap = 12;
constexpr int kFieldX = kMargin + kLabelColumn + kLabelGap;
constexpr int kFieldWidth = kWidth - kMargin - kFieldX;
constexpr int kFieldHeight = 44;

constexpr int kHeaderTop = 26;
constexpr int kRuleY = 96;
constexpr int kAppRowY = 114;
constexpr int kPairRowY = kAppRowY + kFieldHeight + 12;
constexpr int kMeterRuleY = kPairRowY + kFieldHeight + 22;
constexpr int kMeterY = kMeterRuleY + 24;
constexpr int kMeterRowHeight = 34;
constexpr int kMeterBarHeight = 10;
constexpr int kFooterY = kMeterY + kMeterRowHeight * 3 + 18;

} // namespace

CaptureEditor::CaptureEditor(CaptureProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&editLookAndFeel());

    setOpaque(true);
    setSize(kWidth, kHeight);

    // ---- header -------------------------------------------------------------
    logo = juce::ImageFileFormat::loadFrom(hs_assets::logo_png, hs_assets::logo_pngSize);

    title.setText(zh("绘声Studio"), juce::dontSendNotification);
    title.setFont(font(24.0f, 0.62f));
    title.setColour(juce::Label::textColourId, textPrimary);
    addAndMakeVisible(title);

    subtitle.setText("Capture", juce::dontSendNotification);
    subtitle.setFont(font(12.5f));
    subtitle.setColour(juce::Label::textColourId, textTertiary);
    addAndMakeVisible(subtitle);

    addAndMakeVisible(agentDot);

    agentLabel.setFont(font(13.5f));
    agentLabel.setColour(juce::Label::textColourId, textSecondary);
    agentLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(agentLabel);

    // ---- app picker ---------------------------------------------------------
    appCaption.setText(zh("应用"), juce::dontSendNotification);
    appCaption.setFont(font(20.0f));
    appCaption.setColour(juce::Label::textColourId, textSecondary);
    addAndMakeVisible(appCaption);

    appButton.setLookAndFeel(&buttonLookAndFeel());
    appButton.onClick = [this] { showAppMenu(); };

    // Per-App capture is a process tap, which only exists from macOS 14.2. On
    // older systems the picker stays visible but inert, and says why.
    if (! hs::systemSupportsAppCapture())
    {
        appButton.setEnabled(false);
        appButton.setButtonText(zh("需要 macOS 14.2+"));
        appButton.setTooltip(zh("应用采集需要 macOS 14.2 或更高，当前系统是 ")
                             + juce::String(hs::systemVersionText()) + zh("。可以改用虚拟通道。"));
    }
    else
    {
        appButton.setButtonText(zh("选择软件"));
        appButton.setTooltip(zh("点这里勾选要采集的软件，可以同时选多个（最多 ")
                             + juce::String(hs::kSlotApps)
                             + zh(" 个），它们会一起混进这条轨道。"));
    }

    addAndMakeVisible(appButton);

    refreshButton.setLookAndFeel(&buttonLookAndFeel());
    refreshButton.setButtonText(zh("刷新"));
    refreshButton.setTooltip(zh("重新扫描应用列表"));
    refreshButton.onClick = [this]
    {
        refreshApps();
        processor.reconnect();
        updateStatus();
    };
    addAndMakeVisible(refreshButton);

    // ---- pair picker --------------------------------------------------------
    pairCaption.setText(zh("虚拟通道"), juce::dontSendNotification);
    pairCaption.setFont(font(20.0f));
    pairCaption.setColour(juce::Label::textColourId, textSecondary);
    addAndMakeVisible(pairCaption);

    pairBox.setLookAndFeel(&editLookAndFeel());
    pairBox.onChange = [this]
    {
        const int id = pairBox.getSelectedId();

        if (id == 1)
        {
            processor.setUsePair(false);
        }
        else if (id >= 2)
        {
            processor.selectPair(id - 2);
        }

        updateStatus();
    };
    addAndMakeVisible(pairBox);

    // The pair is named after the half an outside app plays into, which is the
    // device the user has to look for in that app's output list.
    for (int i = 0; i < kDevicePairs; ++i)
        pairBox.addItem(juce::String::fromUTF8(kPairOutputName[i]), i + 2);

    pairBox.addItem(zh("不使用"), 1);

    // ---- meters -------------------------------------------------------------
    auto setupMeter = [this](LevelMeter& meter, juce::Label& label, const char* text)
    {
        addAndMakeVisible(meter);
        label.setText(zh(text), juce::dontSendNotification);
        label.setFont(font(13.5f));
        label.setColour(juce::Label::textColourId, textTertiary);
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
    };

    setupMeter(appMeter, appMeterLabel, "应用");
    setupMeter(pairMeter, pairMeterLabel, "通道");
    setupMeter(outMeter, outMeterLabel, "输出");

    // ---- footer -------------------------------------------------------------
    footer.setFont(font(13.5f));
    footer.setColour(juce::Label::textColourId, textSecondary);
    footer.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(footer);

    // ---- registration -------------------------------------------------------
    registerButton.setLookAndFeel(&buttonLookAndFeel());
    registerButton.setTooltip(zh("一个注册码对应一个名字，不绑机器"));
    registerButton.onClick = [this]
    {
        hs::ui::showLicensePrompt(this, [this]
        {
            licence = hs::license::current();
            updateStatus();
        });
    };
    addAndMakeVisible(registerButton);

    licence = hs::license::current();

    refreshApps();
    syncFromProcessor();
    updateStatus();

    startTimer(120);
}

CaptureEditor::~CaptureEditor()
{
    stopTimer();

    appButton.setLookAndFeel(nullptr);
    pairBox.setLookAndFeel(nullptr);
    refreshButton.setLookAndFeel(nullptr);
    registerButton.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

//==================================================================================================
#pragma mark Layout
//==================================================================================================

void CaptureEditor::paint(juce::Graphics& g)
{
    // One flat surface with a barely-there vertical lift; the grouping is done
    // with spacing and two hairlines, not with boxes inside boxes.
    g.setGradientFill(juce::ColourGradient(background.brighter(0.045f), 0.0f, 0.0f,
                                           background, 0.0f, static_cast<float>(getHeight()),
                                           false));
    g.fillRect(getLocalBounds());

    if (logo.isValid())
        g.drawImage(logo, juce::Rectangle<float>(static_cast<float>(kMargin),
                                                 static_cast<float>(kHeaderTop + 2), 40.0f, 40.0f),
                    juce::RectanglePlacement::centred, false);

    paintHairline(g, kMargin, kWidth - kMargin, kRuleY);
    paintHairline(g, kMargin, kWidth - kMargin, kMeterRuleY);
}

void CaptureEditor::resized()
{
    // ---- header -------------------------------------------------------------
    const int textX = kMargin + 40 + 14;

    title.setBounds(textX, kHeaderTop, 300, 30);
    subtitle.setBounds(textX, kHeaderTop + 28, 300, 16);

    agentLabel.setBounds(kWidth - kMargin - 158, kHeaderTop + 14, 138, 20);
    agentDot.setBounds(kWidth - kMargin - 14, kHeaderTop + 17, 12, 12);

    // ---- source rows --------------------------------------------------------
    appCaption.setBounds(kMargin, kAppRowY, kLabelColumn, kFieldHeight);

    constexpr int refreshWidth = 56;
    refreshButton.setBounds(kFieldX + kFieldWidth - refreshWidth, kAppRowY,
                            refreshWidth, kFieldHeight);
    appButton.setBounds(kFieldX, kAppRowY, kFieldWidth - refreshWidth - 10, kFieldHeight);

    pairCaption.setBounds(kMargin, kPairRowY, kLabelColumn, kFieldHeight);
    pairBox.setBounds(kFieldX, kPairRowY, kFieldWidth, kFieldHeight);

    // ---- meters -------------------------------------------------------------
    // Label column and bar share the field edges exactly, so the three bars are
    // identical in x and width by construction.
    auto layoutMeter = [](LevelMeter& meter, juce::Label& label, int y)
    {
        label.setBounds(kMargin, y, kLabelColumn, kMeterRowHeight);
        meter.setBounds(kFieldX, y + (kMeterRowHeight - kMeterBarHeight) / 2,
                        kFieldWidth, kMeterBarHeight);
    };

    layoutMeter(appMeter, appMeterLabel, kMeterY);
    layoutMeter(pairMeter, pairMeterLabel, kMeterY + kMeterRowHeight);
    layoutMeter(outMeter, outMeterLabel, kMeterY + kMeterRowHeight * 2);

    // ---- footer -------------------------------------------------------------
    // The registration state shares the footer row: one line of detail on the
    // left, one quiet button on the right.
    constexpr int registerWidth = 190;
    footer.setBounds(kMargin, kFooterY, kWidth - kMargin * 2 - registerWidth - 12, 24);
    registerButton.setBounds(kWidth - kMargin - registerWidth, kFooterY - 5, registerWidth, 30);
}

//==================================================================================================
#pragma mark Data
//==================================================================================================

void CaptureEditor::refreshApps()
{
    // 不取图标：listAudioProcesses(true) 会给每个能抓的进程读一次应用图标、
    // 再编成 PNG 带过来（hub/ProcessList.mm），十几二十个进程就是几百毫秒到
    // 一秒的停顿。它跑在宿主的界面线程上（编辑器构造 + 点"刷新"），表现出来
    // 就是"点插件卡一下、窗口半天不出来、打开一片空白"。列表里只显示名字。
    processes = listAudioProcesses(false);
    images.clear();

    syncFromProcessor();
}

void CaptureEditor::syncFromProcessor()
{
    // The button names what is being captured: one App, a count, or 不使用.
    const int count = processor.appCount();

    if (count <= 0)
    {
        appButton.setButtonText(zh("不使用"));
    }
    else if (count == 1)
    {
        appButton.setButtonText(processor.appName(0));
    }
    else
    {
        int waiting = 0;

        for (int i = 0; i < count; ++i)
        {
            if (processor.appState(i) != State::Running)
                ++waiting;
        }

        juce::String text = processor.appName(0) + zh(" 等 ") + juce::String(count) + zh(" 个");
        text += waiting > 0 ? zh("（") + juce::String(waiting) + zh(" 个未连接）") : zh("软件");

        appButton.setButtonText(text);
    }

    const int pair = processor.selectedPair();
    pairBox.setSelectedId(processor.pairSelected() ? pair + 2 : 1, juce::dontSendNotification);
}

// The App picker is a check list: one Capture instance can take several Apps and
// sum them into the same track, so ticking a second App adds it instead of
// replacing the first one.
void CaptureEditor::showAppMenu()
{
    if (! hs::systemSupportsAppCapture())
        return;

    juce::PopupMenu menu;
    menu.setLookAndFeel(&editLookAndFeel());
    menu.addItem(1, zh("清空"), processor.appCount() > 0);

    if (! processes.empty())
        menu.addSeparator();

    for (size_t i = 0; i < processes.size(); ++i)
    {
        const auto& info = processes[i];
        auto name = juce::String::fromUTF8(info.name.c_str());

        if (! info.audioRunning)
            name = juce::String::fromUTF8("○ ") + name;

        bool ticked = false;

        for (int a = 0; a < processor.appCount(); ++a)
        {
            if (processor.appPID(a) == info.pid)
            {
                ticked = true;
                break;
            }
        }

        menu.addItem(static_cast<int>(i) + 2, name, true, ticked);
    }

    // A project may remember an App that is not running right now: keep it in the
    // list, ticked, so it can be dropped instead of silently disappearing.
    for (int a = 0; a < processor.appCount(); ++a)
    {
        bool running = false;

        for (const auto& info : processes)
        {
            if (info.pid == processor.appPID(a))
            {
                running = true;
                break;
            }
        }

        if (! running && processor.appName(a).isNotEmpty())
            menu.addItem(1000 + a, juce::String::fromUTF8("○ ") + processor.appName(a), true, true);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&appButton)
                                                  .withMinimumWidth(240),
                       [this](int result)
    {
        if (result == 0)
            return;

        if (result == 1)
        {
            processor.clearApps();
            updateStatus();
            return;
        }

        if (result >= 1000)
        {
            processor.removeApp(result - 1000);
            updateStatus();
            return;
        }

        const int index = result - 2;

        if (index < 0 || index >= static_cast<int>(processes.size()))
            return;

        const auto& info = processes[static_cast<size_t>(index)];
        int existing = -1;

        for (int a = 0; a < processor.appCount(); ++a)
        {
            if (processor.appPID(a) == info.pid)
                existing = a;
        }

        if (existing >= 0)
            processor.removeApp(existing);
        else
            processor.addApp(info.pid, juce::String::fromUTF8(info.bundleID.c_str()),
                             juce::String::fromUTF8(info.name.c_str()));

        updateStatus();
    });
}

void CaptureEditor::updateStatus()
{
    const bool alive = processor.agentAlive();
    const auto state = processor.agentState();

    // Both sources have their own grant: the App tap needs 系统录音, the
    // virtual pair needs 麦克风. Only complain about the one in use, and only
    // when the OS says it was refused (0), not while it is undecided (1).
    const int tapPermission = processor.audioCapturePermissionRaw();
    const int micPermission = processor.microphonePermissionRaw();
    const bool pairWanted = processor.pairSelected();

    const bool needsTapPermission = processor.appSelected() && tapPermission == 0;
    const bool needsMicPermission = pairWanted && micPermission == 0;
    const bool needsPermission = alive && (needsTapPermission || needsMicPermission);

    juce::String stateText;
    juce::Colour dotColour = textTertiary;
    bool dotOn = false;

    // 没有注册码时 Agent 会拒掉采集，并把原因写在 fault 里。
    const bool unlicensed = alive
                            && (processor.tapFault() == Fault::Unlicensed
                                || processor.channelFault() == Fault::Unlicensed);

    if (! alive)
    {
        stateText = zh("后台未运行");
    }
    else if (unlicensed)
    {
        // 必须说明白，否则用户以为软件坏了。
        stateText = zh("未注册");
        dotColour = warning;
        dotOn = true;
    }
    else if (needsPermission)
    {
        stateText = zh("需要系统权限");
        dotColour = danger;
        dotOn = true;
    }
    else if (state == State::Running)
    {
        stateText = zh("运行中");
        dotColour = positive;
        dotOn = true;
    }
    else if (state == State::Starting)
    {
        stateText = zh("连接中");
        dotColour = warning;
        dotOn = true;
    }
    else if (state == State::Error)
    {
        stateText = zh("无法采集");
        dotColour = danger;
        dotOn = true;
    }
    else
    {
        stateText = zh("待命");
        dotColour = textTertiary;
    }

    agentLabel.setText(stateText, juce::dontSendNotification);
    agentDot.setState(dotOn, dotColour);

    // Footer is a single line that changes with the situation, never a log.
    juce::String detail;

    if (! hs::systemSupportsAppCapture() && ! processor.pairSelected())
    {
        detail = zh("当前 macOS ") + juce::String(hs::systemVersionText())
                 + zh(" 不支持应用采集，请改用虚拟通道");
    }
    else if (processor.agentVersionStale())
    {
        detail = zh("检测到旧版后台服务，正在自动重启…");
    }
    else if (! alive)
    {
        detail = processor.agentLaunchStalled()
                     ? zh("后台服务没起来：到「应用程序」里双击打开「绘声Studio」")
                     : zh("插件会自动启动后台服务，请稍候");
    }
    else if (unlicensed)
    {
        detail = zh("在右下角填入注册码后即可采集");
    }
    else if (needsPermission)
    {
        detail = needsMicPermission
                     ? zh("系统设置 → 隐私与安全性 → 麦克风 → 绘声Studio")
                     : zh("系统设置 → 隐私与安全性 → 录屏与系统录音 → 绘声Studio");
    }
    else if (! processor.slotClaimed())
    {
        detail = zh("同时最多 ") + juce::String(hs::kCaptureSlots)
                 + zh(" 路，请先关掉不用的插件窗口");
    }
    else if (processor.appSelected()
             && processor.tapState() == State::Waiting
             && processor.tapFault() == Fault::ProcessGone)
    {
        detail = zh("应用未在播放，重新打开后会自动连接");
    }
    else if (processor.appSelected()
             && processor.channelRate() > 0
             && processor.hostRate() > 0
             && processor.channelRate() != processor.hostRate())
    {
        detail = zh("虚拟通道 ") + juce::String(processor.channelRate()) + zh(" Hz，工程 ")
                 + juce::String(processor.hostRate()) + zh(" Hz");
    }
    else
    {
        const int ppm = processor.driftPPM();
        const int appCount = processor.appCount();
        const uint32_t appRate = appCount > 0 ? processor.appRate(0) : 0u;

        // Each App keeps running at its own device rate and the bridge resamples
        // it, so show both rates: the two numbers differ in normal operation.
        if (appCount > 1)
            detail = juce::String(appCount) + zh(" 个软件一起送进这条轨道   ·   ")
                     + juce::String(processor.hostRate()) + " Hz";
        else if (appCount == 1 && appRate > 0 && appRate != processor.hostRate())
            detail = zh("应用 ") + juce::String(appRate) + zh(" Hz，工程 ")
                     + juce::String(processor.hostRate()) + zh(" Hz");
        else
            detail = juce::String(processor.hostRate()) + " Hz";

        if (processor.activeSlots() > 1)
            detail = zh("第 ") + juce::String(processor.slotIndex() + 1) + zh(" 路   ·   ") + detail;

        if (ppm != 0)
            detail += zh("   ·   漂移补偿 ") + (ppm > 0 ? "+" : "") + juce::String(ppm) + " ppm";

        if (processor.underruns() > 0)
            detail += zh("   ·   缓冲 ") + juce::String(static_cast<int>(processor.underruns()));
    }

    footer.setText(detail, juce::dontSendNotification);

    registerButton.setButtonText(hs::ui::licenseStatusText(licence));
    registerButton.setToggleState(licence.valid, juce::dontSendNotification);
}

void CaptureEditor::timerCallback()
{
    if (++statusTicks >= 8)
    {
        statusTicks = 0;
        refreshApps();
    }

    appMeter.setLevel(processor.appSelected() ? processor.tapPeak() : 0.0f);
    pairMeter.setLevel(processor.pairSelected() ? processor.channelPeak() : 0.0f);
    outMeter.setLevel(processor.outputPeak());

    updateStatus();
}
