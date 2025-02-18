# Copyright 2024 Huawei Technologies Co., Ltd
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

import numpy as np

import mindspore as ms
from mindspore import context, Tensor, Parameter
from mindspore.nn import Cell
from mindspore.ops.operations._infer_ops import QuantV2
from parallel.utils.utils import ParallelValidator, compile_net

def setup_function():
    context.set_auto_parallel_context(dataset_strategy="full_batch")


class QuantNet(Cell):
    def __init__(self, strategy):
        super().__init__()
        self.anti_quant = QuantV2().shard(strategy)

    def construct(self, data, scale, offset):
        out = self.anti_quant(data, scale, offset, False, "ROUND", ms.int8)
        return out


def test_quant_1D():
    """
    Feature: test quant ops
    Description:
    Expectation: compile success
    """
    context.set_auto_parallel_context(parallel_mode="semi_auto_parallel", device_num=4, global_rank=0)

    strategy = ((4,), (4,), (4,))

    net = QuantNet(strategy)

    data = Parameter(Tensor(np.ones([4096]), dtype=ms.int8), "data")
    scale = Parameter(Tensor(np.ones([4096]), dtype=ms.float32), "scale")
    offset = Parameter(Tensor(np.ones([4096]), dtype=ms.float32), "offset")

    net.set_inputs(data, scale, offset)

    phase = compile_net(net, data, scale, offset)
    validator = ParallelValidator(net, phase)

    assert validator.check_parameter_shape("data", [1024])
    assert validator.check_parameter_shape("scale", [1024])
    assert validator.check_parameter_shape("offset", [1024])


def test_quant_2D():
    """
    Feature: test quant ops
    Description:
    Expectation: compile success
    """
    context.set_auto_parallel_context(parallel_mode="semi_auto_parallel", device_num=4, global_rank=0)

    strategy = ((4, 1,), (1,), (1,))

    net = QuantNet(strategy)

    data = Parameter(Tensor(np.ones([4096, 512]), dtype=ms.int8), "data")
    scale = Parameter(Tensor(np.ones([512]), dtype=ms.float32), "scale")
    offset = Parameter(Tensor(np.ones([512]), dtype=ms.float32), "offset")

    net.set_inputs(data, scale, offset)

    phase = compile_net(net, data, scale, offset)
    validator = ParallelValidator(net, phase)

    assert validator.check_parameter_shape("data", [1024, 512])
    assert validator.check_parameter_shape("scale", [512])
    assert validator.check_parameter_shape("offset", [512])

def test_quant_3D():
    """
    Feature: test quant ops
    Description:
    Expectation: compile success
    """
    context.set_auto_parallel_context(parallel_mode="semi_auto_parallel", device_num=4, global_rank=0)

    strategy = ((2, 2, 1), (1,), (1,))

    net = QuantNet(strategy)

    data = Parameter(Tensor(np.ones([128, 32, 32]), dtype=ms.int8), "data")
    scale = Parameter(Tensor(np.ones([32]), dtype=ms.float32), "scale")
    offset = Parameter(Tensor(np.ones([32]), dtype=ms.float32), "offset")

    net.set_inputs(data, scale, offset)

    phase = compile_net(net, data, scale, offset)
    validator = ParallelValidator(net, phase)

    assert validator.check_parameter_shape("data", [64, 16, 32])
    assert validator.check_parameter_shape("scale", [32])
    assert validator.check_parameter_shape("offset", [32])

def test_quant_4D():
    """
    Feature: test quant ops
    Description:
    Expectation: compile success
    """
    context.set_auto_parallel_context(parallel_mode="semi_auto_parallel", device_num=4, global_rank=0)

    strategy = ((2, 1, 2, 1), (1,), (1,))

    net = QuantNet(strategy)

    data = Parameter(Tensor(np.ones([128, 32, 32, 32]), dtype=ms.int8), "data")
    scale = Parameter(Tensor(np.ones([32]), dtype=ms.float32), "scale")
    offset = Parameter(Tensor(np.ones([32]), dtype=ms.float32), "offset")

    net.set_inputs(data, scale, offset)

    phase = compile_net(net, data, scale, offset)
    validator = ParallelValidator(net, phase)

    assert validator.check_parameter_shape("data", [64, 32, 16, 32])
    assert validator.check_parameter_shape("scale", [32])
    assert validator.check_parameter_shape("offset", [32])
