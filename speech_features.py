"""
@ Filename:       speech_features.py
@ Author:         Danc1elion
@ Create Date:    2019-10-01   
@ Update Date:    2019-10-01 
@ Description:    Implement speech_features
"""

from basic_functions import *


def shortEnergy(samples, params, normalize=False, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short energy of a given sample
    :param samples: sample data
    :param param: speech parameters
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short energy
    """
    # get basic information
    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)

    if normalize:
        samples = normalization(samples)

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


def shortZcc(samples, params, normalize=False, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short count of a given sample
    :param samples: sample data
    :param param: speech parameters
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short zero crossing count
    """
    if normalize:
        samples = normalization(samples)

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


def Spectogram(samples, params, normalize=False, fft_points=None, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the spectogram of samples
    :param samples: speech samples
    :param params: speech parameters
    :param normalize: whether to normalize data
    :param fft_points: fft_points
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display spectogram
    :return: spectogram
    """
    if normalize:
        samples = normalization(samples)

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


def shortCorrelation(samples, params, normalize=False, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the short correlation of a given speech sample
    :param samples: speech samples
    :param params: speech parameters
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display correlation
    :return: correlation of each frame
    """
    if normalize:
        samples = normalization(samples)

    frames = enframe(samples, overlapping=overlapping, window_length=window_length, window_type=window_type)
    correlation = np.zeros(shape=frames.shape)

    for k in range(len(frames[0])):
        for i in range(len(frames[0]) - k):
            correlation[:, k] += frames[:, i] * frames[:, i + k]

    if display:
        time = np.arange(0, len(frames[100]))
        plt.plot(time, correlation[100])
        plt.title("100th Frame Correlation")
        plt.ylabel("Correlation")
        plt.xlabel("Delay")
        plt.show()

    return correlation


def shortAverageMagnitudeDifference(samples, params, normalize=True, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    calculate the average magnitude difference of a given speech sample
    :param samples: speech samples
    :param params: speech parameters
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display average magnitude difference
    :return: average magnitude difference of each frame
    """
    if normalize:
        samples = normalization(samples)

    frames = enframe(samples, overlapping=overlapping, window_length=window_length, window_type=window_type)
    difference = np.zeros(shape=frames.shape)

    for k in range(1, len(frames[0])):
        for i in range(len(frames[0]) - k):
            difference[:, k] += abs(frames[:, i] - frames[:, i + k])

    for n in range(len(frames)):
        difference[n] /= abs(np.average(frames[n]))

    if display:
        time = np.arange(0, len(frames[100]))
        plt.plot(time, difference[100])
        plt.title("100th Frame Average Magnitude Difference")
        plt.ylabel("Average Magnitude Difference")
        plt.xlabel("Delay")
        plt.show()

    return difference

def estimatePitch(samples, params, normalize=False, method='Correlation', smooth='None', L=2, overlapping=0, window_length=240, window_type='Rectangle', display=True):
    """
    estimate pitch
    :param samples: speech samples
    :param params: speech parameters
    :param normalize: whether to normalize data
    :param method: estimation method
    :param smooth: smooth method
    :param L: smooth window length
    :param overlapping: overlapping length
    :param window_length: indow length
    :param window_type: window type
    :param display:whether to display pitch
    :return: pitch
    """
    if normalize:
        samples = normalization(samples)

    nchannels, sampwidth, framerate, nframes, comptype, compname = getParams(params)
    if method == 'Correlation':
        correlation = shortCorrelation(samples, params, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(correlation))
        for i in range(len(correlation)):
            index = np.argmax(correlation[i][30:]) + 1
            pitch[i] = framerate / index
    elif method == 'AMDF':
        AMDF = shortAverageMagnitudeDifference(samples, params, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(AMDF))
        for i in range(len(AMDF)):
            index = np.argmin(AMDF[i][30:])
            pitch[i] = framerate / index
    else:
        raise NameError('Unrecongnized estimate method')

    if smooth == 'median':
        smoothed_pitch = np.zeros(len(pitch))
        for i in range(len(pitch)):
            if i < L:
                window = pitch[ : L]
            elif i == len(pitch) - 1:
                window = pitch[-L:]
            else:
                window = pitch[i - L : i + L + 1]
            window = np.sort(window)
            smoothed_pitch[i] = window[L // 2]
    elif smooth == 'linear':
        smoothed_pitch = np.zeros(len(pitch))
        mask = 1 / (2 * L + 1)
        for i in range(len(pitch)):
            if i < L:
                window = pitch[: L] * mask
            elif i == len(pitch) - 1:
                window = pitch[-L:] * mask
            else:
                window = pitch[i - L: i + L + 1] * mask
            smoothed_pitch[i] = np.sum(window)
    elif smooth == 'None':
        smoothed_pitch = pitch
    else:
        raise NameError('Unrecongnized smooth method')

    if display:
        time = np.arange(0, len(smoothed_pitch))
        plt.scatter(time, smoothed_pitch)
        plt.title("Pitch")
        plt.ylabel("Frequency")
        plt.xlabel("Frames")
        plt.show()

    return smoothed_pitch
