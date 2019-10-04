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


def normalization(data):
    """
    normalize data into [-1, 1]
    :param data: input data
    :return: normalized data
    """
    normalized_data = 2 * (data - min(data)) / (max(data) - min(data)) - 1
    return normalized_data

def getParams(param):
    """
    unpack paramters
    :param param: speech paramters
    :return: nchannels, sampwidth, framerate, nframes, comptype, compname
    """
    nchannels, sampwidth, framerate, nframes, comptype, compname = param[0], param[1], param[2], param[3], param[4], param[5]
    return nchannels, sampwidth, framerate, nframes, comptype, compname


def enframe(samples, overlapping=0, window_length=240, window_type='Rectangle'):
    """
    divede samples into frame
    :param samples:
    :param frame_num:
    :param window_length:
    :param window_type:
    :return: enframed frames
    """

    frames = []
    i = window_length
    while i < len(samples):
        if i + window_length < len(samples):
            frames.append(windows(samples[i:i+window_length], type=window_type))
            i += window_length - overlapping
        else:
            frames.append(windows(samples[i:], type=window_type))
            break

    return np.array(frames)

def preEmphasis(samples, params, alpha=0.9375, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    per emphasis speech
    :param samples: sample data
    :param param: speech parameters
    :param alpha: parameter
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display processed speech
    :return: processed speech
    """

    # get basic information
    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)

    y = np.zeros(len(samples))
    y[0] = samples[0]

    # pre emphasis
    for i in range(1, len(samples)):
        y[i] = samples[i] - alpha * samples[i-1]

    if display:
        time = np.arange(0, nframes) * (1.0 / framerate)
        plt.plot(time, samples)
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


def displaySpeech(samples, params):
    """
    display waveform of a given speech sample
    :param sample_name: speech sample name
    :param params: speech parameters
    :return:
    """
    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)
    time = np.arange(0, nframes) * (1.0 / framerate)

    plt.plot(time, samples)
    plt.title("Speech")
    plt.xlabel("time (seconds)")
    plt.show()
