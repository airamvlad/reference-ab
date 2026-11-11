// End-to-end check of the audio engine without a host: loads a reference
// file, switches A/B and verifies level matching and state save/restore.

#include "../Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void expect (const char* what, bool ok)
{
    std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what);

    if (! ok)
        ++failures;
}

static void expectNear (const char* what, double actual, double expected, double tolerance)
{
    const bool ok = std::abs (actual - expected) <= tolerance;
    std::printf ("%s %-46s got %8.2f, expected %8.2f (+/- %.2f)\n",
                 ok ? "PASS" : "FAIL", what, actual, expected, tolerance);

    if (! ok)
        ++failures;
}

static juce::File writeSine (double sampleRate, double seconds, double levelDb, double frequency)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("referenceab-test", ".wav");
    const int length = (int) (sampleRate * seconds);
    juce::AudioBuffer<float> buffer (2, length);
    const double amplitude = std::pow (10.0, levelDb / 20.0);

    for (int i = 0; i < length; ++i)
    {
        const auto v = (float) (amplitude * std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate));
        buffer.setSample (0, i, v);
        buffer.setSample (1, i, v);
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24);

    if (auto writer = wav.createWriterFor (stream, options))
        writer->writeFromAudioSampleBuffer (buffer, 0, length);

    return file;
}

// Feeds a sine "mix" through the processor and returns the peak of the last block.
static float run (ReferenceABProcessor& processor, double sampleRate, double seconds, double mixLevelDb, double& phase)
{
    const int blockSize = 512;
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const double amplitude = std::pow (10.0, mixLevelDb / 20.0);
    const double increment = 2.0 * juce::MathConstants<double>::pi * 997.0 / sampleRate;
    float peak = 0.0f;

    for (auto remaining = (juce::int64) (seconds * sampleRate); remaining > 0; remaining -= blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto v = (float) (amplitude * std::sin (phase));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
            phase += increment;
        }

        processor.processBlock (buffer, midi);
        peak = buffer.getMagnitude (0, blockSize);
    }

    return peak;
}

static void setParameter (ReferenceABProcessor& processor, const char* id, float plainValue)
{
    auto* parameter = processor.apvts.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
}

static bool waitForTrack (ReferenceABProcessor& processor, int slot)
{
    for (int i = 0; i < 500; ++i)
    {
        if (auto track = processor.getTrack (slot))
            if (track->hasAudio() || track->error.isNotEmpty())
                return track->hasAudio();

        juce::Thread::sleep (20);
    }

    return false;
}

