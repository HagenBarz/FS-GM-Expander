/*
  ==============================================================================

    FS-GM-Expander | FluidSynth General MIDI Expander | PluginProcessor.h

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <fluidsynth.h>
#include <array>
#include <vector>
#include <atomic>
#include <memory>

//==============================================================================
struct PresetInfo
{
    int bank = 0;
    int program = 0;
    juce::String name;
};

struct ChannelState
{
    bool isDrum = false;            // false = Melodic, true = Drum (default true on Ch 10)
    bool routeToAux = false;        // false = Master (Out 1/2), true = Aux CH 01..16
    int bank = 0;                   // 0 for Ch 1-9 & 11-16, 128 for Ch 10
    int program = 0;                // 0-127
    int volume = 100;               // CC 7 (0-127)
    int pan = 64;                   // CC 10 (0-127, 64 = Center)
    int expression = 127;           // CC 11 (0-127)
    int reverb = 40;                // CC 91 (0-127)
    int chorus = 0;                 // CC 93 (0-127)

    // Pitch & Performance
    int coarseTune = 0;             // -48 .. +48 semitones (GEN_COARSETUNE)
    int fineTune = 0;               // -99 .. +99 cents (GEN_FINETUNE)
    int pitchBendRange = 2;         // 0 .. 24 semitones
    bool monoMode = false;          // false = Poly, true = Mono
    int portamentoTime = 0;         // CC 5 (0-127, >0 enables CC 65)
    int interpMethod = FLUID_INTERP_DEFAULT; // fluid_interp enum

    // Sound Controllers (64 = neutral / 0 offset)
    int cutoff = 64;                // CC 74 (0-127)
    int resonance = 64;             // CC 71 (0-127)
    int attack = 64;                // CC 73 (0-127)
    int decay = 64;                 // CC 75 (0-127)
    int release = 64;               // CC 72 (0-127)

    bool mute = false;
    bool solo = false;
    std::atomic<float> midiActivity { 0.0f };

    void resetToDefault (int channelIndex)
    {
        isDrum         = (channelIndex == 9);
        routeToAux     = false;
        bank           = isDrum ? 128 : 0;
        program        = 0;
        volume         = 100;
        pan            = 64;
        expression     = 127;
        reverb         = 40;
        chorus         = 0;
        coarseTune     = 0;
        fineTune       = 0;
        pitchBendRange = 2;
        monoMode       = false;
        portamentoTime = 0;
        interpMethod   = FLUID_INTERP_DEFAULT;
        cutoff         = 64;
        resonance      = 64;
        attack         = 64;
        decay          = 64;
        release        = 64;
        mute           = false;
        solo           = false;
    }
};

struct GlobalFxState
{
    float masterGain = 0.5f;        // 0.0 .. 2.0
    int polyphony = 256;            // 16 .. 512

    bool reverbActive = true;
    float reverbLevel = 0.5f;       // 0.0 .. 1.0
    float reverbRoomSize = 0.6f;    // 0.0 .. 1.0
    float reverbDamp = 0.3f;        // 0.0 .. 1.0
    float reverbWidth = 0.8f;       // 0.0 .. 100.0 (FluidSynth width 0..100)

    bool chorusActive = true;
    float chorusLevel = 0.5f;       // 0.0 .. 1.0
    int chorusVoices = 3;           // 1 .. 16
    float chorusSpeed = 0.3f;       // 0.1 .. 5.0 Hz
    float chorusDepth = 8.0f;       // 0.0 .. 21.0 ms
    int chorusType = FLUID_CHORUS_MOD_SINE; // 0 = Sine, 1 = Triangle
};

//==============================================================================
class FSGMExpanderAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    FSGMExpanderAudioProcessor();
    ~FSGMExpanderAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // SoundFont & Presets
    bool loadSoundFont (const juce::File& file);
    juce::String getCurrentSoundFontPath() const;
    juce::String getCurrentSoundFontName() const;
    juce::File getLastSoundFontFolder() const;
    std::vector<juce::File> getAvailableSoundFontsInCurrentFolder() const;
    std::vector<PresetInfo> getPresets() const;
    std::vector<int> getAvailableBanks() const;

    void gmReset();
    void resetChannelSoundShaping (int channelIndex);
    void panic();
    void triggerAuditionNote (int channelIndex);

    // Channel Accessors & Setters
    const ChannelState& getChannelState (int channelIndex) const { return channels[static_cast<size_t> (juce::jlimit (0, 15, channelIndex))]; }
    ChannelState& getChannelState (int channelIndex)             { return channels[static_cast<size_t> (juce::jlimit (0, 15, channelIndex))]; }

    void setChannelDrumMode (int channelIndex, bool isDrum);
    void setChannelRouteToAux (int channelIndex, bool routeToAux);
    void setAllChannelsRouteToAux (bool routeToAux);
    bool areAllChannelsRoutedToAux() const;

    void setChannelBank (int channelIndex, int bank);
    void setChannelPreset (int channelIndex, int bank, int program);
    void setChannelVolume (int channelIndex, int volume);
    void setChannelPan (int channelIndex, int pan);
    void setChannelExpression (int channelIndex, int expression);
    void setChannelReverb (int channelIndex, int reverb);
    void setChannelChorus (int channelIndex, int chorus);

    void setChannelCoarseTune (int channelIndex, int semitones);
    void setChannelFineTune (int channelIndex, int cents);
    void setChannelPitchBendRange (int channelIndex, int semitones);
    void setChannelMonoMode (int channelIndex, bool mono);
    void setChannelPortamentoTime (int channelIndex, int time);
    void setChannelInterpMethod (int channelIndex, int interpMethod);

    void setChannelCutoff (int channelIndex, int val);
    void setChannelResonance (int channelIndex, int val);
    void setChannelAttack (int channelIndex, int val);
    void setChannelDecay (int channelIndex, int val);
    void setChannelRelease (int channelIndex, int val);

    void setChannelMute (int channelIndex, bool shouldMute);
    void setChannelSolo (int channelIndex, bool shouldSolo);

    // Global FX & Synth Engine
    const GlobalFxState& getGlobalFxState() const { return globalFx; }
    void setMasterGain (float newGain);
    void setPolyphony (int voices);

    void setReverbActive (bool active);
    void setReverbLevel (float newLevel);
    void setReverbRoomSize (float roomSize);
    void setReverbDamp (float damp);
    void setReverbWidth (float width);

    void setChorusActive (bool active);
    void setChorusLevel (float newLevel);
    void setChorusVoices (int voices);
    void setChorusSpeed (float speedHz);
    void setChorusDepth (float depthMs);
    void setChorusType (int type);

    int getActiveVoiceCount() const;
    double getCpuLoad() const;
    double getCurrentEngineSampleRate() const { return currentSampleRate; }

    // Master Peak Metering
    float consumeMasterPeakLeft()  { return masterPeakL.exchange (0.0f, std::memory_order_relaxed); }
    float consumeMasterPeakRight() { return masterPeakR.exchange (0.0f, std::memory_order_relaxed); }
    bool hasMasterClipped() const  { return masterClipped.load (std::memory_order_relaxed); }
    void clearMasterClip()         { masterClipped.store (false, std::memory_order_relaxed); }

    bool checkAndClearGuiUpdateFlag()
    {
        return stateChangedForGui.exchange (false, std::memory_order_acquire);
    }

private:
    //==============================================================================
    static BusesProperties createBusesProperties();
    void initPropertiesFile();
    void saveLastSoundFontToProperties (const juce::File& file);
    void initFluidSynth (double sampleRate);
    void cleanupFluidSynth();
    void applyGlobalFxToSynth();
    void applyAllChannelStatesToSynth();
    void applyChannelStateToSynth (int channelIndex);
    void applyChannelGeneratorsToSynth (int channelIndex);
    void syncChannelStatesFromSynth();
    void updatePresetListFromLoadedSoundFont();
    bool isChannelAudible (int channelIndex) const;
    void handleMidiEvent (const juce::MidiMessage& msg);
    void renderFluidSynthSegment (juce::AudioBuffer<float>& buffer, int startSample, int numFrames);
    void stopActiveAuditionNotes();

    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    mutable juce::CriticalSection synthLock;

    int currentSfontId = -1;
    juce::String currentSfontPath;
    juce::String lastSoundFontFolder;
    std::vector<PresetInfo> presets;

    std::unique_ptr<juce::PropertiesFile> appProperties;

    std::array<ChannelState, 16> channels;
    GlobalFxState globalFx;

    juce::AudioBuffer<float> multiOutScratch;

    // Audition state
    int auditionChannel = -1;
    std::array<int, 3> auditionNotes { -1, -1, -1 };
    int auditionSamplesRemaining = 0;

    // Master metering
    std::atomic<float> masterPeakL { 0.0f };
    std::atomic<float> masterPeakR { 0.0f };
    std::atomic<bool> masterClipped { false };

    double currentSampleRate = 44100.0;
    std::atomic<bool> stateChangedForGui { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FSGMExpanderAudioProcessor)
};
