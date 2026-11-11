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
namespace
{
    juce::String signedDb (float value, int decimals = 1)
    {
        return (value > 0.0f ? "+" : "") + juce::String (value, decimals);
    }

    juce::String frequencyLabel (float hz)
    {
        if (hz < 1000.0f)
            return juce::String (juce::roundToInt (hz));

        const int hundreds = juce::roundToInt (hz / 100.0f);
        return (hundreds % 10 == 0 ? juce::String (hundreds / 10) : juce::String ((float) hundreds / 10.0f, 1)) + "k";
    }
}

AnalysisPanel::AnalysisPanel (ReferenceABProcessor& p) : processor (p)
{
    const char* names[numTabs] = { "Spectrum", "Balance", "Dynamics" };

    for (int i = 0; i < numTabs; ++i)
    {
        auto& button = tabButtons[(size_t) i];
        button.setButtonText (names[i]);
        button.setClickingTogglesState (true);
        button.setRadioGroupId (1001);
        button.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                | (i < numTabs - 1 ? juce::Button::ConnectedOnRight : 0));
        button.setColour (juce::TextButton::buttonColourId, Theme::background);
        button.setColour (juce::TextButton::buttonOnColourId, Theme::outline);
        button.setColour (juce::TextButton::textColourOffId, Theme::dimText);
        button.setColour (juce::TextButton::textColourOnId, Theme::text);
        button.onClick = [this, i] { setTab (i); };
        addAndMakeVisible (button);
    }

    mixHistory.fill (LoudnessMeter::silence);
    refHistory.fill (LoudnessMeter::silence);

    tab = juce::jlimit (0, numTabs - 1, (int) processor.apvts.state.getProperty ("analysisTab", 0));
    tabButtons[(size_t) tab].setToggleState (true, juce::dontSendNotification);
}

void AnalysisPanel::setTab (int newTab)
{
    tab = newTab;
    processor.apvts.state.setProperty ("analysisTab", tab, nullptr);
    repaint();
}

void AnalysisPanel::resized()
{
    auto row = getLocalBounds().reduced (10, 8).removeFromTop (24);

    for (auto& button : tabButtons)
        button.setBounds (row.removeFromLeft (96));
}

void AnalysisPanel::clearHistory()
{
    mixHistory.fill (LoudnessMeter::silence);
    refHistory.fill (LoudnessMeter::silence);
    historyWrite = 0;
    historyCount = 0;
}

void AnalysisPanel::tick()
{
    if (++tickCounter % 3 == 0)
    {
        const auto track = processor.getTrack (processor.getSelectedSlot());
        const bool hasReference = track != nullptr && track->hasAudio();
        const float refShortTerm = processor.refMeter.getShortTerm();

        mixHistory[(size_t) historyWrite] = processor.mixMeter.getShortTerm();
        refHistory[(size_t) historyWrite] = hasReference && refShortTerm > -70.0f
                                                ? refShortTerm + processor.getAppliedGainDb()
                                                : LoudnessMeter::silence;
        historyWrite = (historyWrite + 1) % historySize;
        historyCount = juce::jmin (historyCount + 1, historySize);
    }

    repaint();
}

void AnalysisPanel::paint (juce::Graphics& g)
{
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    processor.mixAnalyzer.getSnapshot (snapshot);
    Analyzer::analyse (snapshot, mix);

    const auto track = processor.getTrack (processor.getSelectedSlot());

    if (track != nullptr && track->hasAudio())
    {
        processor.refAnalyzer.getSnapshot (snapshot);
        Analyzer::analyse (snapshot, ref);
    }
    else
    {
        ref = {};
    }

    auto area = getLocalBounds().reduced (10, 8);
    auto top = area.removeFromTop (24);
    area.removeFromTop (6);

    // Legend, right of the tabs.
    g.setFont (font (12.0f));
    auto legend = top.removeFromRight (230);
    g.setColour (Theme::reference);
    g.fillRect (legend.removeFromRight (12).withSizeKeepingCentre (12, 3));
    g.drawText ("Reference", legend.removeFromRight (70), juce::Justification::centredRight);
    legend.removeFromRight (14);
    g.setColour (Theme::mix);
    g.fillRect (legend.removeFromRight (12).withSizeKeepingCentre (12, 3));
    g.drawText ("Mix", legend.removeFromRight (34), juce::Justification::centredRight);

    if (! mix.valid && ! ref.valid && tab != dynamicsTab)
    {
        g.setColour (Theme::dimText);
        g.setFont (font (14.0f));
        g.drawText ("Play your project for a few seconds to see the analysis", area, juce::Justification::centred);
        return;
    }

    if (tab == spectrumTab)       paintSpectrum (g, area);
    else if (tab == balanceTab)   paintBalance (g, area);
    else                          paintDynamics (g, area);
}

