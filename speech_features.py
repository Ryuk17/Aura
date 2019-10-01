"""
@ Filename:       speech_features.py
@ Author:         Danc1elion
@ Create Date:    2019-10-01   
@ Update Date:    2019-10-01 
@ Description:    Implement speech_features
"""

import numpy as np
import matplotlib.pyplot as plt
import wave

def displaySpeech(sample):
    """
    display waveform of a given speech sample
    :param sample: speech smaple
    :return:
    """
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)
    time = np.arange(0, nframes) * (1.0 / framerate)

    plt.plot(time, wave_data)
    plt.xlabel("time (seconds)")
    plt.show()


def shortEnergy(sample_name, overlapping=0, window_length=20, window_type='rectangle', display=True):
    """
    calculate the short energy of a given sample
    :param sample_name: speech sample name
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short energy
    """
    sample = wave.open(sample_name)
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)

    frame_num = len(wave_data) // window_length
    energy = np.zeros(frame_num)

    if window_type == 'rectangle':
        for i in range(frame_num):
            tmp = np.sum(np.square(wave_data[i:i+window_length]))
            energy[i] = tmp

    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, energy)
        plt.ylabel("Short Energy")
        plt.xlabel("Time (seconds)")
        plt.show()

    sample.close()
    return energy



def shortzcr(sample, window_length=20, window_type='rectangle'):
    pass
