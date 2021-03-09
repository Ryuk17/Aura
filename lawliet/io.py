"""
@FileName: io.py
@Description: Implement io
@Author: Ryuk
@CreateDate: 2021/03/09
@LastEditTime: 2021/03/09
@LastEditors: Please set LastEditors
@Version: v0.1
"""

import struct
import array
import os
import numpy as np


def read_pcm(pcm_path, bitwidth):
    """
    read pcm file with given sample rate
    :param pcm_path: pcm path to read
    :param bitwidth: sample bit width
    :return: pcm data
    """
    file = open(pcm_path, 'rb')
    if bitwidth == 8:
        pcm_data = array.array('b')
    elif bitwidth == 16:
        pcm_data = array.array('h')
    else:
        raise Exception("Invalid bitwidth!")

    size = int(os.path.getsize(pcm_path) / pcm_data.itemsize)
    pcm_data.fromfile(file, size)
    file.close()

    pcm_data = np.array(pcm_data)
    if bitwidth == 8:
        pcm_data = pcm_data / 256
    elif bitwidth == 16:
        pcm_data = pcm_data / 32768
    else:
        raise Exception("Invalid bitwidth!")


    return pcm_data


def write_pcm(pcm_path, pcm_data, bitwidth):
    """
    write pcm file to pcm path
    :param pcm_path: pcm path to write
    :param pcm_data: pcm data
    :param bitwidth: sample bit width
    :return:
    """
    file = open(pcm_path, 'wb')

    if bitwidth == 8:
        pcm_data = pcm_data * 256
    elif bitwidth == 16:
        pcm_data = pcm_data * 32768
    else:
        raise Exception("Invalid bitwidth!")


    pcm_data = pcm_data.astype(np.int)
    for sample in pcm_data:
        if bitwidth == 8:
            sample = struct.pack('b', sample)
        elif bitwidth == 16:
            sample = struct.pack('h', sample)
        else:
            raise Exception("Invalid bitwidth!")
        file.write(sample)
