#pragma once

#include <JuceHeader.h>

#include "License.h"
#include "Theme.h"

#include <functional>

// 注册入口：两个编辑器共用一个按钮文案 + 一个粘贴框，样子跟其他控件一致。
namespace hs::ui {

inline juce::String licenseStatusText(const license::Result& result)
{
    if (result.valid)
        return theme::zh("已注册 · ") + juce::String::fromUTF8(result.serial.c_str());

    return theme::zh("未注册 · 输入注册码");
}

// 注册码一次性粘贴，不绑机器、不联网。失败时只提示，不改动已有状态。
inline void showLicensePrompt(juce::Component* parent, std::function<void()> onFinished = {})
{
    auto* window = new juce::AlertWindow(
        theme::zh("注册"),
        theme::zh("把注册码粘贴进来。一个注册码对应一个名字，不绑机器、不用联网。"),
        juce::MessageBoxIconType::NoIcon);

    window->addTextEditor("code", juce::String::fromUTF8(license::storedCode().c_str()),
                          theme::zh("注册码"));
    window->addButton(theme::zh("确定"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton(theme::zh("取消"), 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<juce::Component> safeParent(parent);

    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [window, safeParent, onFinished](int result) mutable
            {
                if (result == 1)
                {
                    const auto entered = window->getTextEditorContents("code").trim();
                    const auto verified = license::activate(entered.toStdString());

                    if (! verified.valid)
                    {
                        juce::AlertWindow::showAsync(
                            juce::MessageBoxOptions()
                                .withIconType(juce::MessageBoxIconType::WarningIcon)
                                .withTitle(theme::zh("注册码无效"))
                                .withMessage(theme::zh("这个码不是本产品生成的，或者抄错了一个字。"))
                                .withButton(theme::zh("好")),
                            nullptr);
                    }

                    if (safeParent != nullptr && onFinished != nullptr)
                        onFinished();
                }
            }),
        true);
}

} // namespace hs::ui
