#include "ReturnEditor.h"

#include "AgentLauncher.h"
#include "LicensePrompt.h"

#include <BinaryData.h>

using namespace hs_assets;

using namespace hs;
using namespace hs::theme;

namespace {

// Same grid as the Capture editor, so the two windows read as one product.
constexpr int kWidth = 520;
constexpr int kHeight = 344;

constexpr int kMargin = 24;
constexpr int kLabelColumn = 100;
constexpr int kLabelGap = 12;
constexpr int kFieldX = kMargin + kLabelColumn + kLabelGap;
constexpr int kFieldWidth = kWidth - kMargin - kFieldX;
constexpr int kFieldHeight = 44;

constexpr int kHeaderTop = 26;
constexpr int kRuleY = 96;
constexpr int kRowY = 114;
constexpr int kMeterRuleY = 180;
constexpr int kMeterRowY = 204;
constexpr int kMeterRowHeight = 34;
constexpr int kMeterBarHeight = 10;
constexpr int kMonitorY = 252;
constexpr int kFooterY = 300;

} // namespace

ReturnEditor::ReturnEditor(ReturnProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&editLookAndFeel());
    setOpaque(true);
    setSize(kWidth, kHeight);

    logo = juce::ImageFileFormat::loadFrom(hs_assets::logo_png, hs_assets::logo_pngSize);

    title.setText(zh("绘声Studio"), juce::dontSendNotification);
    title.setFont(font(24.0f, 0.62f));
    title.setColour(juce::Label::textColourId, textPrimary);
    addAndMakeVisible(title);

    subtitle.setText("Return", juce::dontSendNotification);
    subtitle.setFont(font(12.5f));
    subtitle.setColour(juce::Label::textColourId, textTertiary);
    addAndMakeVisible(subtitle);

    addAndMakeVisible(agentDot);

    agentLabel.setFont(font(13.5f));
    agentLabel.setColour(juce::Label::textColourId, textSecondary);
    agentLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(agentLabel);

    caption.setText(zh("回传通道"), juce::dontSendNotification);
    caption.setFont(font(20.0f));
    caption.setColour(juce::Label::textColourId, textSecondary);
    addAndMakeVisible(caption);

    pairBox.setLookAndFeel(&editLookAndFeel());
    pairBox.onChange = [this]
    {
        const int id = pairBox.getSelectedId();

        if (id >= 1 && id <= kDevicePairs)
            processor.selectPair(id - 1);

        updateStatus();
    };

    // Named after the half the host plays into; the caption below tells the
    // user which half OBS has to pick.
    for (int i = 0; i < kDevicePairs; ++i)
        pairBox.addItem(juce::String::fromUTF8(kPairOutputName[i]), i + 1);

    addAndMakeVisible(pairBox);

    addAndMakeVisible(meter);

    meterCaption.setText(zh("LEVEL"), juce::dontSendNotification);
    meterCaption.setFont(font(13.5f));
    meterCaption.setColour(juce::Label::textColourId, textTertiary);
    meterCaption.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(meterCaption);

    monitor.setFont(font(15.0f));
    monitor.setColour(juce::Label::textColourId, textSecondary);
    monitor.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(monitor);

    footer.setFont(font(13.5f));
    footer.setColour(juce::Label::textColourId, textTertiary);
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

    pairBox.setSelectedId(processor.selectedPair() + 1, juce::dontSendNotification);
    updateStatus();
    startTimer(120);
}

ReturnEditor::~ReturnEditor()
{
    stopTimer();
    pairBox.setLookAndFeel(nullptr);
    registerButton.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void ReturnEditor::paint(juce::Graphics& g)
{
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

void ReturnEditor::resized()
{
    // ---- header -------------------------------------------------------------
    const int textX = kMargin + 40 + 14;

    title.setBounds(textX, kHeaderTop, 300, 30);
    subtitle.setBounds(textX, kHeaderTop + 28, 300, 16);

    agentLabel.setBounds(kWidth - kMargin - 158, kHeaderTop + 14, 138, 20);
    agentDot.setBounds(kWidth - kMargin - 14, kHeaderTop + 17, 12, 12);

    // ---- return pair --------------------------------------------------------
    caption.setBounds(kMargin, kRowY, kLabelColumn, kFieldHeight);
    pairBox.setBounds(kFieldX, kRowY, kFieldWidth, kFieldHeight);

    // ---- level --------------------------------------------------------------
    meterCaption.setBounds(kMargin, kMeterRowY, kLabelColumn, kMeterRowHeight);
    meter.setBounds(kFieldX, kMeterRowY + (kMeterRowHeight - kMeterBarHeight) / 2,
                    kFieldWidth, kMeterBarHeight);

    monitor.setBounds(kMargin, kMonitorY, kFieldWidth + kLabelColumn + kLabelGap, 22);

    constexpr int registerWidth = 190;
    footer.setBounds(kMargin, kFooterY, kFieldWidth + kLabelColumn + kLabelGap - registerWidth - 12, 22);
    registerButton.setBounds(kWidth - kMargin - registerWidth, kFooterY - 6, registerWidth, 30);
}

void ReturnEditor::updateStatus()
{
    const bool alive = processor.agentAlive();
    const auto state = processor.returnState();
    const int pair = processor.selectedPair();
    // 没注册就不回传：界面得说明白，不然看起来像插件坏了。
    const bool unlicensed = alive && processor.returnFault() == Fault::Unlicensed;

    agentLabel.setText(! alive ? zh("后台未运行")
                               : (unlicensed ? zh("未注册")
                                             : (state == State::Running
                                                    ? zh("运行中")
                                                    : (state == State::Error ? zh("不可用")
                                                                             : zh("连接中")))),
                       juce::dontSendNotification);

    agentDot.setState(alive, unlicensed ? warning
                                        : (state == State::Running
                                               ? positive
                                               : (state == State::Error ? danger : warning)));

    meter.setLevel(processor.outputPeak());

    // OBS must record the *input* half of the same pair.
    monitor.setText(zh("OBS 麦克风选 ") + juce::String::fromUTF8(kPairInputName[pair]),
                    juce::dontSendNotification);

    if (unlicensed)
        footer.setText(zh("在右下角填入注册码后即可回传"), juce::dontSendNotification);
    else if (state == State::Error)
        footer.setText(zh("采集通道与回传通道不能是同一对"), juce::dontSendNotification);
    else if (! alive)
        footer.setText(zh("插件会自动启动后台服务"), juce::dontSendNotification);
    else
        footer.setText(juce::String(processor.hostRate()) + " Hz", juce::dontSendNotification);

    pairBox.setSelectedId(pair + 1, juce::dontSendNotification);

    registerButton.setButtonText(hs::ui::licenseStatusText(licence));
    registerButton.setToggleState(licence.valid, juce::dontSendNotification);
}

void ReturnEditor::timerCallback()
{
    updateStatus();
}
