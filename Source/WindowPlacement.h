#pragma once

#include <juce_graphics/juce_graphics.h>

namespace AudioSim
{
    // The available area is in JUCE logical coordinates, with the native
    // window frame removed so the title bar also stays on the primary display.
    inline juce::Rectangle<int> getStartupWindowBounds (juce::Rectangle<int> saved,
                                                       juce::Rectangle<int> availableArea,
                                                       bool hasSavedPosition)
    {
        int width = juce::jmax (500, saved.getWidth());
        int height = juce::jmax (300, saved.getHeight());
        if (availableArea.isEmpty())
            return { 0, 0, width, height };

        width = juce::jmin (width, availableArea.getWidth());
        height = juce::jmin (height, availableArea.getHeight());
        const auto bounds = saved.withSize (width, height);

        if (hasSavedPosition && availableArea.contains (bounds))
            return bounds;

        return availableArea.withSizeKeepingCentre (width, height);
    }
}
