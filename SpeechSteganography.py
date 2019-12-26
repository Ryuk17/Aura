"""
@ Filename:       SpeechSteganography.py
@ Author:         Danc1elion
@ Create Date:    2019-12-25   
@ Update Date:    2019-12-25 
@ Description:    Implement SpeechSteganography
"""
import scipy.io.wavfile as wav
from scipy.fftpack import dct, idct
from scipy.fftpack import fft
import numpy as np

class DCTEmbedder:
    def __init__(self, seed, rate=1, mode='single'):
        self.rate = rate
        self.seed = seed
        self.mode = mode
        self.stop_mark = [1,1,1,1,1,1,1,1,1,1]
        self.frame_length = 80
        self.m = 1
        self.epsilon = 0.015
        self.channels = None
        self.fs = None
        self.left_signal = None
        self.right_signal = None
        self.wavsignal = None

    def _waveReader(self, path):
        """
        read wave file and its corresponding
        :param path: wav fi
        :return:
        """
        fs, wavsignal = wav.read(path)
        self.fs = fs
        if len(wavsignal) == 2:
            self.channels = 2
            self.left_signal = wavsignal[0]
            self.right_signal = wavsignal[1]
        else:
            self.channels = 1
            self.wavsignal = wavsignal

    def _saveWave(self, stego, cover_path, stego_path, inplace=False):
        """
        save stego wave
        :param stego: stego wavsignal
        :param cover_path: cover path
        :param stego_path: stego path
        :param inplace: whether to save in cover path
        :return:
        """
        if inplace:
            wav.write(cover_path, self.fs, stego)
        else:
            assert stego_path is not None
            wav.write(stego_path, self.fs, stego)

    def _DCTReplace(self, wavsignal, secret_message):
        np.random.seed(self.seed)
        roulette = np.random.rand(len(wavsignal) // self.frame_length)

        stego = np.array(wavsignal)
        k = 0
        n = 0
        for i in range(0, len(wavsignal), self.frame_length):
            frame = wavsignal[i:i+self.frame_length]
            # frame = frame * np.hamming(self.frame_length)
            dct_coefficients = dct(frame, norm='ortho')

            if roulette[n] <= self.rate:
                # replace last m coefficients
                for j in range(-self.m, 0):
                    if k < len(secret_message) and int(secret_message[k]) == 0:
                        dct_coefficients[j] = 0
                        k += 1
                    elif k < len(secret_message) and int(secret_message[k]) == 1:
                        dct_coefficients[j] = self.epsilon
                        k += 1
            n += 1

            # IDCT
            frame = idct(dct_coefficients, norm='ortho')
            stego[i:i+self.frame_length] = frame

        # stego = np.array(stego).astype(np.int16)
        return stego

    def embed(self, cover_path, stego_path, secret_message, inplace=False):

        # add stop mark
        secret_message = np.concatenate([secret_message, self.stop_mark], axis=0)
        # pre check
        self._waveReader(cover_path)
        # assert len(self.wavsignal) / self.frame_length * self.m * self.rate >= 1.1 * len(secret_message)
        assert self.channels in [1, 2]

        # embed secret message
        if self.channels == 1:
            if self.mode == 'single':
                stego = self._DCTReplace(self.wavsignal, secret_message)
                self._saveWave(stego, cover_path, stego_path, inplace)
            elif self.mode == 'batch':
                for i in range(len(stego_path)):
                    stego = self._DCTReplace(self.wavsignal, secret_message)
                    self._saveWave(stego, cover_path, stego_path[i], inplace)

        elif self.channels == 2:
            if self.mode == 'single':
                left_stego = self._DCTReplace(self.left_signal, secret_message)
                right_stego = self._DCTReplace(self.right_signal, secret_message)
                stego = [left_stego, right_stego]
                self._saveWave(stego, cover_path, stego_path, inplace)
            elif self.mode == 'batch':
                # the same secret messages are embedding in different carrier
                for i in range(len(stego_path)):
                    left_stego = self._DCTReplace(self.left_signal, secret_message)
                    right_stego = self._DCTReplace(self.right_signal, secret_message)
                    stego = [left_stego, right_stego]
                    self._saveWave(stego, cover_path[i], stego_path[i], inplace)

class DCTExtractor:
    def __init__(self, seed, rate=1):
        self.seed = seed
        self.stop_mark = '1111111111'
        self.rate = rate
        self.frame_length = 80
        self.m = 1
        self.epsilon = 0.015
        self.fs = None
        self.channels= None
        self.wavsignal = None
        self.left_signal = None
        self.right_signal = None

    def _waveReader(self, path):
        """
        read wave file and its corresponding
        :param path: wav fi
        :return:
        """
        fs, wavsignal = wav.read(path)
        self.fs = fs
        if len(wavsignal) == 2:
            self.channels = 2
            self.left_signal = wavsignal[0]
            self.right_signal = wavsignal[1]
        else:
            self.channels = 1
            self.wavsignal = wavsignal

    def _checkHeader(self, header):
        """
        check the validness of header
        :param header: header
        :return: True/False
        """
        return True

    def _checkStop(self, message):
        """
        check stop
        :param message: secret message
        :return: True/False
        """

        count = 0
        for i in range(len(message) - 1, len(message) - 11, -1):
            if message[i] == self.stop_mark[count]:
                count += 1

        if count == 10:
            return True
        else:
            return False

    def _DCTExtract(self, roulette, wavsignal):
        """
        extract last m DCT coefficients from stego wavsignal
        :param roulette:
        :param wavsignal:
        :return: secret message
        """
        message = ""
        n = 0
        k = 0
        for i in range(0, len(wavsignal), self.frame_length):
            frame = wavsignal[i:i + self.frame_length]
            # frame = frame * np.hamming(self.frame_length)
            dct_coefficients = dct(frame, norm='ortho')

            if roulette[n] <= self.rate:
                # replace last m coefficients
                for j in range(-self.m, 0):
                    if dct_coefficients[j] < self.epsilon:
                        message += '0'
                        k += 1
                    else:
                        message += '1'
                        k += 1

            # check the validness of header
            if len(message) == 44:
                assert self._checkHeader(message) is True

            # check stop mark
            if len(message) >= len(self.stop_mark) and self._checkStop(message):
                return message

            n += 1
        return message

    def extract(self, wave_path, message_path):
        """
        extract secret message from stego wave
        :param wave_path: wave path
        :param message_path: save secret message
        :return: message
        """

        self._waveReader(wave_path)
        np.random.seed(self.seed)
        roulette = np.random.rand(len(self.wavsignal) // self.frame_length)

        message = ""
        if self.channels == 1:
            message += self._DCTExtract(roulette, self.wavsignal)
        elif self.channels == 2:
            message += self._DCTExtract(roulette, self.left_signal)
            message += self._DCTExtract(roulette, self.right_signal)

        with open(message_path, "w", encoding='utf-8') as f:
            f.write(message)
        return message

def main():
    np.random.seed(0)
    wave_path = './1.wav'
    stego_path = './s1.wav'
    message_path = './s.txt'
    secret_message = np.random.randint(0, 2, 1600)
    for i in range(len(secret_message)-1, len(secret_message) - 11, -1):
        secret_message[i] = 1
    alice = DCTEmbedder(0)
    alice.embed(wave_path, stego_path, secret_message)

    bob = DCTExtractor(0)
    m = bob.extract(stego_path, message_path)

    count = 0
    for i in range(len(m)):
        if int(m[i]) != int(secret_message[i]):
            count += 1

    print('BER', count / len(m))

if __name__ == '__main__':
    main()
