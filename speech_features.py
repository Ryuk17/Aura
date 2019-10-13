"""
@ Filename:       speech_features.py
@ Author:         Danc1elion
@ Create Date:    2019-10-01   
@ Update Date:    2019-10-04
@ Description:    Implement speech_features
"""

from basic_functions import *
from matplotlib.ticker import FuncFormatter
from tqdm import tqdm

def shortEnergy(samples, fs, normalize=False, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the short energy of a given sample
    :param samples: sample data
    :param fs: sample frequency
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short energy
    """
    if normalize:
        samples = normalization(samples)

    # enframe
    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)
    energy = np.zeros([len(frames), 1])

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


def shortZcc(samples, fs, normalize=False, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the short count of a given sample
    :param samples: sample data
    :param fs: sample frequency
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short zero crossing count
    """

    if normalize:
        samples = normalization(samples)

    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)
    zcc = np.zeros([len(frames), 1])

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


def extractSpectogram(samples, fs, normalize=False, fft_points=512, overlapping=0, window_length=240, window_type='Hamming', display=False):
    """
    calculate the spectogram of samples
    :param samples: speech samples
    :param fs: sample frequency
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

    time_signal = preEmphasis(samples, fs, display=False)
    frames = enframe(time_signal, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)
    length = fft_points // 2 + 1
    spectogram = np.zeros([len(frames), length])
    for i in range(len(frames)):
        Y = fft(frames[i], n=fft_points)
        P2 = np.abs(Y)
        P1 = P2[:length]
        spectogram[i] = P1

    spectogram = np.log(spectogram + 1)
    if display:
        plt.imshow(spectogram.T,  cmap='Greys', origin='lower')
        plt.colorbar(cax=None, ax=None, shrink=0.5)
        plt.title("Spectogram")
        plt.ylabel("Frequency")
        plt.xlabel("Frames")
        plt.show()

    return spectogram


def shortCorrelation(samples, fs, normalize=True, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the short correlation of a given speech sample
    :param samples: speech samples
    :param fs: sample frequency
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display correlation
    :return: correlation of each frame
    """
    if normalize:
        samples = normalization(samples)

    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)
    correlation = np.zeros(shape=frames.shape)

    for k in range(len(frames[0])):
        for i in range(len(frames[0]) - k):
            correlation[:, k] += frames[:, i] * frames[:, i + k]

    if display:
        plt.imshow(correlation.T, interpolation='bilinear', cmap='jet', origin='lower')
        plt.colorbar(cax=None, ax=None, shrink=0.8)
        plt.title("Correlogram")
        plt.ylabel("Delay")
        plt.xlabel("Frames")
        plt.show()

    return correlation


