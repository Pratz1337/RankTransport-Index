"""Compare the repaired write path with preserved pre-fix Python modules."""
from pathlib import Path
import importlib.util
import io
import json
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def run_case():
    spec = importlib.util.spec_from_file_location("transaction_regression", ROOT / "tests/test_consolidation_failure.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    suite = unittest.TestSuite([module.ConsolidationFailureTests(
        "test_pending_failure_preserves_both_ledgers_before_and_after_publish")])
    log = io.StringIO()
    result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    return result, log.getvalue()


current, current_log = run_case()
assert current.wasSuccessful(), current_log
import hli.rank_transport as current_rank
import hli.signed_delta as current_delta
baseline = ROOT / "results_q1/reassessment_20260905/python_transaction_source_before"
for name in ("signed_delta", "rank_transport"):
    module_name = f"hli.{name}"
    module = types.ModuleType(module_name)
    module.__file__ = str(baseline / f"{name}.py")
    sys.modules[module_name] = module
    exec(compile(Path(module.__file__).read_bytes(), module.__file__, "exec"), module.__dict__)
try:
    old, old_log = run_case()
    assert len(old.failures) == 8 and not old.errors, old_log
finally:
    sys.modules["hli.rank_transport"] = current_rank
    sys.modules["hli.signed_delta"] = current_delta
print(json.dumps({"current_passes": current.wasSuccessful(),
                  "preserved_pre_fix_failures": len(old.failures),
                  "preserved_pre_fix_errors": len(old.errors),
                  "current_output": current_log, "pre_fix_output": old_log}, indent=2))
