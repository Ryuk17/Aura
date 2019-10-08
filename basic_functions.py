"""
@ Filename:       basic_functions.py
@ Author:         Danc1elion
@ Create Date:    2019-10-03   
@ Update Date:    2019-10-04
@ Description:    Implement basic_functions
"""

import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
from scipy import special
from scipy.fftpack import dct
import scipy.io.wavfile as wav
from scipy.fftpack import fft

def normalization(data):
    """
    normalize data into [-1, 1]
    :param data: input data
    :return: normalized data
    """
    normalized_data = 2 * (data - min(data)) / (max(data) - min(data)) - 1
    return normalized_data


def enframe(samples, fs, beta=8.5, overlapping=0, window_length=240, window_type='Rectangle'):
    """
    divede samples into frame
    :param samples:
    :param fs: sample frequency
    :param frame_num:
    :param window_length:
    :param window_type:
    :return: enframed frames
    """

    frames_num = len(samples) // (window_length - overlapping)
    frames = np.zeros([frames_num, window_length])
    for i in range(frames_num):
        start = i * (window_length - overlapping)
        end = start + window_length
        data = samples[start:end]

        N = len(data)
        x = np.linspace(0, N - 1, N, dtype=np.int64)

        if window_type == 'Rectangle':
            data = samples
        elif window_type == 'Triangle':
            for i in range(N):
                if i < N:
                    data[i] = 2 * i / (N - 1)
                else:
                    data[i] = 2 - 2 * i / (N - 1)
        elif window_type == 'Hamming':
            w = 0.54 - 0.46 * np.cos(2 * np.pi * x / (N - 1))
            data = data * w
        elif window_type == 'Hanning':
            w = 0.5 * (1 - np.cos(2 * np.pi * x / (N - 1)))
            data = data * w
        elif window_type == 'Blackman':
            w = 0.42 - 0.5 * (1 - np.cos(2 * np.pi * x / (N - 1))) + 0.08 * np.cos(4 * np.pi * x / (N - 1))
            data = data * w
        elif window_type == 'Kaiser':
            w = special.j0(beta * np.sqrt(1 - np.square(1 - 2 * x / (N - 1)))) / special.j0(beta)
            data = data * w
        else:
            raise NameError('Unrecongnized window type')

        frames[i] = data
    return frames


def preEmphasis(samples, fs, alpha=0.9375, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    per emphasis speech
    :param samples: sample data
    :param fs: sample frequency
    :param alpha: parameter
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display processed speech
    :return: processed speech
    """

    y = np.zeros(len(samples))
    y[0] = samples[0]

    # pre emphasis
    for i in range(1, len(samples)):
        y[i] = samples[i] - alpha * samples[i-1]

    if display:
        time = np.arange(0, len(samples)) * (1.0 / fs)
        plt.plot(time, samples)
        plt.title("Pre-emphasis")
        plt.ylabel("Waveform")
        plt.xlabel("Time (seconds)")
        plt.show()

    return y


def windows(samples, beta=8.5, window_type='Rectangle'):
    """
    calculate the output of different windows
    :param samples: samples
    :param beta: parameter for kaiser window
    :param type: window type
    :return: data after windowed
    """
    N = len(samples)
    data = samples
    x = np.linspace(0, N - 1, N, dtype=np.int64)
    if window_type == 'Rectangle':
        data = samples
    elif window_type == 'Triangle':
        for i in range(N):
            if i < N:
                data[i] = 2 * i / (N - 1)
            else:
                data[i] = 2 - 2 * i / (N - 1)
    elif window_type == 'Hamming':
        w = 0.54 - 0.46 * np.cos(2 * np.pi * x / (N - 1))
        data = data * w
    elif window_type == 'Hanning':
        w = 0.5 * (1 - np.cos(2 * np.pi * x / (N - 1)))
        data = data * w
    elif window_type == 'Blackman':
        w = 0.42 - 0.5 * (1 - np.cos(2 * np.pi * x / (N - 1))) + 0.08 * np.cos(4 * np.pi * x / (N - 1))
        data = data * w
    elif window_type == 'Kaiser':
        w = special.j0(beta * np.sqrt(1 - np.square(1 - 2 * x /(N - 1)))) / special.j0(beta)
        data = data * w
    else:
        raise NameError('Unrecongnized window type')
    return data


def displaySpeech(samples, fs):
    """
    display waveform of a given speech sample
    :param sample_name: speech sample name
    :param fs: sample frequency
    :return:
    """
    time = np.arange(0, len(samples)) * (1.0 / fs)

    plt.plot(time, samples)
    plt.title("Speech")
    plt.xlabel("time (seconds)")
    plt.show()
