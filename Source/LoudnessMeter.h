#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

/**
    Loudness meter following ITU-R BS.1770-4 / EBU R128.

    Measures momentary (400 ms), short-term (3 s) and gated integrated loudness,
    loudness range (LRA), true peak (4x oversampled) and a gated loudness over
    the most recent 30 s of non-silent audio (used for level matching).

    process() is realtime safe after prepare(). The getters may be called from
    any thread.
*/
class LoudnessMeter
{
public:
    static constexpr float silence = -100.0f;

    LoudnessMeter();

    void prepare (double sampleRate, int numChannels, int maxBlockSize);

    /** Feeds audio into the meter. Channels beyond the prepared count are ignored. */
    void process (const juce::AudioBuffer<float>& buffer, int numSamples);

    /** Clears all history. Safe to call from any thread; applied on the next process(). */
    void requestReset() noexcept                { resetRequested.store (true); }

    /** Clears all history immediately. Only call when process() is not running. */
    void reset();

    float getMomentary() const noexcept         { return momentary.load(); }
    float getShortTerm() const noexcept         { return shortTerm.load(); }
    float getIntegrated() const noexcept        { return integrated.load(); }
    float getRecent() const noexcept            { return recent.load(); }
    float getRange() const noexcept             { return range.load(); }
    float getTruePeakDb() const noexcept        { return truePeakDb.load(); }

    /** Peak to short-term loudness ratio over the last 3 s: a measure of how punchy the audio is. */
    float getPsr() const noexcept               { return psr.load(); }

    /** Peak to (integrated) loudness ratio: lower means more compressed overall. */
    float getPlr() const noexcept
    {
        const float peak = truePeakDb.load(), loudness = integrated.load();
        return peak > -70.0f && loudness > -70.0f ? peak - loudness : 0.0f;
    }

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        inline double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void clear() noexcept { z1 = z2 = 0.0; }
    };

    static constexpr int histogramBins = 750;          // -70 .. +5 LU in 0.1 steps
    static constexpr int shortTermBlocks = 30;         // 30 x 100 ms
    static constexpr int recentBlocks = 300;           // 30 s of 100 ms hops

    static float powerToLufs (double power) noexcept;
    static double lufsToPower (double lufs) noexcept;
    static int binFor (double lufs) noexcept;
    static double binLufs (int bin) noexcept;

    void finishSubBlock();
    void updateTruePeak (const juce::AudioBuffer<float>& buffer, int start, int num);

    double sampleRate = 48000.0;
    int channels = 2;
    int maxBlock = 512;
    int subBlockLength = 4800;
    int subBlockFill = 0;
    double subBlockSum = 0.0;

    std::vector<Biquad> shelf, highpass;

    std::array<double, shortTermBlocks> subBlocks {};
    int subBlockIndex = 0;
    int subBlocksSeen = 0;

    std::array<float, shortTermBlocks> subBlockPeaks {};
    float subBlockPeak = 0.0f;

    std::array<double, recentBlocks> recentPowers {};
    int recentIndex = 0;
    int recentCount = 0;

    std::array<uint32_t, histogramBins> momentaryHistogram {};
    std::array<uint32_t, histogramBins> shortTermHistogram {};

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    float truePeakLinear = 0.0f;

    std::atomic<float> momentary { silence }, shortTerm { silence }, integrated { silence },
                       recent { silence }, range { 0.0f }, truePeakDb { silence }, psr { 0.0f };
    std::atomic<bool> resetRequested { false };
};
