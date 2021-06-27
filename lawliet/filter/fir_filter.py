"""
@FileName: fir_filter.py
@Description: Implement fir_filter
@Author: Ryuk
@CreateDate: 2021/06/27
@LastEditTime: 2021/06/27
@LastEditors: Please set LastEditors
@Version: v0.1
"""

import numpy as np
from scipy import signal

def fir_lpf(x, fs, cutoff_freq):
    if cutoff_freq == 0:
        return x
    else:
        factor = np.exp(-1 / (fs / cutoff_freq))
        y = signal.filtfilt([1 - factor], [1, -factor], x)
        return y