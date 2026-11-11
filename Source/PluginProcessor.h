#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "Analyzer.h"
#include "LoudnessMeter.h"
#include "ReferenceTrack.h"

class ReferenceABProcessor : public juce::AudioProcessor,
                             public juce::ChangeBroadcaster
{
public:
    static constexpr int maxTracks = 8;

    enum SyncMode { syncFree = 0, syncTransport, syncTimeline };
    enum Band { bandFull = 0, bandLow, bandMid, bandHigh };

    ReferenceABProcessor();
    ~ReferenceABProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Reference AB"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    // The reference keeps playing with a silent input, so the host must never suspend us.
    double getTailLengthSeconds() const override            { return std::numeric_limits<double>::infinity(); }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Reference tracks. All of these may be called from the message thread.

    /** Loads a file into a slot (replacing whatever was there). */
    void loadFile (int slot, const juce::File& file, double loopStart = 0.0, double loopEnd = 0.0);

    /** Loads a file into the first empty slot. Returns the slot, or -1 if all are in use. */
    int addFile (const juce::File& file);

    void removeTrack (int slot);

    /** Sets (or with equal values clears) the loop region of a slot and re-measures its loudness. */
    void setLoopRegion (int slot, double start, double end);
    ReferenceTrack::Ptr getTrack (int slot) const;

    int getSelectedSlot() const;
    void selectSlot (int slot);

    /** Jumps to a position in the selected track, 0..1. */
    void seek (double normalisedPosition)                   { seekRequest.store (juce::jlimit (0.0, 1.0, normalisedPosition)); }

    /** Play position of a slot in samples. */
    juce::int64 getPosition (int slot) const noexcept       { return positions[(size_t) slot].load(); }

    float getAppliedGainDb() const noexcept                 { return appliedGainDb.load(); }
    bool isReferenceAudible() const noexcept                { return abParam->load() > 0.5f; }

    juce::String getSupportedWildcard() const               { return formatManager.getWildcardForAllFormats(); }
    bool canLoad (const juce::File& file) const;

    juce::AudioProcessorValueTreeState apvts;
    LoudnessMeter mixMeter, refMeter;
    Analyzer mixAnalyzer, refAnalyzer;

    /** Clears all measurements (meters and analysis). */
    void resetMeasurements();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void installTrack (int slot, ReferenceTrack::Ptr track);
    bool renderReference (int slot, int numSamples, bool playing, bool useLoop, int syncMode, juce::int64 hostSamples);

    juce::AudioFormatManager formatManager;
    juce::ThreadPool loader { 1 };

    // Slots are swapped under this lock; the audio thread only ever try-locks it.
    mutable juce::SpinLock slotLock;
    std::array<ReferenceTrack::Ptr, maxTracks> slots;
    std::array<std::atomic<int>, maxTracks> generations;
    std::array<std::atomic<juce::int64>, maxTracks> positions;

    juce::CriticalSection managementLock;
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<double> seekRequest { -1.0 };
    std::atomic<float> appliedGainDb { 0.0f };

    std::atomic<float>* abParam = nullptr;
    std::atomic<float>* matchParam = nullptr;
    std::atomic<float>* trimParam = nullptr;
    std::atomic<float>* trackParam = nullptr;
    std::atomic<float>* loopParam = nullptr;
    std::atomic<float>* syncParam = nullptr;
    std::atomic<float>* monoParam = nullptr;
    std::atomic<float>* bandParam = nullptr;

    // Audio thread state.
    juce::AudioBuffer<float> refBuffer;
    int audioSlot = 0;
    float refFade = 0.0f;
    float activeRefLufs = LoudnessMeter::silence;
    int lastBand = 0;
    juce::SmoothedValue<float> gainSmoother, abSmoother;
    juce::dsp::LinkwitzRileyFilter<float> lowCut, highCut;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceABProcessor)
};
