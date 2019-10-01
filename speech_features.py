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

from base_functions import *


def shortEnergy(sample_name, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short energy of a given sample
    :param sample_name: speech sample name
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short energy
    """
    # get basic information
    sample = wave.open(sample_name)
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    wave_data = np.fromstring(str_data, dtype=np.short)

    # enframe
    frame_num = len(wave_data) // window_length
    energy = np.zeros(frame_num)

    # calculate the short energy
    for i in range(48, frame_num):
        frame = windows(wave_data[i*window_length:(i+1)*window_length], window_type)
        energy[i] = np.sum(np.array(frame, dtype='int64') ** 2)

    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, energy)
        plt.ylabel("Short Energy")
        plt.xlabel("Time (seconds)")
        plt.show()

    sample.close()
    return energy


def shortZcc(sample_name, overlapping=0, window_length=240, window_type='Rectangle', display=True):
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


    for i in range(frame_num):
        frame = windows(wave_data[i*window_length:(i+1)*window_length], window_type)
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
