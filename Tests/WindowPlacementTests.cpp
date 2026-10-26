#include "WindowPlacement.h"

class WindowPlacementTests : public juce::UnitTest
{
public:
    WindowPlacementTests() : UnitTest ("Startup window placement", "AudioSim") {}

    void runTest() override
    {
        using Rect = juce::Rectangle<int>;
        const Rect primary (8, 31, 1520, 873);

        beginTest ("First launch centres on the primary work area");
        const auto first = AudioSim::getStartupWindowBounds ({ 0, 0, 1200, 750 }, primary, false);
        expect (primary.contains (first));
        expect (first.getCentre() == primary.getCentre());
        expectEquals (first.getWidth(), 1200);
        expectEquals (first.getHeight(), 750);

        beginTest ("Previously saved secondary-display window fits the primary display");
        // Actual coordinates reported by the affected installation.
        const auto restored = AudioSim::getStartupWindowBounds ({ 1622, 286, 1536, 889 }, primary, true);
        expect (restored == primary);

        beginTest ("Secondary displays to the left or above also return to primary");
        for (const auto& saved : { Rect (-1600, 80, 900, 600), Rect (50, -900, 900, 600),
                                  Rect (2100, 100, 900, 600), Rect (50, 1100, 900, 600) })
        {
            const auto bounds = AudioSim::getStartupWindowBounds (saved, primary, true);
            expect (primary.contains (bounds));
            expect (bounds.getCentre() == primary.getCentre());
            expectEquals (bounds.getWidth(), 900);
            expectEquals (bounds.getHeight(), 600);
        }

        beginTest ("A saved position fully inside primary is retained");
        const Rect saved (100, 90, 1000, 650);
        expect (AudioSim::getStartupWindowBounds (saved, primary, true) == saved);

        beginTest ("A window spanning displays or behind the taskbar is contained");
        for (const auto& savedBounds : { Rect (1200, 80, 900, 600), Rect (100, 750, 900, 600) })
            expect (primary.contains (AudioSim::getStartupWindowBounds (savedBounds, primary, true)));

        beginTest ("Small or scaled work areas take precedence over minimum size");
        const Rect small (0, 40, 450, 240);
        expect (AudioSim::getStartupWindowBounds ({ 0, 0, 1200, 750 }, small, false) == small);
        const auto minimum = AudioSim::getStartupWindowBounds ({ 100, 100, 0, -10 }, primary, true);
        expectEquals (minimum.getWidth(), 500);
        expectEquals (minimum.getHeight(), 300);

        beginTest ("Work area offsets and negative coordinates are supported");
        const Rect offset (-1500, -800, 1400, 760);
        const Rect negativeSaved (-1400, -750, 900, 600);
        expect (AudioSim::getStartupWindowBounds (negativeSaved, offset, true) == negativeSaved);
        expect (offset.contains (AudioSim::getStartupWindowBounds ({ 2000, 0, 1200, 750 }, offset, true)));
    }
};

static WindowPlacementTests windowPlacementTests;