void AnalysisPanel::paintSpectrum (juce::Graphics& g, juce::Rectangle<int> area)
{
    const bool both = mix.valid && ref.valid;
    auto labels = area.removeFromBottom (14);
    auto difference = both ? area.removeFromBottom (juce::roundToInt ((float) area.getHeight() * 0.32f)) : juce::Rectangle<int>();

    if (both)
        area.removeFromBottom (6);

    const auto plot = area.toFloat();
    const float logRange = std::log (1000.0f);
    auto xFor = [&] (float hz) { return plot.getX() + plot.getWidth() * std::log (hz / 20.0f) / logRange; };

    // Vertical range follows the loudest point of either curve.
    float peak = -200.0f;

    for (int i = 0; i < Analyzer::Result::numPoints; ++i)
    {
        if (mix.valid) peak = juce::jmax (peak, mix.spectrumDb[(size_t) i]);
        if (ref.valid) peak = juce::jmax (peak, ref.spectrumDb[(size_t) i]);
    }

    const float topDb = std::ceil ((peak + 3.0f) / 6.0f) * 6.0f;
    const float bottomDb = topDb - 54.0f;
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (bottomDb, topDb, db), bottomDb, topDb, plot.getBottom(), plot.getY()); };

    g.setFont (font (10.5f));

    for (const float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = xFor (hz);
        g.setColour (Theme::grid);
        g.fillRect (x, plot.getY(), 1.0f, plot.getHeight() + (both ? (float) difference.getHeight() + 6.0f : 0.0f));
        g.setColour (Theme::dimText);
        g.drawText (frequencyLabel (hz), juce::Rectangle<float> (x - 20.0f, (float) labels.getY(), 40.0f, (float) labels.getHeight()),
                    hz <= 20.0f ? juce::Justification::centredRight : (hz >= 20000.0f ? juce::Justification::centredLeft : juce::Justification::centred));
    }

    for (float db = topDb - 6.0f; db > bottomDb; db -= 12.0f)
    {
        g.setColour (Theme::grid);
        g.fillRect (plot.getX(), yFor (db), plot.getWidth(), 1.0f);
    }

    auto makePath = [&] (const Analyzer::Result& result)
    {
        juce::Path path;
        bool started = false;

        for (int i = 0; i < Analyzer::Result::numPoints; ++i)
        {
            const float db = result.spectrumDb[(size_t) i];

            if (db < -150.0f)
                continue;

            const float x = xFor (Analyzer::Result::pointFrequency (i)), y = yFor (db);

            if (started)  path.lineTo (x, y);
            else          path.startNewSubPath (x, y);

            started = true;
        }

        return path;
    };

    g.saveState();
    g.reduceClipRegion (area);

    if (ref.valid)
    {
        const auto path = makePath (ref);
        juce::Path fill (path);
        fill.lineTo (path.getCurrentPosition().x, plot.getBottom());
        fill.lineTo (path.getBounds().getX(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (Theme::reference.withAlpha (0.16f));
        g.fillPath (fill);
        g.setColour (Theme::reference);
        g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
    }

    if (mix.valid)
    {
        g.setColour (Theme::mix);
        g.strokePath (makePath (mix), juce::PathStrokeType (1.8f, juce::PathStrokeType::curved));
    }

    g.restoreState();

    if (! both)
        return;

    // Difference strip: where the mix has more (above the line) or less than the reference.
    const auto strip = difference.toFloat();
    const float range = 12.0f;
    auto diffY = [&] (float db) { return juce::jmap (juce::jlimit (-range, range, db), -range, range, strip.getBottom(), strip.getY()); };

    g.setColour (Theme::background.withAlpha (0.5f));
    g.fillRect (strip);
    g.setColour (Theme::grid);
    g.fillRect (strip.getX(), diffY (6.0f), strip.getWidth(), 1.0f);
    g.fillRect (strip.getX(), diffY (-6.0f), strip.getWidth(), 1.0f);
    g.setColour (Theme::dimText);
    g.fillRect (strip.getX(), strip.getCentreY(), strip.getWidth(), 1.0f);

    juce::Path line;
    bool started = false;
    float firstX = 0.0f, lastX = 0.0f;

    for (int i = 0; i < Analyzer::Result::numPoints; ++i)
    {
        const float a = mix.spectrumDb[(size_t) i], b = ref.spectrumDb[(size_t) i];

        // Ignore the region where either signal has no real content (e.g. above an MP3's cutoff).
        if (a < bottomDb - 12.0f || b < bottomDb - 12.0f)
            continue;

        const float x = xFor (Analyzer::Result::pointFrequency (i)), y = diffY (a - b);

        if (started)  line.lineTo (x, y);
        else        { line.startNewSubPath (x, y); firstX = x; }

        started = true;
        lastX = x;
    }

    if (started)
    {
        juce::Path fill (line);
        fill.lineTo (lastX, strip.getCentreY());
        fill.lineTo (firstX, strip.getCentreY());
        fill.closeSubPath();
        g.setColour (Theme::text.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (Theme::text);
        g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
    }

    g.setFont (font (10.5f));
    g.setColour (Theme::dimText);
    g.drawText ("Mix minus reference, level-matched  (+/- 12 dB)", difference.reduced (6, 2), juce::Justification::topLeft);
}

juce::StringArray AnalysisPanel::buildSummary (const ReferenceTrack* track) const
{
    juce::StringArray lines;

    if (track == nullptr || ! track->hasAudio())
        return lines;

    const float mixLoudness = processor.mixMeter.getIntegrated();

    if (mixLoudness > -70.0f && track->integratedLufs > -70.0f)
    {
        const float d = mixLoudness - track->integratedLufs;
        lines.add (std::abs (d) < 0.5f
                       ? juce::String ("Loudness: the same as the reference")
                       : "Loudness: mix is " + juce::String (std::abs (d), 1) + " LU " + (d < 0.0f ? "quieter" : "louder")
                             + " (" + juce::String (mixLoudness, 1) + " vs " + juce::String (track->integratedLufs, 1) + " LUFS)");
    }

    if (mix.valid && ref.valid)
    {
        // The two tonal bands that differ the most.
        int first = -1, second = -1;

        for (int band = 0; band < Analyzer::Result::numToneBands; ++band)
        {
            const float d = std::abs (mix.toneDb[(size_t) band] - ref.toneDb[(size_t) band]);

            if (d < 1.5f)
                continue;

            if (first < 0 || d > std::abs (mix.toneDb[(size_t) first] - ref.toneDb[(size_t) first]))
            {
                second = first;
                first = band;
            }
            else if (second < 0 || d > std::abs (mix.toneDb[(size_t) second] - ref.toneDb[(size_t) second]))
            {
                second = band;
            }
        }

        auto describe = [this] (int band)
        {
            const float d = mix.toneDb[(size_t) band] - ref.toneDb[(size_t) band];
            return juce::String (Analyzer::toneBandNames()[(size_t) band]) + " is " + juce::String (std::abs (d), 1)
                       + " dB " + (d > 0.0f ? "stronger" : "weaker");
        };

        if (first < 0)
            lines.add ("Tone: close to the reference");
        else
            lines.add ("Tone: " + describe (first) + (second >= 0 ? ", " + describe (second) : juce::String()));

        const float sideDifference = mix.sideTotalPercent - ref.sideTotalPercent;
        lines.add (std::abs (sideDifference) < 4.0f
                       ? juce::String ("Stereo: width is close to the reference")
                       : juce::String ("Stereo: mix is ") + (sideDifference < 0.0f ? "narrower" : "wider") + " (side "
                             + juce::String (juce::roundToInt (mix.sideTotalPercent)) + "% vs "
                             + juce::String (juce::roundToInt (ref.sideTotalPercent)) + "%)");
    }

    const float mixPlr = processor.mixMeter.getPlr();
    const float refPlr = track->truePeakDb > -70.0f && track->integratedLufs > -70.0f ? track->truePeakDb - track->integratedLufs : 0.0f;

    if (mixPlr > 0.0f && refPlr > 0.0f)
    {
        const float d = mixPlr - refPlr;
        lines.add (std::abs (d) < 1.0f
                       ? juce::String ("Dynamics: similar to the reference")
                       : juce::String ("Dynamics: mix is ") + (d < 0.0f ? "more compressed" : "more dynamic") + " (PLR "
                             + juce::String (mixPlr, 1) + " vs " + juce::String (refPlr, 1) + " dB)");
    }

    return lines;
}

void AnalysisPanel::paintBalance (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto track = processor.getTrack (processor.getSelectedSlot());
    const bool both = mix.valid && ref.valid;

    auto summaryArea = area.removeFromRight (juce::roundToInt ((float) area.getWidth() * 0.36f));
    area.removeFromRight (16);
    auto toneArea = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * 0.56f));
    area.removeFromLeft (16);
    auto stereoArea = area;

    auto heading = [&] (juce::Rectangle<int>& column, const juce::String& title)
    {
        g.setColour (Theme::dimText);
        g.setFont (font (11.5f));
        g.drawText (title, column.removeFromTop (18), juce::Justification::centredLeft);
    };

    // Tonal balance: how much more or less of each band the mix has.
    heading (toneArea, both ? "TONE: MIX VS REFERENCE (dB)" : "TONE (needs a playing reference)");
    const int toneRow = toneArea.getHeight() / Analyzer::Result::numToneBands;
    const auto& edges = Analyzer::toneBandEdges();

    for (int band = 0; band < Analyzer::Result::numToneBands; ++band)
    {
        auto row = toneArea.removeFromTop (toneRow).reduced (0, 3);
        g.setColour (Theme::text);
        g.setFont (font (12.5f));
        g.drawText (Analyzer::toneBandNames()[(size_t) band], row.removeFromLeft (62), juce::Justification::centredLeft);
        g.setColour (Theme::dimText);
        g.setFont (font (10.5f));
        g.drawText (frequencyLabel (edges[(size_t) band]) + "-" + frequencyLabel (edges[(size_t) band + 1]),
                    row.removeFromLeft (52), juce::Justification::centredLeft);

        auto valueArea = row.removeFromRight (46);
        const auto bar = row.toFloat().reduced (0.0f, 3.0f);
        g.setColour (Theme::background);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (Theme::dimText);
        g.fillRect (bar.getCentreX() - 0.5f, bar.getY(), 1.0f, bar.getHeight());

        if (both)
        {
            const float d = mix.toneDb[(size_t) band] - ref.toneDb[(size_t) band];
            const float extent = juce::jlimit (-1.0f, 1.0f, d / 9.0f) * bar.getWidth() * 0.5f;
            const auto colour = std::abs (d) < 1.5f ? Theme::dimText : (std::abs (d) < 3.0f ? Theme::reference : Theme::warning);
            g.setColour (colour);
            g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (bar.getCentreX(), bar.getCentreX() + extent), bar.getY(),
                                                            std::abs (extent), bar.getHeight()), 2.0f);
            g.setColour (Theme::text);
            g.setFont (font (12.5f, true));
            g.drawText (signedDb (d), valueArea, juce::Justification::centredRight);
        }
    }

    // Stereo: share of the energy that sits in the side channel, per band.
    heading (stereoArea, "STEREO WIDTH (side %)");
    const int stereoRow = stereoArea.getHeight() / (Analyzer::Result::numStereoBands + 1);

    for (int band = 0; band <= Analyzer::Result::numStereoBands; ++band)
    {
        const bool isCorrelation = band == Analyzer::Result::numStereoBands;
        auto row = stereoArea.removeFromTop (stereoRow).reduced (0, 3);
        g.setColour (Theme::text);
        g.setFont (font (12.5f));
        g.drawText (isCorrelation ? "Correl." : Analyzer::stereoBandNames()[(size_t) band], row.removeFromLeft (50), juce::Justification::centredLeft);

        auto values = row.removeFromRight (74);
        auto upper = row.removeFromTop (row.getHeight() / 2).toFloat().reduced (0.0f, 1.5f);
        auto lower = row.toFloat().reduced (0.0f, 1.5f);

        auto drawBar = [&] (juce::Rectangle<float> bar, const Analyzer::Result& result, juce::Colour colour)
        {
            g.setColour (Theme::background);
            g.fillRoundedRectangle (bar, 2.0f);

            if (! result.valid)
                return;

            const float amount = isCorrelation ? juce::jlimit (0.0f, 1.0f, 0.5f * (result.correlation + 1.0f))
                                               : juce::jlimit (0.0f, 1.0f, result.sidePercent[(size_t) band] / 60.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, amount * bar.getWidth())), 2.0f);
        };

        drawBar (upper, mix, Theme::mix);
        drawBar (lower, ref, Theme::reference);

        auto text = [&] (const Analyzer::Result& result)
        {
            if (! result.valid)
                return juce::String ("-");

            return isCorrelation ? juce::String (result.correlation, 2)
                                 : juce::String (juce::roundToInt (result.sidePercent[(size_t) band])) + "%";
        };

        g.setFont (font (12.0f, true));
        g.setColour (Theme::mix);
        g.drawText (text (mix), values.removeFromLeft (37), juce::Justification::centredRight);
        g.setColour (Theme::reference);
        g.drawText (text (ref), values, juce::Justification::centredRight);
    }

    // Plain-language summary.
    heading (summaryArea, "SUMMARY");
    const auto lines = buildSummary (track.get());
    g.setFont (font (12.5f));

    if (lines.isEmpty())
    {
        g.setColour (Theme::dimText);
        g.drawFittedText ("Load a reference and play both for a few seconds.", summaryArea, juce::Justification::topLeft, 3);
        return;
    }

    const int lineHeight = juce::jmin (40, summaryArea.getHeight() / juce::jmax (1, lines.size()));

    for (const auto& line : lines)
    {
        const auto row = summaryArea.removeFromTop (lineHeight);
        const auto label = line.upToFirstOccurrenceOf (":", true, false);
        g.setColour (Theme::dimText);
        g.drawText (label, row.withHeight (15), juce::Justification::centredLeft);
        g.setColour (Theme::text);
        g.drawFittedText (line.fromFirstOccurrenceOf (": ", false, false), row.withTrimmedTop (15), juce::Justification::topLeft, 2);
    }
}

