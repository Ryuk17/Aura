"""
@ Filename:       SpeechDenoising.py
@ Author:         Danc1elion
@ Create Date:    2019-10-14   
@ Update Date:    2019-10-14 
@ Description:    Implement SpeechDenoising
"""

class denoiser:
    def __init__(self, samples, fs, normalize=False, model='DAE', overlapping=0, window_length=240, window_type='Rectangle'):
        self.samples = samples
        self.fs = fs
        self.normalize = normalize
        self.mode = model
        self.overlapping = overlapping
        self.window_length = window_length
        self.window_type = window_type



