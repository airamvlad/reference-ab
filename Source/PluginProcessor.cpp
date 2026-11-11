#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float lowCrossover = 250.0f;
    constexpr float highCrossover = 4000.0f;
    constexpr float maxMatchDb = 24.0f;
}

juce::AudioProcessorValueTreeState::ParameterLayout ReferenceABProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "ab", 1 }, "A/B (Reference)", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "match", 1 }, "Level Match", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "trim", 1 }, "Reference Trim",
                                                       NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "track", 1 }, "Reference Track", 1, maxTracks, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Loop Region", true));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "sync", 1 }, "Sync",
                                                        StringArray { "Free", "Transport", "Timeline" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "mono", 1 }, "Mono", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "band", 1 }, "Listen Band",
                                                        StringArray { "Full", "Low", "Mid", "High" }, 0));
    return layout;
}

ReferenceABProcessor::ReferenceABProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    formatManager.registerBasicFormats();

    abParam    = apvts.getRawParameterValue ("ab");
    matchParam = apvts.getRawParameterValue ("match");
    trimParam  = apvts.getRawParameterValue ("trim");
    trackParam = apvts.getRawParameterValue ("track");
    loopParam  = apvts.getRawParameterValue ("loop");
    syncParam  = apvts.getRawParameterValue ("sync");
    monoParam  = apvts.getRawParameterValue ("mono");
    bandParam  = apvts.getRawParameterValue ("band");

    for (auto& g : generations)  g.store (0);
    for (auto& p : positions)    p.store (0);
}

ReferenceABProcessor::~ReferenceABProcessor()
{
    loader.removeAllJobs (true, 10000);
}

//==============================================================================
bool ReferenceABProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ReferenceABProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int maxBlockSize = juce::jmax (16, samplesPerBlock);

    mixMeter.prepare (sampleRate, 2, maxBlockSize);
    refMeter.prepare (sampleRate, 2, maxBlockSize);
    mixAnalyzer.prepare (sampleRate);
    refAnalyzer.prepare (sampleRate);
    refBuffer.setSize (2, maxBlockSize);
    refBuffer.clear();

    gainSmoother.reset (sampleRate, 0.25);
    gainSmoother.setCurrentAndTargetValue (1.0f);
    abSmoother.reset (sampleRate, 0.012);
    abSmoother.setCurrentAndTargetValue (abParam->load() > 0.5f ? 1.0f : 0.0f);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 2 };
    lowCut.prepare (spec);
    highCut.prepare (spec);
    lastBand = -1;
    refFade = 0.0f;

    // Tracks are stored at the host sample rate, so reload them if it changed.
    const bool rateChanged = std::abs (currentSampleRate.exchange (sampleRate) - sampleRate) > 0.5;

    for (int slot = 0; slot < maxTracks; ++slot)
    {
        if (auto track = getTrack (slot))
        {
            const bool neverLoaded = ! track->hasAudio() && track->error.isEmpty();
            const bool wrongRate = track->hasAudio() && std::abs (track->sampleRate - sampleRate) > 0.5;

            if (neverLoaded || (rateChanged && wrongRate))
                loadFile (slot, track->file, track->loopStart.load(), track->loopEnd.load());
        }
    }
}

void ReferenceABProcessor::releaseResources() {}

void ReferenceABProcessor::resetMeasurements()
{
    mixMeter.requestReset();
    refMeter.requestReset();
    mixAnalyzer.requestReset();
    refAnalyzer.requestReset();
}

