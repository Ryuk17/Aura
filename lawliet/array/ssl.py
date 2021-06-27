"""
@FileName: ssl.py
@Description: Implement ssl
@Author: Ryuk
@CreateDate: 2021/06/27
@LastEditTime: 2021/06/27
@LastEditors: Please set LastEditors
@Version: v0.1
"""

import numpy as np

__all__ = [
    "gcc",
    "itd",
    "srp",
    "music",

]


def gcc_phat(ref, sig, sr, mic_distance, sound_speed=343):
    """
    ssl with gcc phat
    :param ref: ref microphone signal
    :param sig: signal microphone
    :param sr: sample rate
    :param mic_distance: mic distance between microphone (metric: m)
    :param sound_speed: sound speed, default 343m/s
    :return: tau: delay, theta: angle
    """
    MAX_TDOA = mic_distance / float(sound_speed)

    n_point = 2 * ref.shape[0] - 1
    X = np.fft.fft(ref, n_point)
    Y = np.fft.fft(sig, n_point)
    XY = X * np.conj(Y)

    c = XY / (abs(X) * abs(Y) + 10e-6)
    c = np.real(np.fft.ifft(c))
    end = len(c)
    center_point = end // 2

    # fft shift
    c = np.hstack((c[center_point + 1:], c[:center_point + 1]))
    lag = np.argmax(abs(c)) - len(ref) + 1
    tau = lag / sr
    theta = np.arcsin(tau / MAX_TDOA) * 180 / np.pi
    return tau, theta


def itd():
    pass

def srp():
    pass


def music():
    pass
