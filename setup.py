"""
@FileName: setup.py
@Description: Implement setup
@Author: Ryuk
@CreateDate: 2021/06/27
@LastEditTime: 2021/06/27
@LastEditors: Please set LastEditors
@Version: v0.1
"""


from setuptools import setup

NAME = 'lawliet'
VERSION = '1.0'
PY_MODULES = ['lawliet','test']

setup(name = NAME
        , version = VERSION
        , py_modules = PY_MODULES)