//==============================================================================
bool ReferenceABProcessor::renderReference (int slot, int numSamples, bool playing, bool useLoop,
                                            int syncMode, juce::int64 hostSamples)
{
    auto* track = slots[(size_t) slot].get();

    if (track == nullptr || ! track->hasAudio())
    {
        activeRefLufs = LoudnessMeter::silence;
        return false;
    }

    activeRefLufs = track->integratedLufs;

    if (! playing)
        return false;

    const juce::int64 length = track->getNumSamples();
    juce::int64 regionStart = 0, regionEnd = length;

    if (useLoop)
    {
        const double a = track->loopStart.load(), b = track->loopEnd.load();

        if (b > a)
        {
            const auto s = juce::jlimit ((juce::int64) 0, length, (juce::int64) (a * (double) length));
            const auto e = juce::jlimit ((juce::int64) 0, length, (juce::int64) (b * (double) length));

            if (e - s >= 256)
            {
                regionStart = s;
                regionEnd = e;
            }
        }
    }

    juce::int64 position = positions[(size_t) slot].load();

    if (syncMode == syncTimeline && hostSamples >= 0)
        position = regionStart + (hostSamples % (regionEnd - regionStart));

    if (position < regionStart || position >= regionEnd)
        position = regionStart;

    int done = 0;

    while (done < numSamples)
    {
        const int chunk = (int) juce::jmin ((juce::int64) (numSamples - done), regionEnd - position);

        for (int ch = 0; ch < 2; ++ch)
            refBuffer.copyFrom (ch, done, track->audio, ch, (int) position, chunk);

        position += chunk;
        done += chunk;

        if (position >= regionEnd)
            position = regionStart;
    }

    positions[(size_t) slot].store (position);
    return true;
}

void ReferenceABProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (buffer.getNumChannels() < 2 || numSamples == 0)
        return;

    // The mix is always measured, also while the reference is being heard.
    mixMeter.process (buffer, numSamples);
    mixAnalyzer.process (buffer, numSamples);

    if (refBuffer.getNumSamples() < numSamples)
        refBuffer.setSize (2, numSamples, false, false, true);

    refBuffer.clear (0, 0, numSamples);
    refBuffer.clear (1, 0, numSamples);

    bool hostPlaying = false;
    juce::int64 hostSamples = -1;

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            hostPlaying = position->getIsPlaying();

            if (const auto samples = position->getTimeInSamples())
                hostSamples = *samples;
        }
    }

    const int syncMode = (int) syncParam->load();
    const bool playing = syncMode == syncFree || hostPlaying;
    const bool useLoop = loopParam->load() > 0.5f;
    const int desiredSlot = juce::jlimit (0, maxTracks - 1, (int) std::lround (trackParam->load()) - 1);
    double seek = seekRequest.load();
    const bool wantsJump = desiredSlot != audioSlot || seek >= 0.0;
    bool rendered = false;

    {
        const juce::SpinLock::ScopedTryLockType lock (slotLock);

        if (lock.isLocked())
        {
            if (wantsJump && refFade > 0.0f)
            {
                // Fade the current material out over this block, jump on the next one.
                rendered = renderReference (audioSlot, numSamples, playing, useLoop, syncMode, hostSamples);
                refBuffer.applyGainRamp (0, numSamples, 1.0f, 0.0f);
                refFade = 0.0f;
            }
            else
            {
                if (desiredSlot != audioSlot)
                {
                    audioSlot = desiredSlot;
                    refMeter.requestReset();
                    refAnalyzer.requestReset();
                }

                if (seek >= 0.0 && seekRequest.compare_exchange_strong (seek, -1.0))
                    if (auto* track = slots[(size_t) audioSlot].get())
                        positions[(size_t) audioSlot].store ((juce::int64) (seek * (double) juce::jmax (0, track->getNumSamples() - 1)));

                rendered = renderReference (audioSlot, numSamples, playing, useLoop, syncMode, hostSamples);

                if (rendered && refFade < 1.0f)
                {
                    refBuffer.applyGainRamp (0, numSamples, 0.0f, 1.0f);
                    refFade = 1.0f;
                }
                else if (! rendered)
                {
                    refFade = 0.0f;
                }
            }
        }
    }

    if (rendered)
    {
        refMeter.process (refBuffer, numSamples);
        refAnalyzer.process (refBuffer, numSamples);
    }

    // Reference gain: manual trim plus automatic loudness matching to the mix.
    float gainDb = trimParam->load();

    if (matchParam->load() > 0.5f)
    {
        const float mixLufs = mixMeter.getRecent();

        if (mixLufs > -70.0f && activeRefLufs > -70.0f)
            gainDb += juce::jlimit (-maxMatchDb, maxMatchDb, mixLufs - activeRefLufs);
    }

    appliedGainDb.store (gainDb);
    gainSmoother.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
    abSmoother.setTargetValue (abParam->load() > 0.5f ? 1.0f : 0.0f);

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    const auto* refLeft = refBuffer.getReadPointer (0);
    const auto* refRight = refBuffer.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        const float gain = gainSmoother.getNextValue();
        const float mixAmount = abSmoother.getNextValue();
        const float dry = 1.0f - mixAmount;
        const float wet = mixAmount * gain;

        left[i] = left[i] * dry + refLeft[i] * wet;
        right[i] = right[i] * dry + refRight[i] * wet;
    }

    // Monitoring tools act on whatever is being heard.
    const int band = (int) bandParam->load();

    if (band != lastBand)
    {
        lowCut.reset();
        highCut.reset();
        lowCut.setCutoffFrequency (lowCrossover);
        highCut.setCutoffFrequency (highCrossover);
        lowCut.setType (band == bandLow ? juce::dsp::LinkwitzRileyFilterType::lowpass
                                        : juce::dsp::LinkwitzRileyFilterType::highpass);
        highCut.setType (band == bandHigh ? juce::dsp::LinkwitzRileyFilterType::highpass
                                          : juce::dsp::LinkwitzRileyFilterType::lowpass);
        lastBand = band;
    }

    if (band != bandFull)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        auto stereo = block.getSubsetChannelBlock (0, 2);
        juce::dsp::ProcessContextReplacing<float> context (stereo);

        if (band == bandLow || band == bandMid)
            lowCut.process (context);

        if (band == bandHigh || band == bandMid)
            highCut.process (context);
    }

    if (monoParam->load() > 0.5f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float mid = 0.5f * (left[i] + right[i]);
            left[i] = mid;
            right[i] = mid;
        }
    }
}

