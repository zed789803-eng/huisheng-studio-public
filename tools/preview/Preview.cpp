// Development-only: renders both editors offscreen to PNG so the layout can be
// inspected without a DAW. Not installed with the product.
#include <JuceHeader.h>

#include "CaptureEditor.h"
#include "CaptureProcessor.h"
#include "ReturnEditor.h"
#include "ReturnProcessor.h"

#include <cmath>
#include <cstdio>

namespace {

void dumpTree(juce::Component& c, int depth = 0)
{
    const auto& b = c.getBounds();

    juce::String extra;

    if (auto* label = dynamic_cast<juce::Label*>(&c))
        extra = "  font=" + juce::String(label->getFont().getHeight())
                + "  text=" + label->getText();
    else if (auto* box = dynamic_cast<juce::ComboBox*>(&c))
        extra = "  font=" + juce::String(box->getLookAndFeel().getComboBoxFont(*box).getHeight())
                + "  text=" + box->getText();

    std::printf("%*s%-28s x=%-4d y=%-4d w=%-4d h=%-4d%s%s\n", depth * 2, "",
                typeid(c).name(), b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                c.isVisible() ? "" : " (hidden)", extra.toRawUTF8());

    for (auto* child : c.getChildren())
        dumpTree(*child, depth + 1);
}

void render(juce::Component& component, const juce::File& file, float scale)
{
    juce::Image image(juce::Image::ARGB,
                      static_cast<int>(component.getWidth() * scale),
                      static_cast<int>(component.getHeight() * scale),
                      true);

    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));
    component.paintEntireComponent(g, true);

    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    png.writeImageToStream(image, stream);
    std::printf("wrote %s\n", file.getFullPathName().toRawUTF8());
}

// Three things CI has to be able to see, all of them what users hit:
//   * every instance used to share one ring, so a second instance stole frames
//     from the first (dropouts and clicks);
//   * an App whose device rate differs from the project used to mute its own
//     track, which read as "the App meter moves but nothing comes out";
//   * one instance could only take one App, so "several platforms into one
//     track" needed a track each — now the Apps an instance takes are summed.
void feed(hs::RingBuffer& ring, uint32_t rate, float amplitude, int blocks)
{
    std::vector<float> frames(256 * 2, 0.0f);
    const double step = juce::MathConstants<double>::twoPi * 220.0 / static_cast<double>(rate);

    ring.resetForProducer(rate, "preview-test");

    for (int block = 0; block < blocks; ++block)
    {
        for (int i = 0; i < 256; ++i)
        {
            const auto sample = amplitude * static_cast<float>(
                std::sin(step * static_cast<double>(block * 256 + i)));
            frames[i * 2] = sample;
            frames[i * 2 + 1] = sample;
        }

        ring.write(frames.data(), 256);
    }

    ring.setActive(true);
}

float runPeak(CaptureProcessor& processor, int blocks)
{
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    float peak = 0.0f;

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
        peak = juce::jmax(peak, processor.outputPeak());
    }

    return peak;
}

