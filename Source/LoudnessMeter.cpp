#include "LoudnessMeter.h"

#include <cmath>

namespace
{
    constexpr double absoluteGate = -70.0;
    constexpr double pi = 3.14159265358979323846;
}

LoudnessMeter::LoudnessMeter() = default;

float LoudnessMeter::powerToLufs (double power) noexcept
{
    if (power <= 1.0e-12)
        return silence;

    return (float) (-0.691 + 10.0 * std::log10 (power));
}

double LoudnessMeter::lufsToPower (double lufs) noexcept
{
    return std::pow (10.0, (lufs + 0.691) / 10.0);
}

int LoudnessMeter::binFor (double lufs) noexcept
{
    return juce::jlimit (0, histogramBins - 1, (int) std::floor ((lufs - absoluteGate) * 10.0));
}

double LoudnessMeter::binLufs (int bin) noexcept
{
    return absoluteGate + ((double) bin + 0.5) * 0.1;
}

void LoudnessMeter::prepare (double newSampleRate, int numChannels, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    channels = juce::jmax (1, numChannels);
    maxBlock = juce::jmax (16, maxBlockSize);
    subBlockLength = juce::jmax (1, (int) std::round (sampleRate * 0.1));

    // K-weighting, coefficients recomputed for the current sample rate.
    Biquad shelfProto, highpassProto;
    {
        const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (pi * f0 / sampleRate);
        const double vh = std::pow (10.0, gain / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelfProto.b0 = (vh + vb * k / q + k * k) / a0;
        shelfProto.b1 = 2.0 * (k * k - vh) / a0;
        shelfProto.b2 = (vh - vb * k / q + k * k) / a0;
        shelfProto.a1 = 2.0 * (k * k - 1.0) / a0;
        shelfProto.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (pi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;
        highpassProto.b0 = 1.0;
        highpassProto.b1 = -2.0;
        highpassProto.b2 = 1.0;
        highpassProto.a1 = 2.0 * (k * k - 1.0) / a0;
        highpassProto.a2 = (1.0 - k / q + k * k) / a0;
    }

    shelf.assign ((size_t) channels, shelfProto);
    highpass.assign ((size_t) channels, highpassProto);

    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) channels, (size_t) 2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampler->initProcessing ((size_t) maxBlock);

    reset();
}

void LoudnessMeter::reset()
{
    for (auto& f : shelf)     f.clear();
    for (auto& f : highpass)  f.clear();

    if (oversampler != nullptr)
        oversampler->reset();

    subBlockFill = 0;
    subBlockSum = 0.0;
    subBlocks.fill (0.0);
    subBlockPeaks.fill (0.0f);
    subBlockPeak = 0.0f;
    psr.store (0.0f);
    subBlockIndex = 0;
    subBlocksSeen = 0;
    recentPowers.fill (0.0);
    recentIndex = 0;
    recentCount = 0;
    momentaryHistogram.fill (0);
    shortTermHistogram.fill (0);
    truePeakLinear = 0.0f;

    momentary.store (silence);
    shortTerm.store (silence);
    integrated.store (silence);
    recent.store (silence);
    range.store (0.0f);
    truePeakDb.store (silence);
    resetRequested.store (false);
}

void LoudnessMeter::process (const juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (resetRequested.exchange (false))
        reset();

    const int numCh = juce::jmin (channels, buffer.getNumChannels());

    if (numCh <= 0 || numSamples <= 0)
        return;

    for (int start = 0; start < numSamples; start += maxBlock)
        updateTruePeak (buffer, start, juce::jmin (maxBlock, numSamples - start));

    int position = 0;

    while (position < numSamples)
    {
        const int todo = juce::jmin (numSamples - position, subBlockLength - subBlockFill);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* data = buffer.getReadPointer (ch, position);
            auto& s = shelf[(size_t) ch];
            auto& h = highpass[(size_t) ch];
            double sum = 0.0;

            for (int i = 0; i < todo; ++i)
            {
                const double y = h.process (s.process ((double) data[i]));
                sum += y * y;
            }

            subBlockSum += sum;
        }

        for (int ch = 0; ch < numCh; ++ch)
            subBlockPeak = juce::jmax (subBlockPeak, buffer.getMagnitude (ch, position, todo));

        subBlockFill += todo;
        position += todo;

        if (subBlockFill >= subBlockLength)
            finishSubBlock();
    }

    // Keep the filter states out of the denormal range.
    for (auto& f : shelf)     { if (std::abs (f.z1) < 1.0e-30) f.z1 = 0.0; if (std::abs (f.z2) < 1.0e-30) f.z2 = 0.0; }
    for (auto& f : highpass)  { if (std::abs (f.z1) < 1.0e-30) f.z1 = 0.0; if (std::abs (f.z2) < 1.0e-30) f.z2 = 0.0; }
}

void LoudnessMeter::updateTruePeak (const juce::AudioBuffer<float>& buffer, int start, int num)
{
    const int numCh = juce::jmin (channels, buffer.getNumChannels());
    float peak = truePeakLinear;

    if (numCh == channels && oversampler != nullptr)
    {
        const float* pointers[16] = {};
        const int usable = juce::jmin (numCh, 16);

        for (int ch = 0; ch < usable; ++ch)
            pointers[ch] = buffer.getReadPointer (ch, start);

        juce::dsp::AudioBlock<const float> input (pointers, (size_t) usable, (size_t) num);
        auto up = oversampler->processSamplesUp (input);

        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax (up.getChannelPointer (ch), up.getNumSamples());
            peak = juce::jmax (peak, -r.getStart(), r.getEnd());
        }
    }
    else
    {
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, buffer.getMagnitude (ch, start, num));
    }

    if (peak > truePeakLinear)
    {
        truePeakLinear = peak;
        truePeakDb.store (juce::Decibels::gainToDecibels (peak, silence));
    }
}

