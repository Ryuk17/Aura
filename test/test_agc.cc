#include <iostream>
#include "common_audio/wav_file.h"
#include "modules/audio_processing/agc/agc.h"

#define FRAME_LEN (160)

using namespace webrtc;

int main(int argc, char **argv) 
{
    char wav_file[1024] = "data/voice_engine/audio_short16.wav";

    WavReader wav_reader(wav_file);
    std::cout<< "sample_rate: " << wav_reader.sample_rate() << std::endl;
    std::cout<< "num_channels: " << wav_reader.num_channels() << std::endl;
    std::cout<< "read samples: " << wav_reader.num_samples() << std::endl;

    WavWriter wav_writer(
        "data/voice_engine/audio_short16_agc_out.wav", 
        wav_reader.sample_rate(),
        wav_reader.num_channels(), 
        WavFile::SampleFormat::kInt16
    );

    auto rate = wav_reader.sample_rate();

    Agc agc;
    agc.set_target_level_dbfs(-3);

    int total_samples = 0;
    int16_t wav_data[FRAME_LEN] = {0};
    while(true) 
    {
        int read_samples = wav_reader.ReadSamples(FRAME_LEN, wav_data);
        ArrayView<int16_t> input(wav_data);
        for(int i = 0; i < read_samples; i++)
        {
            printf("%d ", input[i]);
        }
        printf("\n");
        agc.Process(input);
        for(int i = 0; i < read_samples; i++)
        {
            printf("%d ", input[i]);
        }
        printf("\n");
        exit(0);
        std::copy(input.begin(), input.end(), wav_data);
        wav_writer.WriteSamples(wav_data, read_samples);

        total_samples += read_samples;
        if(read_samples < FRAME_LEN)
        {
            break;
        }
    }
    std::cout<< "total write samples: " << total_samples << std::endl;
    return 0;
}
