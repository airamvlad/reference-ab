#include "PluginEditor.h"

namespace
{
    juce::String formatLufs (float value)
    {
        return value <= -70.0f ? juce::String ("-") : juce::String (value, 1);
    }

    juce::String formatTime (double seconds)
    {
        const int total = (int) seconds;
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }

    juce::FontOptions font (float height, bool bold = false)
    {
        return juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain);
    }
}

//==============================================================================
TrackList::TrackList (ReferenceABProcessor& p) : processor (p) {}

juce::Rectangle<int> TrackList::rowBounds (int slot) const
{
    const int rowHeight = getHeight() / ReferenceABProcessor::maxTracks;
    return { 0, slot * rowHeight, getWidth(), rowHeight - 4 };
}

juce::Rectangle<int> TrackList::removeBounds (int slot) const
{
    return rowBounds (slot).removeFromRight (26);
}

int TrackList::slotAt (juce::Point<int> point) const
{
    for (int slot = 0; slot < ReferenceABProcessor::maxTracks; ++slot)
        if (rowBounds (slot).contains (point))
            return slot;

    return -1;
}

void TrackList::paint (juce::Graphics& g)
{
    const int selected = processor.getSelectedSlot();

    for (int slot = 0; slot < ReferenceABProcessor::maxTracks; ++slot)
    {
        const auto row = rowBounds (slot);
        const auto track = processor.getTrack (slot);
        const bool isSelected = slot == selected;

        g.setColour (isSelected ? Theme::reference.withAlpha (0.18f) : Theme::panel);
        g.fillRoundedRectangle (row.toFloat(), 5.0f);
        g.setColour (isSelected ? Theme::reference : Theme::outline);
        g.drawRoundedRectangle (row.toFloat().reduced (0.5f), 5.0f, 1.0f);

        auto content = row.reduced (8, 0);
        g.setColour (isSelected ? Theme::reference : Theme::dimText);
        g.setFont (font (14.0f, true));
        g.drawText (juce::String (slot + 1), content.removeFromLeft (18), juce::Justification::centredLeft);

        if (track == nullptr)
        {
            g.setColour (Theme::dimText.withAlpha (0.6f));
            g.setFont (font (13.0f));
            g.drawText ("Empty - drop a song here", content, juce::Justification::centredLeft);
            continue;
        }

        content.removeFromRight (22);
        g.setColour (Theme::dimText);
        g.setFont (font (14.0f));
        g.drawText ("x", removeBounds (slot), juce::Justification::centred);

        auto top = content.removeFromTop (content.getHeight() / 2 + 2);
        g.setColour (Theme::text);
        g.setFont (font (13.5f));
        g.drawText (track->name, top, juce::Justification::bottomLeft, true);

        g.setFont (font (11.5f));

        if (track->error.isNotEmpty())
        {
            g.setColour (Theme::warning);
            g.drawText (track->error, content, juce::Justification::topLeft, true);
        }
        else if (! track->hasAudio())
        {
            g.setColour (Theme::dimText);
            g.drawText ("Loading...", content, juce::Justification::topLeft, true);
        }
        else
        {
            g.setColour (Theme::dimText);
            g.drawText (formatLufs (track->integratedLufs) + " LUFS   " + formatTime (track->getLengthSeconds()),
                        content, juce::Justification::topLeft, true);
        }
    }
}

void TrackList::mouseDown (const juce::MouseEvent& event)
{
    const int slot = slotAt (event.getPosition());

    if (slot < 0)
        return;

    if (processor.getTrack (slot) != nullptr && removeBounds (slot).contains (event.getPosition()))
        processor.removeTrack (slot);
    else
        processor.selectSlot (slot);

    repaint();
}

//==============================================================================
WaveformView::WaveformView (ReferenceABProcessor& p) : processor (p) {}