void AnalysisPanel::paintDynamics (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto track = processor.getTrack (processor.getSelectedSlot());
    const bool hasReference = track != nullptr && track->hasAudio();

    auto numbers = area.removeFromRight (230);
    area.removeFromRight (16);

    // Short-term loudness over the last minute. The reference is shown as heard (after level matching).
    g.setColour (Theme::dimText);
    g.setFont (font (11.5f));
    g.drawText ("SHORT-TERM LOUDNESS, LAST 60 s (LUFS)", area.removeFromTop (18), juce::Justification::centredLeft);

    auto axis = area.removeFromLeft (28);
    const auto plot = area.toFloat();

    float topLufs = -6.0f;

    for (int i = 0; i < historySize; ++i)
        topLufs = juce::jmax (topLufs, mixHistory[(size_t) i], refHistory[(size_t) i]);

    topLufs = std::ceil (topLufs / 6.0f) * 6.0f;
    const float bottomLufs = topLufs - 36.0f;
    auto yFor = [&] (float lufs) { return juce::jmap (juce::jlimit (bottomLufs, topLufs, lufs), bottomLufs, topLufs, plot.getBottom(), plot.getY()); };

    g.setColour (Theme::background.withAlpha (0.5f));
    g.fillRect (plot);
    g.setFont (font (10.5f));

    for (float lufs = topLufs; lufs >= bottomLufs; lufs -= 6.0f)
    {
        const float y = yFor (lufs);
        g.setColour (Theme::grid);
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);
        g.setColour (Theme::dimText);
        g.drawText (juce::String ((int) lufs), juce::Rectangle<float> ((float) axis.getX(), y - 7.0f, (float) axis.getWidth() - 5.0f, 14.0f),
                    juce::Justification::centredRight);
    }

    auto drawHistory = [&] (const std::array<float, historySize>& values, juce::Colour colour)
    {
        juce::Path path;
        bool started = false;

        for (int i = 0; i < historyCount; ++i)
        {
            const int index = (historyWrite - historyCount + i + historySize) % historySize;
            const float value = values[(size_t) index];

            if (value <= -70.0f)
            {
                started = false;
                continue;
            }

            const float x = plot.getRight() - plot.getWidth() * (float) (historyCount - 1 - i) / (float) (historySize - 1);
            const float y = yFor (value);

            if (started)  path.lineTo (x, y);
            else          path.startNewSubPath (x, y);

            started = true;
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.6f));
    };

    g.saveState();
    g.reduceClipRegion (area);
    drawHistory (refHistory, Theme::reference);
    drawHistory (mixHistory, Theme::mix);
    g.restoreState();

    // Numbers.
    const float mixPsr = processor.mixMeter.getPsr(), mixPlr = processor.mixMeter.getPlr();
    const float refPsr = hasReference ? processor.refMeter.getPsr() : 0.0f;
    const float refPlr = hasReference && track->truePeakDb > -70.0f && track->integratedLufs > -70.0f
                             ? track->truePeakDb - track->integratedLufs : 0.0f;

    auto value = [] (float v) { return v > 0.0f ? juce::String (v, 1) : juce::String ("-"); };

    auto block = [&] (const juce::String& title, const juce::String& hint, float mixValue, float refValue)
    {
        auto box = numbers.removeFromTop (numbers.getHeight() > 150 ? 84 : numbers.getHeight());
        g.setColour (Theme::dimText);
        g.setFont (font (11.5f));
        g.drawText (title, box.removeFromTop (18), juce::Justification::centredLeft);
        auto row = box.removeFromTop (30);
        g.setFont (font (22.0f, true));
        g.setColour (Theme::mix);
        g.drawText (value (mixValue), row.removeFromLeft (row.getWidth() / 2), juce::Justification::centredLeft);
        g.setColour (Theme::reference);
        g.drawText (value (refValue), row, juce::Justification::centredLeft);
        g.setColour (Theme::dimText);
        g.setFont (font (10.5f));
        g.drawFittedText (hint, box.removeFromTop (28), juce::Justification::topLeft, 2);
    };

    block ("PSR: PUNCH RIGHT NOW (dB)", "Peak to short-term loudness. Under about 8 means heavily squashed.", mixPsr, refPsr);
    block ("PLR: WHOLE SONG (dB)", "True peak to integrated loudness. Lower means more limiting.", mixPlr, refPlr);
}

//==============================================================================
ReferenceABEditor::ReferenceABEditor (ReferenceABProcessor& p)
    : AudioProcessorEditor (&p), processor (p), trackList (p), waveform (p), meters (p), analysis (p)
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
                                                                     &addButton, &resetButton, &trackList, &waveform, &meters, &analysis })
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
        processor.resetMeasurements();
        analysis.clearHistory();
    };

    processor.addChangeListener (this);
    setSize (960, 720);
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

    meters.setBounds (area.removeFromBottom (100));
    area.removeFromBottom (10);
    analysis.setBounds (area.removeFromBottom (256));
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
    analysis.tick();
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
