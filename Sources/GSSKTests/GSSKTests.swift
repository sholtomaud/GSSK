import XCTest
@testable import GSSK

/// Smoke tests for the Swift wrapper over the GSSK C kernel.
final class GSSKTests: XCTestCase {

    // MARK: - Test fixtures

    static let decayModelJSON = """
    {
        "nodes": [
            { "id": "biomass",     "type": "storage", "value": 100.0 },
            { "id": "environment", "type": "sink",    "value": 0.0   }
        ],
        "edges": [
            {
                "id": "respiration",
                "origin": "biomass",
                "target": "environment",
                "logic": "linear",
                "params": { "k": 0.05 }
            }
        ],
        "config": {
            "t_start": 0.0,
            "t_end": 20.0,
            "dt": 0.5,
            "method": "rk4"
        }
    }
    """

    // MARK: - Initialisation tests

    func testDecayModelInitialises() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        // The kernel tracks all nodes (storage + sink) in the state vector.
        // decay_model has 2 nodes: biomass (storage) + environment (sink).
        XCTAssertEqual(sim.stateSize, 2)
        XCTAssertEqual(sim.startTime, 0.0)
        XCTAssertEqual(sim.endTime, 20.0)
        XCTAssertEqual(sim.defaultDt, 0.5)
    }

    func testDecayModelNodeLookup() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        XCTAssertEqual(sim.nodeID(at: 0), "biomass")
        XCTAssertEqual(sim.nodeIndex(id: "biomass"), 0)
        XCTAssertNil(sim.nodeIndex(id: "nonexistent"))
    }

    // MARK: - State tests

    func testDecayModelInitialState() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let s = sim.state()
        XCTAssertEqual(s.count, 2)          // biomass + environment
        XCTAssertEqual(s[0], 100.0, accuracy: 1e-9)  // biomass initial value
        XCTAssertEqual(s[1], 0.0,   accuracy: 1e-9)  // environment initial value
    }

    func testDecayModelSingleStep() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let s = try sim.step()
        // After one RK4 step of dt=0.5 with k=0.05:
        // Exact: Q(0.5) = 100 * exp(-0.05 * 0.5) ≈ 97.531
        XCTAssertEqual(s[0], 100.0 * exp(-0.05 * 0.5), accuracy: 1e-3, "biomass should decay")
    }

    func testDecayModelRunToCompletion() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        let results = try sim.run()
        // 20 / 0.5 = 40 steps expected
        XCTAssertEqual(results.count, 40)
        // Final state: Q(20) = 100 * exp(-0.05 * 20) ≈ 36.788
        let finalBiomass = results.last![0]
        XCTAssertEqual(finalBiomass, 100.0 * exp(-0.05 * 20.0), accuracy: 1e-3)
    }

    func testDecayModelReset() throws {
        let sim = try GSSKSimulator(json: Self.decayModelJSON)
        _ = try sim.run()
        sim.reset()
        XCTAssertEqual(sim.currentTime, 0.0)
        let s = sim.state()
        XCTAssertEqual(s[0], 100.0, accuracy: 1e-9, "biomass should be restored")
        XCTAssertEqual(s[1], 0.0,   accuracy: 1e-9, "environment should be restored")
    }

    // MARK: - Error-path tests

    func testInvalidJSONThrows() {
        XCTAssertThrowsError(try GSSKSimulator(json: "not json at all")) { error in
            guard case GSSKError.invalidJSON = error else {
                return XCTFail("Expected GSSKError.invalidJSON, got \(error)")
            }
        }
    }

    func testMissingNodesKeyThrows() {
        // Completely wrong schema — should fail at JSON parse or schema level.
        XCTAssertThrowsError(try GSSKSimulator(json: "{}"))
    }
}