void LoudnessMeter::finishSubBlock()
{
    // Mean square per channel summed over channels (L/R weights are 1.0).
    const double power = subBlockSum / (double) subBlockLength;
    subBlockSum = 0.0;
    subBlockFill = 0;

    subBlocks[(size_t) subBlockIndex] = power;
    subBlockPeaks[(size_t) subBlockIndex] = subBlockPeak;
    subBlockPeak = 0.0f;
    subBlockIndex = (subBlockIndex + 1) % shortTermBlocks;
    subBlocksSeen = juce::jmin (subBlocksSeen + 1, shortTermBlocks);

    auto meanOfLast = [this] (int count)
    {
        double sum = 0.0;

        for (int i = 1; i <= count; ++i)
            sum += subBlocks[(size_t) ((subBlockIndex - i + shortTermBlocks) % shortTermBlocks)];

        return sum / (double) count;
    };

    // Momentary: 400 ms window, 100 ms hop.
    if (subBlocksSeen >= 4)
    {
        const double p = meanOfLast (4);
        const float m = powerToLufs (p);
        momentary.store (m);

        if (m >= (float) absoluteGate)
        {
            ++momentaryHistogram[(size_t) binFor (m)];

            recentPowers[(size_t) recentIndex] = p;
            recentIndex = (recentIndex + 1) % recentBlocks;
            recentCount = juce::jmin (recentCount + 1, recentBlocks);
        }
    }

    // Short-term: 3 s window.
    if (subBlocksSeen >= shortTermBlocks)
    {
        const float s = powerToLufs (meanOfLast (shortTermBlocks));
        shortTerm.store (s);

        float peak = 0.0f;

        for (const float p : subBlockPeaks)
            peak = juce::jmax (peak, p);

        psr.store (s > (float) absoluteGate && peak > 0.0f ? juce::Decibels::gainToDecibels (peak) - s : 0.0f);

        if (s >= (float) absoluteGate)
            ++shortTermHistogram[(size_t) binFor (s)];
    }
    else if (subBlocksSeen >= 4)
    {
        shortTerm.store (powerToLufs (meanOfLast (subBlocksSeen)));
    }

    // Integrated: absolute gate (already applied) then relative gate at -10 LU.
    {
        double sum = 0.0;
        uint64_t count = 0;

        for (int b = 0; b < histogramBins; ++b)
        {
            const auto n = momentaryHistogram[(size_t) b];

            if (n > 0)
            {
                sum += (double) n * lufsToPower (binLufs (b));
                count += n;
            }
        }

        if (count > 0)
        {
            const double threshold = (double) powerToLufs (sum / (double) count) - 10.0;
            double gatedSum = 0.0;
            uint64_t gatedCount = 0;

            for (int b = binFor (threshold); b < histogramBins; ++b)
            {
                const auto n = momentaryHistogram[(size_t) b];

                if (n > 0 && binLufs (b) >= threshold)
                {
                    gatedSum += (double) n * lufsToPower (binLufs (b));
                    gatedCount += n;
                }
            }

            integrated.store (gatedCount > 0 ? powerToLufs (gatedSum / (double) gatedCount) : silence);
        }
    }

    // Gated loudness of the most recent non-silent 30 s.
    if (recentCount > 0)
    {
        double sum = 0.0;

        for (int i = 0; i < recentCount; ++i)
            sum += recentPowers[(size_t) i];

        const double threshold = lufsToPower ((double) powerToLufs (sum / (double) recentCount) - 10.0);
        double gatedSum = 0.0;
        int gatedCount = 0;

        for (int i = 0; i < recentCount; ++i)
        {
            if (recentPowers[(size_t) i] >= threshold)
            {
                gatedSum += recentPowers[(size_t) i];
                ++gatedCount;
            }
        }

        recent.store (gatedCount > 0 ? powerToLufs (gatedSum / (double) gatedCount) : silence);
    }

    // Loudness range (EBU Tech 3342): short-term values, -20 LU relative gate,
    // distance between the 10th and 95th percentiles.
    {
        double sum = 0.0;
        uint64_t count = 0;

        for (int b = 0; b < histogramBins; ++b)
        {
            const auto n = shortTermHistogram[(size_t) b];

            if (n > 0)
            {
                sum += (double) n * lufsToPower (binLufs (b));
                count += n;
            }
        }

        float lra = 0.0f;

        if (count > 0)
        {
            const double threshold = (double) powerToLufs (sum / (double) count) - 20.0;
            const int firstBin = binFor (threshold);
            uint64_t gated = 0;

            for (int b = firstBin; b < histogramBins; ++b)
                gated += shortTermHistogram[(size_t) b];

            if (gated > 1)
            {
                const auto lowTarget = (uint64_t) std::llround (0.10 * (double) (gated - 1));
                const auto highTarget = (uint64_t) std::llround (0.95 * (double) (gated - 1));
                double low = 0.0, high = 0.0;
                bool lowFound = false;
                uint64_t seen = 0;

                for (int b = firstBin; b < histogramBins; ++b)
                {
                    seen += shortTermHistogram[(size_t) b];

                    if (! lowFound && seen > lowTarget)
                    {
                        low = binLufs (b);
                        lowFound = true;
                    }

                    if (seen > highTarget)
                    {
                        high = binLufs (b);
                        break;
                    }
                }

                lra = (float) juce::jmax (0.0, high - low);
            }
        }

        range.store (lra);
    }
}
