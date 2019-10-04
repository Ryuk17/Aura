"""
@ Filename:       Test.py
@ Author:         Danc1elion
@ Create Date:    2019-10-03   
@ Update Date:    2019-10-03 
@ Description:    Implement Test
"""

import wave
from speech_features import *
import matplotlib.pyplot as plt
import numpy as np


sample = wave.open('./dataset/wav/1.wav')
params = list(sample.getparams())
nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
str_data = sample.readframes(nframes)
wave_data = np.fromstring(str_data, dtype=np.short)

y =shortCorrelation(wave_data, params)

# time = np.arange(0, nframes) * (1.0 / framerate)
# plt.plot(time, wave_data)
# plt.xlabel("time (seconds)")
# plt.show()