"""
@ Filename:       Test.py
@ Author:         Danc1elion
@ Create Date:    2019-10-03   
@ Update Date:    2019-10-03 
@ Description:    Implement Test
"""

import wave
import matplotlib.pyplot as plt
import numpy as np
from sklearn.model_selection import train_test_split
import pickle
import tensorflow as tf
from sklearn.preprocessing import minmax_scale,OneHotEncoder
from tqdm import tqdm
import scipy.io.wavfile as wav
from scipy.fftpack import fft
from basic_functions import *


# sample = wave.open('./dataset/wav/1.wav')
# params = list(sample.getparams())
# nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
# str_data = sample.readframes(nframes)
# sample.close()
# wave_data = np.fromstring(str_data, dtype=np.short)
fs, wavsignal = wav.read('./dataset/wav/1.wav')
res = addNoise(wavsignal, fs)
