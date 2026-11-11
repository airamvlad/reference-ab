#include "Analyzer.h"

#include <cmath>

namespace
{
    constexpr double averagingSeconds = 6.0;
    constexpr float silencePower = 1.0e-7f;     // about -70 dBFS RMS

    // Mean power between two frequencies, interpolating when the range is narrower than a bin.
    float meanPower (const std::array<float, Analyzer::numBins>& power, double binHz, double lowHz, double highHz)
    {
        const double lowBin = lowHz / binHz, highBin = highHz / binHz;
        const int first = (int) std::ceil (lowBin), last = (int) std::floor (highBin);
        const int top = Analyzer::numBins - 1;

        if (last - first < 1)
        {
            const double centre = juce::jlimit (0.0, (double) top, 0.5 * (lowBin + highBin));
            const int index = juce::jmin (top - 1, (int) centre);
            const auto fraction = (float) (centre - (double) index);
            return power[(size_t) index] * (1.0f - fraction) + power[(size_t) index + 1] * fraction;
        }

        double sum = 0.0;
        int count = 0;

        for (int bin = juce::jmax (0, first); bin <= juce::jmin (top, last); ++bin)
        {
            sum += power[(size_t) bin];
            ++count;
        }

        return count > 0 ? (float) (sum / count) : 0.0f;
    }

    double sumPower (const std::array<float, Analyzer::numBins>& power, double binHz, double lowHz, double highHz)
    {
        const int first = juce::jmax (1, (int) std::ceil (lowHz / binHz));
        const int last = juce::jmin (Analyzer::numBins - 1, (int) std::floor (highHz / binHz));
        double sum = 0.0;

        for (int bin = first; bin <= last; ++bin)
            sum += power[(size_t) bin];

        return sum;
    }

    float toDb (double power)
    {
        return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : -200.0f;
    }
}

//==============================================================================
float Analyzer::Result::pointFrequency (int index) noexcept
{
    return 20.0f * std::pow (1000.0f, (float) index / (float) (numPoints - 1));
}

const std::array<float, Analyzer::Result::numToneBands + 1>& Analyzer::toneBandEdges() noexcept
{
    static const std::array<float, Result::numToneBands + 1> edges { 20.0f, 60.0f, 250.0f, 2000.0f, 6000.0f, 20000.0f };
    return edges;
}

const std::array<const char*, Analyzer::Result::numToneBands>& Analyzer::toneBandNames() noexcept
{
    static const std::array<const char*, Result::numToneBands> names { "Sub", "Bass", "Mids", "Presence", "Air" };
    return names;
}

const std::array<float, Analyzer::Result::numStereoBands + 1>& Analyzer::stereoBandEdges() noexcept
{
    static const std::array<float, Result::numStereoBands + 1> edges { 20.0f, 250.0f, 4000.0f, 20000.0f };
    return edges;
}

const std::array<const char*, Analyzer::Result::numStereoBands>& Analyzer::stereoBandNames() noexcept
{
    static const std::array<const char*, Result::numStereoBands> names { "Low", "Mid", "High" };
    return names;
}

//==============================================================================
Analyzer::Analyzer()
{
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                              juce::dsp::WindowingFunction<float>::hann, false);
}

void Analyzer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    framesForAverage = juce::jmax (4, (int) (averagingSeconds * sampleRate / hopSize));
    reset();
}

void Analyzer::reset()
{
    fifoLeft.fill (0.0f);
    fifoRight.fill (0.0f);
    fifoFill = 0;
    left.fill (0.0f);
    right.fill (0.0f);
    cross.fill (0.0f);
    frames = 0;
    resetRequested.store (false);

    const juce::SpinLock::ScopedLockType lock (publishLock);
    published.valid = false;
    published.sampleRate = sampleRate;
}

void Analyzer::process (const juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (resetRequested.exchange (false))
        reset();

    if (buffer.getNumChannels() < 2)
        return;

    const float* inLeft = buffer.getReadPointer (0);
    const float* inRight = buffer.getReadPointer (1);
    int position = 0;

    while (position < numSamples)
    {
        const int todo = juce::jmin (numSamples - position, fftSize - fifoFill);
        std::copy (inLeft + position, inLeft + position + todo, fifoLeft.begin() + fifoFill);
        std::copy (inRight + position, inRight + position + todo, fifoRight.begin() + fifoFill);
        fifoFill += todo;
        position += todo;

        if (fifoFill == fftSize)
        {
            processFrame();
            std::copy (fifoLeft.begin() + hopSize, fifoLeft.end(), fifoLeft.begin());
            std::copy (fifoRight.begin() + hopSize, fifoRight.end(), fifoRight.begin());
            fifoFill = fftSize - hopSize;
        }
    }
}