int slotChecks()
{
    int failed = 0;

    CaptureProcessor first;
    first.prepareToPlay(48000.0, 256);
    const int slotA = first.slotIndex();
    std::printf("slot check: instance A -> slot %d\n", slotA);

    if (slotA < 0)
    {
        std::printf("FAIL slot check: instance A got no slot\n");
        return 1;
    }

    CaptureProcessor second;
    second.prepareToPlay(48000.0, 256);
    const int slotB = second.slotIndex();
    std::printf("slot check: instance B -> slot %d\n", slotB);

    if (slotB < 0 || slotB == slotA)
    {
        std::printf("FAIL slot check: two instances must own two slots\n");
        ++failed;
    }

    // Give the second instance an App of its own too, so its ring exists: the
    // check at the end then proves the two instances do not share a ring, instead
    // of silently skipping when there is no ring to look at.
    second.addApp(2000000003, "com.huishengstudio.preview.b", "Preview B");

    // Two Apps in one instance, which is what "several platforms into one track"
    // means. The pids are deliberately not real processes, so the Agent has
    // nothing to tap and these rings belong to the check alone: CI runs this both
    // with and without a background service, so it must not depend on one.
    // Distinct bundle ids: an entry is one App, so two entries have to be two
    // different Apps, exactly like two platforms in the real case.
    first.addApp(2000000000, "com.huishengstudio.preview.a", "Preview A");
    first.addApp(2000000001, "com.huishengstudio.preview.b", "Preview B");
    first.setUsePair(false);

    std::printf("slot check: instance A holds %d App(s)\n", first.appCount());

    if (first.appCount() != 2)
    {
        std::printf("FAIL slot check: an instance must hold every App it was given\n");
        ++failed;
    }

    const auto nameA = hs::slotTapRingName(slotA, 0);
    const auto nameB = hs::slotTapRingName(slotA, 1);

    hs::RingBuffer tapA;
    hs::RingBuffer tapB;

    if (! tapA.open(nameA) || ! tapB.open(nameB))
    {
        std::printf("FAIL slot check: cannot open %s / %s\n", nameA.c_str(), nameB.c_str());
        ++failed;
    }
    else
    {
        // A 44.1 kHz App inside a 48 kHz project, on both of the instance's Apps.
        feed(tapA, 44100, 0.2f, 16);
        feed(tapB, 44100, 0.2f, 16);

        // Read the rate off the slot's own ring instead of the Agent-published
        // field: with no background service running there is nobody to publish it.
        const auto sourceRate = tapA.sampleRate();
        const float bothPeak = runPeak(first, 200);

        std::printf("slot check: 2 Apps at %u Hz in a 48000 Hz project -> output peak %.4f\n",
                    sourceRate, bothPeak);

        if (sourceRate != 44100)
        {
            std::printf("FAIL slot check: the slot ring must carry the App's own rate\n");
            ++failed;
        }

        if (bothPeak <= 0.001f)
        {
            std::printf("FAIL slot check: a rate difference must not mute the track\n");
            ++failed;
        }

        // One instance with one App, fed the same signal: two Apps have to come
        // out as their sum, not as one of them.
        CaptureProcessor solo;
        solo.prepareToPlay(48000.0, 256);
        const int slotSolo = solo.slotIndex();
        solo.addApp(2000000002, "com.huishengstudio.preview.solo", "Preview solo");
        solo.setUsePair(false);

        hs::RingBuffer tapSolo;

        if (slotSolo < 0 || ! tapSolo.open(hs::slotTapRingName(slotSolo, 0)))
        {
            std::printf("FAIL slot check: the single-App reference got no ring\n");
            ++failed;
        }
        else
        {
            feed(tapSolo, 44100, 0.2f, 16);
            const float soloPeak = runPeak(solo, 200);

            std::printf("slot check: 1 App -> peak %.4f, 2 Apps -> peak %.4f\n",
                        soloPeak, bothPeak);

            if (soloPeak <= 0.001f || bothPeak < soloPeak * 1.5f)
            {
                std::printf("FAIL slot check: the Apps of one instance must be summed\n");
                ++failed;
            }

            tapSolo.close();
        }

        // The neighbouring instance must not see one frame of it.
        hs::RingBuffer neighbour;

        if (neighbour.open(hs::slotTapRingName(slotB, 0)))
        {
            std::printf("slot check: slot %d holds %u frame(s) while slot %d plays\n",
                        slotB, neighbour.available(), slotA);

            if (neighbour.available() != 0)
            {
                std::printf("FAIL slot check: instances must not share a ring\n");
                ++failed;
            }

            neighbour.close();
        }
        else
        {
            std::printf("FAIL slot check: instance B has no ring of its own\n");
            ++failed;
        }

        tapA.close();
        tapB.close();
    }

    return failed;
}

// A plugin that the host never runs looks dead to the background service: no
// heartbeat, no sequence change, so the service never re-decides anything and
// the editor paints a stale state. Push a quiet tone through first, like a host
// would, then let the service catch up before the editor is built.
void pump(juce::AudioProcessor& processor, double seconds, float gain)
{
    constexpr int block = 256;
    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;

    const double step = juce::MathConstants<double>::twoPi * 220.0 / rate;
    double phase = 0.0;
    const int total = static_cast<int>(rate * seconds);

    for (int done = 0; done < total; done += block)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* w = buffer.getWritePointer(ch);

            for (int i = 0; i < block; ++i)
                w[i] = gain * static_cast<float>(std::sin(phase + i * step));
        }

        phase += block * step;
        processor.processBlock(buffer, midi);
    }

    std::printf("pumped %.1f s of audio\n", seconds);
}

} // namespace

class PreviewApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "huishengstudio preview"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto out = juce::File::getCurrentWorkingDirectory();

        // CI reads this log while the process may still be running (it kills the
        // app after a timeout), so nothing may sit in the stdio buffer.
        std::setvbuf(stdout, nullptr, _IONBF, 0);

        // Run the behavioural checks first: a failure must be loud in the log,
        // and the shots below still get written so the log has something to sit
        // next to.
        const int failed = slotChecks();
        std::printf("slot checks: %s\n", failed == 0 ? "PASSED" : "FAILED");
        std::fflush(stdout);
        setApplicationReturnValue(failed == 0 ? 0 : 1);

        {
            CaptureProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            pump(processor, 1.5, 0.1f);
            juce::Thread::sleep(1200);
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            editor->setSize(editor->getWidth(), editor->getHeight());
            std::printf("capture %dx%d\n", editor->getWidth(), editor->getHeight());
            dumpTree(*editor);
            render(*editor, out.getChildFile("capture.png"), 2.0f);
        }

        {
            ReturnProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            pump(processor, 1.5, 0.1f);
            juce::Thread::sleep(1200);
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            editor->setSize(editor->getWidth(), editor->getHeight());
            std::printf("return %dx%d\n", editor->getWidth(), editor->getHeight());
            dumpTree(*editor);
            render(*editor, out.getChildFile("return.png"), 2.0f);
        }

        quit();
    }

    void shutdown() override {}
    void systemRequestedQuit() override { quit(); }
};

START_JUCE_APPLICATION(PreviewApp)