int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    const double hostRate = 48000.0;
    const auto file = writeSine (44100.0, 6.0, -10.0, 997.0);
    juce::MemoryBlock savedState;

    {
        ReferenceABProcessor processor;
        processor.setPlayConfigDetails (2, 2, hostRate, 512);
        processor.prepareToPlay (hostRate, 512);
        processor.loadFile (0, file);

        expect ("reference file loads", waitForTrack (processor, 0));
        const auto track = processor.getTrack (0);

        if (track == nullptr || ! track->hasAudio())
        {
            std::printf ("Cannot continue without a loaded track.\n");
            return 1;
        }

        expectNear ("resampled to host rate (seconds)", track->getLengthSeconds(), 6.0, 0.01);
        expectNear ("file loudness (LUFS)", track->integratedLufs, -10.0, 0.15);
        expectNear ("file true peak (dB)", track->truePeakDb, -10.0, 0.3);

        double phase = 0.0;

        // A: the mix passes through untouched.
        float peak = run (processor, hostRate, 5.0, -20.0, phase);
        expectNear ("A: mix passes through (dB)", juce::Decibels::gainToDecibels (peak), -20.0, 0.1);
        expectNear ("mix loudness measured (LUFS)", processor.mixMeter.getIntegrated(), -20.0, 0.3);

        // B with level match: the -10 LUFS reference is turned down to the -20 LUFS mix.
        setParameter (processor, "ab", 1.0f);
        peak = run (processor, hostRate, 3.0, -20.0, phase);
        expectNear ("B: reference level matched (dB)", juce::Decibels::gainToDecibels (peak), -20.0, 0.5);
        expectNear ("applied match gain (dB)", processor.getAppliedGainDb(), -10.0, 0.5);

        // B without level match: the reference plays at its own level.
        setParameter (processor, "match", 0.0f);
        peak = run (processor, hostRate, 2.0, -20.0, phase);
        expectNear ("B: unmatched reference level (dB)", juce::Decibels::gainToDecibels (peak), -10.0, 0.3);

        // Low band solo removes most of a 997 Hz tone (crossover at 250 Hz).
        setParameter (processor, "band", (float) ReferenceABProcessor::bandLow);
        peak = run (processor, hostRate, 1.0, -20.0, phase);
        expect ("low band solo attenuates 1 kHz", juce::Decibels::gainToDecibels (peak) < -30.0f);
        setParameter (processor, "band", (float) ReferenceABProcessor::bandFull);

        // Loop region: playback stays inside it.
        processor.setLoopRegion (0, 0.25, 0.5);

        for (int i = 0; i < 200 && track->regionLufs.load() < -70.0f; ++i)
            juce::Thread::sleep (10);

        expectNear ("loop region loudness measured (LUFS)", track->regionLufs.load(), -10.0, 0.2);
        processor.seek (0.25);
        run (processor, hostRate, 4.0, -20.0, phase);
        const double position = (double) processor.getPosition (0) / (double) track->getNumSamples();
        expect ("playback stays inside loop region", position >= 0.25 && position < 0.5);

        // Back to A.
        setParameter (processor, "ab", 0.0f);
        peak = run (processor, hostRate, 1.0, -20.0, phase);
        expectNear ("back to A: mix again (dB)", juce::Decibels::gainToDecibels (peak), -20.0, 0.1);

        processor.getStateInformation (savedState);
    }

    {
        ReferenceABProcessor restored;
        restored.setPlayConfigDetails (2, 2, hostRate, 512);
        restored.setStateInformation (savedState.getData(), (int) savedState.getSize());
        restored.prepareToPlay (hostRate, 512);

        expect ("restored project reloads the file", waitForTrack (restored, 0));

        if (auto track = restored.getTrack (0))
        {
            expect ("restored file path", track->file == file);
            expectNear ("restored loop start", track->loopStart.load(), 0.25, 1.0e-6);
            expectNear ("restored loop end", track->loopEnd.load(), 0.5, 1.0e-6);
        }

        expect ("restored level match setting", restored.apvts.getRawParameterValue ("match")->load() < 0.5f);
    }

    {
        // Analysis: a mono, bass-heavy "mix" against a wide, flat reference.
        std::printf ("-- analysis\n");
        juce::Random random (1234);
        const int length = (int) (44100.0 * 8.0);
        juce::AudioBuffer<float> noise (2, length);

        for (int i = 0; i < length; ++i)
        {
            const float a = random.nextFloat() * 0.2f - 0.1f, b = random.nextFloat() * 0.2f - 0.1f;
            noise.setSample (0, i, a);
            noise.setSample (1, i, 0.5f * a + 0.87f * b);
        }

        const auto noiseFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getNonexistentChildFile ("referenceab-noise", ".wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = noiseFile.createOutputStream();
            const auto options = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24);

            if (auto writer = wav.createWriterFor (stream, options))
                writer->writeFromAudioSampleBuffer (noise, 0, length);
        }

        ReferenceABProcessor processor;
        processor.setPlayConfigDetails (2, 2, hostRate, 512);
        processor.prepareToPlay (hostRate, 512);
        processor.loadFile (0, noiseFile);
        expect ("noise reference loads", waitForTrack (processor, 0));

        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float lowpassed = 0.0f;

        for (int block = 0; block < (int) (hostRate * 8.0 / 512.0); ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float white = random.nextFloat() * 0.2f - 0.1f;
                lowpassed += 0.02f * (white - lowpassed);       // about 150 Hz
                const float v = 0.5f * white + 4.0f * lowpassed;
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }

            processor.processBlock (buffer, midi);
        }

        auto snapshot = std::make_unique<Analyzer::Snapshot>();
        Analyzer::Result mix, ref;
        processor.mixAnalyzer.getSnapshot (*snapshot);
        Analyzer::analyse (*snapshot, mix);
        processor.refAnalyzer.getSnapshot (*snapshot);
        Analyzer::analyse (*snapshot, ref);

        expect ("mix and reference analysed", mix.valid && ref.valid);
        expect ("bass-heavy mix shows more bass", mix.toneDb[1] - ref.toneDb[1] > 3.0f);
        expect ("bass-heavy mix shows less air", mix.toneDb[4] - ref.toneDb[4] < -1.5f);
        expectNear ("mono mix correlation", mix.correlation, 1.0, 0.01);
        expectNear ("mono mix side energy (%)", mix.sideTotalPercent, 0.0, 0.5);
        expect ("wide reference has side energy", ref.sideTotalPercent > 15.0f && ref.sideTotalPercent < 40.0f);
        expectNear ("reference correlation", ref.correlation, 0.5, 0.05);

        // White noise must rise 3 dB per octave on the pink-flat display.
        expectNear ("white noise slope, 1 kHz to 8 kHz (dB)", ref.spectrumDb[104] - ref.spectrumDb[68], 9.0, 1.5);
        expectNear ("live spectrum close to the average at 1 kHz (dB)", ref.fastDb[68] - ref.spectrumDb[68], 0.0, 3.0);

        {
            float scopePeak = 0.0f;

            for (const float v : snapshot->scopeRight)
                scopePeak = juce::jmax (scopePeak, std::abs (v));

            expect ("goniometer receives samples", scopePeak > 0.01f && scopePeak < 0.2f);
        }

        expect ("mix punch (PSR) measured", processor.mixMeter.getPsr() > 3.0f && processor.mixMeter.getPsr() < 25.0f);

        // Optional: REFERENCEAB_SNAPSHOT=/path/to.png saves a picture of the editor
        // (REFERENCEAB_TAB=0..2 picks the analysis tab).
        if (const auto snapshotPath = juce::SystemStats::getEnvironmentVariable ("REFERENCEAB_SNAPSHOT", {}); snapshotPath.isNotEmpty())
        {
            processor.apvts.state.setProperty ("analysisTab", juce::SystemStats::getEnvironmentVariable ("REFERENCEAB_TAB", "0").getIntValue(), nullptr);
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            const juce::File target (snapshotPath);
            target.deleteFile();

            if (auto stream = target.createOutputStream())
                juce::PNGImageFormat().writeImageToStream (image, *stream);
        }

        noiseFile.deleteFile();
    }

    file.deleteFile();
    std::printf ("%s\n", failures == 0 ? "All tests passed." : "Some tests FAILED.");
    return failures == 0 ? 0 : 1;
}
