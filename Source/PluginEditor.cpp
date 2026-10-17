/*
  ==============================================================================

    FS-GM-Expander | FluidSynth General MIDI Expander | PluginEditor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    int encodePresetItemId (int bank, int program)
    {
        return ((bank & 0x3fff) << 8) | (program & 0x7f) | 0x400000;
    }

    void decodePresetItemId (int itemId, int& bankOut, int& programOut)
    {
        bankOut    = (itemId >> 8) & 0x3fff;
        programOut = itemId & 0x7f;
    }

    juce::String formatProgramOnlyLabel (int program, const juce::String& name)
    {
        return juce::String::formatted ("%03d: ", program) + name;
    }

    juce::String formatBankLabel (int bank)
    {
        if (bank == 0)
            return "Bank 000 (GM Melodic)";
        if (bank == 128)
            return "Bank 128 (Drums)";
        return juce::String::formatted ("Bank %03d (Variation)", bank);
    }

    juce::String formatPanText (double val)
    {
        const int pan = static_cast<int> (std::round (val)) - 64;
        if (pan == 0)
            return "C";
        if (pan < 0)
            return "L" + juce::String (-pan);
        return "R" + juce::String (pan);
    }

    juce::String formatSignedOffset (double val)
    {
        const int diff = static_cast<int> (std::round (val)) - 64;
        if (diff > 0)
            return "+" + juce::String (diff);
        return juce::String (diff);
    }
}

//==============================================================================
FSGMExpanderAudioProcessorEditor::FSGMExpanderAudioProcessorEditor (FSGMExpanderAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // --- Top Bar Controls ---
    loadSfButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2b384e));
    loadSfButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadSfButton.setTooltip ("Browse for a SoundFont (.sf2 / .sf3) file...");
    loadSfButton.onClick = [this] { openSoundFontFileChooser(); };
    addAndMakeVisible (loadSfButton);

    prevSfButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff232a36));
    prevSfButton.setTooltip ("Previous SoundFont in folder");
    prevSfButton.onClick = [this] { stepSoundFontInFolder (-1); };
    addAndMakeVisible (prevSfButton);

    sfFolderCombo.setEditableText (false);
    sfFolderCombo.setJustificationType (juce::Justification::centredLeft);
    sfFolderCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff141820));
    sfFolderCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2d3545));
    sfFolderCombo.setColour (juce::ComboBox::textColourId,       juce::Colour (0xff7ee0ff));
    sfFolderCombo.onChange = [this]
    {
        if (isUpdatingGui)
            return;

        const int idx = sfFolderCombo.getSelectedId() - 1;
        if (idx >= 0 && idx < static_cast<int> (folderSoundFonts.size()))
        {
            if (audioProcessor.loadSoundFont (folderSoundFonts[static_cast<size_t> (idx)]))
            {
                rebuildSoundFontFolderCombo();
                rebuildBankAndPresetLists();
                syncGuiFromProcessor();
            }
        }
    };
    addAndMakeVisible (sfFolderCombo);

    nextSfButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff232a36));
    nextSfButton.setTooltip ("Next SoundFont in folder");
    nextSfButton.onClick = [this] { stepSoundFontInFolder (+1); };
    addAndMakeVisible (nextSfButton);

    gmResetButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a313c));
    gmResetButton.setTooltip ("Reset all 16 channels to General MIDI defaults");
    gmResetButton.onClick = [this]
    {
        audioProcessor.gmReset();
        syncGuiFromProcessor();
    };
    addAndMakeVisible (gmResetButton);

    panicButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff5a2328));
    panicButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffffd0d0));
    panicButton.setTooltip ("All Notes & Sounds Off (MIDI Panic)");
    panicButton.onClick = [this] { audioProcessor.panic(); };
    addAndMakeVisible (panicButton);

    auditionButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff264438));
    auditionButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff9effc8));
    auditionButton.setTooltip ("Play a short audition chord / drum hit on the selected channel");
    auditionButton.onClick = [this] { audioProcessor.triggerAuditionNote (selectedChannel); };
    addAndMakeVisible (auditionButton);

    allToAuxButton.setClickingTogglesState (true);
    allToAuxButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff263042));
    allToAuxButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2e9e6b));
    allToAuxButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    allToAuxButton.setTooltip ("Route all 16 MIDI channels to their individual Stereo Aux Outputs (Out 3/4 .. 33/34) in REAPER");
    allToAuxButton.onClick = [this]
    {
        if (! isUpdatingGui)
        {
            audioProcessor.setAllChannelsRouteToAux (allToAuxButton.getToggleState());
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (allToAuxButton);

    globalFxToggleButton.setClickingTogglesState (true);
    globalFxToggleButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff263042));
    globalFxToggleButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7ab8));
    globalFxToggleButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    globalFxToggleButton.onClick = [this]
    {
        setGlobalFxView (globalFxToggleButton.getToggleState());
    };
    addAndMakeVisible (globalFxToggleButton);

    auto setupHeaderSlider = [this] (juce::Label& lbl, const juce::String& text,
                                     juce::Slider& sld, double minVal, double maxVal, double step, double initVal)
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xffc4d0e0));
        lbl.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (lbl);

        sld.setSliderStyle (juce::Slider::LinearHorizontal);
        sld.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 20);
        sld.setRange (minVal, maxVal, step);
        sld.setValue (initVal, juce::dontSendNotification);
        sld.setColour (juce::Slider::trackColourId,             juce::Colour (0xff3c8dbc));
        sld.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff8be0ff));
        sld.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffdce4f0));
        sld.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff141820));
        sld.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xff283040));
        addAndMakeVisible (sld);
    };

    const auto& gfx = audioProcessor.getGlobalFxState();
    setupHeaderSlider (masterGainLabel,  "GAIN",   masterGainSlider,  0.0, 2.0, 0.01, gfx.masterGain);
    setupHeaderSlider (reverbLevelLabel, "REVERB", reverbLevelSlider, 0.0, 1.0, 0.01, gfx.reverbLevel);
    setupHeaderSlider (chorusLevelLabel, "CHORUS", chorusLevelSlider, 0.0, 1.0, 0.01, gfx.chorusLevel);

    masterGainSlider.setDoubleClickReturnValue (true, 0.5);
    reverbLevelSlider.setDoubleClickReturnValue (true, 0.5);
    chorusLevelSlider.setDoubleClickReturnValue (true, 0.5);

    masterGainSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setMasterGain (static_cast<float> (masterGainSlider.getValue()));
    };
    reverbLevelSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
        {
            const float val = static_cast<float> (reverbLevelSlider.getValue());
            audioProcessor.setReverbLevel (val);
            fxReverbLevelSlider.setValue (val, juce::dontSendNotification);
        }
    };
    chorusLevelSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
        {
            const float val = static_cast<float> (chorusLevelSlider.getValue());
            audioProcessor.setChorusLevel (val);
            fxChorusLevelSlider.setValue (val, juce::dontSendNotification);
        }
    };

    // --- Left 16-Channel Rack Rows ---
    for (int ch = 0; ch < 16; ++ch)
    {
        auto& row = rows[static_cast<size_t> (ch)];

        row.selectButton.setButtonText (juce::String::formatted ("%02d", ch + 1));
        row.selectButton.setTooltip ("Select Channel " + juce::String (ch + 1) + " for Inspector");
        row.selectButton.onClick = [this, ch] { selectChannel (ch); };
        addAndMakeVisible (row.selectButton);

        row.muteButton.setClickingTogglesState (true);
        row.muteButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff252b36));
        row.muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd94848));
        row.muteButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        row.muteButton.onClick = [this, ch]
        {
            if (! isUpdatingGui)
            {
                audioProcessor.setChannelMute (ch, rows[static_cast<size_t> (ch)].muteButton.getToggleState());
                selectChannel (ch);
            }
        };
        addAndMakeVisible (row.muteButton);

        row.soloButton.setClickingTogglesState (true);
        row.soloButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff252b36));
        row.soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0a800));
        row.soloButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
        row.soloButton.onClick = [this, ch]
        {
            if (! isUpdatingGui)
            {
                audioProcessor.setChannelSolo (ch, rows[static_cast<size_t> (ch)].soloButton.getToggleState());
                selectChannel (ch);
            }
        };
        addAndMakeVisible (row.soloButton);

        row.prevPresetButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff252b36));
        row.prevPresetButton.onClick = [this, ch]
        {
            selectChannel (ch);
            stepPresetForChannel (ch, -1);
        };
        addAndMakeVisible (row.prevPresetButton);

        row.presetCombo.setEditableText (false);
        row.presetCombo.setJustificationType (juce::Justification::centredLeft);
        row.presetCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff141820));
        row.presetCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2c3444));
        row.presetCombo.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffe6eef8));
        row.presetCombo.onChange = [this, ch]
        {
            if (isUpdatingGui)
                return;

            const int id = rows[static_cast<size_t> (ch)].presetCombo.getSelectedId();
            if (id > 0)
            {
                int bank = 0, prog = 0;
                decodePresetItemId (id, bank, prog);
                audioProcessor.setChannelPreset (ch, bank, prog);
                selectChannel (ch);
            }
        };
        addAndMakeVisible (row.presetCombo);

        row.nextPresetButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff252b36));
        row.nextPresetButton.onClick = [this, ch]
        {
            selectChannel (ch);
            stepPresetForChannel (ch, +1);
        };
        addAndMakeVisible (row.nextPresetButton);

        row.volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        row.volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 18);
        row.volumeSlider.setRange (0.0, 127.0, 1.0);
        row.volumeSlider.setDoubleClickReturnValue (true, 100.0);
        row.volumeSlider.setColour (juce::Slider::trackColourId,             juce::Colour (0xff2e9e6b));
        row.volumeSlider.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xff141820));
        row.volumeSlider.setColour (juce::Slider::thumbColourId,             juce::Colour (0xffd6e4f0));
        row.volumeSlider.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffd0dae8));
        row.volumeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff141820));
        row.volumeSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xff252c3a));
        row.volumeSlider.onValueChange = [this, ch]
        {
            if (! isUpdatingGui)
            {
                audioProcessor.setChannelVolume (ch, static_cast<int> (rows[static_cast<size_t> (ch)].volumeSlider.getValue()));
                selectChannel (ch);
            }
        };
        addAndMakeVisible (row.volumeSlider);
    }

    // --- Right Panel: Channel Inspector Setup ---
    auto setupInspectorSlider = [this] (juce::Label& lbl, const juce::String& text,
                                        juce::Slider& sld, double minV, double maxV, double stepV,
                                        double defV, juce::Colour trackCol)
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setFont (juce::FontOptions (11.5f, juce::Font::plain));
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
        lbl.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);

        sld.setSliderStyle (juce::Slider::LinearHorizontal);
        sld.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
        sld.setRange (minV, maxV, stepV);
        sld.setDoubleClickReturnValue (true, defV);
        sld.setColour (juce::Slider::trackColourId,             trackCol);
        sld.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xff12161e));
        sld.setColour (juce::Slider::thumbColourId,             juce::Colour (0xffdce8f6));
        sld.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffdce4f0));
        sld.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff12161e));
        sld.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xff262e3d));
        addAndMakeVisible (sld);
    };

    // Card 1: Mode, Output Bus, Bank, Interpolation
    chModeLabel.setText ("Channel Mode", juce::dontSendNotification);
    chModeLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    chModeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (chModeLabel);

    chMelodicButton.setClickingTogglesState (true);
    chMelodicButton.setRadioGroupId (101);
    chMelodicButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chMelodicButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7ab8));
    chMelodicButton.onClick = [this]
    {
        if (! isUpdatingGui && chMelodicButton.getToggleState())
        {
            audioProcessor.setChannelDrumMode (selectedChannel, false);
            rebuildPresetComboForChannel (selectedChannel);
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (chMelodicButton);

    chDrumButton.setClickingTogglesState (true);
    chDrumButton.setRadioGroupId (101);
    chDrumButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chDrumButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc88a20));
    chDrumButton.onClick = [this]
    {
        if (! isUpdatingGui && chDrumButton.getToggleState())
        {
            audioProcessor.setChannelDrumMode (selectedChannel, true);
            rebuildPresetComboForChannel (selectedChannel);
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (chDrumButton);

    chOutBusLabel.setText ("Output Bus", juce::dontSendNotification);
    chOutBusLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    chOutBusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (chOutBusLabel);

    chMasterOutButton.setClickingTogglesState (true);
    chMasterOutButton.setRadioGroupId (103);
    chMasterOutButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chMasterOutButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7ab8));
    chMasterOutButton.onClick = [this]
    {
        if (! isUpdatingGui && chMasterOutButton.getToggleState())
        {
            audioProcessor.setChannelRouteToAux (selectedChannel, false);
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (chMasterOutButton);

    chAuxOutButton.setClickingTogglesState (true);
    chAuxOutButton.setRadioGroupId (103);
    chAuxOutButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chAuxOutButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2e9e6b));
    chAuxOutButton.onClick = [this]
    {
        if (! isUpdatingGui && chAuxOutButton.getToggleState())
        {
            audioProcessor.setChannelRouteToAux (selectedChannel, true);
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (chAuxOutButton);

    chBankLabel.setText ("Bank Select", juce::dontSendNotification);
    chBankLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    chBankLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (chBankLabel);

    chBankCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff12161e));
    chBankCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2c3444));
    chBankCombo.onChange = [this]
    {
        if (! isUpdatingGui && chBankCombo.getSelectedId() > 0)
        {
            const int bank = chBankCombo.getSelectedId() - 1000;
            audioProcessor.setChannelBank (selectedChannel, bank);
            rebuildPresetComboForChannel (selectedChannel);
            syncGuiFromProcessor();
        }
    };
    addAndMakeVisible (chBankCombo);

    chInterpLabel.setText ("Interpolation", juce::dontSendNotification);
    chInterpLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    chInterpLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (chInterpLabel);

    chInterpCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff12161e));
    chInterpCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2c3444));
    chInterpCombo.addItem ("None (Lo-Fi Retro)",     100 + FLUID_INTERP_NONE);
    chInterpCombo.addItem ("Linear (Fast)",          100 + FLUID_INTERP_LINEAR);
    chInterpCombo.addItem ("4th Order (Default)",    100 + FLUID_INTERP_4THORDER);
    chInterpCombo.addItem ("Sinc 11th Order (Mid)",  100 + FLUID_INTERP_MID);
    chInterpCombo.addItem ("Sinc 17th Order (High)", 100 + FLUID_INTERP_HIGH);
    chInterpCombo.onChange = [this]
    {
        if (! isUpdatingGui && chInterpCombo.getSelectedId() >= 100)
            audioProcessor.setChannelInterpMethod (selectedChannel, chInterpCombo.getSelectedId() - 100);
    };
    addAndMakeVisible (chInterpCombo);

    // Card 2: Stereo, Expression & FX Sends
    setupInspectorSlider (chPanLabel,        "Pan (CC 10)",        chPanSlider,        0.0, 127.0, 1.0, 64.0,  juce::Colour (0xff3c78b8));
    chPanSlider.textFromValueFunction = [] (double v) { return formatPanText (v); };
    chPanSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelPan (selectedChannel, static_cast<int> (chPanSlider.getValue()));
    };

    setupInspectorSlider (chExpressionLabel, "Expression (CC 11)", chExpressionSlider, 0.0, 127.0, 1.0, 127.0, juce::Colour (0xff2e9e6b));
    chExpressionSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelExpression (selectedChannel, static_cast<int> (chExpressionSlider.getValue()));
    };

    setupInspectorSlider (chReverbLabel,     "Reverb Send (CC 91)", chReverbSlider,    0.0, 127.0, 1.0, 40.0,  juce::Colour (0xff7c5cbf));
    chReverbSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelReverb (selectedChannel, static_cast<int> (chReverbSlider.getValue()));
    };

    setupInspectorSlider (chChorusLabel,     "Chorus Send (CC 93)", chChorusSlider,    0.0, 127.0, 1.0, 0.0,   juce::Colour (0xff2b8ea8));
    chChorusSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelChorus (selectedChannel, static_cast<int> (chChorusSlider.getValue()));
    };

    // Card 3: Pitch & Performance
    setupInspectorSlider (chCoarseLabel,     "Transpose (st)",     chCoarseSlider,     -48.0, 48.0, 1.0, 0.0,  juce::Colour (0xffc47d38));
    chCoarseSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelCoarseTune (selectedChannel, static_cast<int> (chCoarseSlider.getValue()));
    };

    setupInspectorSlider (chFineLabel,       "Fine Tune (ct)",     chFineSlider,       -99.0, 99.0, 1.0, 0.0,  juce::Colour (0xffc47d38));
    chFineSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelFineTune (selectedChannel, static_cast<int> (chFineSlider.getValue()));
    };

    setupInspectorSlider (chBendRangeLabel,  "Bend Range (st)",    chBendRangeSlider,  0.0, 24.0, 1.0, 2.0,    juce::Colour (0xffc47d38));
    chBendRangeSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelPitchBendRange (selectedChannel, static_cast<int> (chBendRangeSlider.getValue()));
    };

    chVoiceModeLabel.setText ("Voice Mode", juce::dontSendNotification);
    chVoiceModeLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    chVoiceModeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (chVoiceModeLabel);

    chPolyButton.setClickingTogglesState (true);
    chPolyButton.setRadioGroupId (102);
    chPolyButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chPolyButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7ab8));
    chPolyButton.onClick = [this]
    {
        if (! isUpdatingGui && chPolyButton.getToggleState())
            audioProcessor.setChannelMonoMode (selectedChannel, false);
    };
    addAndMakeVisible (chPolyButton);

    chMonoButton.setClickingTogglesState (true);
    chMonoButton.setRadioGroupId (102);
    chMonoButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    chMonoButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7ab8));
    chMonoButton.onClick = [this]
    {
        if (! isUpdatingGui && chMonoButton.getToggleState())
            audioProcessor.setChannelMonoMode (selectedChannel, true);
    };
    addAndMakeVisible (chMonoButton);

    setupInspectorSlider (chPortaLabel,      "Glide Time (CC 5)",  chPortaSlider,      0.0, 127.0, 1.0, 0.0,   juce::Colour (0xffc47d38));
    chPortaSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChannelPortamentoTime (selectedChannel, static_cast<int> (chPortaSlider.getValue()));
    };

    // Card 4: Filter & Amp Envelope (Sound Shaping)
    chResetShapingButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a3242));
    chResetShapingButton.setTooltip ("Reset Pitch, Filter and Envelope offsets for this channel");
    chResetShapingButton.onClick = [this]
    {
        audioProcessor.resetChannelSoundShaping (selectedChannel);
        syncGuiFromProcessor();
    };
    addAndMakeVisible (chResetShapingButton);

    auto setupShapingSlider = [this, &setupInspectorSlider] (juce::Label& lbl, const juce::String& text,
                                                             juce::Slider& sld, std::function<void (int)> setter)
    {
        setupInspectorSlider (lbl, text, sld, 0.0, 127.0, 1.0, 64.0, juce::Colour (0xff4ea885));
        sld.textFromValueFunction = [] (double v) { return formatSignedOffset (v); };
        sld.updateText();
        sld.onValueChange = [this, &sld, setter]
        {
            if (! isUpdatingGui)
                setter (static_cast<int> (sld.getValue()));
        };
    };

    setupShapingSlider (chCutoffLabel,    "Cutoff (CC 74)",    chCutoffSlider,    [this] (int v) { audioProcessor.setChannelCutoff    (selectedChannel, v); });
    setupShapingSlider (chResonanceLabel, "Resonance (CC 71)", chResonanceSlider, [this] (int v) { audioProcessor.setChannelResonance (selectedChannel, v); });
    setupShapingSlider (chAttackLabel,    "Attack (CC 73)",    chAttackSlider,    [this] (int v) { audioProcessor.setChannelAttack    (selectedChannel, v); });
    setupShapingSlider (chDecayLabel,     "Decay (CC 75)",     chDecaySlider,     [this] (int v) { audioProcessor.setChannelDecay     (selectedChannel, v); });
    setupShapingSlider (chReleaseLabel,   "Release (CC 72)",   chReleaseSlider,   [this] (int v) { audioProcessor.setChannelRelease   (selectedChannel, v); });

    // --- Right Panel: Global FX & Synth Engine Setup ---
    fxReverbEnableButton.setClickingTogglesState (true);
    fxReverbEnableButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    fxReverbEnableButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff7c5cbf));
    fxReverbEnableButton.onClick = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setReverbActive (fxReverbEnableButton.getToggleState());
    };
    addAndMakeVisible (fxReverbEnableButton);

    setupInspectorSlider (fxReverbLevelLabel, "Reverb Level", fxReverbLevelSlider, 0.0, 1.0, 0.01, 0.5, juce::Colour (0xff7c5cbf));
    fxReverbLevelSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
        {
            const float val = static_cast<float> (fxReverbLevelSlider.getValue());
            audioProcessor.setReverbLevel (val);
            reverbLevelSlider.setValue (val, juce::dontSendNotification);
        }
    };

    setupInspectorSlider (fxReverbRoomLabel,  "Room Size",    fxReverbRoomSlider,  0.0, 1.0, 0.01, 0.6, juce::Colour (0xff7c5cbf));
    fxReverbRoomSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setReverbRoomSize (static_cast<float> (fxReverbRoomSlider.getValue()));
    };

    setupInspectorSlider (fxReverbDampLabel,  "Damping",      fxReverbDampSlider,  0.0, 1.0, 0.01, 0.3, juce::Colour (0xff7c5cbf));
    fxReverbDampSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setReverbDamp (static_cast<float> (fxReverbDampSlider.getValue()));
    };

    setupInspectorSlider (fxReverbWidthLabel, "Stereo Width", fxReverbWidthSlider, 0.0, 100.0, 0.5, 0.8, juce::Colour (0xff7c5cbf));
    fxReverbWidthSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setReverbWidth (static_cast<float> (fxReverbWidthSlider.getValue()));
    };

    fxChorusEnableButton.setClickingTogglesState (true);
    fxChorusEnableButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222834));
    fxChorusEnableButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b8ea8));
    fxChorusEnableButton.onClick = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChorusActive (fxChorusEnableButton.getToggleState());
    };
    addAndMakeVisible (fxChorusEnableButton);

    setupInspectorSlider (fxChorusLevelLabel,  "Chorus Level", fxChorusLevelSlider,  0.0, 1.0,  0.01, 0.5, juce::Colour (0xff2b8ea8));
    fxChorusLevelSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
        {
            const float val = static_cast<float> (fxChorusLevelSlider.getValue());
            audioProcessor.setChorusLevel (val);
            chorusLevelSlider.setValue (val, juce::dontSendNotification);
        }
    };

    setupInspectorSlider (fxChorusVoicesLabel, "Voices (Nr)",  fxChorusVoicesSlider, 1.0, 16.0, 1.0,  3.0, juce::Colour (0xff2b8ea8));
    fxChorusVoicesSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChorusVoices (static_cast<int> (fxChorusVoicesSlider.getValue()));
    };

    setupInspectorSlider (fxChorusSpeedLabel,  "Speed (Hz)",   fxChorusSpeedSlider,  0.1, 5.0,  0.05, 0.3, juce::Colour (0xff2b8ea8));
    fxChorusSpeedSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChorusSpeed (static_cast<float> (fxChorusSpeedSlider.getValue()));
    };

    setupInspectorSlider (fxChorusDepthLabel,  "Depth (ms)",   fxChorusDepthSlider,  0.0, 21.0, 0.1,  8.0, juce::Colour (0xff2b8ea8));
    fxChorusDepthSlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChorusDepth (static_cast<float> (fxChorusDepthSlider.getValue()));
    };

    fxChorusTypeLabel.setText ("Waveform", juce::dontSendNotification);
    fxChorusTypeLabel.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    fxChorusTypeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8c4d4));
    addAndMakeVisible (fxChorusTypeLabel);

    fxChorusTypeCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff12161e));
    fxChorusTypeCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff2c3444));
    fxChorusTypeCombo.addItem ("Sine Wave",     1);
    fxChorusTypeCombo.addItem ("Triangle Wave", 2);
    fxChorusTypeCombo.onChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setChorusType (fxChorusTypeCombo.getSelectedId() == 2 ? FLUID_CHORUS_MOD_TRIANGLE : FLUID_CHORUS_MOD_SINE);
    };
    addAndMakeVisible (fxChorusTypeCombo);

    setupInspectorSlider (fxPolyphonyLabel, "Max Polyphony", fxPolyphonySlider, 16.0, 512.0, 1.0, 256.0, juce::Colour (0xff3c8dbc));
    fxPolyphonySlider.onValueChange = [this]
    {
        if (! isUpdatingGui)
            audioProcessor.setPolyphony (static_cast<int> (fxPolyphonySlider.getValue()));
    };

    fxEngineStatusLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    fxEngineStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff90e0a8));
    fxEngineStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (fxEngineStatusLabel);

    setSize (960, 662);

    rebuildSoundFontFolderCombo();
    rebuildBankAndPresetLists();
    updateRightPanelVisibility();
    syncGuiFromProcessor();

    startTimerHz (30);
}

FSGMExpanderAudioProcessorEditor::~FSGMExpanderAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void FSGMExpanderAudioProcessorEditor::selectChannel (int ch)
{
    selectedChannel = juce::jlimit (0, 15, ch);
    if (showingGlobalFx)
    {
        showingGlobalFx = false;
        globalFxToggleButton.setToggleState (false, juce::dontSendNotification);
        updateRightPanelVisibility();
    }
    syncGuiFromProcessor();
    repaint();
}

void FSGMExpanderAudioProcessorEditor::setGlobalFxView (bool showGlobal)
{
    showingGlobalFx = showGlobal;
    globalFxToggleButton.setToggleState (showGlobal, juce::dontSendNotification);
    updateRightPanelVisibility();
    syncGuiFromProcessor();
    repaint();
}

void FSGMExpanderAudioProcessorEditor::updateRightPanelVisibility()
{
    const bool showCh = ! showingGlobalFx;

    chModeLabel.setVisible (showCh);
    chMelodicButton.setVisible (showCh);
    chDrumButton.setVisible (showCh);
    chOutBusLabel.setVisible (showCh);
    chMasterOutButton.setVisible (showCh);
    chAuxOutButton.setVisible (showCh);
    chBankLabel.setVisible (showCh);
    chBankCombo.setVisible (showCh);
    chInterpLabel.setVisible (showCh);
    chInterpCombo.setVisible (showCh);

    chPanLabel.setVisible (showCh);
    chPanSlider.setVisible (showCh);
    chExpressionLabel.setVisible (showCh);
    chExpressionSlider.setVisible (showCh);
    chReverbLabel.setVisible (showCh);
    chReverbSlider.setVisible (showCh);
    chChorusLabel.setVisible (showCh);
    chChorusSlider.setVisible (showCh);

    chCoarseLabel.setVisible (showCh);
    chCoarseSlider.setVisible (showCh);
    chFineLabel.setVisible (showCh);
    chFineSlider.setVisible (showCh);
    chBendRangeLabel.setVisible (showCh);
    chBendRangeSlider.setVisible (showCh);
    chVoiceModeLabel.setVisible (showCh);
    chPolyButton.setVisible (showCh);
    chMonoButton.setVisible (showCh);
    chPortaLabel.setVisible (showCh);
    chPortaSlider.setVisible (showCh);

    chResetShapingButton.setVisible (showCh);
    chCutoffLabel.setVisible (showCh);
    chCutoffSlider.setVisible (showCh);
    chResonanceLabel.setVisible (showCh);
    chResonanceSlider.setVisible (showCh);
    chAttackLabel.setVisible (showCh);
    chAttackSlider.setVisible (showCh);
    chDecayLabel.setVisible (showCh);
    chDecaySlider.setVisible (showCh);
    chReleaseLabel.setVisible (showCh);
    chReleaseSlider.setVisible (showCh);

    const bool showFx = showingGlobalFx;
    fxReverbEnableButton.setVisible (showFx);
    fxReverbLevelLabel.setVisible (showFx);
    fxReverbLevelSlider.setVisible (showFx);
    fxReverbRoomLabel.setVisible (showFx);
    fxReverbRoomSlider.setVisible (showFx);
    fxReverbDampLabel.setVisible (showFx);
    fxReverbDampSlider.setVisible (showFx);
    fxReverbWidthLabel.setVisible (showFx);
    fxReverbWidthSlider.setVisible (showFx);

    fxChorusEnableButton.setVisible (showFx);
    fxChorusLevelLabel.setVisible (showFx);
    fxChorusLevelSlider.setVisible (showFx);
    fxChorusVoicesLabel.setVisible (showFx);
    fxChorusVoicesSlider.setVisible (showFx);
    fxChorusSpeedLabel.setVisible (showFx);
    fxChorusSpeedSlider.setVisible (showFx);
    fxChorusDepthLabel.setVisible (showFx);
    fxChorusDepthSlider.setVisible (showFx);
    fxChorusTypeLabel.setVisible (showFx);
    fxChorusTypeCombo.setVisible (showFx);

    fxPolyphonyLabel.setVisible (showFx);
    fxPolyphonySlider.setVisible (showFx);
    fxEngineStatusLabel.setVisible (showFx);
}

void FSGMExpanderAudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
    if (meterArea.contains (event.getPosition()))
    {
        audioProcessor.clearMasterClip();
        displayClipped = false;
        repaint (meterArea);
        return;
    }

    const int rackTop   = 97;
    const int rowHeight = 35;
    if (event.x < 555 && event.y >= rackTop && event.y < rackTop + 16 * rowHeight)
    {
        const int clickedCh = (event.y - rackTop) / rowHeight;
        selectChannel (clickedCh);
    }
}

//==============================================================================
void FSGMExpanderAudioProcessorEditor::openSoundFontFileChooser()
{
    const juce::File initialDir = audioProcessor.getLastSoundFontFolder();

    fileChooser = std::make_unique<juce::FileChooser> (
        "Select a SoundFont (.sf2 / .sf3)",
        initialDir,
        "*.sf2;*.sf3;*.SF2;*.SF3");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        const auto result = fc.getResult();
        if (result.existsAsFile())
        {
            if (audioProcessor.loadSoundFont (result))
            {
                rebuildSoundFontFolderCombo();
                rebuildBankAndPresetLists();
                syncGuiFromProcessor();
            }
        }
    });
}

void FSGMExpanderAudioProcessorEditor::rebuildSoundFontFolderCombo()
{
    const juce::ScopedValueSetter<bool> svs (isUpdatingGui, true);

    folderSoundFonts = audioProcessor.getAvailableSoundFontsInCurrentFolder();
    sfFolderCombo.clear (juce::dontSendNotification);

    const juce::String currentPath = audioProcessor.getCurrentSoundFontPath();
    int selectedId = 0;

    for (size_t i = 0; i < folderSoundFonts.size(); ++i)
    {
        const int id = static_cast<int> (i) + 1;
        sfFolderCombo.addItem (folderSoundFonts[i].getFileName(), id);
        if (folderSoundFonts[i].getFullPathName() == currentPath)
            selectedId = id;
    }

    if (selectedId > 0)
        sfFolderCombo.setSelectedId (selectedId, juce::dontSendNotification);
    else
        sfFolderCombo.setText (audioProcessor.getCurrentSoundFontName(), juce::dontSendNotification);

    sfFolderCombo.setTooltip (currentPath.isNotEmpty()
                                  ? currentPath
                                  : ("Folder: " + audioProcessor.getLastSoundFontFolder().getFullPathName()));
}

void FSGMExpanderAudioProcessorEditor::stepSoundFontInFolder (int delta)
{
    if (folderSoundFonts.empty())
        rebuildSoundFontFolderCombo();

    const int numItems = sfFolderCombo.getNumItems();
    if (numItems <= 0)
        return;

    int nextIndex = sfFolderCombo.getSelectedItemIndex() + delta;
    if (nextIndex < 0)
        nextIndex = numItems - 1;
    else if (nextIndex >= numItems)
        nextIndex = 0;

    sfFolderCombo.setSelectedItemIndex (nextIndex, juce::sendNotificationSync);
}

void FSGMExpanderAudioProcessorEditor::rebuildBankAndPresetLists()
{
    const juce::ScopedValueSetter<bool> svs (isUpdatingGui, true);

    cachedPresets = audioProcessor.getPresets();
    cachedBanks   = audioProcessor.getAvailableBanks();

    chBankCombo.clear (juce::dontSendNotification);
    for (int bank : cachedBanks)
        chBankCombo.addItem (formatBankLabel (bank), 1000 + bank);

    for (int ch = 0; ch < 16; ++ch)
        rebuildPresetComboForChannel (ch);
}

void FSGMExpanderAudioProcessorEditor::rebuildPresetComboForChannel (int ch)
{
    const juce::ScopedValueSetter<bool> svs (isUpdatingGui, true);

    auto& combo    = rows[static_cast<size_t> (ch)].presetCombo;
    const auto& st = audioProcessor.getChannelState (ch);
    combo.clear (juce::dontSendNotification);

    if (! cachedPresets.empty())
    {
        for (const auto& p : cachedPresets)
        {
            if (p.bank == st.bank)
            {
                const int itemId = encodePresetItemId (p.bank, p.program);
                combo.addItem (formatProgramOnlyLabel (p.program, p.name), itemId);
            }
        }

        if (combo.getNumItems() == 0)
        {
            for (const auto& p : cachedPresets)
            {
                const int itemId = encodePresetItemId (p.bank, p.program);
                combo.addItem (juce::String::formatted ("[%03d:%03d] ", p.bank, p.program) + p.name, itemId);
            }
        }
    }
    else
    {
        if (st.bank == 128)
        {
            combo.addItem ("000: Standard Drum Kit",   encodePresetItemId (128, 0));
            combo.addItem ("008: Room Drum Kit",       encodePresetItemId (128, 8));
            combo.addItem ("016: Power Drum Kit",      encodePresetItemId (128, 16));
            combo.addItem ("024: Electronic Drum Kit", encodePresetItemId (128, 24));
            combo.addItem ("025: TR-808 Drum Kit",     encodePresetItemId (128, 25));
            combo.addItem ("032: Jazz Drum Kit",       encodePresetItemId (128, 32));
            combo.addItem ("040: Brush Drum Kit",      encodePresetItemId (128, 40));
            combo.addItem ("048: Orchestra Drum Kit",  encodePresetItemId (128, 48));
        }
        else
        {
            for (int prog = 0; prog < 128; ++prog)
            {
                const char* gmName = juce::MidiMessage::getGMInstrumentName (prog);
                const juce::String name = (gmName != nullptr) ? juce::String (gmName) : "Program " + juce::String (prog);
                combo.addItem (formatProgramOnlyLabel (prog, name), encodePresetItemId (st.bank, prog));
            }
        }
    }
}

void FSGMExpanderAudioProcessorEditor::selectPresetForChannel (int ch, int bank, int program)
{
    auto& combo = rows[static_cast<size_t> (ch)].presetCombo;
    const int targetId = encodePresetItemId (bank, program);

    if (combo.indexOfItemId (targetId) < 0)
        rebuildPresetComboForChannel (ch);

    if (combo.indexOfItemId (targetId) >= 0)
    {
        combo.setSelectedId (targetId, juce::dontSendNotification);
        return;
    }

    for (const auto& p : cachedPresets)
    {
        if (p.program == program && (p.bank == bank || (! audioProcessor.getChannelState (ch).isDrum && p.bank == 0)))
        {
            combo.setSelectedId (encodePresetItemId (p.bank, p.program), juce::dontSendNotification);
            return;
        }
    }

    combo.setText (juce::String::formatted ("[%03d:%03d] Preset", bank, program), juce::dontSendNotification);
}

void FSGMExpanderAudioProcessorEditor::stepPresetForChannel (int ch, int delta)
{
    auto& combo = rows[static_cast<size_t> (ch)].presetCombo;
    const int numItems = combo.getNumItems();
    if (numItems <= 0)
        return;

    int nextIndex = combo.getSelectedItemIndex() + delta;
    if (nextIndex < 0)
        nextIndex = numItems - 1;
    else if (nextIndex >= numItems)
        nextIndex = 0;

    combo.setSelectedItemIndex (nextIndex, juce::sendNotificationSync);
}

void FSGMExpanderAudioProcessorEditor::syncGuiFromProcessor()
{
    const juce::ScopedValueSetter<bool> svs (isUpdatingGui, true);

    const auto currentPresets = audioProcessor.getPresets();
    if (currentPresets.size() != cachedPresets.size())
    {
        rebuildSoundFontFolderCombo();
        rebuildBankAndPresetLists();
    }
    else
    {
        rebuildSoundFontFolderCombo();
    }

    allToAuxButton.setToggleState (audioProcessor.areAllChannelsRoutedToAux(), juce::dontSendNotification);

    const auto& gfx = audioProcessor.getGlobalFxState();
    masterGainSlider.setValue  (gfx.masterGain,  juce::dontSendNotification);
    reverbLevelSlider.setValue (gfx.reverbLevel, juce::dontSendNotification);
    chorusLevelSlider.setValue (gfx.chorusLevel, juce::dontSendNotification);

    // Update 16 left rows
    for (int ch = 0; ch < 16; ++ch)
    {
        const auto& st = audioProcessor.getChannelState (ch);
        auto& row      = rows[static_cast<size_t> (ch)];

        const bool isSelected = (! showingGlobalFx && ch == selectedChannel);
        const juce::String btnLabel = st.routeToAux
            ? juce::String::formatted ("%02dA", ch + 1)
            : juce::String::formatted ("%02d",  ch + 1);
        row.selectButton.setButtonText (btnLabel);

        row.selectButton.setColour (juce::TextButton::buttonColourId,
                                    isSelected ? juce::Colour (0xff2b7ab8)
                                               : (st.isDrum ? juce::Colour (0xff3a2f1d) : juce::Colour (0xff232936)));
        row.selectButton.setColour (juce::TextButton::textColourOffId,
                                    isSelected ? juce::Colours::white
                                               : (st.routeToAux ? juce::Colour (0xff7ef5b0)
                                                                : (st.isDrum ? juce::Colour (0xffffc857) : juce::Colour (0xffd0dae8))));

        row.muteButton.setToggleState (st.mute, juce::dontSendNotification);
        row.soloButton.setToggleState (st.solo, juce::dontSendNotification);
        selectPresetForChannel (ch, st.bank, st.program);
        row.volumeSlider.setValue (st.volume, juce::dontSendNotification);
    }

    // Update Right Panel: Selected Channel Inspector
    const auto& sel = audioProcessor.getChannelState (selectedChannel);
    chMelodicButton.setToggleState (! sel.isDrum, juce::dontSendNotification);
    chDrumButton.setToggleState    (sel.isDrum,   juce::dontSendNotification);

    const int auxOutChStart = (selectedChannel + 1) * 2 + 1;
    chAuxOutButton.setButtonText (juce::String::formatted ("Aux CH %02d (%d/%d)",
                                                           selectedChannel + 1, auxOutChStart, auxOutChStart + 1));
    chMasterOutButton.setToggleState (! sel.routeToAux, juce::dontSendNotification);
    chAuxOutButton.setToggleState    (sel.routeToAux,   juce::dontSendNotification);

    const int bankItemId = 1000 + sel.bank;
    if (chBankCombo.indexOfItemId (bankItemId) < 0)
        chBankCombo.addItem (formatBankLabel (sel.bank), bankItemId);
    chBankCombo.setSelectedId (bankItemId, juce::dontSendNotification);

    chInterpCombo.setSelectedId (100 + sel.interpMethod, juce::dontSendNotification);

    chPanSlider.setValue        (sel.pan,        juce::dontSendNotification);
    chExpressionSlider.setValue (sel.expression, juce::dontSendNotification);
    chReverbSlider.setValue     (sel.reverb,     juce::dontSendNotification);
    chChorusSlider.setValue     (sel.chorus,     juce::dontSendNotification);

    chCoarseSlider.setValue    (sel.coarseTune,     juce::dontSendNotification);
    chFineSlider.setValue      (sel.fineTune,       juce::dontSendNotification);
    chBendRangeSlider.setValue (sel.pitchBendRange, juce::dontSendNotification);
    chPolyButton.setToggleState (! sel.monoMode,    juce::dontSendNotification);
    chMonoButton.setToggleState (sel.monoMode,      juce::dontSendNotification);
    chPortaSlider.setValue     (sel.portamentoTime, juce::dontSendNotification);

    chCutoffSlider.setValue    (sel.cutoff,    juce::dontSendNotification);
    chResonanceSlider.setValue (sel.resonance, juce::dontSendNotification);
    chAttackSlider.setValue    (sel.attack,    juce::dontSendNotification);
    chDecaySlider.setValue     (sel.decay,     juce::dontSendNotification);
    chReleaseSlider.setValue   (sel.release,   juce::dontSendNotification);

    // Update Right Panel: Global FX Inspector
    fxReverbEnableButton.setToggleState (gfx.reverbActive, juce::dontSendNotification);
    fxReverbLevelSlider.setValue        (gfx.reverbLevel,    juce::dontSendNotification);
    fxReverbRoomSlider.setValue         (gfx.reverbRoomSize, juce::dontSendNotification);
    fxReverbDampSlider.setValue         (gfx.reverbDamp,     juce::dontSendNotification);
    fxReverbWidthSlider.setValue        (gfx.reverbWidth,    juce::dontSendNotification);

    fxChorusEnableButton.setToggleState (gfx.chorusActive, juce::dontSendNotification);
    fxChorusLevelSlider.setValue        (gfx.chorusLevel,  juce::dontSendNotification);
    fxChorusVoicesSlider.setValue       (gfx.chorusVoices, juce::dontSendNotification);
    fxChorusSpeedSlider.setValue        (gfx.chorusSpeed,  juce::dontSendNotification);
    fxChorusDepthSlider.setValue        (gfx.chorusDepth,  juce::dontSendNotification);
    fxChorusTypeCombo.setSelectedId     (gfx.chorusType == FLUID_CHORUS_MOD_TRIANGLE ? 2 : 1, juce::dontSendNotification);

    fxPolyphonySlider.setValue (gfx.polyphony, juce::dontSendNotification);
}

void FSGMExpanderAudioProcessorEditor::timerCallback()
{
    if (audioProcessor.checkAndClearGuiUpdateFlag())
        syncGuiFromProcessor();

    const int activeVoices = audioProcessor.getActiveVoiceCount();
    const int maxPoly      = audioProcessor.getGlobalFxState().polyphony;

    if (showingGlobalFx)
    {
        const double cpu = audioProcessor.getCpuLoad();
        const double srKhz = audioProcessor.getCurrentEngineSampleRate() / 1000.0;
        fxEngineStatusLabel.setText (
            juce::String::formatted ("Engine SR: %.1f kHz   |   Voices: %d / %d   |   DSP: %.1f%%",
                                     srKhz, activeVoices, maxPoly, cpu),
            juce::dontSendNotification);
    }

    // Update Master Stereo Peak Meter
    const float inPeakL = audioProcessor.consumeMasterPeakLeft();
    const float inPeakR = audioProcessor.consumeMasterPeakRight();
    const bool  clipped = audioProcessor.hasMasterClipped();

    const float prevPeakL = displayPeakL;
    const float prevPeakR = displayPeakR;

    displayPeakL = (inPeakL > displayPeakL) ? inPeakL : std::max (0.0f, displayPeakL * 0.80f - 0.01f);
    displayPeakR = (inPeakR > displayPeakR) ? inPeakR : std::max (0.0f, displayPeakR * 0.80f - 0.01f);

    if (clipped != displayClipped || std::abs (displayPeakL - prevPeakL) > 0.005f || std::abs (displayPeakR - prevPeakR) > 0.005f)
    {
        displayClipped = clipped;
        repaint (meterArea);
    }

    bool needsLedRepaint = false;
    for (int ch = 0; ch < 16; ++ch)
    {
        auto& st  = audioProcessor.getChannelState (ch);
        auto& row = rows[static_cast<size_t> (ch)];

        const float incoming = st.midiActivity.exchange (0.0f, std::memory_order_relaxed);
        const float prevLed  = row.ledLevel;

        if (incoming > row.ledLevel)
            row.ledLevel = incoming;
        else
            row.ledLevel = std::max (0.0f, row.ledLevel * 0.78f - 0.02f);

        if (std::abs (row.ledLevel - prevLed) > 0.01f)
            needsLedRepaint = true;
    }

    if (needsLedRepaint)
        repaint (0, 97, 68, 560);
}

//==============================================================================
void FSGMExpanderAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff12151c));

    // --- Top Header Bar (y = 0 .. 70) ---
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff202634), 0.0f, 0.0f,
                                             juce::Colour (0xff171b24), 0.0f, 70.0f, false));
    g.fillRect (0, 0, getWidth(), 70);
    g.setColour (juce::Colour (0xff30394c));
    g.drawHorizontalLine (70, 0.0f, static_cast<float> (getWidth()));

    g.setColour (juce::Colour (0xff7ee0ff));
    g.setFont (juce::FontOptions (18.5f, juce::Font::bold));
    g.drawText ("FS-GM-EXPANDER", 12, 11, 160, 24, juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff94a2b8));
    g.setFont (juce::FontOptions (11.5f, juce::Font::plain));
    g.drawText ("16-CH FLUIDSYNTH MODULE", 12, 36, 160, 18, juce::Justification::centredLeft);

    // --- Stereo Peak Meter in Top Header ---
    if (! meterArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff11151d));
        g.fillRoundedRectangle (meterArea.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff283142));
        g.drawRoundedRectangle (meterArea.toFloat(), 4.0f, 1.0f);

        g.setColour (juce::Colour (0xffa8b8cc));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("L", meterArea.getX() + 4, meterArea.getY() + 7,  12, 14, juce::Justification::centred);
        g.drawText ("R", meterArea.getX() + 4, meterArea.getY() + 25, 12, 14, juce::Justification::centred);

        const int barX = meterArea.getX() + 18;
        const int barW = meterArea.getWidth() - 52;

        auto drawMeterBar = [&g, barX, barW] (int y, float level)
        {
            g.setColour (juce::Colour (0xff0b0d12));
            g.fillRect (barX, y, barW, 11);

            const float clamped = juce::jlimit (0.0f, 1.0f, level);
            const int fillW = static_cast<int> (std::round (clamped * static_cast<float> (barW)));
            if (fillW > 0)
            {
                juce::ColourGradient grad (juce::Colour (0xff2bd975), static_cast<float> (barX), 0.0f,
                                           juce::Colour (0xffff4848), static_cast<float> (barX + barW), 0.0f, false);
                grad.addColour (0.72, juce::Colour (0xfff0c030));
                g.setGradientFill (grad);
                g.fillRect (barX, y, fillW, 11);
            }
        };

        drawMeterBar (meterArea.getY() + 9,  displayPeakL);
        drawMeterBar (meterArea.getY() + 27, displayPeakR);

        // Clip LED Box
        const juce::Rectangle<int> clipBox (meterArea.getRight() - 30, meterArea.getY() + 9, 26, 29);
        g.setColour (displayClipped ? juce::Colour (0xffe03232) : juce::Colour (0xff22181a));
        g.fillRoundedRectangle (clipBox.toFloat(), 3.0f);
        g.setColour (displayClipped ? juce::Colours::white : juce::Colour (0xff66484c));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText ("CLIP", clipBox, juce::Justification::centred);
    }

    // --- Sub-Header Strip (Left Rack Columns + Right Inspector Title, y = 71 .. 96) ---
    g.setColour (juce::Colour (0xff191e28));
    g.fillRect (0, 71, getWidth(), 25);
    g.setColour (juce::Colour (0xff283040));
    g.drawHorizontalLine (96, 0.0f, static_cast<float> (getWidth()));

    g.setColour (juce::Colour (0xff93a1b5));
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.drawText ("CH",           8,   71, 56,  25, juce::Justification::centred);
    g.drawText ("M / S",        68,  71, 50,  25, juce::Justification::centred);
    g.drawText ("PRESET",       124, 71, 288, 25, juce::Justification::centredLeft);
    g.drawText ("VOLUME (CC7)", 418, 71, 130, 25, juce::Justification::centred);

    // Vertical divider between Left Rack and Right Inspector
    g.setColour (juce::Colour (0xff2c3546));
    g.drawVerticalLine (555, 71.0f, static_cast<float> (getHeight()));

    // Right Inspector Header Title
    g.setColour (showingGlobalFx ? juce::Colour (0xffffc857) : juce::Colour (0xff7ee0ff));
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    if (showingGlobalFx)
    {
        g.drawText ("GLOBAL FX & SYNTH ENGINE", 568, 71, 380, 25, juce::Justification::centredLeft);
    }
    else
    {
        const auto& st = audioProcessor.getChannelState (selectedChannel);
        const juce::String modeStr = st.isDrum ? "DRUM CHANNEL" : "MELODIC CHANNEL";
        g.drawText (juce::String::formatted ("CHANNEL %02d INSPECTOR  |  ", selectedChannel + 1) + modeStr,
                    568, 71, 380, 25, juce::Justification::centredLeft);
    }

    // --- Left 16-Channel Rack Rows ---
    const int rackTop   = 97;
    const int rowHeight = 35;

    for (int ch = 0; ch < 16; ++ch)
    {
        const int y = rackTop + ch * rowHeight;
        const auto& row = rows[static_cast<size_t> (ch)];
        const auto& st  = audioProcessor.getChannelState (ch);
        const bool isSelected = (! showingGlobalFx && ch == selectedChannel);

        juce::Colour rowBg = (ch % 2 == 0) ? juce::Colour (0xff161a22) : juce::Colour (0xff1a1f29);
        if (st.isDrum)
            rowBg = juce::Colour (0xff211f1a);
        if (isSelected)
            rowBg = juce::Colour (0xff1f2b3d);

        g.setColour (rowBg);
        g.fillRect (0, y, 555, rowHeight - 1);

        if (isSelected)
        {
            g.setColour (juce::Colour (0xff3c8dbc));
            g.fillRect (0, y, 3, rowHeight - 1);
        }

        g.setColour (juce::Colour (0xff222834));
        g.drawHorizontalLine (y + rowHeight - 1, 0.0f, 555.0f);

        // MIDI Activity LED
        const auto led = row.ledBounds;
        g.setColour (juce::Colour (0xff0b0e13));
        g.fillEllipse (led.expanded (1.5f));

        const juce::Colour offCol (0xff1f3328);
        const juce::Colour onCol  (0xff2dff7e);
        g.setColour (offCol.interpolatedWith (onCol, juce::jlimit (0.0f, 1.0f, row.ledLevel)));
        g.fillEllipse (led);
    }

    // --- Right Panel Section Cards ---
    auto drawCard = [&g] (juce::Rectangle<int> bounds, const juce::String& title)
    {
        g.setColour (juce::Colour (0xff181d27));
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);
        g.setColour (juce::Colour (0xff283142));
        g.drawRoundedRectangle (bounds.toFloat(), 5.0f, 1.0f);

        g.setColour (juce::Colour (0xff8fa0b8));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (title, bounds.getX() + 10, bounds.getY() + 4, bounds.getWidth() - 20, 16,
                    juce::Justification::centredLeft);

        g.setColour (juce::Colour (0xff232b3a));
        g.drawHorizontalLine (bounds.getY() + 21, static_cast<float> (bounds.getX() + 6),
                              static_cast<float> (bounds.getRight() - 6));
    };

    const int rx = 566;
    const int rw = 384;

    if (! showingGlobalFx)
    {
        drawCard ({ rx, 100, rw, 118 }, "BANK, MODE & OUTPUT ROUTING");
        drawCard ({ rx, 223, rw, 122 }, "STEREO, EXPRESSION & FX SENDS");
        drawCard ({ rx, 350, rw, 144 }, "PITCH, TUNING & PERFORMANCE");
        drawCard ({ rx, 499, rw, 154 }, "FILTER & AMP ENVELOPE OFFSETS");
    }
    else
    {
        drawCard ({ rx, 102, rw, 174 }, "GLOBAL REVERB ENGINE");
        drawCard ({ rx, 284, rw, 196 }, "GLOBAL CHORUS ENGINE");
        drawCard ({ rx, 488, rw, 95  }, "SYNTHESIS ENGINE & DIAGNOSTICS");
    }
}

void FSGMExpanderAudioProcessorEditor::resized()
{
    // --- Top Header Layout (y = 0 .. 70) ---
    loadSfButton.setBounds         (174, 7,  72,  26);
    gmResetButton.setBounds        (250, 7,  66,  26);
    panicButton.setBounds          (320, 7,  60,  26);
    auditionButton.setBounds       (384, 7,  66,  26);
    allToAuxButton.setBounds       (454, 7,  68,  26);
    globalFxToggleButton.setBounds (526, 7,  68,  26);

    prevSfButton.setBounds         (174, 38, 26,  25);
    sfFolderCombo.setBounds        (202, 38, 364, 25);
    nextSfButton.setBounds         (568, 38, 26,  25);

    const int globalStartX = 602;
    const int globalWidth  = 80;
    const int globalGap    = 5;

    masterGainLabel.setBounds   (globalStartX,                        8,  globalWidth, 18);
    masterGainSlider.setBounds  (globalStartX,                        29, globalWidth, 34);

    reverbLevelLabel.setBounds  (globalStartX + (globalWidth + globalGap),     8,  globalWidth, 18);
    reverbLevelSlider.setBounds (globalStartX + (globalWidth + globalGap),     29, globalWidth, 34);

    chorusLevelLabel.setBounds  (globalStartX + (globalWidth + globalGap) * 2, 8,  globalWidth, 18);
    chorusLevelSlider.setBounds (globalStartX + (globalWidth + globalGap) * 2, 29, globalWidth, 34);

    meterArea = juce::Rectangle<int> (globalStartX + (globalWidth + globalGap) * 3, 11, 94, 48);

    // --- Left 16-Channel Rack Layout (x = 0 .. 555) ---
    const int rackTop   = 97;
    const int rowHeight = 35;

    for (int ch = 0; ch < 16; ++ch)
    {
        auto& row      = rows[static_cast<size_t> (ch)];
        const int y    = rackTop + ch * rowHeight;
        const int padY = y + 5;
        const int ctlH = 24;

        row.ledBounds = juce::Rectangle<float> (10.0f, static_cast<float> (y + 12), 9.0f, 9.0f);
        row.selectButton.setBounds (24, padY, 38, ctlH);

        row.muteButton.setBounds (68, padY, 24, ctlH);
        row.soloButton.setBounds (94, padY, 24, ctlH);

        row.prevPresetButton.setBounds (124, padY, 22,  ctlH);
        row.presetCombo.setBounds      (148, padY, 240, ctlH);
        row.nextPresetButton.setBounds (390, padY, 22,  ctlH);

        row.volumeSlider.setBounds (418, padY, 130, ctlH);
    }

    // --- Right Panel: Channel Inspector Layout (x = 566 .. 950) ---
    const int lx = 576;
    const int lw = 128;
    const int cx = 706;
    const int cw = 234;

    // Card 1: Bank, Mode & Output Routing (y = 100 .. 218)
    chModeLabel.setBounds       (lx,       124, lw,  20);
    chMelodicButton.setBounds   (cx,       124, 114, 20);
    chDrumButton.setBounds      (cx + 120, 124, 114, 20);

    chOutBusLabel.setBounds     (lx,       147, lw,  20);
    chMasterOutButton.setBounds (cx,       147, 114, 20);
    chAuxOutButton.setBounds    (cx + 120, 147, 114, 20);

    chBankLabel.setBounds       (lx, 170, lw, 20);
    chBankCombo.setBounds       (cx, 170, cw, 20);

    chInterpLabel.setBounds     (lx, 193, lw, 20);
    chInterpCombo.setBounds     (cx, 193, cw, 20);

    // Card 2: Stereo, Expression & FX Sends (y = 223 .. 345)
    chPanLabel.setBounds         (lx, 247, lw, 21);
    chPanSlider.setBounds        (cx, 247, cw, 21);

    chExpressionLabel.setBounds  (lx, 270, lw, 21);
    chExpressionSlider.setBounds (cx, 270, cw, 21);

    chReverbLabel.setBounds      (lx, 293, lw, 21);
    chReverbSlider.setBounds     (cx, 293, cw, 21);

    chChorusLabel.setBounds      (lx, 316, lw, 21);
    chChorusSlider.setBounds     (cx, 316, cw, 21);

    // Card 3: Pitch & Performance (y = 350 .. 494)
    chCoarseLabel.setBounds    (lx, 374, lw, 21);
    chCoarseSlider.setBounds   (cx, 374, cw, 21);

    chFineLabel.setBounds      (lx, 397, lw, 21);
    chFineSlider.setBounds     (cx, 397, cw, 21);

    chBendRangeLabel.setBounds  (lx, 420, lw, 21);
    chBendRangeSlider.setBounds (cx, 420, cw, 21);

    chVoiceModeLabel.setBounds (lx,       443, lw,  20);
    chPolyButton.setBounds     (cx,       443, 114, 20);
    chMonoButton.setBounds     (cx + 120, 443, 114, 20);

    chPortaLabel.setBounds     (lx, 466, lw, 21);
    chPortaSlider.setBounds    (cx, 466, cw, 21);

    // Card 4: Filter & Amp Envelope Offsets (y = 499 .. 653)
    chResetShapingButton.setBounds (892, 502, 50, 16);

    chCutoffLabel.setBounds     (lx, 523, lw, 22);
    chCutoffSlider.setBounds    (cx, 523, cw, 22);

    chResonanceLabel.setBounds  (lx, 547, lw, 22);
    chResonanceSlider.setBounds (cx, 547, cw, 22);

    chAttackLabel.setBounds     (lx, 571, lw, 22);
    chAttackSlider.setBounds    (cx, 571, cw, 22);

    chDecayLabel.setBounds      (lx, 595, lw, 22);
    chDecaySlider.setBounds     (cx, 595, cw, 22);

    chReleaseLabel.setBounds    (lx, 619, lw, 22);
    chReleaseSlider.setBounds   (cx, 619, cw, 22);

    // --- Right Panel: Global FX & Synth Engine Layout ---
    fxReverbEnableButton.setBounds (840, 105, 102, 17);

    fxReverbLevelLabel.setBounds  (lx, 130, lw, 24);
    fxReverbLevelSlider.setBounds (cx, 130, cw, 24);

    fxReverbRoomLabel.setBounds   (lx, 164, lw, 24);
    fxReverbRoomSlider.setBounds  (cx, 164, cw, 24);

    fxReverbDampLabel.setBounds   (lx, 198, lw, 24);
    fxReverbDampSlider.setBounds  (cx, 198, cw, 24);

    fxReverbWidthLabel.setBounds  (lx, 232, lw, 24);
    fxReverbWidthSlider.setBounds (cx, 232, cw, 24);

    fxChorusEnableButton.setBounds (840, 287, 102, 17);

    fxChorusLevelLabel.setBounds  (lx, 312, lw, 24);
    fxChorusLevelSlider.setBounds (cx, 312, cw, 24);

    fxChorusVoicesLabel.setBounds  (lx, 344, lw, 24);
    fxChorusVoicesSlider.setBounds (cx, 344, cw, 24);

    fxChorusSpeedLabel.setBounds   (lx, 376, lw, 24);
    fxChorusSpeedSlider.setBounds  (cx, 376, cw, 24);

    fxChorusDepthLabel.setBounds   (lx, 408, lw, 24);
    fxChorusDepthSlider.setBounds  (cx, 408, cw, 24);

    fxChorusTypeLabel.setBounds    (lx, 442, lw, 22);
    fxChorusTypeCombo.setBounds    (cx, 442, cw, 22);

    fxPolyphonyLabel.setBounds    (lx, 516, lw, 24);
    fxPolyphonySlider.setBounds   (cx, 516, cw, 24);
    fxEngineStatusLabel.setBounds (lx, 548, 364, 24);
}
