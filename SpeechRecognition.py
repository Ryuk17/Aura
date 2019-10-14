"""
@ Filename:       SpeechRecognition.py
@ Author:         Danc1elion
@ Create Date:    2019-10-06   
@ Update Date:    2019-10-06 
@ Description:    Implement SpeechRecognition
"""
import tensorflow as tf

class Recognizer():
    def __init__(self, samples, fs, normalize=False, Acoustics_model='cnn_ctc', Language_model='transformer', overlapping=0, window_length=240, window_type='Rectangle'):
        self.samples = samples
        self.params = fs
        self.normalize = normalize
        self.overlapping = overlapping
        self.window_length = window_length
        self.window_type = window_type
        self.AM = Acoustics_model
        self.LM = Language_model

        if self.AM == 'cnn_ctc':
            pass
        else:
            raise NameError('Unrecongnized acoustics model')

        if self.LM == 'transformer':
            pass
        else:
            raise NameError('Unrecongnized Language model')

