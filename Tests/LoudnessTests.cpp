// Checks the loudness meter against the EBU R128 calibration signal:
// a 997 Hz stereo sine at -23 dBFS must read -23.0 LUFS.

#include "../Source/LoudnessMeter.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void expectNear (const char* what, double actual, double expected, double tolerance)
{
    const bool ok = std::abs (actual - expected) <= tolerance;
    std::printf ("%s %-44s got %8.2f, expected %8.2f (+/- %.2f)\n",
                 ok ? "PASS" : "FAIL", what, actual, expected, tolerance);

    if (! ok)
        ++failures;
}

static void runSine (double sampleRate, double levelDb, double seconds, LoudnessMeter& meter)
{
    const int blockSize = 512;
    meter.prepare (sampleRate, 2, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    const double amplitude = std::pow (10.0, levelDb / 20.0);
    const double increment = 2.0 * 3.14159265358979323846 * 997.0 / sampleRate;
    double phase = 0.0;

    for (auto remaining = (juce::int64) (seconds * sampleRate); remaining > 0; remaining -= blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto v = (float) (amplitude * std::sin (phase));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
            phase += increment;
        }

        meter.process (buffer, blockSize);
    }
}

int main()
{
    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        LoudnessMeter meter;
        runSine (sampleRate, -23.0, 10.0, meter);

        std::printf ("-- %.0f Hz\n", sampleRate);
        expectNear ("integrated, -23 dBFS sine", meter.getIntegrated(), -23.0, 0.1);
        expectNear ("momentary, -23 dBFS sine", meter.getMomentary(), -23.0, 0.1);
        expectNear ("short-term, -23 dBFS sine", meter.getShortTerm(), -23.0, 0.1);
        expectNear ("recent window, -23 dBFS sine", meter.getRecent(), -23.0, 0.1);
        expectNear ("true peak, -23 dBFS sine", meter.getTruePeakDb(), -23.0, 0.3);
        expectNear ("loudness range, steady sine", meter.getRange(), 0.0, 0.2);
    }

    {
        // Gating: 10 s at -20 dBFS then 10 s at -60 dBFS. The quiet part falls
        // under the relative gate, so the result stays at -20 LUFS.
        LoudnessMeter meter;
        const double sampleRate = 48000.0;
        const int blockSize = 512;
        meter.prepare (sampleRate, 2, blockSize);
        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;
        const double increment = 2.0 * 3.14159265358979323846 * 997.0 / sampleRate;

        for (int part = 0; part < 2; ++part)
        {
            const double amplitude = std::pow (10.0, (part == 0 ? -20.0 : -60.0) / 20.0);

            for (auto remaining = (juce::int64) (10.0 * sampleRate); remaining > 0; remaining -= blockSize)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = (float) (amplitude * std::sin (phase));
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                    phase += increment;
                }

                meter.process (buffer, blockSize);
            }
        }

        std::printf ("-- gating\n");
        expectNear ("integrated, loud then quiet", meter.getIntegrated(), -20.0, 0.2);
    }

    {
        LoudnessMeter meter;
        meter.prepare (48000.0, 2, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();

        for (int i = 0; i < 500; ++i)
            meter.process (buffer, 512);

        std::printf ("-- silence\n");
        expectNear ("integrated, silence", meter.getIntegrated(), LoudnessMeter::silence, 0.01);
    }

    std::printf ("%s\n", failures == 0 ? "All tests passed." : "Some tests FAILED.");
    return failures == 0 ? 0 : 1;
}