double WaveformView::xToPosition (int x) const
{
    return juce::jlimit (0.0, 1.0, (double) x / (double) juce::jmax (1, getWidth()));
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    const int slot = processor.getSelectedSlot();
    const auto track = processor.getTrack (slot);

    if (track == nullptr || ! track->hasAudio())
    {
        g.setColour (Theme::dimText);
        g.setFont (font (15.0f));
        g.drawText (track == nullptr ? "Drop audio files anywhere on the plugin (WAV, AIFF, FLAC, MP3, OGG)"
                                     : (track->error.isNotEmpty() ? track->name + ": " + track->error
                                                                  : "Loading " + track->name + "..."),
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    const float width = bounds.getWidth();
    const float centre = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.5f - 8.0f;
    const double loopStart = track->loopStart.load(), loopEnd = track->loopEnd.load();
    const bool hasLoop = loopEnd > loopStart;

    if (hasLoop)
    {
        const auto region = juce::Rectangle<float> ((float) loopStart * width, 0.0f,
                                                    (float) (loopEnd - loopStart) * width, bounds.getHeight());
        g.setColour (Theme::reference.withAlpha (0.14f));
        g.fillRect (region);
        g.setColour (Theme::reference.withAlpha (0.8f));
        g.fillRect (region.withWidth (1.5f));
        g.fillRect (region.withLeft (region.getRight() - 1.5f));
    }

    const auto& peaks = track->peaks;
    const int columns = juce::jmax (1, (int) width);

    for (int x = 0; x < columns; ++x)
    {
        const auto first = (size_t) ((juce::int64) x * (juce::int64) peaks.size() / columns);
        const auto last = juce::jmax (first + 1, (size_t) ((juce::int64) (x + 1) * (juce::int64) peaks.size() / columns));
        float peak = 0.0f;

        for (auto i = first; i < juce::jmin (last, peaks.size()); ++i)
            peak = juce::jmax (peak, peaks[i]);

        const double position = (double) x / (double) columns;
        const bool inLoop = ! hasLoop || (position >= loopStart && position < loopEnd);
        const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak) * halfHeight);

        g.setColour (inLoop ? Theme::reference.withAlpha (0.85f) : Theme::dimText.withAlpha (0.55f));
        g.fillRect ((float) x, centre - h, 1.0f, h * 2.0f);
    }

    const double playPosition = (double) processor.getPosition (slot) / (double) juce::jmax (1, track->getNumSamples());
    g.setColour (Theme::text);
    g.fillRect ((float) playPosition * width - 0.75f, 0.0f, 1.5f, bounds.getHeight());

    g.setColour (Theme::text.withAlpha (0.9f));
    g.setFont (font (12.0f));
    g.drawText (formatTime (playPosition * track->getLengthSeconds()) + " / " + formatTime (track->getLengthSeconds()),
                getLocalBounds().reduced (8, 5), juce::Justification::topRight);
    g.setColour (Theme::dimText);
    g.drawText ("Click: jump   Drag: set loop   Double-click: clear loop",
                getLocalBounds().reduced (8, 5), juce::Justification::bottomLeft);
}

void WaveformView::mouseDown (const juce::MouseEvent& event)
{
    dragAnchor = xToPosition (event.x);
    dragging = false;
}

