// Patchy — Custom Standalone Application

#if JUCE_STANDALONE_APPLICATION

#include "StandaloneApp.h"
#include "SimulationDriver.h"
#include "WindowPlacement.h"

// ─────────────────────────────────────────────────────────────────────────────
// StandaloneWindow
// ─────────────────────────────────────────────────────────────────────────────
StandaloneWindow::StandaloneWindow()
    : DocumentWindow (juce::String (AudioSim::appName) + "  v" + juce::String (JucePlugin_VersionString),
                      juce::Desktop::getInstance().getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);

    processor = std::make_unique<PatchyProcessor>();
    processor->isStandalone = true;
    simulation = std::make_unique<SimulationDriver> (*processor);

    // Create editor via JUCE mechanism so getActiveEditor() works
    editor = dynamic_cast<PatchyEditor*> (processor->createEditorIfNeeded());

    // Wire bridge callbacks
    auto& bridge = editor->getBridge();
    bridge.isStandalone = true;
    processor->isStandalone = true;

    // Push standalone mode + audio settings when UI is ready
    bridge.onUIReady = [this, &bridge]()
    {
        bridge.pushToUI ("onStandaloneMode", "true");
        pushAudioSettingsToUI();
    };

    bridge.onSetAudioEngineSettings = [this](double sr, int buf, bool mute)
    {
        AudioSettings s;
        s.sampleRate   = sr;
        s.bufferSize   = buf;
        s.muteFeedback = mute;
        applyAudioSettings (s);
    };

    setContentNonOwned (editor, true);

    // Initialise settings storage
    juce::PropertiesFile::Options opts;
    opts.applicationName     = AudioSim::appName;
    opts.filenameSuffix      = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters (opts);

    restoreWindowBounds();
    setVisible (true);
    restoreAudioSettings();
}

StandaloneWindow::~StandaloneWindow()
{
    simulation->stop();
    simulation.reset();
    clearContentComponent();
    if (editor)
    {
        processor->editorBeingDeleted (editor);
        delete editor;
        editor = nullptr;
    }
    processor.reset();
}

void StandaloneWindow::applyAudioSettings (const AudioSettings& requested)
{
    simulation->stop();
    audioSettings = requested;
    audioSettings.sampleRate = std::isfinite (requested.sampleRate)
        ? juce::jlimit (AudioSim::minSampleRate, AudioSim::maxSampleRate, requested.sampleRate) : 44100.0;
    audioSettings.bufferSize = juce::jlimit (AudioSim::minBlockSize, AudioSim::maxBlockSize, requested.bufferSize);
    // Cancel queued driver probes before changing the engine to a simulation
    // rate. An in-flight driver call may finish, but cannot start another.
    if (editor && ! AudioSim::isHardwareSampleRate (audioSettings.sampleRate))
        editor->getBridge().stopAudioDeviceDiscovery (audioSettings.sampleRate);
    processor->getAudioDeviceManager().setMuteFeedback (audioSettings.muteFeedback);
    processor->configureSimulation (audioSettings.sampleRate, audioSettings.bufferSize);
    simulation->start (audioSettings.sampleRate, audioSettings.bufferSize);
    saveAudioSettings();
    pushAudioSettingsToUI();
    if (editor) editor->getBridge().pushAudioDevices();
}

