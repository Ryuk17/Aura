"""
@FileName: resample.py
@Description: Implement resample
@Author: Ryuk
@CreateDate: 2021/06/27
@LastEditTime: 2021/06/27
@LastEditors: Please set LastEditors
@Version: v0.1
"""

import numpy as np
import math


__all__ = [
    "direct_interpolation",
    "lagrange_interpolation",
    "sine_interpolation",
]

def direct_interpolation(x, L, M):
    """
    resample signal with direct interpolation
    :param x: input signal
    :param L: original frequency
    :param M:  target frequency
    :return: resampled signal
    """
    N = len(x)
    K = int((M / L) * N)
    factor = L / M
    y = np.zeros(K)
    for k in range(K):
        nk = factor * k
        n = math.floor(nk)
        if n + 1 >= len(x): continue
        w1 = nk - n
        w2 = 1 - w1
        y[k] = w1 * x[n + 1] + w2 * x[n]
    return y


def lagrange_interpolation(x, w, L, M):
    N = len(x)
    K = int((M / L) * N)
    factor = L / M
    y = np.zeros(K)
    for k in range(K):
        nk = factor * k
        n = math.floor(nk) - 1
        for i in range(-w, w, 1):
            numerator = 1
            denominator = 1
            if n  -  i >= len(x): continue
            for j in range(-w, w, 1):
                if i != j:
                    numerator *= nk - (n - j)
                    denominator *= (j - i)
            y[k] += x[n - i] * numerator / denominator
    return y


def sine_interpolation(x, w, L, M):
    N = len(x)
    K = int((M / L) * N)
    factor = L / M
    y = np.zeros(K)
    for k in range(K):
        nk = factor * k
        n = math.floor(nk)
        for i in range(-w, w, 1):
            if n  -  i >= len(x): continue
            if nk - n + i == 0: continue
            numerator = math.sin((nk - n + i))
            denominator = math.pi * (nk - n +i)
            y[k] += x[n - i] * numerator / denominator
    return y
