"""
@ Filename:       Machinelearning_test.py
@ Author:         Danc1elion
@ Create Date:    2019-10-06   
@ Update Date:    2019-10-06 
@ Description:    Implement Machinelearning_test
"""

import pickle
from sklearn.model_selection import train_test_split
import xgboost as xgb
from sklearn import metrics

with open("./vad_data.pkl", 'rb') as f:
    data = pickle.load(f, encoding='utf-8')

feature = data['x']
label = data['y']

x_train, x_test, y_train, y_test = train_test_split(feature, label, test_size=0.3)

clf = xgb.XGBClassifier(colsample_bytree=0.4603, gamma=0.0468,
                             learning_rate=0.05, max_depth=3,
                             min_child_weight=6, n_estimators=760,
                             reg_alpha=0.6640, reg_lambda=0.8571,
                             subsample=0.5213, silent=True,
                             random_state=7, nthread=-1)
clf.fit(x_train, y_train)
y_pred = clf.predict(x_test)
clf.save_model('./models/VAD.model')
c_matrix = metrics.confusion_matrix(y_test, y_pred)
tp = c_matrix[0][0]
fp = c_matrix[1][0]
fn = c_matrix[0][1]
tn = c_matrix[1][1]
print("Accuracy = %.4g FPR = %.4g FNR = %.4g" % ((tp + tn) / (tp + tn + fp + fn), fp / (tn + fp), fn / (tp + fn)))



