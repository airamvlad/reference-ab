#include "ReferenceTrack.h"
#include "LoudnessMeter.h"

#include <juce_dsp/juce_dsp.h>

ReferenceTrack::Ptr ReferenceTrack::placeholder (const juce::File& fileToUse)
{
    Ptr track (new ReferenceTrack());
    track->file = fileToUse;
    track->name = fileToUse.getFileNameWithoutExtension();
    track->loading = true;
    return track;
}

ReferenceTrack::Ptr ReferenceTrack::load (const juce::File& fileToLoad, double targetSampleRate,
                                          juce::AudioFormatManager& formats)
{
    Ptr track (new ReferenceTrack());
    track->file = fileToLoad;
    track->name = fileToLoad.getFileNameWithoutExtension();

    if (! fileToLoad.existsAsFile())
    {
        track->error = "File not found";
        return track;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (fileToLoad));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        track->error = "Unsupported file";
        return track;
    }

    if ((double) reader->lengthInSamples / reader->sampleRate > maxLengthSeconds)
    {
        track->error = "Longer than 15 minutes";
        return track;
    }

    const int sourceLength = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> source (2, sourceLength);
    source.clear();

    if (! reader->read (&source, 0, sourceLength, 0, true, true))
    {
        track->error = "Could not read file";
        return track;
    }

    if (reader->numChannels == 1)
        source.copyFrom (1, 0, source, 0, 0, sourceLength);

    const double sourceRate = reader->sampleRate;
    reader.reset();

    if (targetSampleRate <= 0.0 || std::abs (sourceRate - targetSampleRate) < 0.5)
    {
        track->audio = std::move (source);
        track->sampleRate = sourceRate;
    }
    else
    {
        const double ratio = sourceRate / targetSampleRate;

        if (ratio > 1.0)
        {
            // Going down in sample rate: remove everything above the new Nyquist first.
            const auto sections = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                (float) (0.45 * targetSampleRate), sourceRate, 10);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = source.getWritePointer (ch);

                for (auto* coefficients : sections)
                {
                    juce::dsp::IIR::Filter<float> filter (coefficients);

                    for (int i = 0; i < sourceLength; ++i)
                        data[i] = filter.processSample (data[i]);
                }
            }
        }

        const int targetLength = juce::jmax (1, (int) ((double) sourceLength / ratio));
        track->audio.setSize (2, targetLength);
        track->audio.clear();

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::WindowedSincInterpolator interpolator;
            interpolator.process (ratio, source.getReadPointer (ch), track->audio.getWritePointer (ch),
                                  targetLength, sourceLength, 0);
        }

        track->sampleRate = targetSampleRate;
    }

    const int length = track->audio.getNumSamples();

    // Loudness of the whole file, measured once.
    {
        const int chunk = 4096;
        LoudnessMeter meter;
        meter.prepare (track->sampleRate, 2, chunk);

        for (int start = 0; start < length; start += chunk)
        {
            const int num = juce::jmin (chunk, length - start);
            juce::AudioBuffer<float> view (track->audio.getArrayOfWritePointers(), 2, start, num);
            meter.process (view, num);
        }

        track->integratedLufs = meter.getIntegrated();
        track->truePeakDb = meter.getTruePeakDb();
        track->loudnessRange = meter.getRange();
    }

    // Waveform overview.
    track->peaks.assign ((size_t) numPeaks, 0.0f);

    for (int i = 0; i < numPeaks; ++i)
    {
        const int start = (int) ((juce::int64) i * length / numPeaks);
        const int end = juce::jmax (start + 1, (int) ((juce::int64) (i + 1) * length / numPeaks));
        const int num = juce::jmin (end, length) - start;

        if (num > 0)
            track->peaks[(size_t) i] = juce::jmax (track->audio.getMagnitude (0, start, num),
                                                   track->audio.getMagnitude (1, start, num));
    }

    return track;
}
