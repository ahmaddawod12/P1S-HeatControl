import pytest


@pytest.mark.esp32
def test_all_unity_cases(dut):
    # The [runtime] and [api] cases wait on real 500 ms sampling, SSE
    # broadcasts, and reconnect backoff, so they need more than the default
    # per-case budget.
    dut.run_all_single_board_cases(timeout=90)
