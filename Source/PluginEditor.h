#pragma once

#include "PluginProcessor.h"

namespace Theme
{
    const juce::Colour background { 0xff14161b };
    const juce::Colour panel      { 0xff1d2129 };
    const juce::Colour outline    { 0xff2c323d };
    const juce::Colour text       { 0xffe6e9ef };
    const juce::Colour dimText    { 0xff8b93a3 };
    const juce::Colour mix        { 0xff4aa3ff };
    const juce::Colour reference  { 0xffffa23a };
    const juce::Colour warning    { 0xffff5d5d };
}

//==============================================================================
/** The eight reference slots. Click to select, click the x to remove. */
class TrackList : public juce::Component
{
public:
    explicit TrackList (ReferenceABProcessor&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    /** Slot under a point in this component's coordinates, or -1. */
    int slotAt (juce::Point<int>) const;

private:
    juce::Rectangle<int> rowBounds (int slot) const;
    juce::Rectangle<int> removeBounds (int slot) const;

    ReferenceABProcessor& processor;
};

//==============================================================================
/** Waveform of the selected reference with playhead and loop region. */
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (ReferenceABProcessor&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    double xToPosition (int x) const;

    ReferenceABProcessor& processor;
    double dragAnchor = 0.0;
    bool dragging = false;
};

//==============================================================================
/** Loudness read-outs for the mix and the reference. */
class MeterPanel : public juce::Component
{
public:
    explicit MeterPanel (ReferenceABProcessor&);
    void paint (juce::Graphics&) override;

private:
    ReferenceABProcessor& processor;
};

//==============================================================================
class ReferenceABEditor : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    explicit ReferenceABEditor (ReferenceABProcessor&);
    ~ReferenceABEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override    { dropHighlight = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override               { dropHighlight = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void addFiles (const juce::StringArray& paths, int preferredSlot);
    void updateABButton();

    using APVTS = juce::AudioProcessorValueTreeState;

    ReferenceABProcessor& processor;
    juce::LookAndFeel_V4 lookAndFeel;

    juce::TextButton abButton;
    juce::ToggleButton matchButton { "Level match" }, loopButton { "Loop region" }, monoButton { "Mono" };
    juce::Slider trimSlider;
    juce::Label trimLabel, gainLabel, syncLabel, bandLabel, hintLabel;
    juce::ComboBox syncBox, bandBox;
    juce::TextButton addButton { "Add files..." }, resetButton { "Reset meters" };

    TrackList trackList;
    WaveformView waveform;
    MeterPanel meters;

    std::unique_ptr<APVTS::ButtonAttachment> abAttachment, matchAttachment, loopAttachment, monoAttachment;
    std::unique_ptr<APVTS::SliderAttachment> trimAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> syncAttachment, bandAttachment;
    std::unique_ptr<juce::FileChooser> chooser;

    bool dropHighlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceABEditor)
};