def shortAverageMagnitudeDifference(samples, fs, normalize=True, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the average magnitude difference of a given speech sample
    :param samples: speech samples
    :param fs: sample frequency
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display average magnitude difference
    :return: average magnitude difference of each frame
    """
    if normalize:
        samples = normalization(samples)

    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)
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

def estimatePitch(samples, fs, normalize=False, method='Correlation', smooth='None', L=2, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    estimate pitch
    :param samples: speech samples
    :param fs: sample frequency
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

    if method == 'Correlation':
        correlation = shortCorrelation(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(correlation))
        for i in range(len(correlation)):
            index = np.argmax(correlation[i][30:]) + 1
            pitch[i] = fs / index
    elif method == 'AMDF':
        AMDF = shortAverageMagnitudeDifference(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(AMDF))
        for i in range(len(AMDF)):
            index = np.argmin(AMDF[i][30:])
            pitch[i] = fs / index
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


def extractMFCC(samples, fs, normalize=False, low_freq=0, high_freq=8000, fft_points=256, Mel_filters=40, Mel_cofficients=12, overlapping=0, window_length=240, window_type='Hamming', display=False):
    """
    extract MFCC from speech
    :param samples: speech sample
    :param fs: sample frequency
    :param normalize: whether to normalize speech
    :param fft_points: fft points
    :param low_freq: minimum frequency
    :param high_freq: maximum frequency
    :param Mel_filters: the number of Mel filters
    :param Mel_cofficients: the number of Mel cofficients
    :param overlapping: overlapping length
    :param window_length: frame length
    :param window_type: window type
    :param display: whether to display
    :return: MFCC
    """
    if normalize:
        samples = normalization(samples)

    # pre emphasis
    samples = preEmphasis(samples, fs, display=False)

    # enframe
    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)

    # fft and power spectrum
    spectrum = np.fft.fft(frames, fft_points)
    power = np.abs(spectrum)[:, 0: fft_points // 2 + 1]
    power = power ** 2 / fft_points

    # transfer frequency into Mel-frequency
    low_freq = 2595 * np.log10(1 + low_freq / 700)
    high_freq = min(2595 * np.log10(1 + high_freq / 700), fs / 2)
    mel_freq = np.linspace(low_freq, high_freq, Mel_filters + 2)
    hz_freq = (700 * (10 ** (mel_freq / 2595) - 1))

    bin = np.floor((fft_points // 2 + 1) * hz_freq / fs)

    fbank = np.zeros((Mel_filters, int(np.floor(fft_points // 2 + 1))))

    for m in range(1, Mel_filters + 1):
        low = int(bin[m - 1])
        center = int(bin[m])
        high = int(bin[m + 1])
        for k in range(low, center):
            fbank[m - 1, k] = (k - bin[m - 1]) / (bin[m] - bin[m - 1])
        for k in range(center, high):
            fbank[m - 1, k] = (bin[m + 1] - k) / (bin[m + 1] - bin[m])

    filter_banks = np.dot(power, fbank.T)
    filter_banks = np.where(filter_banks == 0, np.finfo(float).eps, filter_banks)
    filter_banks = 20 * np.log10(filter_banks).clip(1e-5, np.inf)
    filter_banks -= (np.mean(filter_banks, axis=0) + 1e-8)

    mfcc = dct(filter_banks, type=2, axis=1, norm='ortho')[:, 1: (Mel_cofficients + 1)]

    if display:
        plt.imshow(mfcc.T, cmap='jet', origin='lower')
        plt.axis('auto')
        plt.colorbar(cax=None, ax=None)
        plt.title("Mel Frequency Cepstrum Coefficient")
        plt.ylabel("MFCC")
        plt.xlabel("Frames")
        plt.show()

    return mfcc


def extractBFCC(samples, fs, normalize=False, low_freq=0, high_freq=8000, fft_points=256, Bark_filters=24, Bark_cofficients=12, overlapping=0, window_length=240, window_type='Hamming', display=False):
    if normalize:
        samples = normalization(samples)

    # pre emphasis
    samples = preEmphasis(samples, fs, display=False)

    # enframe
    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)

    # fft and power spectrum
    spectrum = np.fft.fft(frames, fft_points)
    power = np.abs(spectrum)[:,0: fft_points // 2 + 1]
    power = power ** 2 / fft_points

    # transfer frequency into Bark-frequency
    low_freq = 6. * np.arcsinh(low_freq / 600.)
    high_freq = min(6. * np.arcsinh(high_freq / 600.), fs / 2)
    bark_freq = np.linspace(low_freq, high_freq, Bark_filters + 2)
    hz_freq = 600. * np.sinh(bark_freq / 6.)

    bin = (fft_points // 2 + 1) * hz_freq / fs

    fbank = np.zeros((Bark_filters, int(np.floor(fft_points // 2 + 1))))

    for m in range(1, Bark_filters + 1):
        low = int(bin[m - 1])
        center = int(bin[m])
        high = int(bin[m + 1])
        for k in range(low, high):
            delta = center - k
            if delta < -1.3:
                fbank[m - 1, k] = 0
            elif -1.3 <= delta <= -0.5:
                fbank[m - 1, k] = 10 ** (2.5 * (k + 0.5))
            elif -0.5 <= delta <= 0.5:
                fbank[m - 1, k] = 1
            elif 0.5 <= delta <= 2.5:
                fbank[m - 1, k] = 10 ** (-0.1 * (k - 0.5))
            else:
                fbank[m - 1, k] = 0

    filter_banks = np.dot(power, fbank.T)
    filter_banks = np.where(filter_banks == 0, np.finfo(float).eps, filter_banks)
    filter_banks = 20 * np.log10(filter_banks).clip(1e-5, np.inf)
    filter_banks -= (np.mean(filter_banks, axis=0) + 1e-8)

    bfcc = dct(filter_banks, type=2, axis=1, norm='ortho')[:, 1: (Bark_cofficients + 1)]

    if display:
        plt.imshow(bfcc.T, cmap='jet', origin='lower')
        plt.axis('auto')
        plt.colorbar(cax=None, ax=None)
        plt.title("Bark Frequency Cepstrum Coefficient")
        plt.ylabel("BFCC")
        plt.xlabel("Frames")
        plt.show()

    return bfcc


def PLP():
    pass


def GFCC():
    pass


def GF():
    pass


def AMS():
    pass


def MRCG():
    pass
