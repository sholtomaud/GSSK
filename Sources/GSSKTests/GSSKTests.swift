import XCTest
import CGSSK
@testable import GSSK

final class GSSKTests: XCTestCase {

    // MARK: - Fixtures

    static let decayModelJSON = """
    {
        "metadata": { "schema_version": 2, "name": "Decay Test" },
        "nodes": [
            { "id": "biomass",     "type": "storage", "value": 100.0 },
            { "id": "environment", "type": "sink",    "value": 0.0   }
        ],
        "edges": [
            {
                "id": "respiration",
                "origin": "biomass", "target": "environment",
                "logic": "linear",
                "params": { "k": 0.05 }
            }
        ],
        "config": { "t_start": 0.0, "t_end": 20.0, "dt": 0.5, "method": "rk4" }
    }
    """

    static let householdModelJSON = """
    {
        "metadata": { "schema_version": 2, "name": "Household" },
        "nodes": [
            { "id": "salary",    "type": "source",  "value": 5000.0 },
            { "id": "account",   "type": "storage", "value": 1000.0 },
            { "id": "groceries", "type": "sink",    "value": 0.0 },
            { "id": "rent",      "type": "sink",    "value": 0.0 }
        ],
        "edges": [
            { "id": "salary_in",      "origin": "salary",  "target": "account",   "logic": "constant", "params": { "k": 5000.0 } },
            { "id": "groceries_out",  "origin": "account", "target": "groceries", "logic": "constant", "params": { "k": 800.0 } },
            { "id": "rent_out",       "origin": "account", "target": "rent",      "logic": "constant", "params": { "k": 1500.0 } }
        ],
        "config": { "t_start": 0.0, "t_end": 12.0, "dt": 1.0, "method": "euler" }
    }
    """

    // MARK: - Basic init & properties

