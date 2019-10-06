## Welcome to Ryuk

[![GPL-3.0 Licensed](https://img.shields.io/crates/l/rustc-serialize)](https://opensource.org/licenses/GPL-3.0) [![TensorFlow Version](https://img.shields.io/badge/Tensorflow-1.7+-blue.svg)](https://www.tensorflow.org/) [![Keras Version](https://img.shields.io/badge/Keras-2.0+-blue.svg)](https://keras.io/) [![Python Version](https://img.shields.io/badge/Python-3.x-blue.svg)](https://www.python.org/)  
This repository is about Speech processing, Speech ehancement, Speech separation, Speech recognition and etc.

### What's in it
1. [basic_functions](https://github.com/DandelionLau/Ryuk/blob/master/base_functions.py): basic function for speech signal processing, including  
    + **normalization()**: normalize data into [-1, 1]
    + **getParams()**: unpack the speech parameter
    + **enframe()**: split speech into frames
    + **preEmphasis()**: pre emphasis speech
    + **windows()**: various window function
    + **displaySpeech()**: display waveform of a given speech sample
2. [speech_features](https://github.com/DandelionLau/Ryuk/blob/master/speech_features.py): extract basic features from speech, including   
    + **shortEnergy()**: calculate the short energy of a given speech sample
    + **shortZcc()**: calculate the short zero crossing count of a given speech sample
    + **shortCorrelation()**: calculate the short correlation of a given speech sample
    + **shortAverageMagnitudeDifference()**: calculate the short average magnitude difference of a given speech sample
    + **Spectogram()**: calculate the spectogram of a given speech sample
    + **estimatePitch()**: estimate pitch of each frame
    + **extractMFCC()**: extract MFCCs of each frame    
3. [SpeechFingerprinting](https://github.com/DandelionLau/Ryuk/blob/master/SpeechFingerprinting.py)
4. [VAD](https://github.com/DandelionLau/Ryuk/blob/master/VAD.py)