void WaveformView::mouseDrag (const juce::MouseEvent& event)
{
    if (std::abs (event.getDistanceFromDragStartX()) < 4 && ! dragging)
        return;

    if (auto track = processor.getTrack (processor.getSelectedSlot()))
    {
        dragging = true;
        const double current = xToPosition (event.x);
        track->loopStart.store (juce::jmin (dragAnchor, current));
        track->loopEnd.store (juce::jmax (dragAnchor, current));
        repaint();
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& event)
{
    if (dragging)
    {
        // Start listening from the beginning of the new region.
        if (auto track = processor.getTrack (processor.getSelectedSlot()))
            processor.seek (track->loopStart.load());
    }
    else if (! event.mouseWasDraggedSinceMouseDown() && event.getNumberOfClicks() < 2)
    {
        processor.seek (xToPosition (event.x));
    }

    dragging = false;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (auto track = processor.getTrack (processor.getSelectedSlot()))
    {
        track->loopStart.store (0.0);
        track->loopEnd.store (0.0);
        repaint();
    }
}

//==============================================================================
MeterPanel::MeterPanel (ReferenceABProcessor& p) : processor (p) {}

void MeterPanel::paint (juce::Graphics& g)
{
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    const auto track = processor.getTrack (processor.getSelectedSlot());
    const bool hasReference = track != nullptr && track->hasAudio();
    const auto& mix = processor.mixMeter;
    const auto& ref = processor.refMeter;

    const juce::StringArray headers { "", "Momentary", "Short-term", "Integrated", "Range (LU)", "True peak (dB)" };
    const juce::StringArray mixRow { "MIX",
                                     formatLufs (mix.getMomentary()), formatLufs (mix.getShortTerm()),
                                     formatLufs (mix.getIntegrated()), juce::String (mix.getRange(), 1),
                                     formatLufs (mix.getTruePeakDb()) };
    const juce::StringArray refRow { "REFERENCE",
                                     hasReference ? formatLufs (ref.getMomentary()) : "-",
                                     hasReference ? formatLufs (ref.getShortTerm()) : "-",
                                     hasReference ? formatLufs (track->integratedLufs) : "-",
                                     hasReference ? juce::String (track->loudnessRange, 1) : "-",
                                     hasReference ? formatLufs (track->truePeakDb) : "-" };

    auto area = getLocalBounds().reduced (12, 8);
    const int rowHeight = area.getHeight() / 3;
    const int firstColumn = 110;
    const int columnWidth = (area.getWidth() - firstColumn) / (headers.size() - 1);

    auto drawRow = [&] (const juce::StringArray& cells, juce::Rectangle<int> row, juce::Colour colour, float size, bool bold)
    {
        g.setColour (colour);
        g.setFont (font (size, bold));
        g.drawText (cells[0], row.removeFromLeft (firstColumn), juce::Justification::centredLeft);

        for (int i = 1; i < cells.size(); ++i)
            g.drawText (cells[i], row.removeFromLeft (columnWidth), juce::Justification::centred);
    };

    drawRow (headers, area.removeFromTop (rowHeight), Theme::dimText, 12.0f, false);
    drawRow (mixRow, area.removeFromTop (rowHeight), Theme::mix, 17.0f, true);
    drawRow (refRow, area.removeFromTop (rowHeight), Theme::reference, 17.0f, true);
}

//==============================================================================
ReferenceABEditor::ReferenceABEditor (ReferenceABProcessor& p)
    : AudioProcessorEditor (&p), processor (p), trackList (p), waveform (p), meters (p)
{
    lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    lookAndFeel.setColour (juce::ResizableWindow::backgroundColourId, Theme::background);
    lookAndFeel.setColour (juce::ComboBox::backgroundColourId, Theme::panel);
    lookAndFeel.setColour (juce::ComboBox::outlineColourId, Theme::outline);
    lookAndFeel.setColour (juce::TextButton::buttonColourId, Theme::panel);
    lookAndFeel.setColour (juce::ToggleButton::tickColourId, Theme::reference);
    lookAndFeel.setColour (juce::Slider::thumbColourId, Theme::reference);
    lookAndFeel.setColour (juce::Slider::trackColourId, Theme::reference.withAlpha (0.5f));
    lookAndFeel.setColour (juce::Slider::textBoxOutlineColourId, Theme::outline);
    lookAndFeel.setColour (juce::Label::textColourId, Theme::text);
    setLookAndFeel (&lookAndFeel);

    abButton.setClickingTogglesState (true);
    abButton.setColour (juce::TextButton::buttonColourId, Theme::mix);
    abButton.setColour (juce::TextButton::buttonOnColourId, Theme::reference);
    abButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    abButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    abButton.onStateChange = [this] { updateABButton(); };
    addAndMakeVisible (abButton);

    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    trimSlider.setTextValueSuffix (" dB");
    trimSlider.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (trimSlider);

    trimLabel.setText ("Reference trim", juce::dontSendNotification);
    syncLabel.setText ("Sync", juce::dontSendNotification);
    bandLabel.setText ("Listen", juce::dontSendNotification);

    for (auto* label : { &trimLabel, &syncLabel, &bandLabel, &gainLabel, &hintLabel })
    {
        label->setFont (font (13.0f));
        label->setColour (juce::Label::textColourId, Theme::dimText);
        addAndMakeVisible (*label);
    }

    gainLabel.setJustificationType (juce::Justification::centredRight);
    hintLabel.setText ("Bypass or remove the plugin before exporting your song.", juce::dontSendNotification);
    hintLabel.setJustificationType (juce::Justification::centredRight);

    syncBox.addItemList ({ "Free", "Transport", "Timeline" }, 1);
    bandBox.addItemList ({ "Full", "Low", "Mid", "High" }, 1);

    for (auto* component : std::initializer_list<juce::Component*> { &matchButton, &loopButton, &monoButton, &syncBox, &bandBox,
                                                                     &addButton, &resetButton, &trackList, &waveform, &meters })
        addAndMakeVisible (*component);

    abAttachment    = std::make_unique<APVTS::ButtonAttachment> (processor.apvts, "ab", abButton);
    matchAttachment = std::make_unique<APVTS::ButtonAttachment> (processor.apvts, "match", matchButton);
    loopAttachment  = std::make_unique<APVTS::ButtonAttachment> (processor.apvts, "loop", loopButton);
    monoAttachment  = std::make_unique<APVTS::ButtonAttachment> (processor.apvts, "mono", monoButton);
    trimAttachment  = std::make_unique<APVTS::SliderAttachment> (processor.apvts, "trim", trimSlider);
    syncAttachment  = std::make_unique<APVTS::ComboBoxAttachment> (processor.apvts, "sync", syncBox);
    bandAttachment  = std::make_unique<APVTS::ComboBoxAttachment> (processor.apvts, "band", bandBox);
    updateABButton();

    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose reference songs", juce::File(), processor.getSupportedWildcard());
        const auto chooserFlags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::canSelectMultipleItems;

        chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
        {
            juce::StringArray paths;

            for (const auto& file : fc.getResults())
                paths.add (file.getFullPathName());

            addFiles (paths, -1);
        });
    };

    resetButton.onClick = [this]
    {
        processor.mixMeter.requestReset();
        processor.refMeter.requestReset();
    };

    processor.addChangeListener (this);
    setSize (940, 580);
    startTimerHz (30);
}