//==============================================================================
bool ReferenceABProcessor::canLoad (const juce::File& file) const
{
    const auto extension = file.getFileExtension().toLowerCase();

    if (extension.isEmpty())
        return false;

    for (int i = 0; i < formatManager.getNumKnownFormats(); ++i)
        if (formatManager.getKnownFormat (i)->getFileExtensions().contains (extension, true))
            return true;

    return false;
}

ReferenceTrack::Ptr ReferenceABProcessor::getTrack (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, maxTracks))
        return {};

    const juce::SpinLock::ScopedLockType lock (slotLock);
    return slots[(size_t) slot];
}

void ReferenceABProcessor::installTrack (int slot, ReferenceTrack::Ptr track)
{
    ReferenceTrack::Ptr old;

    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        old = slots[(size_t) slot];
        slots[(size_t) slot] = track;
        positions[(size_t) slot].store (0);
    }

    // `old` is released here, outside the lock and never on the audio thread.
    old = nullptr;
    sendChangeMessage();
}

void ReferenceABProcessor::loadFile (int slot, const juce::File& file, double loopStart, double loopEnd)
{
    if (! juce::isPositiveAndBelow (slot, maxTracks))
        return;

    const juce::ScopedLock management (managementLock);
    const int generation = ++generations[(size_t) slot];

    auto standIn = ReferenceTrack::placeholder (file);
    standIn->loopStart.store (loopStart);
    standIn->loopEnd.store (loopEnd);
    installTrack (slot, standIn);

    const double sampleRate = currentSampleRate.load();

    if (sampleRate <= 0.0)
        return;     // loaded once the host tells us its sample rate

    loader.addJob ([this, slot, file, sampleRate, generation]
    {
        auto loaded = ReferenceTrack::load (file, sampleRate, formatManager);

        const juce::ScopedLock lock (managementLock);

        if (generations[(size_t) slot].load() != generation)
            return;     // the slot was changed again while we were loading

        if (auto current = getTrack (slot))
        {
            // Keep a loop region the user may have drawn in the meantime.
            loaded->loopStart.store (current->loopStart.load());
            loaded->loopEnd.store (current->loopEnd.load());
        }

        installTrack (slot, loaded);
    });
}

