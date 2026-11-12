#pragma once

#include <JuceHeader.h>

#include "Theme.h"

#include "Config.h"

namespace hs::theme {

// Combo box that matches the editors: one line, accent underline when open.
class ComboBoxLook final : public juce::LookAndFeel_V4,
                           public juce::ComboBox::Listener
{
public:
    ComboBoxLook()
    {
        setColour(juce::ComboBox::backgroundColourId, surfaceHigh);
        setColour(juce::ComboBox::outlineColourId, stroke);
        setColour(juce::ComboBox::textColourId, textPrimary);
        setColour(juce::ComboBox::arrowColourId, textTertiary);
        setColour(juce::ComboBox::focusedOutlineColourId, accent);
        setColour(juce::PopupMenu::backgroundColourId, surfaceHigh);
        setColour(juce::PopupMenu::textColourId, textPrimary);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.18f));
        setColour(juce::PopupMenu::highlightedTextColourId, textPrimary);
    }

    void comboBoxChanged(juce::ComboBox*) override {}

    void drawComboBox(juce::Graphics& g, int width, int height, bool,
                      int, int, int, int, juce::ComboBox& box) override
    {
        const juce::Rectangle<float> bounds(0.0f, 0.0f, static_cast<float>(width),
                                            static_cast<float>(height));
        g.setColour(surfaceHigh);
        g.fillRoundedRectangle(bounds, 8.0f);

        g.setColour(box.isMouseOver() ? strokeBright : stroke);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);

        const float cx = static_cast<float>(width) - 16.0f;
        const float cy = static_cast<float>(height) * 0.5f;
        juce::Path arrow;
        arrow.addTriangle(cx - 4.5f, cy - 2.0f, cx + 4.5f, cy - 2.0f, cx, cy + 3.0f);
        g.setColour(textTertiary);
        g.fillPath(arrow);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(20.0f); }
    juce::Font getPopupMenuFont() override { return font(18.0f); }

    // The text sits 14 px in so a 20 px value does not touch the rounded edge,
    // and stops short of the chevron.
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(14, 1, box.getWidth() - 14 - 30, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
        label.setJustificationType(juce::Justification::centredLeft);
    }

    // Taller rows in the popup: 18 px type needs the room.
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box,
                                                            juce::Label& label) override
    {
        return juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu(box, label)
            .withStandardItemHeight(38);
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isChecked, bool hasSubMenu,
                           const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        juce::ignoreUnused(hasSubMenu, shortcutKeyText, textColour);

        if (isSeparator)
        {
            g.setColour(stroke);
            g.fillRect(area.reduced(10, 0).withHeight(1).translated(0, area.getHeight() / 2));
            return;
        }

        auto bounds = area.reduced(4, 1);

        if (isHighlighted && isActive)
        {
            g.setColour(accent.withAlpha(0.16f));
            g.fillRoundedRectangle(bounds.toFloat(), 6.0f);
        }

        int textX = bounds.getX() + 10;

        if (icon != nullptr)
        {
            auto iconArea = juce::Rectangle<float>(static_cast<float>(textX),
                                                   bounds.getY() + 3.0f,
                                                   bounds.getHeight() - 6.0f,
                                                   bounds.getHeight() - 6.0f);
            icon->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
            textX += bounds.getHeight() - 2;
        }

        g.setColour(isActive ? textPrimary : textTertiary);
        g.setFont(font(18.0f));
        g.drawText(text, bounds.withTrimmedLeft(textX - bounds.getX()),
                   juce::Justification::centredLeft, true);

        if (isChecked)
        {
            g.setColour(accent);
            g.drawText(juce::String::fromUTF8("✓"), bounds.reduced(10, 0),
                       juce::Justification::centredRight, false);
        }
    }
};

// One-line text button: no chrome until hovered.
class GhostButton final : public juce::LookAndFeel_V4
{
public:
    GhostButton()
    {
        setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour(juce::TextButton::buttonOnColourId, accent.withAlpha(0.16f));
        setColour(juce::TextButton::textColourOffId, textSecondary);
        setColour(juce::TextButton::textColourOnId, textPrimary);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour&, bool hover, bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat();

        // A quiet secondary button: always visible as a control, accent only
        // when it is actually on.
        juce::Colour fill = surfaceHigh.withAlpha(0.55f);
        juce::Colour line = stroke;

        if (button.getToggleState())
        {
            fill = accent.withAlpha(0.16f);
            line = accent;
        }
        else if (down)
        {
            fill = surfaceHigh.brighter(0.12f);
            line = strokeBright;
        }
        else if (hover)
        {
            fill = surfaceHigh.brighter(0.06f);
            line = strokeBright;
        }

        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 9.0f);
        g.setColour(line);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 9.0f, 1.0f);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int height) override
    {
        return font(juce::jmin(15.0f, static_cast<float>(height) * 0.40f));
    }
};

// The editors share one look-and-feel instance so the popup menus match.
inline juce::LookAndFeel_V4& editLookAndFeel()
{
    static ComboBoxLook instance;
    return instance;
}

inline juce::LookAndFeel_V4& buttonLookAndFeel()
{
    static GhostButton instance;
    return instance;
}

} // namespace hs::theme