ReferenceABEditor::~ReferenceABEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void ReferenceABEditor::updateABButton()
{
    abButton.setButtonText (abButton.getToggleState() ? "B  -  REFERENCE" : "A  -  MIX");
}

void ReferenceABEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    g.setColour (Theme::text);
    g.setFont (font (20.0f, true));
    g.drawText ("Reference AB", 16, 12, 200, 28, juce::Justification::centredLeft);
    g.setColour (Theme::dimText);
    g.setFont (font (11.5f));
    g.drawText ("v" JucePlugin_VersionString, 16, 38, 200, 16, juce::Justification::centredLeft);
}

void ReferenceABEditor::paintOverChildren (juce::Graphics& g)
{
    if (dropHighlight)
    {
        g.setColour (Theme::reference.withAlpha (0.12f));
        g.fillAll();
        g.setColour (Theme::reference);
        g.drawRect (getLocalBounds(), 2);
    }
}

void ReferenceABEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    auto header = area.removeFromTop (52);
    header.removeFromLeft (190);
    abButton.setBounds (header.removeFromLeft (230).reduced (0, 2));
    header.removeFromLeft (20);
    matchButton.setBounds (header.removeFromLeft (120));
    trimLabel.setBounds (header.removeFromLeft (96));
    trimSlider.setBounds (header.removeFromLeft (190).reduced (0, 12));
    gainLabel.setBounds (header);

    area.removeFromTop (12);

    auto left = area.removeFromLeft (250);
    addButton.setBounds (left.removeFromBottom (30));
    left.removeFromBottom (8);
    trackList.setBounds (left);

    area.removeFromLeft (14);

    auto bottom = area.removeFromBottom (30);
    resetButton.setBounds (bottom.removeFromLeft (120));
    hintLabel.setBounds (bottom);
    area.removeFromBottom (8);

    meters.setBounds (area.removeFromBottom (120));
    area.removeFromBottom (10);

    auto controls = area.removeFromBottom (30);
    loopButton.setBounds (controls.removeFromLeft (120));
    syncLabel.setBounds (controls.removeFromLeft (40));
    syncBox.setBounds (controls.removeFromLeft (120));
    controls.removeFromLeft (20);
    bandLabel.setBounds (controls.removeFromLeft (48));
    bandBox.setBounds (controls.removeFromLeft (100));
    controls.removeFromLeft (20);
    monoButton.setBounds (controls.removeFromLeft (90));
    area.removeFromBottom (10);

    waveform.setBounds (area);
}

void ReferenceABEditor::timerCallback()
{
    const float gain = processor.getAppliedGainDb();
    gainLabel.setText ("Reference gain " + juce::String (gain > 0.0f ? "+" : "") + juce::String (gain, 1) + " dB",
                       juce::dontSendNotification);
    waveform.repaint();
    meters.repaint();
    trackList.repaint();
}

void ReferenceABEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

bool ReferenceABEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (processor.canLoad (juce::File (path)))
            return true;

    return false;
}

void ReferenceABEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropHighlight = false;
    addFiles (files, trackList.slotAt (trackList.getLocalPoint (this, juce::Point<int> (x, y))));
    repaint();
}

void ReferenceABEditor::addFiles (const juce::StringArray& paths, int preferredSlot)
{
    int firstLoaded = -1;

    for (const auto& path : paths)
    {
        const juce::File file (path);

        if (! processor.canLoad (file))
            continue;

        int slot = -1;

        if (preferredSlot >= 0)
        {
            // Dropped on a specific row: the first file goes exactly there.
            slot = preferredSlot;
            processor.loadFile (slot, file);
            preferredSlot = -1;
        }
        else
        {
            slot = processor.addFile (file);
        }

        if (slot < 0)
            break;      // all eight slots are full

        if (firstLoaded < 0)
            firstLoaded = slot;
    }

    if (firstLoaded >= 0)
        processor.selectSlot (firstLoaded);
}