juce::StringArray StandaloneWindow::getAvailableSampleRates() const
{
    return { "1000", "1500", "2000", "4000", "8000", "11025", "16000", "22050", "24000", "32000", "44100", "48000", "64000", "88200", "96000", "192000" };
}
juce::StringArray StandaloneWindow::getAvailableBufferSizes() const
{
    return { "16", "24", "32", "48", "64", "96", "128", "192", "256", "512", "1024", "2048", "4096" };
}
void StandaloneWindow::pushAudioSettingsToUI()
{
    if (editor == nullptr) return;
    auto* settings = new juce::DynamicObject();
    settings->setProperty ("sampleRate", audioSettings.sampleRate);
    settings->setProperty ("bufferSize", audioSettings.bufferSize);
    settings->setProperty ("hardwareEnabled", AudioSim::isHardwareSampleRate (audioSettings.sampleRate));
    settings->setProperty ("maxChannels", AudioSim::maxAudioChannels);
    settings->setProperty ("muteFeedback", audioSettings.muteFeedback);
    juce::Array<juce::var> rates, sizes;
    for (const auto& rate : getAvailableSampleRates()) rates.add (rate.getIntValue());
    for (const auto& size : getAvailableBufferSizes()) sizes.add (size.getIntValue());
    settings->setProperty ("availableSampleRates", rates);
    settings->setProperty ("availableBufferSizes", sizes);
    editor->getBridge().pushToUI ("onAudioSettings", juce::JSON::toString (juce::var (settings), true));
}

void StandaloneWindow::saveWindowBounds()
{
    if (auto* p = appProperties.getUserSettings())
    {
        p->setValue ("windowX",      getX());
        p->setValue ("windowY",      getY());
        p->setValue ("windowWidth",  getWidth());
        p->setValue ("windowHeight", getHeight());
        // Persist last open directory so file dialogs remember location
        auto& bridge = editor->getBridge();
        if (bridge.getLastOpenDir().isDirectory())
            p->setValue ("lastOpenDir", bridge.getLastOpenDir().getFullPathName());
        p->saveIfNeeded();
    }
}

void StandaloneWindow::restoreWindowBounds()
{
    juce::Rectangle<int> savedBounds (0, 0, 1200, 750);
    bool hasSavedPosition = false;
    if (auto* p = appProperties.getUserSettings())
    {
        savedBounds = { p->getIntValue ("windowX", 0),
                        p->getIntValue ("windowY", 0),
                        p->getIntValue ("windowWidth", 1200),
                        p->getIntValue ("windowHeight", 750) };
        hasSavedPosition = p->containsKey ("windowX") && p->containsKey ("windowY");
        // Restore last open directory
        auto lastDir = juce::File (p->getValue ("lastOpenDir", ""));
        if (lastDir.isDirectory())
            editor->getBridge().setLastOpenDir (lastDir);
    }

    juce::Rectangle<int> availableArea;
    if (const auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        availableArea = primary->userArea;
        if (const auto* peer = getPeer())
            availableArea = peer->getFrameSize().subtractedFrom (availableArea);
    }

    // Old secondary-display coordinates must not move the window off the
    // primary display, including after a monitor or DPI configuration change.
    setBounds (AudioSim::getStartupWindowBounds (savedBounds, availableArea, hasSavedPosition));
}

void StandaloneWindow::closeButtonPressed()
{
    saveWindowBounds();
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void StandaloneWindow::saveAudioSettings()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue ("simulationSampleRate", audioSettings.sampleRate);
        settings->setValue ("audioBufferSize", audioSettings.bufferSize);
        settings->removeValue ("simulationChannels");
        settings->setValue ("muteFeedback", audioSettings.muteFeedback);
        settings->saveIfNeeded();
    }
}
void StandaloneWindow::restoreAudioSettings()
{
    AudioSettings restored;
    if (auto* settings = appProperties.getUserSettings())
    {
        restored.sampleRate = settings->getDoubleValue ("simulationSampleRate", restored.sampleRate);
        restored.bufferSize = settings->getIntValue ("audioBufferSize", restored.bufferSize);
        restored.muteFeedback = settings->getBoolValue ("muteFeedback", restored.muteFeedback);
    }
    applyAudioSettings (restored);
}

void StandaloneApp::initialise (const juce::String&)
{
    mainWindow = std::make_unique<StandaloneWindow>();
}

void StandaloneApp::shutdown()
{
    mainWindow.reset();
}

START_JUCE_APPLICATION (StandaloneApp)

#endif // JUCE_STANDALONE_APPLICATION
