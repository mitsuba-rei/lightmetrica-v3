"""Math tests"""
import pytest
import numpy as np
from numpy.testing import assert_allclose
import lightmetrica as lm
from pylm_test import math as m

def to_lmfloat(v):
    if lm.FloatPrecision == lm.Float32:
        return v.astype(np.float32)
    elif lm.FloatPrecision == lm.Float64:
        return v.astype(np.float64)

def test_from_python():
    """Tests conversion of Python -> C++"""
    # Vector types
    assert m.compSum2(to_lmfloat(np.array([1,2]))) == pytest.approx(3)
    assert m.compSum3(to_lmfloat(np.array([1,2,3]))) == pytest.approx(6)
    assert m.compSum4(to_lmfloat(np.array([1,2,3,4]))) == pytest.approx(10)

    # Matrix types
    mat = np.array([[1,0,0,1],
                    [0,1,0,1],
                    [0,0,1,1],
                    [1,1,0,0]])
    assert m.compMat4(to_lmfloat(mat)) == pytest.approx(8)

def test_to_python():
    """Tests conversion of C++ -> Python"""
    # Vector types
    assert m.getVec2() == pytest.approx([1,2])
    assert m.getVec3() == pytest.approx([1,2,3])
    assert m.getVec4() == pytest.approx([1,2,3,4])

    # Matrix types
    mat = np.array([[1,1,0,1],
                    [1,1,1,0],
                    [0,1,1,1],
                    [1,1,0,1]])
    assert_allclose(m.getMat4(), mat)