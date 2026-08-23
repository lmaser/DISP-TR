#pragma once

class DisperserAudioProcessor;
namespace juce { class AudioProcessorEditor; }

namespace TR::DispUIV2
{
juce::AudioProcessorEditor* createEditor(DisperserAudioProcessor& processor);
}
