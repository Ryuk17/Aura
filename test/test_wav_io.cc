#include "common_audio/wav_file.h"

int main(int argc, char **argv) 
{
    char wav_file[1024] = "data/voice_engine/audio_tiny8.wav";
    int sample_num = 1024;
    
    WavReader wav_reader(wav_file);
    std::cout<<< "sample_rate: " << wav_reader.sample_rate() << std::endl;
    std::cout<<< "num_channels: " << wav_reader.num_channels() << std::endl;
    wav_reader.Reset();

    WavWriter wav_writer("data/voice_engine/audio_tiny8_out.wav", wav_reader.num_channels(), wav_reader.sample_rate());

    float samples[1024];
    while(wav_reader.ReadSamples(sample_num, samples)) 
    {
        wav_writer.WriteSamples(samples, sample_num);
    }
    return 0
}