void Analyzer::processFrame()
{
    float energy = 0.0f;

    for (int i = 0; i < fftSize; ++i)
    {
        energy += fifoLeft[(size_t) i] * fifoLeft[(size_t) i] + fifoRight[(size_t) i] * fifoRight[(size_t) i];
        workLeft[(size_t) i] = fifoLeft[(size_t) i] * window[(size_t) i];
        workRight[(size_t) i] = fifoRight[(size_t) i] * window[(size_t) i];
    }

    if (energy / (float) (2 * fftSize) < silencePower)
        return;     // hold the picture during silence

    std::fill (workLeft.begin() + fftSize, workLeft.end(), 0.0f);
    std::fill (workRight.begin() + fftSize, workRight.end(), 0.0f);
    fft.performRealOnlyForwardTransform (workLeft.data(), true);
    fft.performRealOnlyForwardTransform (workRight.data(), true);

    // Plain mean at first so the curve settles quickly, then a moving average.
    frames = juce::jmin (frames + 1, framesForAverage);
    const float alpha = 1.0f / (float) frames;
    const float scale = 1.0f / ((float) fftSize * (float) fftSize);

    for (int bin = 0; bin < numBins; ++bin)
    {
        const float lr = workLeft[(size_t) (2 * bin)], li = workLeft[(size_t) (2 * bin + 1)];
        const float rr = workRight[(size_t) (2 * bin)], ri = workRight[(size_t) (2 * bin + 1)];

        left[(size_t) bin]  += alpha * ((lr * lr + li * li) * scale - left[(size_t) bin]);
        right[(size_t) bin] += alpha * ((rr * rr + ri * ri) * scale - right[(size_t) bin]);
        cross[(size_t) bin] += alpha * ((lr * rr + li * ri) * scale - cross[(size_t) bin]);
    }

    const juce::SpinLock::ScopedTryLockType lock (publishLock);

    if (lock.isLocked())
    {
        published.left = left;
        published.right = right;
        published.cross = cross;
        published.sampleRate = sampleRate;
        published.valid = frames >= 2;
    }
}

void Analyzer::getSnapshot (Snapshot& destination) const
{
    const juce::SpinLock::ScopedLockType lock (publishLock);
    destination = published;
}

//==============================================================================
void Analyzer::analyse (const Snapshot& snapshot, Result& result)
{
    result = {};

    if (! snapshot.valid)
        return;

    const double binHz = snapshot.sampleRate / (double) fftSize;
    const double nyquist = 0.5 * snapshot.sampleRate;
    const double top = juce::jmin (20000.0, nyquist);

    // Per-bin mid, side and total power.
    static thread_local std::array<float, numBins> total, mid, side;

    for (int bin = 0; bin < numBins; ++bin)
    {
        const float l = snapshot.left[(size_t) bin], r = snapshot.right[(size_t) bin], c = snapshot.cross[(size_t) bin];
        total[(size_t) bin] = 0.5f * (l + r);
        mid[(size_t) bin] = juce::jmax (0.0f, 0.25f * (l + r + 2.0f * c));
        side[(size_t) bin] = juce::jmax (0.0f, 0.25f * (l + r - 2.0f * c));
    }

    const double totalPower = sumPower (total, binHz, 20.0, top);

    if (totalPower <= 1.0e-18)
        return;

    // Spectrum: third-octave smoothing, +3 dB per octave so pink noise is flat.
    const double numBinsInRange = juce::jmax (1.0, (top - 20.0) / binHz);

    for (int i = 0; i < Result::numPoints; ++i)
    {
        const double frequency = Result::pointFrequency (i);
        const double low = frequency * std::pow (2.0, -1.0 / 6.0);
        const double high = juce::jmin (top, frequency * std::pow (2.0, 1.0 / 6.0));

        if (low >= top)
        {
            result.spectrumDb[(size_t) i] = -200.0f;
            continue;
        }

        const double power = meanPower (total, binHz, low, high) * numBinsInRange / totalPower;
        result.spectrumDb[(size_t) i] = toDb (power) + (float) (10.0 * std::log10 (frequency / 1000.0));
    }

    const auto& toneEdges = toneBandEdges();

    for (int band = 0; band < Result::numToneBands; ++band)
        result.toneDb[(size_t) band] = toDb (sumPower (total, binHz, toneEdges[(size_t) band],
                                                       juce::jmin (top, (double) toneEdges[(size_t) band + 1])) / totalPower);

    const auto& stereoEdges = stereoBandEdges();

    for (int band = 0; band < Result::numStereoBands; ++band)
    {
        const double lowHz = stereoEdges[(size_t) band], highHz = juce::jmin (top, (double) stereoEdges[(size_t) band + 1]);
        const double m = sumPower (mid, binHz, lowHz, highHz), s = sumPower (side, binHz, lowHz, highHz);
        result.sidePercent[(size_t) band] = m + s > 0.0 ? (float) (100.0 * s / (m + s)) : 0.0f;
    }

    const double m = sumPower (mid, binHz, 20.0, top), s = sumPower (side, binHz, 20.0, top);
    result.sideTotalPercent = m + s > 0.0 ? (float) (100.0 * s / (m + s)) : 0.0f;

    const double ll = sumPower (snapshot.left, binHz, 20.0, top);
    const double rr = sumPower (snapshot.right, binHz, 20.0, top);
    const double lr = sumPower (snapshot.cross, binHz, 20.0, top);
    result.correlation = ll > 0.0 && rr > 0.0 ? (float) juce::jlimit (-1.0, 1.0, lr / std::sqrt (ll * rr)) : 1.0f;
    result.valid = true;
}
