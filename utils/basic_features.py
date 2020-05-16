"""
@ Filename:       basic_features.py
@ Author:         Danc1elion
@ Create Date:    2019-10-14   
@ Update Date:    2019-10-14 
@ Description:    Implement basic_features
"""

from .basic_functions import *
from scipy.fftpack import dct
import librosa

def shortEnergy(samples, sr, normalize=False, overlapping=0, window_length=240,  display=False):
    """
    calculate the short energy of a given sample
    :param samples: sample data
    :param sr: sample rate
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
    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T
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


def shortZcr(samples, normalize=False, overlapping=0, window_length=240, display=False):
    """
    calculate the short count of a given sample
    :param samples: sample data
    :param sr: sample rate
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: the length of window
    :param window_type: the type of window
    :param display: whether to display short energy
    :return: short zero crossing count
    """

    if normalize:
        samples = normalization(samples)

    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T
    zcr = np.zeros([len(frames), 1])

    def calculateZcr(frame):
        count = 0
        for i in range(1, len(frame)):
            count += np.abs(sgn(frame[i]) - sgn(frame[i - 1]))
        zcr = count / (2 * len(frame))
        return zcr

    for i in range(len(frames)):
        zcr[i] = calculateZcr(frames[i])


    if display:
        time = np.arange(0, len(frames))
        plt.plot(time, zcr)
        plt.title("Zero Crossing Rate")
        plt.ylabel("Zero Crossing Rate")
        plt.xlabel("Frames")
        plt.show()

    return zcr


def shortCorrelation(samples, sr, normalize=True, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the short correlation of a given speech sample
    :param samples: speech samples
    :param sr: sample rate
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display correlation
    :return: correlation of each frame
    """
    if normalize:
        samples = normalization(samples)


    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T
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


def shortAverageMagnitudeDifference(samples, sr, normalize=True, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    calculate the average magnitude difference of a given speech sample
    :param samples: speech samples
    :param sr: sample rate
    :param normalize: whether to normalize data
    :param overlapping: overlapping length
    :param window_length: window length
    :param window_type: window type
    :param display: whether to display average magnitude difference
    :return: average magnitude difference of each frame
    """
    if normalize:
        samples = normalization(samples)

    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T
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


def estimatePitch(samples, sr, normalize=False, method='Correlation', smooth='None', L=2, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    estimate pitch
    :param samples: speech samples
    :param sr: sample rate
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
        correlation = shortCorrelation(samples, sr, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(correlation))
        for i in range(len(correlation)):
            index = np.argmax(correlation[i][30:]) + 1
            pitch[i] = sr / index
    elif method == 'AMDF':
        AMDF = shortAverageMagnitudeDifference(samples, sr, overlapping=overlapping, window_length=window_length, window_type=window_type,display=False)
        pitch = np.zeros(len(AMDF))
        for i in range(len(AMDF)):
            index = np.argmin(AMDF[i][30:])
            pitch[i] = sr / index
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


def extractMFCC(samples, sr, normalize=False, low_freq=0, high_freq=8000, fft_points=256, Mel_filters=40, Mel_cofficients=12, overlapping=0, window_length=240, window_type='Hamming', display=False):
    """
    extract MFCC from speech
    :param samples: speech sample
    :param sr: sample rate
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
    samples = preEmphasis(samples, sr, display=False)


    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T

    # fft and power spectrum
    spectrum = np.fft.fft(frames, fft_points)
    power = np.abs(spectrum)[:, 0: fft_points // 2 + 1]
    power = power ** 2 / fft_points

    # transfer frequency into Mel-frequency
    low_freq = 2595 * np.log10(1 + low_freq / 700)
    high_freq = min(2595 * np.log10(1 + high_freq / 700), sr / 2)
    mel_freq = np.linspace(low_freq, high_freq, Mel_filters + 2)
    hz_freq = (700 * (10 ** (mel_freq / 2595) - 1))

    bin = np.floor((fft_points // 2 + 1) * hz_freq / sr)

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


def extractBFCC(samples, sr, normalize=False, low_freq=0, high_freq=8000, fft_points=256, Bark_filters=24, Bark_cofficients=12, overlapping=0, window_length=240, window_type='Hamming', display=False):
    if normalize:
        samples = normalization(samples)

    # pre emphasis
    samples = preEmphasis(samples, sr, display=False)

    # enframe

    hop_length = (1 - overlapping) * window_length
    frames = librosa.util.frame(samples, frame_length=window_length, hop_length=hop_length).T
    # fft and power spectrum
    spectrum = np.fft.fft(frames, fft_points)
    power = np.abs(spectrum)[:,0: fft_points // 2 + 1]
    power = power ** 2 / fft_points

    # transfer frequency into Bark-frequency
    low_freq = 6. * np.arcsinh(low_freq / 600.)
    high_freq = min(6. * np.arcsinh(high_freq / 600.), sr / 2)
    bark_freq = np.linspace(low_freq, high_freq, Bark_filters + 2)
    hz_freq = 600. * np.sinh(bark_freq / 6.)

    bin = (fft_points // 2 + 1) * hz_freq / sr

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

    bfcc = np.dct(filter_banks, type=2, axis=1, norm='ortho')[:, 1: (Bark_cofficients + 1)]

    if display:
        plt.imshow(bfcc.T, cmap='jet', origin='lower')
        plt.axis('auto')
        plt.colorbar(cax=None, ax=None)
        plt.title("Bark Frequency Cepstrum Coefficient")
        plt.ylabel("BFCC")
        plt.xlabel("Frames")
        plt.show()

    return bfcc


