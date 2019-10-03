"""
@ Filename:       speech_features.py
@ Author:         Danc1elion
@ Create Date:    2019-10-01   
@ Update Date:    2019-10-01 
@ Description:    Implement speech_features
"""

from basic_functions import *


def shortEnergy(samples, params, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short energy of a given sample
    :param samples: sample data
    :param param: speech parameters
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short energy
    """
    # get basic information
    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)

    # enframe
    frame_num = len(samples) // window_length
    energy = np.zeros(frame_num)

    # calculate the short energy
    for i in range(48, frame_num):
        frame = windows(samples[i*window_length:(i+1)*window_length], window_type)
        energy[i] = np.sum(np.array(frame, dtype='int64') ** 2)

    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, energy)
        plt.title("Short Energy")
        plt.ylabel("Short Energy")
        plt.xlabel("Time (seconds)")
        plt.show()

    return energy


def shortZcc(samples, params, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short count of a given sample
    :param samples: sample data
    :param param: speech parameters
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short zero crossing count
    """
    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)

    frame_num = len(samples) // window_length
    zcc = np.zeros(frame_num)


    for i in range(frame_num):
        frame = windows(samples[i*window_length:(i+1)*window_length], window_type)
        for j in range(1, window_length):
            zcc[i] += abs(np.sign(frame[j]) - np.sign(frame[j-1]))

    zcc /= 2
    if display:
        time = np.arange(0, frame_num) * (window_length / framerate)
        plt.plot(time, zcc)
        plt.title("Zero Crossing Count")
        plt.ylabel("Zero Crossing Count")
        plt.xlabel("Time (seconds)")
        plt.show()

    return zcc


def Spectogram(samples, params, fft_points=256, overlapping=0, window_length=240, window_type='Rectangle', display=True):

    time_signal = preEmphasis(samples, params)
    frame_num = len(samples) // window_length







def shortCorrelation(sample_name, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    pass
