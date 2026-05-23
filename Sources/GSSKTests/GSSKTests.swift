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

        XCTAssertEqual(sim.solverConfidence, GSSK_CONFIDENCE_HIGH)
    }
}
