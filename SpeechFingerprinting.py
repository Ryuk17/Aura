"""
@ Filename:       SpeechFingerprinting.py
@ Author:         Danc1elion
@ Create Date:    2019-10-05   
@ Update Date:    2019-10-05 
@ Description:    Implement SpeechFingerprinting
"""

import numpy as np
import matplotlib.pyplot as plt
from scipy import special
from basic_functions import *
from  speech_features import *


def FBEFingerprinting(samples, fs, normalize=False, fft_points=512, n_bands=33, low_freq=300, high_freq=2000, overlapping=0, window_length=240, window_type='Rectangle', display=False):
    """
    extract fingerprinting based on frequency band energy
    :param samples: speech sample
    :param fs: sample frequency
    :param normalize: whether to normalize speech
    :param fft_points:  fft points
    :param n_bands: number of bands
    :param low_freq: minimum frequenc
    :param high_freq: maximum frequency
    :param overlapping: overlapping length
    :param window_length: frame length
    :param window_type: window type
    :param display: whether to display
    :return: fingerprinting
    """
    if normalize:
        samples = normalization(samples)

    # pre emphasis
    samples = preEmphasis(samples, fs, display=False)

    # enframe
    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)

    spectrum = np.fft.fft(frames, fft_points)
    power = np.abs(spectrum)[:, 0: fft_points // 2 + 1]
    power = power ** 2 / fft_points

    # transfer frequency into Bark-frequency
    low_freq = 6. * np.arcsinh(low_freq / 600.)
    high_freq = min(6. * np.arcsinh(high_freq / 600.), fs / 2)
    bark_freq = np.linspace(low_freq, high_freq, n_bands + 2)
    hz_freq = 600. * np.sinh(bark_freq / 6.)

    bin = (fft_points // 2 + 1) * hz_freq / fs

    fbank = np.zeros((n_bands, int(np.floor(fft_points // 2 + 1))))

    for m in range(1, n_bands + 1):
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
    energy = filter_banks

    # bit derivation
    fp = np.zeros((len(frames), n_bands - 1))
    for i in range(1, len(filter_banks)):
        for j in range(len(filter_banks[0]) - 1):
            if energy[i][j] - energy[i][j+1] - (energy[i-1][j] - energy[i-1][j-1]) > 0:
                fp[i][j] = 1
            else:
                fp[i][j] = 0

    if display:
        plt.imshow(fp.T, cmap='Greys', origin='lower')
        plt.axis('auto')
        plt.title("Frequency Band Energy based Fingerprinting")
        plt.ylabel("Fingerprinting")
        plt.xlabel("Frames")
        plt.show()

    return fp


def landmarksFingerprinting(samples, fs, normalize=False, height=64,  width=32, landmarks=1, fft_points=512, overlapping=0, window_length=240, window_type='Rectangle', display=False):

    if normalize:
        samples = normalization(samples)

    # pre emphasis
    samples = preEmphasis(samples, fs, display=False)

    spectogram = extractSpectogram(samples, fs, normalize=normalize, fft_points=fft_points, overlapping=overlapping, window_length=window_length, window_type=window_type, display=False)

    fp = np.zeros(spectogram.shape)
    # get landmarks
    for i in range(0, len(spectogram), height):
        for j in range(0, len(spectogram[0]), width):
            sub_matrix = spectogram[i:i+height, j:j+width]
            pos = np.unravel_index(np.argmax(sub_matrix), sub_matrix.shape)
            x = i + pos[0]
            y = j + pos[1]
            fp[x][y] = 1

    if display:
        plt.imshow(fp.T, cmap='Greys', origin='lower')
        plt.axis('auto')
        plt.title("Landmarks based Fingerprinting")
        plt.ylabel("Fingerprinting")
        plt.xlabel("Frames")
        plt.show()

    return fp

