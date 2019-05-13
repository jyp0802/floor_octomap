#!/usr/bin/env python

from octomap_flatter.srv import *
import rospy

import numpy as np
import numpy.ma as ma
import cv2
import imutils
import scipy

def get_surround(cont, idx):
    if idx in [(0,0),(0,w-1),(h-1,0),(h-1,w-1)]:
        if idx == (0,0):
            neigh = cont[:2,:2].ravel()
        elif idx == (0,w-1):
            neigh = cont[:2,-2:].ravel()
        elif idx == (h-1,0):
            neigh = cont[-2:,:2].ravel()
        else:
            neigh = cont[-2:,-2:].ravel()
    elif idx[0] in [0, h-1]:
        neigh = cont[idx[0],idx[1]-1:idx[1]+2].ravel()
    elif idx[1] in [0, w-1]:
        neigh = cont[idx[0]-1:idx[0]+2,idx[1]].ravel()
    else:
        neigh = cont[idx[0]-1:idx[0]+2, idx[1]-1:idx[1]+2].ravel()
        neigh = np.delete(neigh,[4])
        for x in neigh:
            if np.sum(neigh == x) == 3 and not (x == 0 or x == 255):
                return x
            elif np.sum(neigh == x) > 1 and not (x == 0 or x == 255):
                return x
    neigh = [x for x in neigh if x != 255]
    return max(neigh)

def flatten(img):
    h, w = np.shape(img)
    mx = int(np.max(img))
    fin = img.copy()

    img_pad = np.lib.pad(img, 2, 'constant', constant_values=200)

    horizontal = scipy.ndimage.sobel(img_pad, 0)
    vertical = scipy.ndimage.sobel(img_pad, 1)
    edge_pad = np.hypot(horizontal, vertical)

    edge_pad = cv2.threshold(edge_pad, 5, 255, cv2.THRESH_BINARY)[1]

    edge_pad = edge_pad.astype(np.uint8)
    contours = cv2.findContours(edge_pad, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    contours = imutils.grab_contours(contours)
    contours = [c for c in contours if len(c) >= 8]

    if len(contours) >= 1:
        col = mx
        for c in contours[:-1]:
            col += 1
            cv2.drawContours(edge_pad, [c], -1, col, 2)
            cv2.fillPoly(edge_pad, pts =[c], color=col)
        
        avg = [[0,0] for box in range(col-mx)]
        edge_fill = edge_pad[2:-2,2:-2]
        
        for idx, val in np.ndenumerate(edge_fill):
            if val == 255:
                val = get_surround(edge_fill, idx)
                edge_fill[idx] = val
            if val > mx:
                avg[val-mx-1][0] += img[idx]
                avg[val-mx-1][1] += 1
        
        for a in avg:
            if not a[1] == 0:
                a[0] //= a[1]
        
        for idx, val in np.ndenumerate(edge_fill):
            if val > mx:
                fin[idx] = avg[val-mx-1][0]
        
        msk = ma.masked_equal(edge_fill, 0).mask
        gnd = np.multiply(img,msk)
        avg = np.sum(gnd) / float(np.sum(msk))
        if avg <= 1:
            fin = np.multiply(fin, ~msk)

    return fin

def call_flatten(req):
    return OctoImageResponse(flatten(req.input))

rospy.init_node('flatten_octomap_server')
s = rospy.Service('flatten_octomap', OctoImage, call_flatten)
print("Starting flatten_octomap_server")

rospy.spin()