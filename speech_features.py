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
    frames = enframe(samples, overlapping=overlapping, window_length=window_length, window_type=window_type)
    energy = np.zeros(len(frames))

    # calculate the short energy
    for i in range(len(frames)):
        energy[i] = np.sum(np.array(frames[i], dtype='int64') ** 2)

    if display:
        time = np.arange(0, len(frames))
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

    frames = enframe(samples, overlapping=overlapping, window_length=window_length, window_type=window_type)
    zcc = np.zeros(len(frames))

    for i in range(len(frames)):
        for j in range(1, len(frames[i])):
            zcc[i] += abs(np.sign(frames[i][j]) - np.sign(frames[i][j-1]))

    zcc /= 2
    if display:
        time = np.arange(0, len(frames))
        plt.plot(time, zcc)
        plt.title("Zero Crossing Count")
        plt.ylabel("Zero Crossing Count")
        plt.xlabel("Frames")
        plt.show()

    return zcc


def Spectogram(samples, params, fft_points=None, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the spectogram of samples
    :param samples: speech samples
    :param params: speech parameters
    :param fft_points: fft_points
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display spectogram
    :return: spectogram
    """
    time_signal = preEmphasis(samples, params, display=False)
    frames = enframe(time_signal, overlapping=overlapping, window_length=window_length, window_type=window_type)
    spectogram = []
    for i in range(len(frames)):
        Y = np.fft.fft(frames[i], n=fft_points)
        P2 = np.abs(Y / len(frames[i]))
        P1 = P2[:len(P2) // 2 + 1]
        spectogram.append(P1)

    spectogram = np.array(spectogram)
    if display:
        plt.imshow(spectogram.T,  interpolation='bilinear', cmap='hot', origin='lower')
        plt.colorbar(cax=None, ax=None, shrink=0.5)
        plt.title("Spectogram")
        plt.ylabel("Frequency")
        plt.xlabel("Frames")
        plt.show()

    return spectogram


def shortCorrelation(samples, params, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short correlation of a given speech sample
    :param samples: speech samples
    :param params: speech parameters
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display correlation
    :return: correlation
    """
    frames = enframe(samples, overlapping=overlapping, window_length=window_length, window_type=window_type)
    correlation = np.zeros(shape=frames.shape)

    for k in range(len(frames[0])):
        for i in range(len(frames[0]) - k):
            correlation[:, k] += frames[:, i] * frames[:, i + k]

    if display:
        time = np.arange(0, len(frames[0]))
        plt.plot(time, correlation[0])
        plt.title("First Frame Correlation")
        plt.ylabel("Correlation")
        plt.xlabel("Delay")
        plt.show()

    return correlation



