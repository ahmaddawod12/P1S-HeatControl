import pytest


@pytest.mark.esp32
def test_all_unity_cases(dut):
    # Runtime cases wait on real one-second sampling.
    dut.run_all_single_board_cases(timeout=90)
