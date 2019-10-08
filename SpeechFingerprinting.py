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


def FrequencyBandEnergyFingerprinting(samples, fs, overlapping=0, window_length=240, window_type='Rectangle'):
    samples = preEmphasis(samples, fs)
    frames = enframe(samples, fs, overlapping=overlapping, window_length=window_length, window_type=window_type)



def landmarksFingerprinting():
    pass
