"""
@ Filename:       basic_functions.py
@ Author:         Danc1elion
@ Create Date:    2019-10-03   
@ Update Date:    2019-10-03 
@ Description:    Implement basic_functions
"""

import numpy as np
import matplotlib.pyplot as plt
import wave
from scipy import special


def preEmphasis(sample_name, alpha=0.9375, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    per emphasis speech
    :param sample_name: speech sample name
    :param alpha: parameter
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display processed speech
    :return: processed speech
    """

    # get basic information
    sample = wave.open(sample_name)
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)

    y = np.zeros(len(wave_data))
    y[0] = wave_data[0]

    # pre emphasis
    for i in range(1, len(wave_data)):
        y[i] = wave_data[i] - alpha * wave_data[i-1]

    if display:
        time = np.arange(0, nframes) * (1.0 / framerate)
        plt.plot(time, wave_data)
        plt.title("Pre-emphasis")
        plt.ylabel("Waveform")
        plt.xlabel("Time (seconds)")
        plt.show()

    return y


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
    if type == 'Rectangle':
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


def displaySpeech(sample_name):
    """
    display waveform of a given speech sample
    :param sample_name: speech sample name
    :return:
    """
    sample = wave.open(sample_name)
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)
    time = np.arange(0, nframes) * (1.0 / framerate)

    plt.plot(time, wave_data)
    plt.title("Speech")
    plt.xlabel("time (seconds)")
    plt.show()
