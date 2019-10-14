"""
@ Filename:       SpeechActivityDetection.py
@ Author:         Danc1elion
@ Create Date:    2019-10-14   
@ Update Date:    2019-10-14 
@ Description:    Implement SpeechActivityDetection
"""

from basic_features import *
import xgboost as xgb

class VADetector():
    def __init__(self, samples, fs, normalize=False, overlapping=0, window_length=240, window_type='Rectangle'):
        self.samples = samples
        self.fs = fs
        self.normalize = normalize
        self.overlapping = overlapping
        self.window_length = window_length
        self.window_type = window_type
        self.vad = self.VADetect()

    def VADetect(self):
        energy = shortEnergy(self.samples, self.fs)
        zcc = shortZcc(self.samples, self.fs)
        mfcc = extractMFCC(self.samples, self.fs)
        correlation = shortCorrelation(self.samples, self.fs)
        sample_feature = np.hstack((energy, zcc, mfcc, correlation))

        clf = xgb.Booster(model_file='./models/VAD.model')
        sample_feature = xgb.DMatrix(sample_feature)
        vad = clf.predict(sample_feature)
        vad = [0 if val < 0.5 else 1 for val in vad]
        return vad

    def display(self):
        frames = enframe(self.samples, self.fs, overlapping=self.overlapping, window_length=self.window_length, window_type=self.window_type)

        time = np.arange(0, len(frames)) * (1.0 / self.fs)
        for i in range(len(frames)):
            x = time[i*self.window_length:(i+1)*self.window_length]
            if self.vad[i] == 0:
                plt.plot(x, frames[i], color='blue')
            else:
                plt.plot(x, frames[i], color='red')
        plt.show()