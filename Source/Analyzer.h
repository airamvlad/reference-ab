#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>

/**
    Long-term stereo spectrum analyser.

    The audio thread feeds it with process(). Every 2048 samples it runs an FFT
    of the left and right channels and averages three values per frequency bin:
    left power, right power and the real part of the cross-spectrum. Everything
    shown in the analysis views (spectrum, tonal balance, stereo width and
    correlation per band) is derived from those averages on the UI thread.

    Silent frames are skipped, so the picture holds while playback is stopped.
*/
class Analyzer
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int numBins = fftSize / 2 + 1;
    static constexpr int hopSize = fftSize / 2;
    static constexpr int scopePoints = 1024;

    struct Snapshot
    {
        std::array<float, numBins> left {}, right {}, cross {};
        std::array<float, numBins> fast {};                 // quick-moving total power
        std::array<float, scopePoints> scopeLeft {}, scopeRight {};   // most recent samples
        double sampleRate = 48000.0;
        bool valid = false;
    };

    /** What the views draw, computed from a Snapshot. */
    struct Result
    {
        static constexpr int numPoints = 121;       // 20 Hz .. 20 kHz, 12 per octave
        static constexpr int numToneBands = 5;
        static constexpr int numStereoBands = 3;

        bool valid = false;
        std::array<float, numPoints> spectrumDb {};  // relative to total level, pink noise reads flat
        std::array<float, numPoints> fastDb {};      // the same for the quick-moving spectrum
        std::array<float, numToneBands> toneDb {};   // band level relative to total level
        std::array<float, numStereoBands> sidePercent {};
        float sideTotalPercent = 0.0f;
        float correlation = 1.0f;

        static float pointFrequency (int index) noexcept;
    };

    static const std::array<float, Result::numToneBands + 1>& toneBandEdges() noexcept;
    static const std::array<const char*, Result::numToneBands>& toneBandNames() noexcept;
    static const std::array<float, Result::numStereoBands + 1>& stereoBandEdges() noexcept;
    static const std::array<const char*, Result::numStereoBands>& stereoBandNames() noexcept;

    Analyzer();

    void prepare (double sampleRate);

    /** Audio thread. Uses the first two channels. */
    void process (const juce::AudioBuffer<float>& buffer, int numSamples);

    /** Any thread; applied on the next process(). */
    void requestReset() noexcept            { resetRequested.store (true); }

    /** UI thread. */
    void getSnapshot (Snapshot& destination) const;

    static void analyse (const Snapshot& snapshot, Result& result);

private:
    void reset();
    void processFrame();

    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {};
    std::array<float, fftSize> fifoLeft {}, fifoRight {};
    std::array<float, fftSize * 2> workLeft {}, workRight {};
    int fifoFill = 0;

    std::array<float, numBins> left {}, right {}, cross {}, fast {};
    float fastAlpha = 0.2f;
    int frames = 0;
    int framesForAverage = 100;
    double sampleRate = 48000.0;

    mutable juce::SpinLock publishLock;
    Snapshot published;
    std::atomic<bool> resetRequested { false };
};
