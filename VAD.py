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
import tensorflow as tf
import os
import natsort

# create dataset
wav_files = natsort.natsorted(os.listdir('./dataset/wav'))
vad_files = natsort.natsorted(os.listdir('./dataset/vad'))

frame_length = 240
frame_num = 333
data = np.zeros([frame_num * len(wav_files), frame_length])
for i in range(len(wav_files)):
    sample = wave.open('./dataset/wav/' + wav_files[i])
    params = list(sample.getparams())
    nchannels, sampwidth, framerate, nframes, comptype, compname = sample.getparams()
    str_data = sample.readframes(nframes)
    sample.close()
    wave_data = np.fromstring(str_data, dtype=np.short)
    frames = enframe(wave_data)
    data[i*frame_num:(i+1)*frame_num,:] = frames

label = np.zeros([frame_num * len(wav_files)])
for i in range(len(vad_files)):
    f = open('./dataset/vad/' + vad_files[i])
    vad = [int(val) for val in list(f.read())]
    label[i*frame_num:(i+1)*frame_num] = vad

# extract feature







