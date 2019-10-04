## Welcome to Ryuk

This repository is about Speech processing, Speech ehancement, Speech separation, Speech recognition and etc.

### What's in it
1. [basic_functions](https://github.com/DandelionLau/Ryuk/blob/master/base_functions.py): basic function for speech signal processing, including  
    + **getParams()**: unpack the speech parameter
    + **enframe()**: split speech into frames
    + **preEmphasis()**: pre emphasis speech
    + **windows()**: various window function
    + **displaySpeech()**: display waveform of a given speech sample
2. [speech_features](https://github.com/DandelionLau/Ryuk/blob/master/speech_features.py): extract basic features from speech, including    
    + **shortEnergy()**: calculate the short energy of a given speech sample
    + **shortZcc()**: calculate the short zero crossing count of a given speech sample
    + **shortCorrelation()**: calculate the short correlation of a given speech sample
    + **Spectogram()**: calculate the spectogram of a given speech sample
