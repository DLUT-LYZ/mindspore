
.. py:class:: mindspore.mindrecord.Cifar100ToMR(source, destination)

    将CIFAR-100数据集转换为MindRecord格式数据集。

    .. note::
        示例的详细信息，请参见 `转换CIFAR-10数据集 <https://www.mindspore.cn/tutorials/zh-CN/master/advanced/dataset/record.html#转换cifar-10数据集>`_ 。

    参数：
        - **source** (str) - 待转换的CIFAR-100数据集文件所在目录的路径。
        - **destination** (str) - 转换生成的MindRecord文件路径，需提前创建目录并且目录下不能存在同名文件。

    异常：
        - **ValueError** - 参数 `source` 或 `destination` 无效。

    .. py:method:: transform(fields=None)

        执行从CIFAR-100数据集到MindRecord格式数据集的转换。

        .. note::
            请参考类的示例 :class:`mindspore.mindrecord.Cifar100ToMR` 。

        参数：
            - **fields** (list[str]，可选) - 索引字段的列表，例如['fine_label', 'coarse_label']。默认值： ``None`` 。
              索引字段的设置请参考函数 :func:`mindspore.mindrecord.FileWriter.add_index` 。

        返回：
            MSRStatus，SUCCESS或FAILED。

        异常：
            **ParamTypeError** - 设置MindRecord索引字段失败。
            **MRMOpenError** - 新建MindRecord文件失败。
            **MRMValidateDataError** - 原始数据集数据异常。
            **MRMSetHeaderError** - 设置MindRecord文件头失败。
            **MRMWriteDatasetError** - 创建MindRecord索引失败。
            **TypeError** - 参数 `parallel_writer` 不是bool类型。
            **ValueError** - 参数 `fields` 不合法。
