"""Reproduce the committed-write error on the preserved wrapper, then verify repair."""
from pathlib import Path
import importlib.util
import io
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import hli.hpsfc as current_hpsfc
import hli


def run_case():
    spec = importlib.util.spec_from_file_location("automatic_regression", ROOT / "tests/test_hpsfc_automatic_failure.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    suite = unittest.TestSuite([module.AutomaticConsolidationFailureTests(
        "test_committed_writes_report_startup_failures_through_wait_and_allow_retry")])
    log = io.StringIO()
    result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    return result, log.getvalue()


current, current_log = run_case()
assert current.wasSuccessful(), current_log
baseline = ROOT / "results_q1/reassessment_20260905/automatic_maintenance_source_before/hpsfc.py"
spec = importlib.util.spec_from_file_location("hli.hpsfc", baseline)
old_module = importlib.util.module_from_spec(spec)
sys.modules["hli.hpsfc"] = old_module
hli.hpsfc = old_module
try:
    spec.loader.exec_module(old_module)
    old, old_log = run_case()
    assert len(old.failures) == 12 and not old.errors, old_log
finally:
    sys.modules["hli.hpsfc"] = current_hpsfc
    hli.hpsfc = current_hpsfc
print(json.dumps({"current_passes": current.wasSuccessful(),
                  "preserved_pre_fix_failures": len(old.failures),
                  "preserved_pre_fix_errors": len(old.errors),
                  "current_output": current_log, "pre_fix_output": old_log}, indent=2))
