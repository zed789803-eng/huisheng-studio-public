// 绘声Studio 注册码生成器（macOS 窗口版）。
//
// 这个程序是给你（发行方）自己用的：把买家的名字变成一串注册码。
// 它带着产品密钥——谁拿到它，谁就能出码，所以不要跟安装包一起发出去。
//
// 用法：
//   双击打开 → 输入买家的名字 → 生成 → 复制，把码发给买家。
//   --render <png> [名字]  离屏渲染一张界面图（CI 里用来核对界面）
//   --selftest            自检
#include <JuceHeader.h>
#include <BinaryData.h>

#include "License.h"
#include "Theme.h"

#include <cstdio>

namespace {

constexpr int kWidth = 620;
constexpr int kHeight = 448;

// -----------------------------------------------------------------------------
// Offscreen helpers: also used by CI to prove the window actually renders.
// -----------------------------------------------------------------------------
void dumpTree(juce::Component& c, int depth = 0)
{
    juce::String pad;
    for (int i = 0; i < depth; ++i)
        pad << "  ";

    std::printf("%s%s %s\n", pad.toRawUTF8(),
                typeid(c).name(), c.getBounds().toString().toRawUTF8());

    for (auto* child : c.getChildren())
        dumpTree(*child, depth + 1);
}

void render(juce::Component& component, const juce::File& file, float scale)
{
    juce::Image image(juce::Image::ARGB,
                      juce::roundToInt(component.getWidth() * scale),
                      juce::roundToInt(component.getHeight() * scale), true);

    {
        juce::Graphics g(image);
        g.addTransform(juce::AffineTransform::scale(scale));
        component.paintEntireComponent(g, true);
    }

    file.deleteFile();
    juce::FileOutputStream stream(file);

    if (stream.openedOk())
        juce::PNGImageFormat().writeImageToStream(image, stream);
}

// -----------------------------------------------------------------------------
class KeygenComponent final : public juce::Component
{
public:
    KeygenComponent()
    {
        logo_ = juce::ImageFileFormat::loadFrom(hs_assets::logo_png, hs_assets::logo_pngSize);

        name_.setFont(hs::theme::font(16.0f));
        name_.setTextToShowWhenEmpty(hs::theme::zh("比如：张三"), hs::theme::textTertiary);
        name_.setColour(juce::TextEditor::backgroundColourId, hs::theme::surfaceHigh);
        name_.setColour(juce::TextEditor::outlineColourId, hs::theme::stroke);
        name_.setColour(juce::TextEditor::focusedOutlineColourId, hs::theme::accent);
        name_.setColour(juce::TextEditor::textColourId, hs::theme::textPrimary);
        name_.setColour(juce::TextEditor::highlightColourId, hs::theme::accentDim);
        name_.setColour(juce::TextEditor::highlightedTextColourId, hs::theme::textPrimary);
        name_.onReturnKey = [this] { generate(); };
        name_.onTextChange = [this] { code_.clear(); };
        addAndMakeVisible(name_);

        makeButton_.setButtonText(hs::theme::zh("生成注册码"));
        makeButton_.setColour(juce::TextButton::buttonColourId, hs::theme::accent);
        makeButton_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xdd000000));
        makeButton_.onClick = [this] { generate(); };
        addAndMakeVisible(makeButton_);

        code_.setReadOnly(true);
        code_.setFont(hs::theme::mono(17.0f));
        code_.setJustification(juce::Justification::centred);
        code_.setColour(juce::TextEditor::backgroundColourId, hs::theme::surfaceHigh);
        code_.setColour(juce::TextEditor::outlineColourId, hs::theme::stroke);
        code_.setColour(juce::TextEditor::textColourId, hs::theme::accent.brighter(0.25f));
        addAndMakeVisible(code_);

        copyButton_.setButtonText(hs::theme::zh("复制"));
        copyButton_.setColour(juce::TextButton::buttonColourId, hs::theme::surfaceHigh);
        copyButton_.setColour(juce::TextButton::textColourOffId, hs::theme::textPrimary);
        copyButton_.onClick = [this]
        {
            if (code_.getText().isNotEmpty())
                juce::SystemClipboard::copyTextToClipboard(code_.getText());
        };
        addAndMakeVisible(copyButton_);

