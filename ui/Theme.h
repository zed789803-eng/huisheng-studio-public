#pragma once

#include <JuceHeader.h>

#include "Config.h"

// 绘声Studio visual language.
//
// A single dark surface, one warm accent sampled from the logo, and type that
// stays quiet: labels are small, uppercase-free and never shout. Status is
// carried by a dot and one word, not by a sentence.
namespace hs::theme {

inline const juce::Colour background   { 0xff0e0f11 };
inline const juce::Colour surface      { 0xff17191c };
inline const juce::Colour surfaceHigh  { 0xff1f2226 };
inline const juce::Colour stroke       { 0xff2b2f34 };
inline const juce::Colour strokeBright { 0xff3a4047 };

inline const juce::Colour textPrimary   { 0xfff2f3f5 };
inline const juce::Colour textSecondary { 0xff8b9198 };
inline const juce::Colour textTertiary  { 0xff5d6369 };

inline const juce::Colour accent   { 0xffb45ce0 }; // sampled from the logo mark
inline const juce::Colour accentDim{ 0x33b45ce0 };
inline const juce::Colour positive { 0xff4cd964 };
inline const juce::Colour warning  { 0xffffb020 };
inline const juce::Colour danger   { 0xffff5a52 };

inline constexpr float kRadius = 10.0f;
inline constexpr float kControlHeight = 34.0f;
inline constexpr float kGap = 12.0f;

// The macOS system font handles Chinese correctly at every weight; JUCE's
// default bold rendering of CJK is too heavy, so use Medium/Semibold explicitly.
inline juce::Font font(float size, float weight = 0.4f)
{
    // PingFang SC is the system Chinese face. JUCE's synthetic bold on CJK is
    // too heavy, so weight is expressed through the named style instead.
    juce::Font f(juce::FontOptions(
        juce::String::fromUTF8("PingFang SC"), size, juce::Font::plain));
    f.setTypefaceStyle(weight >= 0.6f ? "Semibold" : "Regular");
    return f;
}

inline juce::Font mono(float size)
{
    return juce::Font(juce::FontOptions(
        juce::String(juce::Font::getDefaultMonospacedFontName()), size, juce::Font::plain));
}

inline juce::String zh(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

inline juce::String dB(float value)
{
    if (value <= 1.0e-6f)
        return zh("−∞");

    const auto db = 20.0f * std::log10(value);
    return (db > -0.05f ? juce::String("0.0") : juce::String(db, 1)) + " dB";
}

// A dot that reads as a status light without needing a legend.
class StatusDot : public juce::Component
{
public:
    void setState(bool on, juce::Colour colour)
    {
        on_ = on;
        colour_ = colour;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        const float radius = juce::jmin(getWidth(), getHeight()) * 0.30f;

        if (on_)
        {
            g.setColour(colour_.withAlpha(0.18f));
            g.fillEllipse(centre.x - radius * 2.1f, centre.y - radius * 2.1f,
                          radius * 4.2f, radius * 4.2f);
        }

        g.setColour(on_ ? colour_ : juce::Colour(0xff4a4f55));
        g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
    }

private:
    bool on_ = false;
    juce::Colour colour_ = positive;
};

// Slim level meter: a quiet gradient bar with a peak tick.
class LevelMeter : public juce::Component
{
public:
    void setLevel(float value)
    {
        const float target = juce::jlimit(0.0f, 1.0f, value);
        level_ = target;

        if (target > peak_)
        {
            peak_ = target;
            peakHold_ = 60;
        }
        else if (--peakHold_ < 0)
        {
            peak_ *= 0.9f;
        }

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(theme::surfaceHigh);
        g.fillRoundedRectangle(bounds, bounds.getHeight() * 0.5f);

        const auto width = bounds.getWidth() * std::sqrt(level_);

        if (width > 1.0f)
        {
            juce::ColourGradient gradient(accent.withAlpha(0.95f), bounds.getX(), 0.0f,
                                          accent.brighter(0.35f), bounds.getRight(), 0.0f, false);
            g.setGradientFill(gradient);
            g.fillRoundedRectangle(bounds.withWidth(width), bounds.getHeight() * 0.5f);
        }

        if (peak_ > 0.001f)
        {
            const float x = bounds.getX() + bounds.getWidth() * std::sqrt(peak_);
            g.setColour(theme::textPrimary.withAlpha(0.55f));
            g.fillRoundedRectangle(x - 1.0f, bounds.getY() + 2.0f, 2.0f,
                                   bounds.getHeight() - 4.0f, 1.0f);
        }
    }

private:
    float level_ = 0.0f;
    float peak_ = 0.0f;
    int peakHold_ = 0;
};

// A hairline that fades out at both ends: reads as a quiet separator instead of
// a hard rule across the panel.
inline void paintHairline(juce::Graphics& g, int x0, int x1, int y)
{
    juce::ColourGradient fade(stroke.withAlpha(0.0f), static_cast<float>(x0), static_cast<float>(y),
                              stroke.withAlpha(0.0f), static_cast<float>(x1), static_cast<float>(y),
                              false);
    fade.addColour(0.5, stroke);

    g.setGradientFill(fade);
    g.fillRect(x0, y, x1 - x0, 1);
}

// Draws a soft rounded container used for every group in the editors.
inline void paintCard(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(surface);
    g.fillRoundedRectangle(bounds, kRadius);
    g.setColour(stroke);
    g.drawRoundedRectangle(bounds.reduced(0.5f), kRadius, 1.0f);
}

inline void paintSectionLabel(juce::Graphics& g, juce::Rectangle<int> bounds,
                              const juce::String& title, const juce::String& detail = {})
{
    g.setColour(textTertiary);
    g.setFont(font(11.0f));
    g.drawText(title, bounds, juce::Justification::centredLeft, false);

    if (detail.isNotEmpty())
    {
        g.setColour(textTertiary.withAlpha(0.75f));
        g.drawText(detail, bounds, juce::Justification::centredRight, false);
    }
}

} // namespace hs::theme