int ReferenceABProcessor::addFile (const juce::File& file)
{
    for (int slot = 0; slot < maxTracks; ++slot)
    {
        if (getTrack (slot) == nullptr)
        {
            loadFile (slot, file);
            return slot;
        }
    }

    return -1;
}

void ReferenceABProcessor::removeTrack (int slot)
{
    if (! juce::isPositiveAndBelow (slot, maxTracks))
        return;

    const juce::ScopedLock management (managementLock);
    ++generations[(size_t) slot];
    installTrack (slot, nullptr);
}

int ReferenceABProcessor::getSelectedSlot() const
{
    return juce::jlimit (0, maxTracks - 1, (int) std::lround (trackParam->load()) - 1);
}

void ReferenceABProcessor::selectSlot (int slot)
{
    if (auto* parameter = apvts.getParameter ("track"))
    {
        const float value = parameter->convertTo0to1 ((float) (juce::jlimit (0, maxTracks - 1, slot) + 1));

        if (std::abs (parameter->getValue() - value) > 1.0e-6f)
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (value);
            parameter->endChangeGesture();
        }
    }
}

//==============================================================================
void ReferenceABProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    while (state.getChildWithName ("TRACKS").isValid())
        state.removeChild (state.getChildWithName ("TRACKS"), nullptr);

    juce::ValueTree tracks ("TRACKS");

    for (int slot = 0; slot < maxTracks; ++slot)
    {
        if (auto track = getTrack (slot))
        {
            juce::ValueTree entry ("TRACK");
            entry.setProperty ("slot", slot, nullptr);
            entry.setProperty ("path", track->file.getFullPathName(), nullptr);
            entry.setProperty ("loopStart", track->loopStart.load(), nullptr);
            entry.setProperty ("loopEnd", track->loopEnd.load(), nullptr);
            tracks.appendChild (entry, nullptr);
        }
    }

    state.appendChild (tracks, nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ReferenceABProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto tracks = state.getChildWithName ("TRACKS").createCopy();
    state.removeChild (state.getChildWithName ("TRACKS"), nullptr);
    apvts.replaceState (state);

    std::array<bool, maxTracks> used {};

    for (const auto& entry : tracks)
    {
        const int slot = entry.getProperty ("slot", -1);
        const juce::String path = entry.getProperty ("path").toString();

        if (juce::isPositiveAndBelow (slot, maxTracks) && path.isNotEmpty())
        {
            used[(size_t) slot] = true;
            const auto existing = getTrack (slot);
            const double loopStart = entry.getProperty ("loopStart", 0.0);
            const double loopEnd = entry.getProperty ("loopEnd", 0.0);

            if (existing != nullptr && existing->file == juce::File (path) && existing->error.isEmpty())
            {
                existing->loopStart.store (loopStart);
                existing->loopEnd.store (loopEnd);
            }
            else
            {
                loadFile (slot, juce::File (path), loopStart, loopEnd);
            }
        }
    }

    for (int slot = 0; slot < maxTracks; ++slot)
        if (! used[(size_t) slot] && getTrack (slot) != nullptr)
            removeTrack (slot);

    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorEditor* ReferenceABProcessor::createEditor()
{
    return new ReferenceABEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReferenceABProcessor();
}
