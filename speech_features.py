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
from scipy import special

def windows(samples, beta=8.5, type='Rectangle'):
    """
    calculate the output of different windows
    :param samples: samples
    :param beta: parameter for kaiser window
    :param type: window type
    :return: data after windowed
    """
    N = len(samples)
    data = samples
    if type == 'Retangle':
        data = samples
    elif type == 'Triangle':
        for i in range(N):
            if i < N:
                data[i] = 2 * i / (N - 1)
            else:
                data[i] = 2 - 2 * i / (N - 1)
    elif type == 'Hamming':
        for i in range(N):
            data[i] = 0.54 - 0.56 * np.cos(2 * np.pi * i / (N - 1))
    elif type == 'Hanning':
        for i in range(N):
            data[i] = 0.5 * (1 - np.cos(2 * np.pi * i / (N - 1)))
    elif type == 'Blackman':
        for i in range(N):
            data[i] = 0.42 - 0.5 * (1 - np.cos(2 * np.pi * i / (N - 1))) + 0.08 * np.cos(4 * np.pi * i / (N - 1))
    elif type == 'Kaiser':
        for i in range(N):
            data[i] = special.j0(beta * np.sqrt(1 - np.square(1 - 2 * i /(N - 1)))) / special.j0(beta)
    else:
        raise NameError('Unrecongnized window type')
    return data

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


def shortEnergy(sample_name, overlapping=0, window_length=20, window_type='Rectangle', display=True):
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
            energy[i] = np.sum(np.square(wave_data[i:i+window_length]))

    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, energy)
        plt.ylabel("Short Energy")
        plt.xlabel("Time (seconds)")
        plt.show()

    sample.close()
    return energy



def shortZcc(sample_name, overlapping=0, window_length=20, window_type='Rectangle', display=True):
    """
    calculate the short count of a given sample
    :param sample_name: speech sample name
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short zero crossing count
    """
    sample = wave.open(sample_name)
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)

    frame_num = len(wave_data) // window_length
    zcc = np.zeros(frame_num)

    if window_type == 'rectangle':
        for i in range(frame_num):
            frame = wave_data[i*window_length:(i+1)*window_length]
            for j in range(1, window_length):
                zcc[i] += abs(np.sign(frame[j]) - np.sign(frame[j-1]))

    zcc /= 2
    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, zcc)
        plt.ylabel("Zero Crossing Count")
        plt.xlabel("Time (seconds)")
        plt.show()

    sample.close()
    return zcc

def shortCorrelation(sample_name, overlapping=0, window_length=20, window_type='Rectangle', display=True):
    pass
