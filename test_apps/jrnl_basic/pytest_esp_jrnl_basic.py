# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

@pytest.mark.generic
@idf_parametrize("target", ["esp32"], indirect=["target"])
def test_jrnl_basic(dut: Dut) -> None:
    dut.expect_unity_test_output()
