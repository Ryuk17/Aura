"""
@ Filename:       base_functions.py
@ Author:         Danc1elion
@ Create Date:    2019-10-01   
@ Update Date:    2019-10-01 
@ Description:    Implement base_functions
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