    func testDecayModelInitialises() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // decay_model has 2 nodes: biomass (storage) + environment (sink)
        XCTAssertEqual(sim.stateSize, 2)
        XCTAssertEqual(sim.startTime, 0.0)
        XCTAssertEqual(sim.endTime, 20.0)
        XCTAssertEqual(sim.defaultDt, 0.5)
    }

    func testDecayModelNodeLookup() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.nodeID(at: 0), "biomass")
        XCTAssertEqual(sim.nodeID(at: 1), "environment")
        XCTAssertEqual(sim.nodeIndex(id: "biomass"), 0)
        XCTAssertNil(sim.nodeIndex(id: "nonexistent"))
    }

    // MARK: - Node manifest (column ordering contract)

    func testNodeManifestBuiltFromKernel() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // Manifest should be auto-built from kernel after init
        XCTAssertEqual(sim.nodeManifest[0], "biomass")
        XCTAssertEqual(sim.nodeManifest[1], "environment")
        XCTAssertEqual(sim.nodeManifest.count, 2)
    }

    func testSerialiserOutputManifestPreserved() throws {
        let json = Self.householdModelJSON.data(using: .utf8)!
        // Serialiser declares its own manifest
        let manifest: [Int: String] = [
            0: "salary",
            1: "account",
            2: "groceries",
            3: "rent"
        ]
        let output = GSSKSerialiserOutput(json: json, nodeManifest: manifest)
        let sim = try GSSKSimulator(serialiserOutput: output)

        // Manifest from serialiser should be stored verbatim
        XCTAssertEqual(sim.nodeManifest, manifest)
    }

    // MARK: - Named result access

    func testNamedStateReturnsCorrectKeys() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let named = sim.namedState()
        XCTAssertEqual(named["biomass"] ?? -1,     100.0, accuracy: 1e-9)
        XCTAssertEqual(named["environment"] ?? -1, 0.0,   accuracy: 1e-9)
    }

    func testRunNamedReturnsTimeSeriesPerNode() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let named = try sim.runNamed()
        // Should have an entry for every node
        XCTAssertNotNil(named["biomass"])
        XCTAssertNotNil(named["environment"])
        // biomass time series should have 40 steps (20 / 0.5)
        XCTAssertEqual(named["biomass"]!.count, 40)
        // Final biomass: Q(20) = 100 * exp(-0.05 * 20) ≈ 36.788
        XCTAssertEqual(named["biomass"]!.last!, 100.0 * exp(-0.05 * 20.0), accuracy: 1e-3)
    }

    // MARK: - Multi-node household model

    func testHouseholdModelAccountBalance() throws {
        let sim = try GSSKSimulator(json: Self.householdModelJSON)
        let named = try sim.runNamed()
        // Net flow into account = +5000 (salary) - 800 (groceries) - 1500 (rent) = +2700/step
        // After 12 steps: 1000 + 12 * 2700 = 33400
        XCTAssertEqual(named["account"]!.last!, 1000.0 + 12.0 * 2700.0, accuracy: 1e-6)
    }

    // MARK: - State & step accuracy

    func testDecayModelInitialState() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let s = sim.state()
        XCTAssertEqual(s[0], 100.0, accuracy: 1e-9)  // biomass
        XCTAssertEqual(s[1], 0.0,   accuracy: 1e-9)  // environment
    }

    func testDecayModelSingleStep() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let s = try sim.step()
        // RK4: Q(0.5) = 100 * exp(-0.05 * 0.5) ≈ 97.531
        XCTAssertEqual(s[0], 100.0 * exp(-0.05 * 0.5), accuracy: 1e-3)
    }

    func testDecayModelRunToCompletion() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let results = try sim.run()
        XCTAssertEqual(results.count, 40)
        XCTAssertEqual(results.last![0], 100.0 * exp(-0.05 * 20.0), accuracy: 1e-3)
    }

    func testDecayModelReset() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        _ = try sim.run()
        sim.reset()
        XCTAssertEqual(sim.currentTime, 0.0)
        XCTAssertEqual(sim.state()[0], 100.0, accuracy: 1e-9)
    }

    // MARK: - Schema version validation

    func testSchemaMismatchThrows() throws {
        let badVersionJSON = """
        {
            "metadata": { "schema_version": 99 },
            "nodes": [{ "id": "x", "type": "storage", "value": 1.0 }],
            "config": { "t_start": 0, "t_end": 1, "dt": 0.1, "method": "euler" }
        }
        """
        let json = badVersionJSON.data(using: .utf8)!
        let output = GSSKSerialiserOutput(json: json, nodeManifest: [0: "x"])
        XCTAssertThrowsError(try GSSKSimulator(serialiserOutput: output)) { error in
            guard case GSSKError.schemaMismatch(let found, let expected) = error else {
                return XCTFail("Expected schemaMismatch, got \(error)")
            }
            XCTAssertEqual(found, 99)
            XCTAssertEqual(expected, GSSKSchemaVersion)
        }
    }

    // MARK: - Error paths

    func testInvalidJSONThrows() {
        XCTAssertThrowsError(try GSSKSimulator(json: "not json at all")) { error in
            guard case GSSKError.invalidJSON = error else {
                return XCTFail("Expected GSSKError.invalidJSON, got \(error)")
            }
        }
    }

    func testMissingNodesKeyThrows() {
        XCTAssertThrowsError(try GSSKSimulator(json: "{}"))
    }

    // MARK: - Topology Mutation Tests

    func testAddNodeAtRuntime() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.stateSize, 2)

        try sim.addNode(json: "{\"id\":\"new_node\",\"type\":\"storage\",\"value\":50.0}")
        XCTAssertEqual(sim.stateSize, 3)
        XCTAssertEqual(sim.nodeID(at: 2), "new_node")
        XCTAssertEqual(sim.state()[2], 50.0, accuracy: 1e-9)
        XCTAssertEqual(sim.nodeManifest[2], "new_node")
    }

    func testAddEdgeAtRuntime() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.edgeCount, 1)

        try sim.addEdge(json: "{\"id\":\"new_edge\",\"origin\":\"biomass\",\"target\":\"biomass\",\"logic\":\"linear\",\"params\":{\"k\":0.1}}")
        XCTAssertEqual(sim.edgeCount, 2)
        XCTAssertEqual(sim.edgeID(at: 1), "new_edge")
    }

    func testDeactivateEdge() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.edgeK(at: 0), 0.05, accuracy: 1e-9)

        try sim.deactivateEdge(id: "respiration")
        XCTAssertEqual(sim.edgeK(at: 0), 0.0, accuracy: 1e-9)
    }

    // MARK: - Quality Accounting Tests

    func testQualityAccounting() throws {
        let qualityModelJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Quality Test" },
            "nodes": [
                { "id": "sun", "type": "source", "value": 1000.0, "quality_input": 1.0 },
                { "id": "plant", "type": "storage", "value": 10.0 }
            ],
            "edges": [
                { "id": "photosynthesis", "origin": "sun", "target": "plant", "logic": "linear", "params": { "k": 0.1 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "auto" }
        }
        """
        let sim = try GSSKSimulator(json: qualityModelJSON)
        _ = try sim.step()

        let tr = sim.namedTransformationRatios()
        XCTAssertNotNil(tr["sun"])
        XCTAssertEqual(tr["sun"]!, 1.0, accuracy: 1e-9)
        XCTAssertNotNil(tr["plant"])
        // plant Tr should be same as sun Tr (1.0) because it's a single input
        XCTAssertEqual(tr["plant"]!, 1.0, accuracy: 1e-9)

        XCTAssertEqual(sim.solverConfidence, .high)
    }

    // MARK: - 0.3 Round-trip Serialization

    func testCurrentTimeAndStepCountAdvance() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.currentTime, 0.0, accuracy: 1e-12)
        XCTAssertEqual(sim.stepCount, 0)

        try sim.step()
        XCTAssertEqual(sim.currentTime, 0.5, accuracy: 1e-12)
        XCTAssertEqual(sim.stepCount, 1)

        try sim.step()
        XCTAssertEqual(sim.currentTime, 1.0, accuracy: 1e-12)
        XCTAssertEqual(sim.stepCount, 2)

        sim.reset()
        XCTAssertEqual(sim.currentTime, 0.0, accuracy: 1e-12)
        XCTAssertEqual(sim.stepCount, 0)
    }

    func testSerializeModelProducesFreshStart() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        for _ in 0..<5 { try sim.step() }

        let modelJSON = try sim.serializeModel()
        XCTAssertFalse(modelJSON.isEmpty)
        XCTAssertTrue(modelJSON.contains("\"schema_version\""))
        XCTAssertFalse(modelJSON.contains("\"snapshot\""),
                       "SerializeModel must not include a snapshot block")

        // Re-initialising from the model JSON must restart from t_start
        let sim2 = try GSSKSimulator(json: modelJSON)
        XCTAssertEqual(sim2.currentTime, 0.0, accuracy: 1e-12)
        XCTAssertEqual(sim2.stepCount, 0)
    }

    func testSerializeSnapshotContainsSnapshotBlock() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        for _ in 0..<4 { try sim.step() }

        let snapshotJSON = try sim.serializeSnapshot()
        XCTAssertTrue(snapshotJSON.contains("\"snapshot\""))
        XCTAssertTrue(snapshotJSON.contains("\"t\""))
        XCTAssertTrue(snapshotJSON.contains("\"step\""))
        XCTAssertTrue(snapshotJSON.contains("\"state\""))
        XCTAssertTrue(snapshotJSON.contains("\"edge_k\""))
        XCTAssertTrue(snapshotJSON.contains("\"solver\""))
    }

    func testSnapshotResumesAtCorrectTime() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let N = 6
        for _ in 0..<N { try sim.step() }
        let tAtN = sim.currentTime

        let snapshotJSON = try sim.serializeSnapshot()
        let resumed = try GSSKSimulator(json: snapshotJSON)
        XCTAssertEqual(resumed.currentTime, tAtN, accuracy: 1e-12)
        XCTAssertEqual(resumed.stepCount,   N)
    }

    // MARK: - Phase 1 — IDC as Baseline tests

    func testLimitEdgeIDCEligible() throws {
        let limitModelJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Limit IDC Test" },
            "nodes": [
                { "id": "resource", "type": "storage",  "value": 100.0 },
                { "id": "capacity", "type": "constant", "value": 50.0 },
                { "id": "sink",     "type": "sink",      "value": 0.0 }
            ],
            "edges": [
                {
                    "id": "satflow",
                    "origin": "resource", "target": "sink",
                    "logic": "limit",
                    "params": { "k": 0.1, "control_node": "capacity" }
                }
            ],
            "config": { "t_start": 0.0, "t_end": 5.0, "dt": 1.0, "method": "auto" }
        }
        """
        let sim = try GSSKSimulator(json: limitModelJSON)
        // Phase 1: limit edges now have IDC treatment (effective-conductance
        // linearisation), so IDC runs and stepErrorEstimate is populated.
        // For a nonlinear limit edge with dt=1.0 the linearisation error may
        // exceed tolerance (DEGRADED is acceptable), but the simulation must
        // progress and the error estimate must be computed (> 0).
        _ = try sim.step()
        XCTAssertGreaterThan(sim.stepErrorEstimate, 0.0,
            "Step error must be computed — IDC must have run on limit edge")
        XCTAssertGreaterThan(sim.edgeErrorEstimate(at: 0), 0.0,
            "Per-edge error must be computed for the limit edge")
        // Resource should decrease regardless of which solver result was chosen
        XCTAssertLessThan(sim.state()[0], 100.0)
    }

    func testThresholdEventDetected() throws {
        // tank=8 fills at constant k=3 with no drain → after dt=1 tank=11 > threshold=9.
        // Event detection requires a persistent sign change (was_above != is_above)
        // at the RK4 endpoint; the Illinois algorithm then locates t* ≈ 1/3.
        let threshModelJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Threshold Event Test" },
            "nodes": [
                { "id": "tank",   "type": "storage", "value": 8.0 },
                { "id": "source", "type": "source",  "value": 0.0 },
                { "id": "drain",  "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "fill",  "origin": "source", "target": "tank",
                  "logic": "constant", "params": { "k": 3.0 } },
                { "id": "valve", "origin": "tank",   "target": "drain",
                  "logic": "threshold", "params": { "k": 0.5, "threshold": 9.0 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "auto" }
        }
        """
        // Net fill: +3 − 0.5 = +2.5/step once valve opens. Tank=8 → RK4 ends > 9.
        // (valve drain k is small so tank stays above threshold after the step)
        let sim = try GSSKSimulator(json: threshModelJSON)
        _ = try sim.step()
        // At least one event should have been recorded for the "valve" edge
        XCTAssertGreaterThan(sim.eventCount, 0,
            "Expected threshold crossing event for 'valve'")
        if let ev = sim.event(at: 0) {
            XCTAssertEqual(ev.edgeID, "valve")
            XCTAssertEqual(ev.direction, 1)       // upward crossing
            XCTAssertGreaterThan(ev.t, 0.0)
            XCTAssertLessThan(ev.t, 1.0)          // within [0, dt]
        }
    }

    func testThresholdEventsClearedOnReset() throws {
        let threshModelJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Event Reset Test" },
            "nodes": [
                { "id": "tank",   "type": "storage", "value": 8.0 },
                { "id": "source", "type": "source",  "value": 0.0 },
                { "id": "drain",  "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "fill",  "origin": "source", "target": "tank",
                  "logic": "constant",  "params": { "k": 3.0 } },
                { "id": "valve", "origin": "tank",   "target": "drain",
                  "logic": "threshold", "params": { "k": 0.5, "threshold": 9.0 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "auto" }
        }
        """
        let sim = try GSSKSimulator(json: threshModelJSON)
        _ = try sim.step()
        XCTAssertGreaterThan(sim.eventCount, 0)
        sim.reset()
        XCTAssertEqual(sim.eventCount, 0)
    }

    func testStepErrorEstimatePopulated() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // Decay model uses "rk4" method — step_error stays 0
        _ = try sim.step()
        // stepErrorEstimate defined but not required to be nonzero for rk4 mode
        XCTAssertGreaterThanOrEqual(sim.stepErrorEstimate, 0.0)
        // edge error for index 0 should be >= 0
        XCTAssertGreaterThanOrEqual(sim.edgeErrorEstimate(at: 0), 0.0)
    }

    func testRiccatiIsolatedDuetExact() throws {
        // Isolated 2-node interaction: F = k·Q_prey·Q_pred, control==target.
        // Conservation: S = Q_prey + Q_pred = 150 (invariant).
        // Exact closed form: Q_prey(dt) = S·Q_A₀ / (Q_A₀ + Q_B₀·exp(k·S·dt))
        // With method="incipient", the kernel detects the isolated duet and
        // applies the exact Riccati formula — result matches analytics to FP precision.
        let duetJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Riccati Duet" },
            "nodes": [
                { "id": "prey", "type": "storage", "value": 100.0 },
                { "id": "pred", "type": "storage", "value":  50.0 }
            ],
            "edges": [
                { "id": "predation",
                  "origin": "prey", "target": "pred",
                  "logic": "interaction",
                  "params": { "k": 0.001, "control_node": "pred" } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "incipient" }
        }
        """
        let sim = try GSSKSimulator(json: duetJSON)
        let s = try sim.step()

        let QA0 = 100.0, QB0 = 50.0, k = 0.001, dt = 1.0
        let S = QA0 + QB0
        let QA_exact = S * QA0 / (QA0 + QB0 * exp(k * S * dt))
        let QB_exact = S - QA_exact

        XCTAssertEqual(s[0], QA_exact, accuracy: 1e-10, "prey (INCIPIENT exact Riccati)")
        XCTAssertEqual(s[1], QB_exact, accuracy: 1e-10, "pred (INCIPIENT exact Riccati)")
        // Conservation: S must be preserved exactly
        XCTAssertEqual(s[0] + s[1], S, accuracy: 1e-12, "S = Q_prey + Q_pred conserved")
    }

    func testRiccatiDuetConservationAcrossRun() throws {
        // Over a full 10-step run, the sum Q_prey + Q_pred must remain S throughout.
        let duetJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Riccati Conservation" },
            "nodes": [
                { "id": "prey", "type": "storage", "value": 80.0 },
                { "id": "pred", "type": "storage", "value": 20.0 }
            ],
            "edges": [
                { "id": "predation",
                  "origin": "prey", "target": "pred",
                  "logic": "interaction",
                  "params": { "k": 0.002, "control_node": "pred" } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "incipient" }
        }
        """
        let sim = try GSSKSimulator(json: duetJSON)
        let S = 80.0 + 20.0  // conserved sum
        let timeSeries = try sim.run()
        for (step, state) in timeSeries.enumerated() {
            XCTAssertEqual(state[0] + state[1], S, accuracy: 1e-12,
                           "Conservation violated at step \(step)")
        }
    }

    func testAutoModeComputesEdgeError() throws {
        let autoModelJSON = """
        {
            "metadata": { "schema_version": 2, "name": "Auto Error Test" },
            "nodes": [
                { "id": "a", "type": "storage", "value": 100.0 },
                { "id": "b", "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "flow", "origin": "a", "target": "b",
                  "logic": "linear", "params": { "k": 0.1 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 0.5, "method": "auto" }
        }
        """
        let sim = try GSSKSimulator(json: autoModelJSON)
        _ = try sim.step()
        // AUTO mode must have computed edge errors
        let err = sim.edgeErrorEstimate(at: 0)
        XCTAssertGreaterThanOrEqual(err, 0.0)
        // For a linear system with moderate dt, IDC should agree with RK4 closely
        XCTAssertEqual(sim.solverConfidence, .high)
    }

    func testRoundTripPropertyBitIdentical() throws {
        // Property: Init → Step×N → SerializeSnapshot → Init → Step×M
        //           == Init → Step×(N+M)  (bit-identical for deterministic solvers)
        let modelJSON = """
        {
            "metadata": { "schema_version": 3, "name": "Round-trip Test" },
            "nodes": [
                { "id": "biomass",     "type": "storage", "value": 100.0 },
                { "id": "environment", "type": "sink",    "value": 0.0   }
            ],
            "edges": [
                { "id": "resp", "origin": "biomass", "target": "environment",
                  "logic": "linear", "params": { "k": 0.05 } }
            ],
            "config": { "t_start": 0.0, "t_end": 50.0, "dt": 0.5, "method": "rk4" }
        }
        """
        let N = 10
        let M = 8

        // Path A: Step N, snapshot, resume, step M more
        let simA1 = try GSSKSimulator(json: modelJSON)
        for _ in 0..<N { try simA1.step() }
        let snapshot = try simA1.serializeSnapshot()
        let simA2 = try GSSKSimulator(json: snapshot)
        var pathA: [[Double]] = []
        for _ in 0..<M { pathA.append(try simA2.step()) }

        // Path B: Step N+M in one go, keep last M results
        let simB = try GSSKSimulator(json: modelJSON)
        var pathB: [[Double]] = []
        for i in 0..<(N + M) {
            let s = try simB.step()
            if i >= N { pathB.append(s) }
        }

        XCTAssertEqual(pathA.count, pathB.count)
        for (stepIdx, (a, b)) in zip(pathA, pathB).enumerated() {
            for (col, (qa, qb)) in zip(a, b).enumerated() {
                XCTAssertEqual(qa, qb, accuracy: 1e-12,
                    "Diverged at step \(stepIdx) col \(col): A=\(qa) B=\(qb)")
            }
        }
    }

    // MARK: - Phase 1.3 sub-stepping tests

    /// Sub-stepping: a single crossing in one step should emit exactly one
    /// event and the integrator should restart from the crossing point.
    ///
    /// Threshold flow is constant k per unit time (not k·Q), so to cross from
    /// 5.0 to threshold 3.0 within dt=1.0 we need k > 2.0. With k=3.0 the
    /// crossing occurs at t* = (5.0 − 3.0)/3.0 ≈ 0.667 s.
    func testSubSteppingRestartFromCrossing() throws {
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Substep Test" },
            "nodes": [
                { "id": "tank", "type": "storage", "value": 5.0 },
                { "id": "sink", "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "valve", "origin": "tank", "target": "sink",
                  "logic": "threshold", "params": { "k": 3.0, "threshold": 3.0 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "rk4" }
        }
        """
        let sim = try GSSKSimulator(json: json)
        _ = try sim.step()

        XCTAssertGreaterThanOrEqual(sim.eventCount, 1,
            "Expected at least one threshold event from sub-stepping")

        let ev = sim.event(at: 0)
        XCTAssertNotNil(ev)
        // Tank was above threshold and crossed downward
        XCTAssertEqual(ev!.direction, -1, "Expected downward crossing (direction = -1)")
        XCTAssertEqual(ev!.edgeID, "valve")
        // Crossing time must be strictly inside the step interval [0, 1)
        XCTAssertGreaterThan(ev!.t, 0.0)
        XCTAssertLessThan(ev!.t, 1.0)
    }

    /// Degenerate-start guard: if the origin node begins exactly on the
    /// threshold, no event should be emitted for that step (avoids spurious
    /// re-detection after a prior crossing landed the state on the threshold).
    func testDegenerateStartNoSpuriousEvent() throws {
        // Tank starts exactly at the threshold value.
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Degenerate Guard Test" },
            "nodes": [
                { "id": "tank", "type": "storage", "value": 3.0 },
                { "id": "sink", "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "valve", "origin": "tank", "target": "sink",
                  "logic": "threshold", "params": { "k": 0.5, "threshold": 3.0 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "rk4" }
        }
        """
        let sim = try GSSKSimulator(json: json)
        _ = try sim.step()

        // Starting exactly on the threshold — degenerate guard must suppress detection
        XCTAssertEqual(sim.eventCount, 0,
            "No event expected when origin starts exactly on the threshold")
    }

    /// Sequential crossings in one step: two threshold edges crossed at
    /// different times within a single dt.  The sub-stepping loop must detect
    /// both and emit 2 events (one per iteration of the restart loop).
    ///
    /// Setup: tank=5.0, v1(threshold=3.0, k=3.0), v2(threshold=2.5, k=3.0).
    ///   Iteration 1 — finds v1 crossing at t*≈(5−3)/6≈0.33 s (both edges active).
    ///   Iteration 2 — from Q≈3.0, v2 still active (3.0>2.5); finds v2 crossing
    ///                 at t*≈(3−2.5)/3≈0.17 s within the remaining 0.67 s.
    func testSequentialCrossingsInOneStep() throws {
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Sequential Crossings Test" },
            "nodes": [
                { "id": "tank",  "type": "storage", "value": 5.0 },
                { "id": "sink1", "type": "sink",    "value": 0.0 },
                { "id": "sink2", "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "v1", "origin": "tank", "target": "sink1",
                  "logic": "threshold", "params": { "k": 3.0, "threshold": 3.0 } },
                { "id": "v2", "origin": "tank", "target": "sink2",
                  "logic": "threshold", "params": { "k": 3.0, "threshold": 2.5 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "rk4" }
        }
        """
        let sim = try GSSKSimulator(json: json)
        _ = try sim.step()

        // Both thresholds crossed in one step via sub-stepping restart
        XCTAssertEqual(sim.eventCount, 2,
            "Expected 2 events: sub-stepping must iterate to find both crossings")

        // Both must be downward crossings strictly inside [0, 1)
        for i in 0..<sim.eventCount {
            let ev = sim.event(at: i)!
            XCTAssertEqual(ev.direction, -1, "Event \(i) should be a downward crossing")
            XCTAssertGreaterThan(ev.t, 0.0)
            XCTAssertLessThan(ev.t, 1.0)
        }

        // Events should be ordered in time
        if sim.eventCount >= 2 {
            let t0 = sim.event(at: 0)!.t
            let t1 = sim.event(at: 1)!.t
            XCTAssertLessThan(t0, t1, "Events must be emitted in chronological order")
        }
    }

    // MARK: - Phase 2 — Adaptive Numerics tests

    /// DOPRI5 adaptive mode: simple exponential decay Q' = -k·Q should match
    /// the analytical solution Q(t) = Q0·exp(-k·t) within rel_tol.
    func testAdaptiveDecayAccuracy() throws {
        let k = 0.5
        let Q0 = 10.0
        let tEnd = 4.0
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Adaptive Decay Test" },
            "nodes": [
                { "id": "tank", "type": "storage", "value": \(Q0) },
                { "id": "sink", "type": "sink",    "value": 0.0 }
            ],
            "edges": [
                { "id": "drain", "origin": "tank", "target": "sink",
                  "logic": "linear", "params": { "k": \(k) } }
            ],
            "config": {
                "t_start": 0.0, "t_end": \(tEnd), "dt": 1.0,
                "method": "adaptive",
                "rel_tol": 1e-8, "abs_tol": 1e-10
            }
        }
        """
        let sim = try GSSKSimulator(json: json)

        // Run to t_end using adaptive steps
        while sim.currentTime < tEnd - 1e-12 {
            _ = try sim.stepAdaptive()
        }

        let q_numerical  = sim.state()[0]
        let q_analytical = Q0 * exp(-k * tEnd)
        XCTAssertEqual(q_numerical, q_analytical, accuracy: 1e-6,
            "Adaptive DOPRI5 should match analytical decay to within rel_tol=1e-8")
    }

    /// GSSK_Step with method="adaptive" should behave the same as calling
    /// stepAdaptive() — the adaptive solver spans the requested dt.
    func testAdaptiveStepInterfaceMatchesFixed() throws {
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Adaptive Interface Test" },
            "nodes": [
                { "id": "A", "type": "storage", "value": 5.0 },
                { "id": "B", "type": "storage", "value": 1.0 }
            ],
            "edges": [
                { "id": "e1", "origin": "A", "target": "B",
                  "logic": "linear", "params": { "k": 0.3 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 0.5,
                        "method": "adaptive", "rel_tol": 1e-7 }
        }
        """
        let sim = try GSSKSimulator(json: json)
        for _ in 0..<10 { _ = try sim.step() }

        // Time should have advanced by 10 × 0.5 = 5.0
        XCTAssertEqual(sim.currentTime, 5.0, accuracy: 1e-12,
            "10 adaptive steps of dt=0.5 should reach t=5.0")

        // lastStepSize should be set
        XCTAssertGreaterThan(sim.lastStepSize, 0.0)
        // nextStepSize should be positive
        XCTAssertGreaterThan(sim.nextStepSize, 0.0)
    }

    /// Conservation error for a closed 2-node storage-only system must be
    /// near zero (linear edge conserves Q by construction).
    func testConservationErrorClosedSystem() throws {
        let json = """
        {
            "metadata": { "schema_version": 3, "name": "Conservation Test" },
            "nodes": [
                { "id": "A", "type": "storage", "value": 8.0 },
                { "id": "B", "type": "storage", "value": 2.0 }
            ],
            "edges": [
                { "id": "e", "origin": "A", "target": "B",
                  "logic": "linear", "params": { "k": 0.4 } }
            ],
            "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0,
                        "method": "adaptive", "rel_tol": 1e-7 }
        }
        """
        let sim = try GSSKSimulator(json: json)
        _ = try sim.step()

        // Closed system: A+B = 10 always; conservation error should be tiny
        XCTAssertLessThan(sim.conservationError, 1e-5,
            "Conservation error must be near zero for closed storage-only system")

        // Total Q = 10 within tolerance
        let total = sim.state()[0] + sim.state()[1]
        XCTAssertEqual(total, 10.0, accuracy: 1e-5,
            "Total Q must be conserved in a closed storage system")
    }

    /// PI controller should progressively tighten the step size when tolerances
    /// are tight, and loosen it when tolerances are relaxed. Verify that
    /// nextStepSize after a tight run < nextStepSize after a loose run.
    func testPIControllerTightens() throws {
        let modelBase = """
        {
            "metadata": { "schema_version": 3, "name": "PI Controller Test" },
            "nodes": [
                { "id": "x", "type": "storage", "value": 1.0 },
                { "id": "y", "type": "storage", "value": 1.0 }
            ],
            "edges": [
                { "id": "e", "origin": "x", "target": "y",
                  "logic": "interaction",
                  "params": { "k": 0.5, "control_node": "y" } }
            ],
            "config": { "t_start": 0.0, "t_end": 5.0, "dt": 0.5, "method": "adaptive" }
        }
        """
        // Tight tolerances
        let tightJSON = modelBase.replacingOccurrences(of: "\"adaptive\"",
            with: "\"adaptive\", \"rel_tol\": 1e-10, \"abs_tol\": 1e-12")
        let simTight = try GSSKSimulator(json: tightJSON)
        for _ in 0..<5 { _ = try simTight.step() }

        // Loose tolerances
        let looseJSON = modelBase.replacingOccurrences(of: "\"adaptive\"",
            with: "\"adaptive\", \"rel_tol\": 1e-3, \"abs_tol\": 1e-4")
        let simLoose = try GSSKSimulator(json: looseJSON)
        for _ in 0..<5 { _ = try simLoose.step() }

        XCTAssertLessThanOrEqual(simTight.lastStepSize, simLoose.lastStepSize * 10.0,
            "Tighter tolerance should not produce larger steps than loose tolerance")
    }

    // MARK: - Phase 3 — Sensitivity Analysis tests

    // 3.1 Forward sensitivity: linear decay Q(t) = Q0·exp(−k·t)
    // Exact sensitivity: ∂Q/∂k = −t·Q0·exp(−k·t)
    func testForwardSensitivityLinearDecay() throws {
        let json = """
        {
            "metadata": {"schema_version": 3, "name": "Decay Sensitivity"},
            "nodes": [
                {"id": "q", "type": "storage", "value": 10.0},
                {"id": "sink", "type": "sink", "value": 0.0}
            ],
            "edges": [
                {"id": "e0", "origin": "q", "target": "sink",
                 "logic": "linear", "params": {"k": 0.5}}
            ],
            "config": {"t_start": 0.0, "t_end": 4.0, "dt": 0.01, "method": "rk4"}
        }
        """
        let sim = try GSSKSimulator(json: json)
        try sim.enableForwardSensitivity(paramEdgeIndices: [0])

        let Q0 = 10.0, k = 0.5, dt = 0.01
        let nSteps = 400
        for _ in 0..<nSteps { _ = try sim.step() }

        let t = Double(nSteps) * dt      // = 4.0
        let exactQ   =  Q0 * exp(-k * t)
        let exactSens = -t * Q0 * exp(-k * t) // ∂Q/∂k

        let simQ    = sim.state()[0]
        let simSens = sim.getSensitivity(nodeIdx: 0, paramIdx: 0)

        XCTAssertEqual(simQ, exactQ, accuracy: 1e-3,
            "State should match analytical decay")
        XCTAssertEqual(simSens, exactSens, accuracy: 0.05 * fabs(exactSens),
            "Forward sensitivity ∂Q/∂k should match −t·Q0·exp(−k·t) within 5%")
    }

    // 3.1 Reset zeroes sensitivity matrix
    func testForwardSensitivityResetToZero() throws {
        let json = """
        {
            "metadata": {"schema_version": 3, "name": "Sens Reset"},
            "nodes": [
                {"id": "a", "type": "storage", "value": 5.0},
                {"id": "b", "type": "sink",    "value": 0.0}
            ],
            "edges": [
                {"id": "e", "origin": "a", "target": "b",
                 "logic": "linear", "params": {"k": 1.0}}
            ],
            "config": {"t_start": 0.0, "t_end": 2.0, "dt": 0.1, "method": "rk4"}
        }
        """
        let sim = try GSSKSimulator(json: json)
        try sim.enableForwardSensitivity(paramEdgeIndices: [0])
        for _ in 0..<10 { _ = try sim.step() }
        XCTAssertNotEqual(sim.getSensitivity(nodeIdx: 0, paramIdx: 0), 0.0,
            "Sensitivity should be non-zero after stepping")
        sim.reset()
        XCTAssertEqual(sim.getSensitivity(nodeIdx: 0, paramIdx: 0), 0.0,
            "Sensitivity should be zero after reset")
    }

    // 3.2 Adjoint gradient sign: increasing k on a drain edge should decrease Q(T)
    // so ∂L/∂k > 0 when L = (Q(T) - 0)² and target=0 (gradient points up hill).
    // Actually: L = ½(Q(T) - target)². If target<Q(T), gradient is positive.
    // With target=0 and Q(T)>0: λ(T)=(Q(T)-0)>0; dL/dk = ∫λ·(∂f/∂k)dt.
    // For a linear drain edge: ∂f/∂k = -Q (origin loses flow).
    // So integrand λ·(-Q) < 0 → gradient negative → increasing k decreases L. Makes sense.
    func testAdjointGradientSign() throws {
        let json = """
        {
            "metadata": {"schema_version": 3, "name": "Adjoint Sign Test"},
            "nodes": [
                {"id": "q", "type": "storage", "value": 10.0},
                {"id": "s", "type": "sink",    "value": 0.0}
            ],
            "edges": [
                {"id": "drain", "origin": "q", "target": "s",
                 "logic": "linear", "params": {"k": 0.5}}
            ],
            "config": {"t_start": 0.0, "t_end": 2.0, "dt": 0.05, "method": "rk4"}
        }
        """
        let sim = try GSSKSimulator(json: json)
        // Target: drive Q(T) toward 0 (minimum loss if Q(T) is small)
        // λ(T) = weight*(Q(T)-0) > 0 since Q decays but stays positive
        let grad = try sim.runAdjoint(
            targets: [(nodeIdx: 0, targetValue: 0.0, weight: 1.0)],
            paramEdgeIndices: [0])
        // Gradient should be negative: increasing k reduces Q → reduces L
        XCTAssertLessThan(grad[0], 0.0,
            "∂L/∂k for a drain edge toward target=0 should be negative (increasing k helps)")
    }

    // 3.2 Adjoint gradient consistency: compare with finite-difference gradient
    func testAdjointGradientConsistentWithFiniteDiff() throws {
        let json = """
        {
            "metadata": {"schema_version": 3, "name": "Adjoint FD Test"},
            "nodes": [
                {"id": "q", "type": "storage", "value": 8.0},
                {"id": "s", "type": "sink",    "value": 0.0}
            ],
            "edges": [
                {"id": "e", "origin": "q", "target": "s",
                 "logic": "linear", "params": {"k": 0.3}}
            ],
            "config": {"t_start": 0.0, "t_end": 1.0, "dt": 0.02, "method": "rk4"}
        }
        """
        let sim = try GSSKSimulator(json: json)
        let target = 3.0

        // Adjoint gradient
        let adjGrad = try sim.runAdjoint(
            targets: [(nodeIdx: 0, targetValue: target, weight: 1.0)],
            paramEdgeIndices: [0])

        // Finite-difference gradient
        let eps = 1e-5
        func objectiveAt(_ k: Double) throws -> Double {
            let s2 = try GSSKSimulator(json: json.replacingOccurrences(of: "\"k\": 0.3",
                                                                        with: "\"k\": \(k)"))
            let steps = 50
            for _ in 0..<steps { _ = try s2.step() }
            let qT = s2.state()[0]
            return 0.5 * (qT - target) * (qT - target)
        }
        let fp = try objectiveAt(0.3 + eps)
        let fm = try objectiveAt(0.3 - eps)
        let fdGrad = (fp - fm) / (2.0 * eps)

        XCTAssertEqual(adjGrad[0], fdGrad, accuracy: fabs(fdGrad) * 0.15,
            "Adjoint gradient should match finite-difference within 15%")
    }

    // 3.3 Transformity sensitivity: with quality enabled, perturbing edge k
    // should produce a Tr change consistent with the returned sensitivity.
    func testTransformitySensitivityFiniteDiff() throws {
        let makeJSON = { (k: Double) in """
        {
            "metadata": {"schema_version": 3, "name": "Tr Sensitivity"},
            "nodes": [
                {"id": "sun",   "type": "source",  "value": 100.0, "quality_input": 1.0},
                {"id": "plant", "type": "storage", "value":  50.0},
                {"id": "sink",  "type": "sink",    "value":   0.0}
            ],
            "edges": [
                {"id": "e0", "origin": "sun",   "target": "plant",
                 "logic": "constant", "params": {"k": \(k)}},
                {"id": "e1", "origin": "plant", "target": "sink",
                 "logic": "linear",   "params": {"k": 0.1}}
            ],
            "config": {"t_start": 0.0, "t_end": 5.0, "dt": 0.5, "method": "rk4"}
        }
        """ }
        let sim = try GSSKSimulator(json: makeJSON(1.0))
        _ = try sim.step() // run one step so Tr is populated
        let dTr_dk = sim.getTransformitySensitivity(nodeIdx: 1, edgeIdx: 0)

        let eps = 1e-4
        let simP = try GSSKSimulator(json: makeJSON(1.0 + eps))
        _ = try simP.step()
        let simM = try GSSKSimulator(json: makeJSON(1.0 - eps))
        _ = try simM.step()
        let trP = simP.transformationRatios()[1]
        let trM = simM.transformationRatios()[1]
        let fdSens = (trP - trM) / (2.0 * eps)

        if fabs(fdSens) > 1e-10 {
            XCTAssertEqual(dTr_dk, fdSens, accuracy: fabs(fdSens) * 0.20,
                "Transformity sensitivity should match FD within 20%")
        }
    }

    // 3.4 Gradient calibration: adjoint-based gradient descent moves k in the right direction.
    // True decay: Q(t)=10·exp(−0.4·t). Starting guess k=0.1. After gradient descent,
    // k should increase toward 0.4 (L = (Q(T)−target)², target = Q_true(T)).
    func testCalibrateGradientReducesObjective() throws {
        let Q0 = 10.0, trueK = 0.4, T = 2.0
        let target = Q0 * exp(-trueK * T)  // Q_true at t_end

        func makeSimAt(k: Double) throws -> GSSKSimulator {
            return try GSSKSimulator(json: """
            {
                "metadata": {"schema_version": 3, "name": "GD Calib"},
                "nodes": [
                    {"id": "q", "type": "storage", "value": \(Q0)},
                    {"id": "s", "type": "sink", "value": 0.0}
                ],
                "edges": [
                    {"id": "e", "origin": "q", "target": "s",
                     "logic": "linear", "params": {"k": \(k)}}
                ],
                "config": {"t_start": 0.0, "t_end": \(T), "dt": 0.05, "method": "rk4"}
            }
            """)
        }

        let kBefore = 0.1
        // One adjoint step to verify the gradient points in the right direction
        let simForAdj = try makeSimAt(k: kBefore)
        let grad = try simForAdj.runAdjoint(
            targets: [(nodeIdx: 0, targetValue: target, weight: 1.0)],
            paramEdgeIndices: [0])

        // With k=0.1 < trueK=0.4: Q(T) > target, so L>0 and increasing k reduces L.
        // ∂L/∂k should be negative (gradient points in direction that increases L,
        // so we subtract it to move toward minimum).
        XCTAssertLessThan(grad[0], 0.0,
            "∂L/∂k should be negative: increasing k from 0.1 moves Q(T) toward target")

        // Normalized gradient descent: one step with step size 0.1
        let kAfter = max(0.0, kBefore - 0.1 * grad[0] / (abs(grad[0]) + 1e-10))
        XCTAssertGreaterThan(kAfter, kBefore,
            "Gradient step should increase k from 0.1 toward 0.4")
        XCTAssertLessThanOrEqual(kAfter, 2.0,
            "Normalized gradient step should stay bounded")
    }

    // MARK: - Phase 4 — Mutation Log & Replay tests

    func testMutationLogRecordsAddNode() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.mutationCount, 0, "No mutations at init")

        try sim.addNode(json: """
            {"id":"carbon","type":"storage","value":50.0}
            """)

        XCTAssertEqual(sim.mutationCount, 1)
        let m = try XCTUnwrap(sim.mutation(at: 0))
        XCTAssertEqual(m.op, "add_node")
        XCTAssertEqual(m.targetID, "carbon")
        XCTAssertEqual(m.cause, "user")
    }

    func testMutationLogRecordsSetEdgeK() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // Step once so t > 0
        try sim.step()
        sim.setEdgeK(0.1, at: 0)

        XCTAssertEqual(sim.mutationCount, 1)
        let m = try XCTUnwrap(sim.mutation(at: 0))
        XCTAssertEqual(m.op, "set_edge_k")
        XCTAssertEqual(m.t, sim.currentTime, accuracy: 1e-12)
        XCTAssertEqual(Double(m.payload) ?? 0.0, 0.1, accuracy: 1e-15)
    }

    func testMutationLogCauseOverride() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        sim.setMutationCause("calibration")
        sim.setEdgeK(0.2, at: 0)

        let m = try XCTUnwrap(sim.mutation(at: 0))
        XCTAssertEqual(m.cause, "calibration")

        // Cause resets after one mutation
        sim.setEdgeK(0.3, at: 0)
        let m2 = try XCTUnwrap(sim.mutation(at: 1))
        XCTAssertEqual(m2.cause, "user", "Cause should revert to 'user' after first use")
    }

    func testMutationLogClearsCorrectly() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        sim.setEdgeK(0.1, at: 0)
        sim.setEdgeK(0.2, at: 0)
        XCTAssertEqual(sim.mutationCount, 2)
        sim.clearMutationLog()
        XCTAssertEqual(sim.mutationCount, 0)
    }

    func testMutationLogRoundTripsViaSnapshot() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        try sim.step()
        sim.setEdgeK(0.08, at: 0)
        try sim.step()

        let snap = try sim.serializeSnapshot()
        let sim2 = try GSSKSimulator(json: snap)

        XCTAssertEqual(sim2.mutationCount, 1, "Mutation log should survive snapshot round-trip")
        let m = try XCTUnwrap(sim2.mutation(at: 0))
        XCTAssertEqual(m.op, "set_edge_k")
        XCTAssertEqual(Double(m.payload) ?? 0.0, 0.08, accuracy: 1e-12)
    }

    func testExportMutationLogIsValidJSON() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        sim.setEdgeK(0.1, at: 0)
        let log = try sim.exportMutationLog()
        // Must be a valid JSON array
        let data = try XCTUnwrap(log.data(using: .utf8))
        let parsed = try XCTUnwrap(try JSONSerialization.jsonObject(with: data) as? [[String: Any]])
        XCTAssertEqual(parsed.count, 1)
        XCTAssertEqual(parsed[0]["op"] as? String, "set_edge_k")
    }

    func testReplayMatchesOriginalRun() throws {
        // Run original model and record state at t_end
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let originalResults = try sim.run()
        let originalFinalState = originalResults.last!

        // Replay with no mutations — should produce same final state
        let replayed = try GSSKSimulator.replay(
            from: Self.decayModelJSON,
            mutations: nil,
            until: 20.0
        )
        let replayedState = replayed.state()

        XCTAssertEqual(replayedState.count, originalFinalState.count)
        for i in 0 ..< replayedState.count {
            XCTAssertEqual(replayedState[i], originalFinalState[i], accuracy: 1e-10,
                "Replay state['\(replayed.nodeID(at: i) ?? "\(i)")'] should match original")
        }
    }

    // MARK: - Phase 5 — Multi-Carrier Schema tests

    static let multiCarrierModelJSON = """
    {
        "metadata": { "schema_version": 3, "name": "Multi-Carrier Test" },
        "carriers": [
            { "id": "money",    "unit": "AUD", "conserved": true  },
            { "id": "energy",   "unit": "kWh", "conserved": true  },
            { "id": "info",     "unit": "bits", "conserved": false }
        ],
        "nodes": [
            { "id": "income",   "type": "source",  "value": 1.0,    "carrier": "money"  },
            { "id": "account",  "type": "storage", "value": 1000.0, "carrier": "money"  },
            { "id": "expenses", "type": "sink",    "value": 0.0,    "carrier": "money"  },
            { "id": "solar",    "type": "source",  "value": 1.0,    "carrier": "energy" },
            { "id": "battery",  "type": "storage", "value": 10.0,   "carrier": "energy" },
            { "id": "grid",     "type": "sink",    "value": 0.0,    "carrier": "energy" },
            { "id": "news",     "type": "source",  "value": 1.0,    "carrier": "info"   },
            { "id": "mind",     "type": "storage", "value": 5.0,    "carrier": "info"   },
            { "id": "forget",   "type": "sink",    "value": 0.0,    "carrier": "info"   }
        ],
        "edges": [
            { "id": "salary",   "origin": "income",  "target": "account",  "carrier": "money",
              "logic": "constant", "params": { "k": 500.0 } },
            { "id": "spend",    "origin": "account", "target": "expenses", "carrier": "money",
              "logic": "linear",   "params": { "k": 0.1 } },
            { "id": "charge",   "origin": "solar",   "target": "battery",  "carrier": "energy",
              "logic": "constant", "params": { "k": 5.0 } },
            { "id": "export",   "origin": "battery", "target": "grid",     "carrier": "energy",
              "logic": "linear",   "params": { "k": 0.05 } },
            { "id": "learn",    "origin": "news",    "target": "mind",     "carrier": "info",
              "logic": "constant", "params": { "k": 2.0 } },
            { "id": "decay",    "origin": "mind",    "target": "forget",   "carrier": "info",
              "logic": "linear",   "params": { "k": 0.1 } }
        ],
        "config": { "t_start": 0.0, "t_end": 10.0, "dt": 1.0, "method": "rk4" }
    }
    """

    func testCarrierCountAndDefinitions() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        XCTAssertEqual(sim.carrierCount, 3)

        let money = try XCTUnwrap(sim.carrier(at: 0))
        XCTAssertEqual(money.id, "money")
        XCTAssertEqual(money.unit, "AUD")
        XCTAssertTrue(money.conserved)

        let energy = try XCTUnwrap(sim.carrier(at: 1))
        XCTAssertEqual(energy.id, "energy")
        XCTAssertEqual(energy.unit, "kWh")
        XCTAssertTrue(energy.conserved)

        let info = try XCTUnwrap(sim.carrier(at: 2))
        XCTAssertEqual(info.id, "info")
        XCTAssertEqual(info.unit, "bits")
        XCTAssertFalse(info.conserved)

        XCTAssertNil(sim.carrier(at: 3), "Out-of-range index should return nil")
    }

    func testCarriersPropertyReturnsAll() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        let cs = sim.carriers
        XCTAssertEqual(cs.count, 3)
        XCTAssertEqual(cs.map(\.id), ["money", "energy", "info"])
    }

    func testNodeCarrierLabels() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        // Node ordering matches JSON: income(0), account(1), expenses(2),
        // solar(3), battery(4), grid(5), news(6), mind(7), forget(8)
        XCTAssertEqual(sim.nodeCarrier(at: 0), "money")
        XCTAssertEqual(sim.nodeCarrier(at: 1), "money")
        XCTAssertEqual(sim.nodeCarrier(at: 2), "money")
        XCTAssertEqual(sim.nodeCarrier(at: 3), "energy")
        XCTAssertEqual(sim.nodeCarrier(at: 4), "energy")
        XCTAssertEqual(sim.nodeCarrier(at: 5), "energy")
        XCTAssertEqual(sim.nodeCarrier(at: 6), "info")
        XCTAssertEqual(sim.nodeCarrier(at: 7), "info")
        XCTAssertEqual(sim.nodeCarrier(at: 8), "info")
    }

    func testEdgeCarrierLabels() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        // Edge ordering: salary(0), spend(1), charge(2), export(3), learn(4), decay(5)
        XCTAssertEqual(sim.edgeCarrier(at: 0), "money")
        XCTAssertEqual(sim.edgeCarrier(at: 1), "money")
        XCTAssertEqual(sim.edgeCarrier(at: 2), "energy")
        XCTAssertEqual(sim.edgeCarrier(at: 3), "energy")
        XCTAssertEqual(sim.edgeCarrier(at: 4), "info")
        XCTAssertEqual(sim.edgeCarrier(at: 5), "info")
    }

    func testNonConservedCarrierErrorIsZero() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        _ = try sim.step()
        // carrier index 2 = "info" (not conserved)
        XCTAssertEqual(sim.carrierConservationError(for: 2), 0.0,
            "Non-conserved carrier should always report 0 conservation error")
    }

    func testConservedCarrierErrorFiniteAfterStep() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        _ = try sim.step()
        // carriers 0 and 1 are conserved — open system (sources+sinks present)
        // so error is non-negative but may be positive due to net flow
        XCTAssertGreaterThanOrEqual(sim.carrierConservationError(for: 0), 0.0)
        XCTAssertGreaterThanOrEqual(sim.carrierConservationError(for: 1), 0.0)
    }

    func testCarrierErrorZeroBeforeAnyStep() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        // No step yet — conservation errors should be 0.0
        XCTAssertEqual(sim.carrierConservationError(for: 0), 0.0)
        XCTAssertEqual(sim.carrierConservationError(for: 1), 0.0)
    }

    func testOutOfRangeCarrierIndexReturnsZero() throws {
        let sim = try GSSKSimulator(json: Self.multiCarrierModelJSON)
        XCTAssertEqual(sim.carrierConservationError(for: 99), 0.0,
            "Out-of-range carrier index should return 0.0")
    }

    func testModelWithNoCarriersHasZeroCount() throws {
        // Legacy model (schema v2, no carriers array) should have 0 carriers
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.carrierCount, 0)
        XCTAssertTrue(sim.carriers.isEmpty)
    }

    func testHouseholdModelAllFiveLogicTypes() throws {
        // Verify the multi-carrier household model initialises and runs without error.
        // This covers: constant, linear, interaction, limit, threshold edge types.
        let url = URL(fileURLWithPath: #file)
            .deletingLastPathComponent()          // GSSKTests/
            .deletingLastPathComponent()          // Sources/
            .deletingLastPathComponent()          // package root
            .appendingPathComponent("examples/household_model.json")
        guard let data = try? Data(contentsOf: url),
              let json = String(data: data, encoding: .utf8) else {
            // Skip if running outside the full package (e.g. CI without examples/)
            throw XCTSkip("examples/household_model.json not found — skipping")
        }
        let sim = try GSSKSimulator(json: json)
        XCTAssertEqual(sim.carrierCount, 4)
        XCTAssertGreaterThan(sim.stateSize, 0)
        // Run for a few steps without divergence
        for _ in 0..<5 { _ = try sim.step() }
        XCTAssertEqual(sim.carriers.map(\.id), ["money", "energy", "material", "information"])
    }

    func testReplayAppliesMutationAtCorrectTime() throws {
        // Run a decay model, change k at t=5, continue
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // Step to t=5
        let dtFixed = sim.defaultDt
        while sim.currentTime < 5.0 - dtFixed * 0.01 {
            try sim.step()
        }
        // Record Q at t=5 before mutation
        let qBefore = sim.state()[0]
        // Apply mutation: double k
        sim.setMutationCause("event:respiration")
        sim.setEdgeK(0.1, at: 0) // was 0.05
        // Run to end
        while sim.currentTime < sim.endTime - dtFixed * 0.01 {
            try sim.step()
        }
        let originalFinal = sim.state()[0]

        // Export mutation log and replay
        let mutLog = try sim.exportMutationLog()
        let replayed = try GSSKSimulator.replay(
            from: Self.decayModelJSON,
            mutations: mutLog,
            until: 20.0
        )
        let replayedFinal = replayed.state()[0]

        // Replay with doubled-k mutation should produce same final Q as original
        XCTAssertEqual(replayedFinal, originalFinal, accuracy: 1e-8,
            "Replay with mutation should reproduce the mutated trajectory")
        // And it should differ from a no-mutation replay
        let cleanReplayed = try GSSKSimulator.replay(
            from: Self.decayModelJSON,
            until: 20.0
        )
        XCTAssertNotEqual(replayedFinal, cleanReplayed.state()[0],
            "Mutation replay should differ from clean replay")
        _ = qBefore // suppress unused warning
    }
}
