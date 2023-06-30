# Copyright 2023 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================

import numpy as np
import pytest

import mindspore as ms
from mindspore import nn, Tensor
from mindspore.ops._tracefunc import trace
from mindspore.ops import functional as F
from mindspore.ops import operations as P


@pytest.mark.level0
@pytest.mark.platform_x86_cpu
@pytest.mark.platform_arm_cpu
@pytest.mark.env_onecard
@pytest.mark.parametrize('mode', [ms.GRAPH_MODE, ms.PYNATIVE_MODE])
def test_trace_basic_cell(mode):
    """
    Feature: trace of cell
    Description: Verify the result of trace
    Expectation: success
    """
    class Net(nn.Cell):
        def __init__(self):
            super(Net, self).__init__()
            self.arg_max = P.ArgMaxWithValue()

        @trace
        def construct(self, x, y):
            if F.is_sequence_shape_unknown(x.shape):
                z = x + y
                return z * z
            z = x + y + 1
            _, b = self.arg_max(z)
            return z * z, b

    ms.set_context(mode=mode)
    net = Net()
    x = Tensor([1, 2, 3, 4])
    y = Tensor([4, 5, 6, 7], ms.float64)
    output, max_ = net(x, y)
    expect = np.array([36., 64., 100., 144.])
    expect_max = np.array([12])
    assert np.allclose(output.asnumpy(), expect)
    assert np.allclose(max_.asnumpy(), expect_max)
    assert output.dtype == ms.float64


@pytest.mark.level0
@pytest.mark.platform_x86_gpu_training
@pytest.mark.env_onecard
def test_trace_python_infer_cell():
    """
    Feature: trace of cell
    Description: Verify the result of trace
    Expectation: success
    """
    class Net(nn.Cell):
        def __init__(self):
            super(Net, self).__init__()
            self.choleskytrsm = P.CholeskyTrsm()

        @trace
        def construct(self, x, y):
            z = self.choleskytrsm(x) + y
            return z * z
    @trace
    def func(x, k):
        z = x[0] * x[0]
        z[2] = z[1]
        z = P.Concat()((z, x[0] + k))
        return z
    ms.set_context(mode=ms.PYNATIVE_MODE)
    x = Tensor([1, 2, 3, 4])
    func_output = func((x,), 10)
    func_expect = np.array([1, 4, 4, 16, 11, 12, 13, 14])
    assert np.allclose(func_output.asnumpy(), func_expect)

    net = Net()
    x = Tensor(np.array([[0.25, 0], [0, 0.25]]), ms.float32)
    y = Tensor(np.array([[1, 2], [2, 1]]), ms.float32)
    output = net(x, y)
    expect = np.array([[9, 4], [4, 9]])
    assert np.allclose(output.asnumpy(), expect)