        status_.setFont(hs::theme::font(12.0f));
        status_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status_);

        setStatus(hs::theme::zh("输入买家的名字，生成一串注册码发给他。"), hs::theme::textTertiary);
        setSize(kWidth, kHeight);
    }

    // Used by the CI screenshot: pretend someone typed a name.
    void prefill(const juce::String& name)
    {
        name_.setText(name, juce::dontSendNotification);
        generate();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(hs::theme::background);

        if (logo_.isValid())
            g.drawImageWithin(logo_, 32, 28, 46, 46, juce::RectanglePlacement::centred);

        g.setColour(hs::theme::textPrimary);
        g.setFont(hs::theme::font(22.0f, 0.7f));
        g.drawText(hs::theme::zh("绘声Studio"), 92, 26, 320, 28, juce::Justification::centredLeft);

        g.setColour(hs::theme::textSecondary);
        g.setFont(hs::theme::font(13.0f));
        g.drawText(hs::theme::zh("注册码生成器"), 92, 52, 320, 20, juce::Justification::centredLeft);

        auto body = getLocalBounds().reduced(32, 0).withTrimmedTop(104).withTrimmedBottom(96);
        hs::theme::paintCard(g, body.toFloat());

        auto footer = getLocalBounds().removeFromBottom(88).reduced(32, 0).withTrimmedTop(12);
        g.setColour(hs::theme::textTertiary);
        g.setFont(hs::theme::font(12.0f));
        g.drawText(hs::theme::zh("一个码对应一个名字，不绑机器、不联网、不会过期。"),
                    footer, juce::Justification::topLeft, true);
    }

    void resized() override
    {
        auto body = getLocalBounds().reduced(32, 0).withTrimmedTop(104).withTrimmedBottom(96);
        auto area = body.reduced(20, 18);

        auto labelRow = area.removeFromTop(22);
        auto nameRow = area.removeFromTop(hs::theme::kControlHeight);
        area.removeFromTop(18);
        auto buttonRow = area.removeFromTop(hs::theme::kControlHeight);
        area.removeFromTop(20);
        auto codeRow = area.removeFromTop(hs::theme::kControlHeight + 6);
        area.removeFromTop(14);
        auto statusRow = area.removeFromTop(20);

        juce::ignoreUnused(labelRow);

        name_.setBounds(nameRow);
        makeButton_.setBounds(buttonRow.withWidth(150));
        code_.setBounds(codeRow.withTrimmedRight(12 + 92));
        copyButton_.setBounds(codeRow.removeFromRight(92));
        status_.setBounds(statusRow);
    }

    void setStatus(const juce::String& text, juce::Colour colour)
    {
        status_.setText(text, juce::dontSendNotification);
        status_.setColour(juce::Label::textColourId, colour);
    }

private:
    void generate()
    {
        const auto name = name_.getText().trim();

        if (name.isEmpty())
        {
            setStatus(hs::theme::zh("先写一个名字。"), hs::theme::warning);
            return;
        }

        const auto code = hs::license::make(name.toRawUTF8());

        if (code.empty())
        {
            setStatus(hs::theme::zh("名字太长了：最多 24 个字节（差不多 8 个汉字）。"), hs::theme::danger);
            return;
        }

        code_.setText(code, juce::dontSendNotification);

        const auto check = hs::license::verify(code);

        if (! check.valid || check.serial != name.toStdString())
        {
            setStatus(hs::theme::zh("自检没通过，请把这串码和名字发回给开发者。"), hs::theme::danger);
            return;
        }

        setStatus(hs::theme::zh("装好之后，插件右下角会显示：已注册 · ") + name, hs::theme::positive);
    }

    juce::Image logo_;
    juce::TextEditor name_;
    juce::TextEditor code_;
    juce::TextButton makeButton_;
    juce::TextButton copyButton_;
    juce::Label status_;
};

// -----------------------------------------------------------------------------
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : juce::DocumentWindow(hs::theme::zh("绘声Studio 注册码生成器"),
                               hs::theme::background,
                               juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new KeygenComponent(), true);
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class KeygenApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "HuishengStudio Keygen"; }
    const juce::String getApplicationVersion() override { return "1.0.12"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);

        if (args.contains("--selftest"))
        {
            selfTest();
            return;
        }

        const int renderIndex = args.indexOf("--render");

        if (renderIndex >= 0 && renderIndex + 1 < args.size())
        {
            KeygenComponent component;

            if (renderIndex + 2 < args.size())
                component.prefill(args[renderIndex + 2]);

            dumpTree(component);
            render(component, juce::File::getCurrentWorkingDirectory()
                                  .getChildFile(args[renderIndex + 1]), 2.0f);
            quit();
            return;
        }

        window_.reset(new MainWindow());

        if (! args.isEmpty() && ! args[0].startsWith("--"))
            setStatusFor(args[0]);
    }

    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    void setStatusFor(const juce::String& name)
    {
        if (auto* content = dynamic_cast<KeygenComponent*>(window_ != nullptr ? window_->getContentComponent() : nullptr))
            content->prefill(name);
    }

    void selfTest()
    {
        const auto code = hs::license::make("abc");

        if (code != "HS1-041P-2RK3-RVNK-G818-70")
        {
            std::printf("FAIL: make(\"abc\") = %s\n", code.c_str());
            setApplicationReturnValue(1);
        }
        else
        {
            std::printf("keygen app: PASS (%s)\n", code.c_str());
        }

        quit();
    }

    std::unique_ptr<MainWindow> window_;
};

} // namespace

START_JUCE_APPLICATION(KeygenApplication)
