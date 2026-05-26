"""
python/test_gssk.py — Test suite for the GSSK Python ctypes binding.

Run from the repository root:
    make test-python

Or directly:
    python3 -m pytest python/test_gssk.py -v
    python3 python/test_gssk.py     # stdlib unittest fallback
"""

import json
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from python.gssk import GSSKSimulator, GSSKSchemaError, GSSKError, from_file

DECAY_JSON = json.dumps({
    "metadata": {"schema_version": 3, "name": "Decay Test"},
    "nodes": [
        {"id": "biomass",     "type": "storage", "value": 100.0},
        {"id": "environment", "type": "sink",    "value": 0.0},
    ],
    "edges": [
        {"id": "resp", "origin": "biomass", "target": "environment",
         "logic": "linear", "params": {"k": 0.05}},
    ],
    "config": {"t_start": 0.0, "t_end": 20.0, "dt": 0.5, "method": "rk4"},
})

MULTICARRIER_JSON = json.dumps({
    "metadata": {"schema_version": 3, "name": "Multi-Carrier"},
    "carriers": [
        {"id": "money",  "unit": "AUD", "conserved": True},
        {"id": "energy", "unit": "kWh", "conserved": True},
    ],
    "nodes": [
        {"id": "income",  "type": "source",  "value": 1.0,    "carrier": "money"},
        {"id": "account", "type": "storage", "value": 1000.0, "carrier": "money"},
        {"id": "spend",   "type": "sink",    "value": 0.0,    "carrier": "money"},
        {"id": "solar",   "type": "source",  "value": 1.0,    "carrier": "energy"},
        {"id": "battery", "type": "storage", "value": 10.0,   "carrier": "energy"},
        {"id": "grid",    "type": "sink",    "value": 0.0,    "carrier": "energy"},
    ],
    "edges": [
        {"id": "salary",  "origin": "income",  "target": "account", "carrier": "money",
         "logic": "constant", "params": {"k": 500.0}},
        {"id": "expense", "origin": "account", "target": "spend",   "carrier": "money",
         "logic": "linear",   "params": {"k": 0.1}},
        {"id": "charge",  "origin": "solar",   "target": "battery", "carrier": "energy",
         "logic": "constant", "params": {"k": 5.0}},
        {"id": "export",  "origin": "battery", "target": "grid",    "carrier": "energy",
         "logic": "linear",   "params": {"k": 0.05}},
    ],
    "config": {"t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "rk4"},
})


class TestInit(unittest.TestCase):

    def test_basic_init(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.state_size, 2)
        self.assertAlmostEqual(sim.start_time, 0.0)
        self.assertAlmostEqual(sim.end_time, 20.0)
        self.assertAlmostEqual(sim.default_dt, 0.5)

    def test_node_manifest(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.node_manifest[0], "biomass")
        self.assertEqual(sim.node_manifest[1], "environment")

    def test_model_name(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.model_name, "Decay Test")

    def test_kernel_version(self):
        v = GSSKSimulator.kernel_version()
        self.assertRegex(v, r"^\d+\.\d+\.\d+$")

    def test_invalid_json_raises(self):
        with self.assertRaises(GSSKError):
            GSSKSimulator("not json")

    def test_schema_violation_raises(self):
        bad = json.dumps({"metadata": {"schema_version": 3},
                          "nodes": [{"id": "a", "type": "storage", "value": 1}],
                          "edges": [{"id": "e", "origin": "a", "target": "missing",
                                     "logic": "linear", "params": {"k": 0.1}}],
                          "config": {"t_start": 0, "t_end": 1, "dt": 0.1}})
        with self.assertRaises(GSSKError):
            GSSKSimulator(bad)


class TestSimulation(unittest.TestCase):

    def test_initial_state(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertAlmostEqual(sim.state[0], 100.0)
        self.assertAlmostEqual(sim.state[1], 0.0)

    def test_single_step_rk4(self):
        sim = GSSKSimulator(DECAY_JSON)
        s = sim.step()
        # RK4 decay: Q(0.5) ≈ 100·exp(−0.05·0.5)
        expected = 100.0 * math.exp(-0.05 * 0.5)
        self.assertAlmostEqual(s[0], expected, places=2)

    def test_run_returns_correct_count(self):
        sim = GSSKSimulator(DECAY_JSON)
        results = sim.run()
        # 20.0 / 0.5 = 40 steps
        self.assertEqual(len(results), 40)

    def test_run_final_value(self):
        sim = GSSKSimulator(DECAY_JSON)
        results = sim.run()
        expected = 100.0 * math.exp(-0.05 * 20.0)
        self.assertAlmostEqual(results[-1][0], expected, places=2)

    def test_run_named(self):
        sim = GSSKSimulator(DECAY_JSON)
        named = sim.run_named()
        self.assertIn("biomass", named)
        self.assertEqual(len(named["biomass"]), 40)

    def test_named_state(self):
        sim = GSSKSimulator(DECAY_JSON)
        ns = sim.named_state
        self.assertAlmostEqual(ns["biomass"], 100.0)

    def test_reset(self):
        sim = GSSKSimulator(DECAY_JSON)
        sim.run()
        sim.reset()
        self.assertAlmostEqual(sim.current_time, 0.0)
        self.assertAlmostEqual(sim.state[0], 100.0)

    def test_time_advances(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertAlmostEqual(sim.current_time, 0.0)
        sim.step()
        self.assertAlmostEqual(sim.current_time, 0.5, places=10)


class TestEdgeAccess(unittest.TestCase):

    def test_edge_count(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.edge_count, 1)

    def test_edge_id(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.edge_id(0), "resp")

    def test_get_set_edge_k(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertAlmostEqual(sim.edge_k(0), 0.05)
        sim.set_edge_k(0, 0.1)
        self.assertAlmostEqual(sim.edge_k(0), 0.1)

    def test_find_edge(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.find_edge("resp"), 0)
        self.assertEqual(sim.find_edge("nonexistent"), -1)


class TestMultiCarrier(unittest.TestCase):

    def test_carrier_count(self):
        sim = GSSKSimulator(MULTICARRIER_JSON)
        self.assertEqual(sim.carrier_count, 2)

    def test_node_carrier_labels(self):
        sim = GSSKSimulator(MULTICARRIER_JSON)
        # income(0), account(1), spend(2) → money; solar(3), battery(4), grid(5) → energy
        self.assertEqual(sim.node_carrier(0), "money")
        self.assertEqual(sim.node_carrier(1), "money")
        self.assertEqual(sim.node_carrier(3), "energy")
        self.assertEqual(sim.node_carrier(4), "energy")

    def test_edge_carrier_labels(self):
        sim = GSSKSimulator(MULTICARRIER_JSON)
        self.assertEqual(sim.edge_carrier(0), "money")
        self.assertEqual(sim.edge_carrier(2), "energy")

    def test_carrier_conservation_error_zero_before_step(self):
        sim = GSSKSimulator(MULTICARRIER_JSON)
        self.assertAlmostEqual(sim.carrier_conservation_error(0), 0.0)

    def test_carrier_conservation_error_nonneg_after_step(self):
        sim = GSSKSimulator(MULTICARRIER_JSON)
        sim.step()
        self.assertGreaterEqual(sim.carrier_conservation_error(0), 0.0)

    def test_no_carriers_legacy_model(self):
        sim = GSSKSimulator(DECAY_JSON)
        self.assertEqual(sim.carrier_count, 0)


class TestSensitivity(unittest.TestCase):

    def test_forward_sensitivity_nonzero_after_steps(self):
        sim = GSSKSimulator(DECAY_JSON)
        sim.enable_forward_sensitivity([0])
        for _ in range(40):
            sim.step()
        s = sim.get_sensitivity(0, 0)
        # ∂Q_biomass/∂k should be negative after 40 steps
        self.assertNotEqual(s, 0.0)
        self.assertLess(s, 0.0)

    def test_sensitivity_zero_before_steps(self):
        sim = GSSKSimulator(DECAY_JSON)
        sim.enable_forward_sensitivity([0])
        self.assertAlmostEqual(sim.get_sensitivity(0, 0), 0.0)

    def test_disable_sensitivity(self):
        sim = GSSKSimulator(DECAY_JSON)
        sim.enable_forward_sensitivity([0])
        sim.disable_forward_sensitivity()
        # After disabling, sensitivity returns 0.0
        self.assertAlmostEqual(sim.get_sensitivity(0, 0), 0.0)


class TestSerialisation(unittest.TestCase):

    def test_serialize_model_no_snapshot(self):
        sim = GSSKSimulator(DECAY_JSON)
        model_str = sim.serialize_model()
        model_obj = json.loads(model_str)
        self.assertNotIn("snapshot", model_obj)
        self.assertIn("nodes", model_obj)

    def test_serialize_snapshot_has_snapshot(self):
        sim = GSSKSimulator(DECAY_JSON)
        for _ in range(5):
            sim.step()
        snap = sim.serialize_snapshot()
        snap_obj = json.loads(snap)
        self.assertIn("snapshot", snap_obj)

    def test_round_trip(self):
        sim1 = GSSKSimulator(DECAY_JSON)
        N = 10
        for _ in range(N):
            sim1.step()
        t_at_n = sim1.current_time
        snap = sim1.serialize_snapshot()

        sim2 = GSSKSimulator(snap)
        self.assertAlmostEqual(sim2.current_time, t_at_n, places=10)


class TestFromFile(unittest.TestCase):

    def test_from_file_household(self):
        path = os.path.join(os.path.dirname(__file__), "..", "examples", "household_model.json")
        if not os.path.exists(path):
            self.skipTest("household_model.json not found")
        sim = from_file(path)
        self.assertEqual(sim.carrier_count, 4)
        self.assertGreater(sim.state_size, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
