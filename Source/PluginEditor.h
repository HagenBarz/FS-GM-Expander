/*
  ==============================================================================

    FS-GM-Expander | FluidSynth General MIDI Expander | PluginEditor.h

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>
#include <memory>

//==============================================================================
class FSGMExpanderAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit FSGMExpanderAudioProcessorEditor (FSGMExpanderAudioProcessor&);
    ~FSGMExpanderAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;

private:
    void timerCallback() override;

    void selectChannel (int ch);
    void setGlobalFxView (bool showGlobal);
    void updateRightPanelVisibility();

    void openSoundFontFileChooser();
    void rebuildSoundFontFolderCombo();
    void stepSoundFontInFolder (int delta);
    void rebuildBankAndPresetLists();
    void rebuildPresetComboForChannel (int ch);
    void syncGuiFromProcessor();
    void selectPresetForChannel (int ch, int bank, int program);
    void stepPresetForChannel (int ch, int delta);

    struct ChannelRowWidgets
    {
        juce::TextButton selectButton;
        juce::TextButton muteButton { "M" };
        juce::TextButton soloButton { "S" };
        juce::TextButton prevPresetButton { "<" };
        juce::ComboBox presetCombo;
        juce::TextButton nextPresetButton { ">" };
        juce::Slider volumeSlider;
        juce::Rectangle<float> ledBounds;
        float ledLevel = 0.0f;
    };

    FSGMExpanderAudioProcessor& audioProcessor;

    int selectedChannel = 0;       // 0..15
    bool showingGlobalFx = false;  // false = Channel Inspector, true = Global FX Inspector

    // --- Top Bar Controls ---
    juce::TextButton loadSfButton { "Browse..." };
    juce::TextButton prevSfButton { "<" };
    juce::ComboBox sfFolderCombo;
    juce::TextButton nextSfButton { ">" };

    juce::TextButton gmResetButton { "GM Reset" };
    juce::TextButton panicButton { "Panic!" };
    juce::TextButton auditionButton { "Test Note" };
    juce::TextButton allToAuxButton { "All->Aux" };
    juce::TextButton globalFxToggleButton { "Global FX" };

    juce::Label masterGainLabel;
    juce::Slider masterGainSlider;
    juce::Label reverbLevelLabel;
    juce::Slider reverbLevelSlider;
    juce::Label chorusLevelLabel;
    juce::Slider chorusLevelSlider;

    juce::Rectangle<int> meterArea;
    float displayPeakL = 0.0f;
    float displayPeakR = 0.0f;
    bool displayClipped = false;

    // --- Left 16-Channel Rack ---
    std::array<ChannelRowWidgets, 16> rows;

    // --- Right Panel: Channel Inspector ---
    juce::Label chModeLabel;
    juce::TextButton chMelodicButton { "Melodic" };
    juce::TextButton chDrumButton { "Drum" };

    juce::Label chOutBusLabel;
    juce::TextButton chMasterOutButton { "Master (1/2)" };
    juce::TextButton chAuxOutButton { "Aux (CH 01)" };

    juce::Label chBankLabel;
    juce::ComboBox chBankCombo;
    juce::Label chInterpLabel;
    juce::ComboBox chInterpCombo;

    juce::Label chPanLabel;
    juce::Slider chPanSlider;
    juce::Label chExpressionLabel;
    juce::Slider chExpressionSlider;
    juce::Label chReverbLabel;
    juce::Slider chReverbSlider;
    juce::Label chChorusLabel;
    juce::Slider chChorusSlider;

    juce::Label chCoarseLabel;
    juce::Slider chCoarseSlider;
    juce::Label chFineLabel;
    juce::Slider chFineSlider;
    juce::Label chBendRangeLabel;
    juce::Slider chBendRangeSlider;
    juce::Label chVoiceModeLabel;
    juce::TextButton chPolyButton { "Poly" };
    juce::TextButton chMonoButton { "Mono" };
    juce::Label chPortaLabel;
    juce::Slider chPortaSlider;

    juce::TextButton chResetShapingButton { "Reset" };
    juce::Label chCutoffLabel;
    juce::Slider chCutoffSlider;
    juce::Label chResonanceLabel;
    juce::Slider chResonanceSlider;
    juce::Label chAttackLabel;
    juce::Slider chAttackSlider;
    juce::Label chDecayLabel;
    juce::Slider chDecaySlider;
    juce::Label chReleaseLabel;
    juce::Slider chReleaseSlider;

    // --- Right Panel: Global FX & Synth Inspector ---
    juce::TextButton fxReverbEnableButton { "Reverb Active" };
    juce::Label fxReverbLevelLabel;
    juce::Slider fxReverbLevelSlider;
    juce::Label fxReverbRoomLabel;
    juce::Slider fxReverbRoomSlider;
    juce::Label fxReverbDampLabel;
    juce::Slider fxReverbDampSlider;
    juce::Label fxReverbWidthLabel;
    juce::Slider fxReverbWidthSlider;

    juce::TextButton fxChorusEnableButton { "Chorus Active" };
    juce::Label fxChorusLevelLabel;
    juce::Slider fxChorusLevelSlider;
    juce::Label fxChorusVoicesLabel;
    juce::Slider fxChorusVoicesSlider;
    juce::Label fxChorusSpeedLabel;
    juce::Slider fxChorusSpeedSlider;
    juce::Label fxChorusDepthLabel;
    juce::Slider fxChorusDepthSlider;
    juce::Label fxChorusTypeLabel;
    juce::ComboBox fxChorusTypeCombo;

    juce::Label fxPolyphonyLabel;
    juce::Slider fxPolyphonySlider;
    juce::Label fxEngineStatusLabel;

    std::vector<juce::File> folderSoundFonts;
    std::vector<PresetInfo> cachedPresets;
    std::vector<int> cachedBanks;
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool isUpdatingGui = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FSGMExpanderAudioProcessorEditor)
};
