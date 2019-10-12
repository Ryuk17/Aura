## Welcome to Ryuk

[![GPL-3.0 Licensed](https://img.shields.io/crates/l/rustc-serialize)](https://opensource.org/licenses/GPL-3.0) [![TensorFlow Version](https://img.shields.io/badge/Tensorflow-1.7+-blue.svg)](https://www.tensorflow.org/) [![Keras Version](https://img.shields.io/badge/Keras-2.0+-blue.svg)](https://keras.io/) [![Python Version](https://img.shields.io/badge/Python-3.x-blue.svg)](https://www.python.org/)  
Ryuk is about Speech processing, Speech ehancement, Speech separation, Speech recognition and etc.

### Usage


### What's in it
1. [basic_functions](https://github.com/DandelionLau/Ryuk/blob/master/base_functions.py): basic function for speech signal processing, including  
    + **displaySpeech()**: display waveform of a given speech sample
    + **enframe()**: split speech into frames
    + **normalization()**: normalize data into [-1, 1]
    + **pesqTest()**: PESQ test for speech
    + **preEmphasis()**: pre emphasis speech

2. [speech_features](https://github.com/DandelionLau/Ryuk/blob/master/speech_features.py): extract basic features from speech, including   
    + **estimatePitch()**: estimate pitch of each frame
    + **extractBFCC()**: extract MFCCs of each frame    
    + **extractMFCC()**: extract MFCCs of each frame 
    + **extractSpectogram()**: calculate the spectogram of a given speech sample
    + **shortEnergy()**: calculate the short energy of a given speech sample
    + **shortZcc()**: calculate the short zero crossing count of a given speech sample
    + **shortCorrelation()**: calculate the short correlation of a given speech sample
    + **shortAverageMagnitudeDifference()**: calculate the short average magnitude difference of a given speech sample


3. [SpeechFingerprinting](https://github.com/DandelionLau/Ryuk/blob/master/SpeechFingerprinting.py)
4. [VAD](https://github.com/DandelionLau/Ryuk/blob/master/VAD.py)：Voice activity detection method

### Dependences
1. Install [Python 3.6](https://www.python.org/)
2. Install [NumPy](http://www.numpy.org/)
2. Install [Scikit-learn](https://scikit-learn.org/)


