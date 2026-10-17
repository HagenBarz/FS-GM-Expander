/*
  ==============================================================================

    FS-GM-Expander | FluidSynth General MIDI Expander | PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

//==============================================================================
juce::AudioProcessor::BusesProperties FSGMExpanderAudioProcessor::createBusesProperties()
{
    BusesProperties bp;
    bp = bp.withOutput ("Master", juce::AudioChannelSet::stereo(), true);
    for (int ch = 0; ch < 16; ++ch)
    {
        bp = bp.withOutput (juce::String::formatted ("Aux CH %02d", ch + 1),
                            juce::AudioChannelSet::stereo(), true);
    }
    return bp;
}

FSGMExpanderAudioProcessor::FSGMExpanderAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (createBusesProperties())
#endif
{
    multiOutScratch.setSize (36, 2048);

    for (int ch = 0; ch < 16; ++ch)
    {
        channels[static_cast<size_t> (ch)].resetToDefault (ch);
        channels[static_cast<size_t> (ch)].midiActivity.store (0.0f, std::memory_order_relaxed);
    }

    initPropertiesFile();
    initFluidSynth (currentSampleRate);

    // Auto-load last used SoundFont if saved in user properties
    if (appProperties != nullptr)
    {
        const juce::String savedSf = appProperties->getValue ("lastSoundFontFile", {});
        if (savedSf.isNotEmpty())
        {
            const juce::File sfFile (savedSf);
            if (sfFile.existsAsFile())
                loadSoundFont (sfFile);
        }
    }
}

FSGMExpanderAudioProcessor::~FSGMExpanderAudioProcessor()
{
    cleanupFluidSynth();
}

//==============================================================================
void FSGMExpanderAudioProcessor::initPropertiesFile()
{
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "FS-GM-Expander";
    opts.filenameSuffix      = "settings";
    opts.folderName          = "Hagen Barz";
    opts.osxLibrarySubFolder = "Application Support";

    appProperties = std::make_unique<juce::PropertiesFile> (opts);
    lastSoundFontFolder = appProperties->getValue ("lastSoundFontFolder", "/usr/share/soundfonts");
}

void FSGMExpanderAudioProcessor::saveLastSoundFontToProperties (const juce::File& file)
{
    lastSoundFontFolder = file.getParentDirectory().getFullPathName();
    if (appProperties != nullptr)
    {
        appProperties->setValue ("lastSoundFontFolder", lastSoundFontFolder);
        appProperties->setValue ("lastSoundFontFile",   file.getFullPathName());
        appProperties->saveIfNeeded();
    }
}

juce::File FSGMExpanderAudioProcessor::getLastSoundFontFolder() const
{
    const juce::ScopedLock sl (synthLock);
    if (currentSfontPath.isNotEmpty())
    {
        const juce::File parent = juce::File (currentSfontPath).getParentDirectory();
        if (parent.isDirectory())
            return parent;
    }

    if (lastSoundFontFolder.isNotEmpty())
    {
        const juce::File folder (lastSoundFontFolder);
        if (folder.isDirectory())
            return folder;
    }

    const juce::File sysDir ("/usr/share/soundfonts");
    if (sysDir.isDirectory())
        return sysDir;

    return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
}

std::vector<juce::File> FSGMExpanderAudioProcessor::getAvailableSoundFontsInCurrentFolder() const
{
    std::vector<juce::File> result;
    const juce::File folder = getLastSoundFontFolder();
    if (! folder.isDirectory())
        return result;

    juce::Array<juce::File> found;
    folder.findChildFiles (found, juce::File::findFiles, false, "*.sf2;*.sf3;*.SF2;*.SF3");

    for (const auto& f : found)
        result.push_back (f);

    std::sort (result.begin(), result.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
               });

    return result;
}

//==============================================================================
void FSGMExpanderAudioProcessor::initFluidSynth (double sampleRate)
{
    const juce::ScopedLock sl (synthLock);

    const juce::String previousSfPath = currentSfontPath;
    cleanupFluidSynth();

    currentSampleRate = juce::jlimit (8000.0, 96000.0, sampleRate);

    settings = new_fluid_settings();
    if (settings != nullptr)
    {
        fluid_settings_setnum (settings, "synth.sample-rate", currentSampleRate);
        fluid_settings_setint (settings, "synth.midi-channels", 16);
        fluid_settings_setint (settings, "synth.audio-channels", 16);
        fluid_settings_setint (settings, "synth.audio-groups", 16);
        fluid_settings_setint (settings, "synth.effects-groups", 1);
        fluid_settings_setint (settings, "synth.polyphony", globalFx.polyphony);
        fluid_settings_setint (settings, "synth.reverb.active", 1);
        fluid_settings_setint (settings, "synth.chorus.active", 1);

        synth = new_fluid_synth (settings);
        if (synth != nullptr)
        {
            if (previousSfPath.isNotEmpty() && juce::File (previousSfPath).existsAsFile())
            {
                const int newId = fluid_synth_sfload (synth, previousSfPath.toRawUTF8(), 0);
                if (newId != FLUID_FAILED)
                {
                    currentSfontId   = newId;
                    currentSfontPath = previousSfPath;
                    updatePresetListFromLoadedSoundFont();
                }
            }

            applyGlobalFxToSynth();
            applyAllChannelStatesToSynth();
        }
    }
}

void FSGMExpanderAudioProcessor::cleanupFluidSynth()
{
    const juce::ScopedLock sl (synthLock);

    auditionSamplesRemaining = 0;
    auditionChannel = -1;

    if (synth != nullptr)
    {
        delete_fluid_synth (synth);
        synth = nullptr;
    }

    if (settings != nullptr)
    {
        delete_fluid_settings (settings);
        settings = nullptr;
    }

    currentSfontId = -1;
}

void FSGMExpanderAudioProcessor::applyGlobalFxToSynth()
{
    if (synth == nullptr)
        return;

    fluid_synth_set_gain (synth, globalFx.masterGain);
    fluid_synth_set_polyphony (synth, globalFx.polyphony);

    fluid_synth_reverb_on (synth, -1, globalFx.reverbActive ? 1 : 0);
    fluid_synth_set_reverb_group_roomsize (synth, -1, static_cast<double> (globalFx.reverbRoomSize));
    fluid_synth_set_reverb_group_damp     (synth, -1, static_cast<double> (globalFx.reverbDamp));
    fluid_synth_set_reverb_group_width    (synth, -1, static_cast<double> (globalFx.reverbWidth));
    fluid_synth_set_reverb_group_level    (synth, -1, static_cast<double> (globalFx.reverbLevel));

    fluid_synth_chorus_on (synth, -1, globalFx.chorusActive ? 1 : 0);
    fluid_synth_set_chorus_group_nr    (synth, -1, globalFx.chorusVoices);
    fluid_synth_set_chorus_group_speed (synth, -1, static_cast<double> (globalFx.chorusSpeed));
    fluid_synth_set_chorus_group_depth (synth, -1, static_cast<double> (globalFx.chorusDepth));
    fluid_synth_set_chorus_group_type  (synth, -1, globalFx.chorusType);
    fluid_synth_set_chorus_group_level (synth, -1, static_cast<double> (globalFx.chorusLevel));
}

//==============================================================================
bool FSGMExpanderAudioProcessor::loadSoundFont (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    const juce::ScopedLock sl (synthLock);
    if (synth == nullptr)
        return false;

    if (currentSfontPath == file.getFullPathName() && currentSfontId >= 0)
    {
        saveLastSoundFontToProperties (file);
        return true;
    }

    fluid_synth_all_notes_off (synth, -1);
    fluid_synth_all_sounds_off (synth, -1);

    const int newId = fluid_synth_sfload (synth, file.getFullPathName().toRawUTF8(), 0);
    if (newId == FLUID_FAILED)
        return false;

    if (currentSfontId >= 0)
        fluid_synth_sfunload (synth, currentSfontId, 0);

    currentSfontId = newId;
    currentSfontPath = file.getFullPathName();
    saveLastSoundFontToProperties (file);

    updatePresetListFromLoadedSoundFont();
    applyAllChannelStatesToSynth();

    stateChangedForGui.store (true, std::memory_order_release);
    return true;
}

void FSGMExpanderAudioProcessor::updatePresetListFromLoadedSoundFont()
{
    presets.clear();

    if (synth == nullptr || currentSfontId < 0)
        return;

    fluid_sfont_t* sfont = fluid_synth_get_sfont_by_id (synth, currentSfontId);
    if (sfont == nullptr)
        return;

    fluid_sfont_iteration_start (sfont);
    while (fluid_preset_t* preset = fluid_sfont_iteration_next (sfont))
    {
        PresetInfo info;
        info.bank    = fluid_preset_get_banknum (preset);
        info.program = fluid_preset_get_num (preset);
        const char* rawName = fluid_preset_get_name (preset);
        info.name    = (rawName != nullptr) ? juce::String::fromUTF8 (rawName) : "Unnamed";
        presets.push_back (std::move (info));
    }

    std::sort (presets.begin(), presets.end(),
               [] (const PresetInfo& a, const PresetInfo& b)
               {
                   if (a.bank != b.bank)
                       return a.bank < b.bank;
                   return a.program < b.program;
               });
}

juce::String FSGMExpanderAudioProcessor::getCurrentSoundFontPath() const
{
    const juce::ScopedLock sl (synthLock);
    return currentSfontPath;
}

juce::String FSGMExpanderAudioProcessor::getCurrentSoundFontName() const
{
    const juce::ScopedLock sl (synthLock);
    if (currentSfontPath.isEmpty())
        return "No SoundFont loaded";
    return juce::File (currentSfontPath).getFileName();
}

std::vector<PresetInfo> FSGMExpanderAudioProcessor::getPresets() const
{
    const juce::ScopedLock sl (synthLock);
    return presets;
}

std::vector<int> FSGMExpanderAudioProcessor::getAvailableBanks() const
{
    const juce::ScopedLock sl (synthLock);
    std::vector<int> banks;

    for (const auto& p : presets)
    {
        if (std::find (banks.begin(), banks.end(), p.bank) == banks.end())
            banks.push_back (p.bank);
    }

    if (banks.empty())
    {
        banks.push_back (0);
        banks.push_back (128);
    }

    std::sort (banks.begin(), banks.end());
    return banks;
}

//==============================================================================
void FSGMExpanderAudioProcessor::applyChannelGeneratorsToSynth (int ch)
{
    if (synth == nullptr || ch < 0 || ch >= 16)
        return;

    const auto& st = channels[static_cast<size_t> (ch)];

    fluid_synth_set_gen (synth, ch, GEN_COARSETUNE, static_cast<float> (st.coarseTune));
    fluid_synth_set_gen (synth, ch, GEN_FINETUNE,   static_cast<float> (st.fineTune));

    const float cutoffOffsetCents = ((static_cast<float> (st.cutoff) - 64.0f) / 64.0f) * 3600.0f;
    const float resOffsetCb       = ((static_cast<float> (st.resonance) - 64.0f) / 64.0f) * 200.0f;
    const float attackOffsetTc    = ((static_cast<float> (st.attack) - 64.0f) / 64.0f) * 3600.0f;
    const float decayOffsetTc     = ((static_cast<float> (st.decay) - 64.0f) / 64.0f) * 3600.0f;
    const float releaseOffsetTc   = ((static_cast<float> (st.release) - 64.0f) / 64.0f) * 3600.0f;

    fluid_synth_set_gen (synth, ch, GEN_FILTERFC,      cutoffOffsetCents);
    fluid_synth_set_gen (synth, ch, GEN_FILTERQ,       resOffsetCb);
    fluid_synth_set_gen (synth, ch, GEN_VOLENVATTACK,  attackOffsetTc);
    fluid_synth_set_gen (synth, ch, GEN_VOLENVDECAY,   decayOffsetTc);
    fluid_synth_set_gen (synth, ch, GEN_VOLENVRELEASE, releaseOffsetTc);
}

void FSGMExpanderAudioProcessor::applyChannelStateToSynth (int ch)
{
    if (synth == nullptr || ch < 0 || ch >= 16)
        return;

    const auto& st = channels[static_cast<size_t> (ch)];

    fluid_synth_reset_basic_channel (synth, ch);
    fluid_synth_set_basic_channel (synth, ch,
                                   st.monoMode ? FLUID_CHANNEL_MODE_OMNIOFF_MONO
                                               : FLUID_CHANNEL_MODE_OMNIOFF_POLY,
                                   1);

    fluid_synth_set_channel_type (synth, ch, st.isDrum ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);

    if (currentSfontId >= 0)
    {
        fluid_synth_program_select (synth, ch, currentSfontId, st.bank, st.program);
    }
    else
    {
        fluid_synth_bank_select (synth, ch, st.bank);
        fluid_synth_program_change (synth, ch, st.program);
    }

    fluid_synth_cc (synth, ch, 7,  juce::jlimit (0, 127, st.volume));
    fluid_synth_cc (synth, ch, 10, juce::jlimit (0, 127, st.pan));
    fluid_synth_cc (synth, ch, 11, juce::jlimit (0, 127, st.expression));
    fluid_synth_cc (synth, ch, 91, juce::jlimit (0, 127, st.reverb));
    fluid_synth_cc (synth, ch, 93, juce::jlimit (0, 127, st.chorus));

    fluid_synth_pitch_wheel_sens (synth, ch, juce::jlimit (0, 24, st.pitchBendRange));
    fluid_synth_set_interp_method (synth, ch, st.interpMethod);

    fluid_synth_cc (synth, ch, 65, (st.portamentoTime > 0) ? 127 : 0);
    fluid_synth_cc (synth, ch, 5,  juce::jlimit (0, 127, st.portamentoTime));

    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::applyAllChannelStatesToSynth()
{
    if (synth == nullptr)
        return;

    fluid_synth_reset_basic_channel (synth, -1);

    for (int ch = 0; ch < 16; ++ch)
        applyChannelStateToSynth (ch);
}

void FSGMExpanderAudioProcessor::syncChannelStatesFromSynth()
{
    if (synth == nullptr)
        return;

    for (int ch = 0; ch < 16; ++ch)
    {
        auto& st = channels[static_cast<size_t> (ch)];

        int sfId = 0, bankNum = st.bank, presetNum = st.program;
        if (fluid_synth_get_program (synth, ch, &sfId, &bankNum, &presetNum) == FLUID_OK)
        {
            st.bank    = bankNum;
            st.program = presetNum;
            st.isDrum  = (bankNum == 128);
        }

        int val = 0;
        if (fluid_synth_get_cc (synth, ch, 7,  &val) == FLUID_OK) st.volume     = val;
        if (fluid_synth_get_cc (synth, ch, 10, &val) == FLUID_OK) st.pan        = val;
        if (fluid_synth_get_cc (synth, ch, 11, &val) == FLUID_OK) st.expression = val;
        if (fluid_synth_get_cc (synth, ch, 91, &val) == FLUID_OK) st.reverb     = val;
        if (fluid_synth_get_cc (synth, ch, 93, &val) == FLUID_OK) st.chorus     = val;
    }

    applyAllChannelStatesToSynth();
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::gmReset()
{
    const juce::ScopedLock sl (synthLock);

    stopActiveAuditionNotes();

    if (synth != nullptr)
        fluid_synth_system_reset (synth);

    for (int ch = 0; ch < 16; ++ch)
        channels[static_cast<size_t> (ch)].resetToDefault (ch);

    applyAllChannelStatesToSynth();
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::resetChannelSoundShaping (int ch)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st = channels[static_cast<size_t> (ch)];
    st.coarseTune     = 0;
    st.fineTune       = 0;
    st.pitchBendRange = 2;
    st.monoMode       = false;
    st.portamentoTime = 0;
    st.interpMethod   = FLUID_INTERP_DEFAULT;
    st.cutoff         = 64;
    st.resonance      = 64;
    st.attack         = 64;
    st.decay          = 64;
    st.release        = 64;

    applyChannelStateToSynth (ch);
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::panic()
{
    const juce::ScopedLock sl (synthLock);

    stopActiveAuditionNotes();

    if (synth != nullptr)
    {
        fluid_synth_all_notes_off (synth, -1);
        fluid_synth_all_sounds_off (synth, -1);
    }
}

void FSGMExpanderAudioProcessor::stopActiveAuditionNotes()
{
    if (synth != nullptr && auditionChannel >= 0 && auditionChannel < 16)
    {
        for (int note : auditionNotes)
        {
            if (note >= 0)
                fluid_synth_noteoff (synth, auditionChannel, note);
        }
    }
    auditionNotes = { -1, -1, -1 };
    auditionChannel = -1;
    auditionSamplesRemaining = 0;
}

void FSGMExpanderAudioProcessor::triggerAuditionNote (int ch)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    if (synth == nullptr)
        return;

    stopActiveAuditionNotes();

    auditionChannel = ch;
    auto& st = channels[static_cast<size_t> (ch)];
    st.midiActivity.store (1.0f, std::memory_order_relaxed);

    if (st.isDrum)
    {
        auditionNotes = { 36, 38, 42 }; // Kick + Snare + Closed Hat
        for (int note : auditionNotes)
            fluid_synth_noteon (synth, ch, note, 105);
        auditionSamplesRemaining = static_cast<int> (currentSampleRate * 0.35);
    }
    else
    {
        auditionNotes = { 60, 64, 67 }; // C4 Major Triad
        fluid_synth_noteon (synth, ch, 60, 96);
        if (! st.monoMode)
        {
            fluid_synth_noteon (synth, ch, 64, 88);
            fluid_synth_noteon (synth, ch, 67, 92);
        }
        auditionSamplesRemaining = static_cast<int> (currentSampleRate * 0.55);
    }
}

//==============================================================================
void FSGMExpanderAudioProcessor::setChannelDrumMode (int ch, bool isDrum)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st = channels[static_cast<size_t> (ch)];
    st.isDrum = isDrum;

    if (isDrum && st.bank == 0)
        st.bank = 128;
    else if (! isDrum && st.bank == 128)
        st.bank = 0;

    applyChannelStateToSynth (ch);
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::setChannelRouteToAux (int ch, bool routeToAux)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].routeToAux = routeToAux;
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::setAllChannelsRouteToAux (bool routeToAux)
{
    const juce::ScopedLock sl (synthLock);
    for (int ch = 0; ch < 16; ++ch)
        channels[static_cast<size_t> (ch)].routeToAux = routeToAux;
    stateChangedForGui.store (true, std::memory_order_release);
}

bool FSGMExpanderAudioProcessor::areAllChannelsRoutedToAux() const
{
    const juce::ScopedLock sl (synthLock);
    for (const auto& ch : channels)
    {
        if (! ch.routeToAux)
            return false;
    }
    return true;
}

void FSGMExpanderAudioProcessor::setChannelBank (int ch, int bank)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st = channels[static_cast<size_t> (ch)];
    st.bank = juce::jmax (0, bank);
    st.isDrum = (st.bank == 128);

    if (! presets.empty())
    {
        bool foundExact = false;
        int firstProgInBank = -1;
        for (const auto& p : presets)
        {
            if (p.bank == st.bank)
            {
                if (firstProgInBank < 0)
                    firstProgInBank = p.program;
                if (p.program == st.program)
                {
                    foundExact = true;
                    break;
                }
            }
        }
        if (! foundExact && firstProgInBank >= 0)
            st.program = firstProgInBank;
    }

    applyChannelStateToSynth (ch);
    stateChangedForGui.store (true, std::memory_order_release);
}

void FSGMExpanderAudioProcessor::setChannelPreset (int ch, int bank, int program)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st   = channels[static_cast<size_t> (ch)];
    st.bank    = juce::jmax (0, bank);
    st.program = juce::jlimit (0, 127, program);
    st.isDrum  = (st.bank == 128);

    if (synth != nullptr)
    {
        fluid_synth_set_channel_type (synth, ch, st.isDrum ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        if (currentSfontId >= 0)
            fluid_synth_program_select (synth, ch, currentSfontId, st.bank, st.program);
        else
        {
            fluid_synth_bank_select (synth, ch, st.bank);
            fluid_synth_program_change (synth, ch, st.program);
        }
        applyChannelGeneratorsToSynth (ch);
    }
}

void FSGMExpanderAudioProcessor::setChannelVolume (int ch, int volume)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st  = channels[static_cast<size_t> (ch)];
    st.volume = juce::jlimit (0, 127, volume);

    if (synth != nullptr)
        fluid_synth_cc (synth, ch, 7, st.volume);
}

void FSGMExpanderAudioProcessor::setChannelPan (int ch, int pan)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st = channels[static_cast<size_t> (ch)];
    st.pan   = juce::jlimit (0, 127, pan);

    if (synth != nullptr)
        fluid_synth_cc (synth, ch, 10, st.pan);
}

void FSGMExpanderAudioProcessor::setChannelExpression (int ch, int expression)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st      = channels[static_cast<size_t> (ch)];
    st.expression = juce::jlimit (0, 127, expression);

    if (synth != nullptr)
        fluid_synth_cc (synth, ch, 11, st.expression);
}

void FSGMExpanderAudioProcessor::setChannelReverb (int ch, int reverb)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st  = channels[static_cast<size_t> (ch)];
    st.reverb = juce::jlimit (0, 127, reverb);

    if (synth != nullptr)
        fluid_synth_cc (synth, ch, 91, st.reverb);
}

void FSGMExpanderAudioProcessor::setChannelChorus (int ch, int chorus)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st  = channels[static_cast<size_t> (ch)];
    st.chorus = juce::jlimit (0, 127, chorus);

    if (synth != nullptr)
        fluid_synth_cc (synth, ch, 93, st.chorus);
}

void FSGMExpanderAudioProcessor::setChannelCoarseTune (int ch, int semitones)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st      = channels[static_cast<size_t> (ch)];
    st.coarseTune = juce::jlimit (-48, 48, semitones);

    if (synth != nullptr)
        fluid_synth_set_gen (synth, ch, GEN_COARSETUNE, static_cast<float> (st.coarseTune));
}

void FSGMExpanderAudioProcessor::setChannelFineTune (int ch, int cents)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st    = channels[static_cast<size_t> (ch)];
    st.fineTune = juce::jlimit (-99, 99, cents);

    if (synth != nullptr)
        fluid_synth_set_gen (synth, ch, GEN_FINETUNE, static_cast<float> (st.fineTune));
}

void FSGMExpanderAudioProcessor::setChannelPitchBendRange (int ch, int semitones)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st          = channels[static_cast<size_t> (ch)];
    st.pitchBendRange = juce::jlimit (0, 24, semitones);

    if (synth != nullptr)
        fluid_synth_pitch_wheel_sens (synth, ch, st.pitchBendRange);
}

void FSGMExpanderAudioProcessor::setChannelMonoMode (int ch, bool mono)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st    = channels[static_cast<size_t> (ch)];
    st.monoMode = mono;

    if (synth != nullptr)
    {
        fluid_synth_reset_basic_channel (synth, ch);
        fluid_synth_set_basic_channel (synth, ch,
                                       mono ? FLUID_CHANNEL_MODE_OMNIOFF_MONO
                                            : FLUID_CHANNEL_MODE_OMNIOFF_POLY,
                                       1);
    }
}

void FSGMExpanderAudioProcessor::setChannelPortamentoTime (int ch, int time)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st          = channels[static_cast<size_t> (ch)];
    st.portamentoTime = juce::jlimit (0, 127, time);

    if (synth != nullptr)
    {
        fluid_synth_cc (synth, ch, 65, (st.portamentoTime > 0) ? 127 : 0);
        fluid_synth_cc (synth, ch, 5,  st.portamentoTime);
    }
}

void FSGMExpanderAudioProcessor::setChannelInterpMethod (int ch, int interpMethod)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    auto& st        = channels[static_cast<size_t> (ch)];
    st.interpMethod = interpMethod;

    if (synth != nullptr)
        fluid_synth_set_interp_method (synth, ch, st.interpMethod);
}

void FSGMExpanderAudioProcessor::setChannelCutoff (int ch, int val)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].cutoff = juce::jlimit (0, 127, val);
    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::setChannelResonance (int ch, int val)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].resonance = juce::jlimit (0, 127, val);
    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::setChannelAttack (int ch, int val)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].attack = juce::jlimit (0, 127, val);
    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::setChannelDecay (int ch, int val)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].decay = juce::jlimit (0, 127, val);
    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::setChannelRelease (int ch, int val)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].release = juce::jlimit (0, 127, val);
    applyChannelGeneratorsToSynth (ch);
}

void FSGMExpanderAudioProcessor::setChannelMute (int ch, bool shouldMute)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].mute = shouldMute;

    if (synth != nullptr && ! isChannelAudible (ch))
        fluid_synth_all_notes_off (synth, ch);
}

void FSGMExpanderAudioProcessor::setChannelSolo (int ch, bool shouldSolo)
{
    if (ch < 0 || ch >= 16)
        return;

    const juce::ScopedLock sl (synthLock);
    channels[static_cast<size_t> (ch)].solo = shouldSolo;

    if (synth != nullptr)
    {
        for (int i = 0; i < 16; ++i)
        {
            if (! isChannelAudible (i))
                fluid_synth_all_notes_off (synth, i);
        }
    }
}

//==============================================================================
void FSGMExpanderAudioProcessor::setMasterGain (float newGain)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.masterGain = juce::jlimit (0.0f, 2.0f, newGain);
    if (synth != nullptr)
        fluid_synth_set_gain (synth, globalFx.masterGain);
}

void FSGMExpanderAudioProcessor::setPolyphony (int voices)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.polyphony = juce::jlimit (16, 512, voices);
    if (synth != nullptr)
        fluid_synth_set_polyphony (synth, globalFx.polyphony);
}

void FSGMExpanderAudioProcessor::setReverbActive (bool active)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.reverbActive = active;
    if (synth != nullptr)
        fluid_synth_reverb_on (synth, -1, active ? 1 : 0);
}

void FSGMExpanderAudioProcessor::setReverbLevel (float newLevel)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.reverbLevel = juce::jlimit (0.0f, 1.0f, newLevel);
    if (synth != nullptr)
        fluid_synth_set_reverb_group_level (synth, -1, static_cast<double> (globalFx.reverbLevel));
}

void FSGMExpanderAudioProcessor::setReverbRoomSize (float roomSize)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.reverbRoomSize = juce::jlimit (0.0f, 1.0f, roomSize);
    if (synth != nullptr)
        fluid_synth_set_reverb_group_roomsize (synth, -1, static_cast<double> (globalFx.reverbRoomSize));
}

void FSGMExpanderAudioProcessor::setReverbDamp (float damp)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.reverbDamp = juce::jlimit (0.0f, 1.0f, damp);
    if (synth != nullptr)
        fluid_synth_set_reverb_group_damp (synth, -1, static_cast<double> (globalFx.reverbDamp));
}

void FSGMExpanderAudioProcessor::setReverbWidth (float width)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.reverbWidth = juce::jlimit (0.0f, 100.0f, width);
    if (synth != nullptr)
        fluid_synth_set_reverb_group_width (synth, -1, static_cast<double> (globalFx.reverbWidth));
}

void FSGMExpanderAudioProcessor::setChorusActive (bool active)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusActive = active;
    if (synth != nullptr)
        fluid_synth_chorus_on (synth, -1, active ? 1 : 0);
}

void FSGMExpanderAudioProcessor::setChorusLevel (float newLevel)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusLevel = juce::jlimit (0.0f, 1.0f, newLevel);
    if (synth != nullptr)
        fluid_synth_set_chorus_group_level (synth, -1, static_cast<double> (globalFx.chorusLevel));
}

void FSGMExpanderAudioProcessor::setChorusVoices (int voices)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusVoices = juce::jlimit (1, 16, voices);
    if (synth != nullptr)
        fluid_synth_set_chorus_group_nr (synth, -1, globalFx.chorusVoices);
}

void FSGMExpanderAudioProcessor::setChorusSpeed (float speedHz)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusSpeed = juce::jlimit (0.1f, 5.0f, speedHz);
    if (synth != nullptr)
        fluid_synth_set_chorus_group_speed (synth, -1, static_cast<double> (globalFx.chorusSpeed));
}

void FSGMExpanderAudioProcessor::setChorusDepth (float depthMs)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusDepth = juce::jlimit (0.0f, 21.0f, depthMs);
    if (synth != nullptr)
        fluid_synth_set_chorus_group_depth (synth, -1, static_cast<double> (globalFx.chorusDepth));
}

void FSGMExpanderAudioProcessor::setChorusType (int type)
{
    const juce::ScopedLock sl (synthLock);
    globalFx.chorusType = (type == FLUID_CHORUS_MOD_TRIANGLE) ? FLUID_CHORUS_MOD_TRIANGLE : FLUID_CHORUS_MOD_SINE;
    if (synth != nullptr)
        fluid_synth_set_chorus_group_type (synth, -1, globalFx.chorusType);
}

int FSGMExpanderAudioProcessor::getActiveVoiceCount() const
{
    const juce::ScopedTryLock stl (synthLock);
    if (! stl.isLocked() || synth == nullptr)
        return 0;
    return fluid_synth_get_active_voice_count (synth);
}

double FSGMExpanderAudioProcessor::getCpuLoad() const
{
    const juce::ScopedTryLock stl (synthLock);
    if (! stl.isLocked() || synth == nullptr)
        return 0.0;
    return fluid_synth_get_cpu_load (synth);
}

bool FSGMExpanderAudioProcessor::isChannelAudible (int channelIndex) const
{
    if (channelIndex < 0 || channelIndex >= 16)
        return false;

    bool anySolo = false;
    for (const auto& ch : channels)
    {
        if (ch.solo)
        {
            anySolo = true;
            break;
        }
    }

    const auto& target = channels[static_cast<size_t> (channelIndex)];
    if (target.mute)
        return false;

    if (anySolo)
        return target.solo;

    return true;
}

//==============================================================================
const juce::String FSGMExpanderAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool FSGMExpanderAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool FSGMExpanderAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool FSGMExpanderAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double FSGMExpanderAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int FSGMExpanderAudioProcessor::getNumPrograms()
{
    return 1;
}

int FSGMExpanderAudioProcessor::getCurrentProgram()
{
    return 0;
}

void FSGMExpanderAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String FSGMExpanderAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void FSGMExpanderAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void FSGMExpanderAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock sl (synthLock);

    multiOutScratch.setSize (36, juce::jmax (512, samplesPerBlock));

    const double clampedRate = juce::jlimit (8000.0, 96000.0, sampleRate);
    if (synth == nullptr || std::abs (clampedRate - currentSampleRate) > 1.0)
        initFluidSynth (clampedRate);
}

void FSGMExpanderAudioProcessor::releaseResources()
{
    panic();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool FSGMExpanderAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto& bus = layouts.outputBuses.getReference (i);
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}
#endif

void FSGMExpanderAudioProcessor::handleMidiEvent (const juce::MidiMessage& msg)
{
    if (synth == nullptr)
        return;

    if (msg.isSysEx())
    {
        const char* sysexData = reinterpret_cast<const char*> (msg.getSysExData());
        const int sysexLen    = msg.getSysExDataSize();
        int handled = 0;

        // Check for Universal GM System On (7E .. 09 01) or Roland GS Reset (41 .. 42 12 40 00 7F 00 41)
        if (sysexLen >= 4 && static_cast<uint8_t> (sysexData[0]) == 0x7e
            && static_cast<uint8_t> (sysexData[2]) == 0x09 && static_cast<uint8_t> (sysexData[3]) == 0x01)
        {
            gmReset();
            return;
        }

        fluid_synth_sysex (synth, sysexData, sysexLen, nullptr, nullptr, &handled, 0);
        if (handled)
            syncChannelStatesFromSynth();
        return;
    }

    if (msg.getChannel() <= 0 || msg.getChannel() > 16)
        return;

    const int ch = msg.getChannel() - 1;
    auto& st = channels[static_cast<size_t> (ch)];

    if (msg.isNoteOn())
    {
        st.midiActivity.store (1.0f, std::memory_order_relaxed);
        if (isChannelAudible (ch))
            fluid_synth_noteon (synth, ch, msg.getNoteNumber(), msg.getVelocity());
    }
    else if (msg.isNoteOff())
    {
        fluid_synth_noteoff (synth, ch, msg.getNoteNumber());
    }
    else if (msg.isProgramChange())
    {
        st.program = juce::jlimit (0, 127, msg.getProgramChangeNumber());
        if (currentSfontId >= 0)
            fluid_synth_program_select (synth, ch, currentSfontId, st.bank, st.program);
        else
            fluid_synth_program_change (synth, ch, st.program);

        applyChannelGeneratorsToSynth (ch);
        stateChangedForGui.store (true, std::memory_order_release);
    }
    else if (msg.isController())
    {
        const int cc  = msg.getControllerNumber();
        const int val = msg.getControllerValue();

        if (cc == 126 || cc == 127)
        {
            st.monoMode = (cc == 126);
            fluid_synth_reset_basic_channel (synth, ch);
            fluid_synth_set_basic_channel (synth, ch,
                                           st.monoMode ? FLUID_CHANNEL_MODE_OMNIOFF_MONO
                                                       : FLUID_CHANNEL_MODE_OMNIOFF_POLY,
                                           1);
            stateChangedForGui.store (true, std::memory_order_release);
            return;
        }
        if (cc == 124 || cc == 125)
        {
            fluid_synth_all_notes_off (synth, ch);
            return;
        }

        fluid_synth_cc (synth, ch, cc, val);

        if (cc == 0)
        {
            st.bank = (st.isDrum && val == 0) ? 128 : val;
            fluid_synth_bank_select (synth, ch, st.bank);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 5)
        {
            st.portamentoTime = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 7)
        {
            st.volume = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 10)
        {
            st.pan = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 11)
        {
            st.expression = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 71)
        {
            st.resonance = val;
            applyChannelGeneratorsToSynth (ch);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 72)
        {
            st.release = val;
            applyChannelGeneratorsToSynth (ch);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 73)
        {
            st.attack = val;
            applyChannelGeneratorsToSynth (ch);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 74)
        {
            st.cutoff = val;
            applyChannelGeneratorsToSynth (ch);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 75)
        {
            st.decay = val;
            applyChannelGeneratorsToSynth (ch);
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 91)
        {
            st.reverb = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 93)
        {
            st.chorus = val;
            stateChangedForGui.store (true, std::memory_order_release);
        }
        else if (cc == 120)
        {
            fluid_synth_all_sounds_off (synth, ch);
        }
        else if (cc == 123)
        {
            fluid_synth_all_notes_off (synth, ch);
        }
    }
    else if (msg.isPitchWheel())
    {
        fluid_synth_pitch_bend (synth, ch, msg.getPitchWheelValue());
    }
    else if (msg.isChannelPressure())
    {
        fluid_synth_channel_pressure (synth, ch, msg.getChannelPressureValue());
    }
    else if (msg.isAftertouch())
    {
        fluid_synth_key_pressure (synth, ch, msg.getNoteNumber(), msg.getAfterTouchValue());
    }
}

void FSGMExpanderAudioProcessor::renderFluidSynthSegment (juce::AudioBuffer<float>& buffer,
                                                          int startSample,
                                                          int numFrames)
{
    if (synth == nullptr || numFrames <= 0)
        return;

    if (multiOutScratch.getNumSamples() < numFrames)
        multiOutScratch.setSize (36, numFrames, false, false, true);

    multiOutScratch.clear (0, numFrames);

    float* outPtrs[32];
    for (int i = 0; i < 32; ++i)
        outPtrs[i] = multiOutScratch.getWritePointer (i);

    float* fxPtrs[4];
    for (int i = 0; i < 4; ++i)
        fxPtrs[i] = multiOutScratch.getWritePointer (32 + i);

    fluid_synth_process (synth, numFrames, 4, fxPtrs, 32, outPtrs);

    auto masterBus = getBusBuffer (buffer, false, 0);
    const int masterChans = masterBus.getNumChannels();
    if (masterChans <= 0)
        return;

    masterBus.addFrom (0, startSample, fxPtrs[0], numFrames);
    masterBus.addFrom (0, startSample, fxPtrs[2], numFrames);
    if (masterChans > 1)
    {
        masterBus.addFrom (1, startSample, fxPtrs[1], numFrames);
        masterBus.addFrom (1, startSample, fxPtrs[3], numFrames);
    }

    const int totalBuses = getBusCount (false);

    for (int ch = 0; ch < 16; ++ch)
    {
        const float* chLeft  = outPtrs[ch * 2];
        const float* chRight = outPtrs[ch * 2 + 1];

        const int auxBusIndex = ch + 1;
        bool routedToAux = false;

        if (channels[static_cast<size_t> (ch)].routeToAux && auxBusIndex < totalBuses)
        {
            auto auxBus = getBusBuffer (buffer, false, auxBusIndex);
            if (auxBus.getNumChannels() >= 2)
            {
                auxBus.addFrom (0, startSample, chLeft,  numFrames);
                auxBus.addFrom (1, startSample, chRight, numFrames);
                routedToAux = true;
            }
        }

        if (! routedToAux)
        {
            masterBus.addFrom (0, startSample, chLeft, numFrames);
            if (masterChans > 1)
                masterBus.addFrom (1, startSample, chRight, numFrames);
        }
    }
}

void FSGMExpanderAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    const juce::ScopedTryLock stl (synthLock);
    if (! stl.isLocked() || synth == nullptr)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    if (auditionSamplesRemaining > 0)
    {
        auditionSamplesRemaining -= numSamples;
        if (auditionSamplesRemaining <= 0)
            stopActiveAuditionNotes();
    }

    int currentSample = 0;

    for (const auto metadata : midiMessages)
    {
        const int eventSample = juce::jlimit (0, numSamples, metadata.samplePosition);
        const int framesToRender = eventSample - currentSample;

        if (framesToRender > 0)
        {
            renderFluidSynthSegment (buffer, currentSample, framesToRender);
            currentSample = eventSample;
        }

        handleMidiEvent (metadata.getMessage());
    }

    const int remainingFrames = numSamples - currentSample;
    if (remainingFrames > 0)
        renderFluidSynthSegment (buffer, currentSample, remainingFrames);

    // Measure Master Stereo Output Peak for GUI Meter
    auto masterBus = getBusBuffer (buffer, false, 0);
    if (masterBus.getNumChannels() > 0)
    {
        const float pkL = masterBus.getMagnitude (0, 0, numSamples);
        const float pkR = (masterBus.getNumChannels() > 1) ? masterBus.getMagnitude (1, 0, numSamples) : pkL;

        float prevL = masterPeakL.load (std::memory_order_relaxed);
        if (pkL > prevL)
            masterPeakL.store (pkL, std::memory_order_relaxed);

        float prevR = masterPeakR.load (std::memory_order_relaxed);
        if (pkR > prevR)
            masterPeakR.store (pkR, std::memory_order_relaxed);

        if (pkL >= 1.0f || pkR >= 1.0f)
            masterClipped.store (true, std::memory_order_relaxed);
    }
}

//==============================================================================
bool FSGMExpanderAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* FSGMExpanderAudioProcessor::createEditor()
{
    return new FSGMExpanderAudioProcessorEditor (*this);
}

//==============================================================================
void FSGMExpanderAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const juce::ScopedLock sl (synthLock);

    juce::XmlElement xml ("FSGMExpanderState");
    xml.setAttribute ("soundFontPath",  currentSfontPath);
    xml.setAttribute ("masterGain",     static_cast<double> (globalFx.masterGain));
    xml.setAttribute ("polyphony",      globalFx.polyphony);

    xml.setAttribute ("reverbActive",   globalFx.reverbActive);
    xml.setAttribute ("reverbLevel",    static_cast<double> (globalFx.reverbLevel));
    xml.setAttribute ("reverbRoomSize", static_cast<double> (globalFx.reverbRoomSize));
    xml.setAttribute ("reverbDamp",     static_cast<double> (globalFx.reverbDamp));
    xml.setAttribute ("reverbWidth",    static_cast<double> (globalFx.reverbWidth));

    xml.setAttribute ("chorusActive",   globalFx.chorusActive);
    xml.setAttribute ("chorusLevel",    static_cast<double> (globalFx.chorusLevel));
    xml.setAttribute ("chorusVoices",   globalFx.chorusVoices);
    xml.setAttribute ("chorusSpeed",    static_cast<double> (globalFx.chorusSpeed));
    xml.setAttribute ("chorusDepth",    static_cast<double> (globalFx.chorusDepth));
    xml.setAttribute ("chorusType",     globalFx.chorusType);

    for (int ch = 0; ch < 16; ++ch)
    {
        const auto& st = channels[static_cast<size_t> (ch)];
        auto* chXml = xml.createNewChildElement ("Channel");
        chXml->setAttribute ("index",          ch);
        chXml->setAttribute ("isDrum",         st.isDrum);
        chXml->setAttribute ("routeToAux",     st.routeToAux);
        chXml->setAttribute ("bank",           st.bank);
        chXml->setAttribute ("program",        st.program);
        chXml->setAttribute ("volume",         st.volume);
        chXml->setAttribute ("pan",            st.pan);
        chXml->setAttribute ("expression",     st.expression);
        chXml->setAttribute ("reverb",         st.reverb);
        chXml->setAttribute ("chorus",         st.chorus);
        chXml->setAttribute ("coarseTune",     st.coarseTune);
        chXml->setAttribute ("fineTune",       st.fineTune);
        chXml->setAttribute ("pitchBendRange", st.pitchBendRange);
        chXml->setAttribute ("monoMode",       st.monoMode);
        chXml->setAttribute ("portamentoTime", st.portamentoTime);
        chXml->setAttribute ("interpMethod",   st.interpMethod);
        chXml->setAttribute ("cutoff",         st.cutoff);
        chXml->setAttribute ("resonance",      st.resonance);
        chXml->setAttribute ("attack",         st.attack);
        chXml->setAttribute ("decay",          st.decay);
        chXml->setAttribute ("release",        st.release);
        chXml->setAttribute ("mute",           st.mute);
        chXml->setAttribute ("solo",           st.solo);
    }

    copyXmlToBinary (xml, destData);
}

void FSGMExpanderAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState == nullptr || ! xmlState->hasTagName ("FSGMExpanderState"))
        return;

    const juce::String sfPath = xmlState->getStringAttribute ("soundFontPath", {});

    {
        const juce::ScopedLock sl (synthLock);
        globalFx.masterGain     = static_cast<float> (xmlState->getDoubleAttribute ("masterGain",     0.5));
        globalFx.polyphony      = xmlState->getIntAttribute ("polyphony", 256);

        globalFx.reverbActive   = xmlState->getBoolAttribute ("reverbActive", true);
        globalFx.reverbLevel    = static_cast<float> (xmlState->getDoubleAttribute ("reverbLevel",    0.5));
        globalFx.reverbRoomSize = static_cast<float> (xmlState->getDoubleAttribute ("reverbRoomSize", 0.6));
        globalFx.reverbDamp     = static_cast<float> (xmlState->getDoubleAttribute ("reverbDamp",     0.3));
        globalFx.reverbWidth    = static_cast<float> (xmlState->getDoubleAttribute ("reverbWidth",    0.8));

        globalFx.chorusActive   = xmlState->getBoolAttribute ("chorusActive", true);
        globalFx.chorusLevel    = static_cast<float> (xmlState->getDoubleAttribute ("chorusLevel",    0.5));
        globalFx.chorusVoices   = xmlState->getIntAttribute ("chorusVoices", 3);
        globalFx.chorusSpeed    = static_cast<float> (xmlState->getDoubleAttribute ("chorusSpeed",    0.3));
        globalFx.chorusDepth    = static_cast<float> (xmlState->getDoubleAttribute ("chorusDepth",    8.0));
        globalFx.chorusType     = xmlState->getIntAttribute ("chorusType", FLUID_CHORUS_MOD_SINE);

        applyGlobalFxToSynth();
    }

    if (sfPath.isNotEmpty())
    {
        const juce::File sfFile (sfPath);
        if (sfFile.existsAsFile())
            loadSoundFont (sfFile);
    }

    {
        const juce::ScopedLock sl (synthLock);
        for (auto* chXml : xmlState->getChildWithTagNameIterator ("Channel"))
        {
            const int ch = chXml->getIntAttribute ("index", -1);
            if (ch >= 0 && ch < 16)
            {
                auto& st          = channels[static_cast<size_t> (ch)];
                st.isDrum         = chXml->getBoolAttribute ("isDrum",         ch == 9);
                st.routeToAux     = chXml->getBoolAttribute ("routeToAux",     false);
                st.bank           = chXml->getIntAttribute  ("bank",           st.isDrum ? 128 : 0);
                st.program        = chXml->getIntAttribute  ("program",        0);
                st.volume         = chXml->getIntAttribute  ("volume",         100);
                st.pan            = chXml->getIntAttribute  ("pan",            64);
                st.expression     = chXml->getIntAttribute  ("expression",     127);
                st.reverb         = chXml->getIntAttribute  ("reverb",         40);
                st.chorus         = chXml->getIntAttribute  ("chorus",         0);
                st.coarseTune     = chXml->getIntAttribute  ("coarseTune",     0);
                st.fineTune       = chXml->getIntAttribute  ("fineTune",       0);
                st.pitchBendRange = chXml->getIntAttribute  ("pitchBendRange", 2);
                st.monoMode       = chXml->getBoolAttribute ("monoMode",       false);
                st.portamentoTime = chXml->getIntAttribute  ("portamentoTime", 0);
                st.interpMethod   = chXml->getIntAttribute  ("interpMethod",   FLUID_INTERP_DEFAULT);
                st.cutoff         = chXml->getIntAttribute  ("cutoff",         64);
                st.resonance      = chXml->getIntAttribute  ("resonance",      64);
                st.attack         = chXml->getIntAttribute  ("attack",         64);
                st.decay          = chXml->getIntAttribute  ("decay",          64);
                st.release        = chXml->getIntAttribute  ("release",        64);
                st.mute           = chXml->getBoolAttribute ("mute",           false);
                st.solo           = chXml->getBoolAttribute ("solo",           false);
            }
        }

        applyAllChannelStatesToSynth();
    }

    stateChangedForGui.store (true, std::memory_order_release);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FSGMExpanderAudioProcessor();
}
