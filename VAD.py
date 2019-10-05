"""
@ Filename:       VAD.py
@ Author:         Danc1elion
@ Create Date:    2019-10-03   
@ Update Date:    2019-10-03 
@ Description:    Implement VAD
"""

from basic_functions import *
from speech_features import *
import wave
import os
import natsort

# create dataset
wav_files = natsort.natsorted(os.listdir('./dataset/wav'))
vad_files = natsort.natsorted(os.listdir('./dataset/vad'))
