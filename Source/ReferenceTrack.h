#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <vector>

/**
    One loaded reference song: fully decoded, converted to stereo and resampled
    to the host sample rate, with its loudness analysed up front.

    A track with no audio is a placeholder (still loading, or the file could not
    be read); it keeps the file path so the slot survives a project save.
*/
class ReferenceTrack : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<ReferenceTrack>;

    static constexpr int numPeaks = 2048;
    static constexpr double maxLengthSeconds = 15.0 * 60.0;

    /** Decodes and analyses a file. Never returns null; on failure the result has no audio and `error` is set. */
    static Ptr load (const juce::File& file, double targetSampleRate, juce::AudioFormatManager& formats);

    /** Creates an empty stand-in shown while the file is being loaded. */
    static Ptr placeholder (const juce::File& file);

    bool hasAudio() const noexcept          { return audio.getNumSamples() > 0; }
    int getNumSamples() const noexcept      { return audio.getNumSamples(); }
    double getLengthSeconds() const noexcept { return sampleRate > 0.0 ? audio.getNumSamples() / sampleRate : 0.0; }

    bool hasLoopRegion() const noexcept     { return loopEnd.load() > loopStart.load(); }

    /** Integrated loudness of a section given as fractions of the length. Slow: never call on the audio thread. */
    float measureLoudness (double start, double end) const;

    juce::File file;
    juce::String name;
    juce::String error;
    bool loading = false;

    juce::AudioBuffer<float> audio;         // 2 channels at `sampleRate`
    double sampleRate = 0.0;

    float integratedLufs = -100.0f;
    float truePeakDb = -100.0f;
    float loudnessRange = 0.0f;
    std::vector<float> peaks;               // numPeaks absolute peak values for the waveform

    // Loop region as a fraction of the track length. Equal values mean "no region".
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 };

    // Loudness of the loop region, or -100 while unknown. Used for level matching.
    std::atomic<float> regionLufs { -100.0f };
};
