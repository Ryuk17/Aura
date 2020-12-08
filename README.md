## Welcome to Lawliet

[![GPL-3.0 Licensed](https://img.shields.io/crates/l/rustc-serialize)](https://opensource.org/licenses/GPL-3.0) [![TensorFlow Version](https://img.shields.io/badge/Tensorflow-1.7+-blue.svg)](https://www.tensorflow.org/) [![Keras Version](https://img.shields.io/badge/Keras-2.0+-blue.svg)](https://keras.io/) [![Python Version](https://img.shields.io/badge/Python-3.x-blue.svg)](https://www.python.org/)  
speechbox is about Speech processing, Speech ehancement, Speech separation, Speech recognition and etc.  
This is a release version of [SpeechAlgorithms](https://github.com/Ryuk17/SpeechAlgorithms)


### What's in it
1. [basic_functions](https://github.com/Ryuk17/speechbox/blob/master/utils/basic_functions.py): basic function for speech signal processing, including  
    + **addNoise()**: add noise to raw speech   
    + **addEcho()**: add echo to raw speech  
    + **addReverberation()**: add reverberation to raw speech  
    + **addHowl()**: add howl to raw speech
    + **displaySpeech()**: display waveform of a given speech sample
    + **normalization()**: normalize data into [-1, 1]
    + **pesqTest()**: PESQ test for speech
    + **preEmphasis()**: pre emphasis speech
    + **getSNR()**: calculate SNR 
    + **nextPow2()**: calculate the nearest pow2 of a given number

2. [basic_features](https://github.com/Ryuk17/speechbox/blob/master/utils/basic_features.py): extract basic features from speech, including     
    + **extractPitch()**: estimate pitch of each frame
    + **extractBFCC()**: extract BFCCs of each frame    
    + **extractMFCC()**: extract MFCCs of each frame 
    + **extractShortEnergy()**: calculate the short energy of a given speech sample
    + **extractShortZcr()**: calculate the short zero crossing rate of a given speech sample
    + **extractShortCorrelation()**: calculate the short correlation of a given speech sample
    + **extractShortAverageMagnitudeDifference()**: calculate the short average magnitude difference of a given speech sample

3. [SpeechFingerprinting](https://github.com/DandelionLau/Ryuk/blob/master/SpeechFingerprinting.py): extract fingerprinting from speech
    + **extractFBEFingerprinting**: extract fingerprinting based on frequency band energy
    + **extractLandmarksFingerprinting()**: extract fingerprinting based on landmarks
    

4. [SpeechDenoising](https://github.com/Ryuk17/speechbox/blob/master/SpeechDenoising.py): Noise reduce  
    + **SpectralSubtractiong**: noise reduce by employing spectral subtraction
    
    
### Dependences
1. Install [Python 3.6](https://www.python.org/)
2. Install [NumPy](http://www.numpy.org/)
3. Install[LibROSA](http://librosa.github.io/librosa/)


