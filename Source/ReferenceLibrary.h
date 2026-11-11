#pragma once

#include <juce_core/juce_core.h>

/**
    A list of reference songs shared by every project and every instance of the
    plugin. Stored as one path per line in the user's application data folder,
    and re-read from disk whenever it is needed.
*/
class ReferenceLibrary
{
public:
    static constexpr int maxEntries = 200;

    static juce::File getFile()
    {
        auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

       #if JUCE_MAC
        folder = folder.getChildFile ("Application Support");
       #endif

        return folder.getChildFile ("ReferenceAB").getChildFile ("library.txt");
    }

    static juce::StringArray load()
    {
        juce::StringArray paths;
        paths.addLines (getFile().loadFileAsString());
        paths.trim();
        paths.removeEmptyStrings();
        paths.removeDuplicates (false);
        return paths;
    }

    static void save (const juce::StringArray& paths)
    {
        const auto file = getFile();
        file.getParentDirectory().createDirectory();
        file.replaceWithText (paths.joinIntoString ("\n") + "\n");
    }

    /** Adds a song (newest first). Does nothing if it is already there. */
    static void add (const juce::File& song)
    {
        auto paths = load();

        if (paths.contains (song.getFullPathName()))
            return;

        paths.insert (0, song.getFullPathName());

        while (paths.size() > maxEntries)
            paths.remove (paths.size() - 1);

        save (paths);
    }

    static void remove (const juce::String& path)
    {
        auto paths = load();
        paths.removeString (path);
        save (paths);
    }
};
