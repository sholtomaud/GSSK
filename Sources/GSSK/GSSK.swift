import CGSSK
import Foundation

// MARK: - Schema version

/// The schema version this wrapper was built against.
/// A serialiser must embed this in the JSON `metadata.schema_version` field.
/// The kernel does not currently enforce this at the C level, but
/// `GSSKSimulator.init(json:)` will reject mismatches to catch drift early.
public let GSSKSchemaVersion: Int = 1

// MARK: - Serialiser contract

/// The output type every domain serialiser must produce.
///
/// The `nodeManifest` maps each state-vector column index to the stable
/// domain node identifier that was serialised into that position. This is
/// the only reliable way to interpret `[[Double]]` results.
///
/// Column ordering matches the `nodes` array order in `json` exactly —
/// see `docs/gssk-schema.md` for the full contract.
public struct GSSKSerialiserOutput {
    /// The GSSK-schema-conforming JSON ready to pass to `GSSKSimulator.init`.
    public let json: Data

    /// Maps state-vector column index → stable domain node ID.
    /// e.g. `[0: "account_abc123", 1: "groceries_def456"]`
    public let nodeManifest: [Int: String]

    public init(json: Data, nodeManifest: [Int: String]) {
        self.json = json
        self.nodeManifest = nodeManifest
    }
}

// MARK: - Error type

/// Swift representation of GSSK_Status error codes.
public enum GSSKError: Error, LocalizedError {
    case invalidJSON
    case mallocFailed
    case schemaViolation(String)
    case divergence
    case unknown
    case noInstance
    /// The JSON `metadata.schema_version` does not match `GSSKSchemaVersion`.
    case schemaMismatch(found: Int, expected: Int)

    public var errorDescription: String? {
        switch self {
        case .invalidJSON:
            return "GSSK: Invalid JSON input."
        case .mallocFailed:
            return "GSSK: Memory allocation failed."
        case .schemaViolation(let msg):
            return "GSSK: Schema violation — \(msg)"
        case .divergence:
            return "GSSK: Numerical divergence (NaN/Inf detected)."
        case .unknown:
            return "GSSK: Unknown error."
        case .noInstance:
            return "GSSK: No active instance."
        case .schemaMismatch(let found, let expected):
            return "GSSK: schema_version mismatch — JSON has \(found), wrapper expects \(expected)."
        }
    }
}

// MARK: - Simulator

/// A high-level Swift wrapper around the GSSK ODE simulation kernel.
///
/// Manages the lifecycle of a `GSSK_Instance` (init/free) automatically via
/// `init`/`deinit`. All C pointer casting is handled internally.
///
/// ## Typical usage
/// ```swift
/// // 1. Serialise your domain model
/// let output = try MySerializer().serialise(model)
///
/// // 2. Initialise the simulator
/// let sim = try GSSKSimulator(serialiserOutput: output)
///
/// // 3. Run
/// let timeSeries = try sim.run()
///
/// // 4. Map results back to domain IDs
/// for (step, state) in timeSeries.enumerated() {
///     for (col, value) in state.enumerated() {
///         let nodeID = output.nodeManifest[col] ?? "unknown"
///         print("t=\(step) \(nodeID) = \(value)")
///     }
/// }
/// ```
public final class GSSKSimulator {

    // MARK: - Private storage

    // GSSK_Instance is an incomplete C struct — Swift imports it as OpaquePointer.
    private var instPtr: OpaquePointer?

    /// The node manifest provided at init time (column → domain node ID).
    /// If the simulator was initialised directly from JSON (not a serialiser),
    /// this is built from `GSSK_GetNodeID` after a successful `GSSK_Init`.
    public private(set) var nodeManifest: [Int: String] = [:]

    // MARK: - Public properties

    /// Number of nodes tracked in the state vector (all types, not just storage).
    public var stateSize: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetStateSize(p))
    }

    /// Simulation start time.
    public var startTime: Double {
        guard let p = instPtr else { return 0 }
        return GSSK_GetTStart(p)
    }

    /// Simulation end time.
    public var endTime: Double {
        guard let p = instPtr else { return 0 }
        return GSSK_GetTEnd(p)
    }

    /// Default time step from the model definition.
    public var defaultDt: Double {
        guard let p = instPtr else { return 0 }
        return GSSK_GetDt(p)
    }

    /// Current simulation time (advances with each call to `step()`).
    public private(set) var currentTime: Double = 0

    // MARK: - Initialisation

    /// Preferred initialiser — accepts the structured output of a domain serialiser.
    ///
    /// Validates `metadata.schema_version` before forwarding to the kernel,
    /// and stores the provided `nodeManifest` for result mapping.
    ///
    /// - Parameter serialiserOutput: The output of a `GSSKModelSerializer`.
    /// - Throws: `GSSKError.schemaMismatch` if the JSON version tag doesn't match,
    ///           or any `GSSKError` if the kernel rejects the model.
    public convenience init(serialiserOutput output: GSSKSerialiserOutput) throws {
        // Check schema version before sending to kernel
        if let jsonObject = try? JSONSerialization.jsonObject(with: output.json) as? [String: Any],
           let metadata = jsonObject["metadata"] as? [String: Any],
           let version = metadata["schema_version"] as? Int {
            guard version == GSSKSchemaVersion else {
                throw GSSKError.schemaMismatch(found: version, expected: GSSKSchemaVersion)
            }
        }

        guard let jsonString = String(data: output.json, encoding: .utf8) else {
            throw GSSKError.invalidJSON
        }
        try self.init(json: jsonString)
        self.nodeManifest = output.nodeManifest
    }

    /// Low-level initialiser — accepts a raw JSON string.
    ///
    /// Builds `nodeManifest` from the kernel's node ordering after a successful
    /// init. Prefer `init(serialiserOutput:)` for production code.
    ///
    /// - Parameter json: The GSSK model topology JSON.
    /// - Throws: `GSSKError` describing any parse or schema failure.
    public init(json: String) throws {
        var rawPtr: OpaquePointer? = nil

        let status = json.withCString { cStr -> GSSK_Status in
            return GSSK_Init(cStr, &rawPtr)
        }

        self.instPtr = rawPtr

        guard status == GSSK_SUCCESS else {
            let msg: String
            if let p = rawPtr, let cStr = GSSK_GetErrorDescription(p) {
                msg = String(cString: cStr)
            } else {
                msg = ""
            }
            GSSK_Free(rawPtr)
            self.instPtr = nil
            throw Self.map(status: status, message: msg)
        }

        self.currentTime = startTime

        // Build manifest from kernel ordering
        for i in 0 ..< stateSize {
            if let id = nodeID(at: i) {
                nodeManifest[i] = id
            }
        }
    }

    deinit {
        GSSK_Free(instPtr)
    }

    // MARK: - Simulation

    /// Advance the simulation by one time step.
    /// - Parameter dt: Time step override. Defaults to `defaultDt`.
    /// - Returns: Current state vector as `[Double]`.
    /// - Throws: `GSSKError.divergence` if NaN/Inf is detected.
    @discardableResult
    public func step(dt: Double? = nil) throws -> [Double] {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let stepDt = dt ?? defaultDt
        let status = GSSK_Step(p, stepDt)
        guard status == GSSK_SUCCESS else {
            throw Self.map(status: status, message: "")
        }
        currentTime += stepDt
        return state()
    }

    /// Run the simulation to completion, returning all state vectors.
    /// - Parameter dt: Time step override. Defaults to `defaultDt`.
    /// - Returns: Array of `[Double]` state snapshots, one per step.
    /// - Throws: `GSSKError` on numerical failure.
    public func run(dt: Double? = nil) throws -> [[Double]] {
        reset()
        var results: [[Double]] = []
        let stepDt = dt ?? defaultDt
        while currentTime < endTime {
            results.append(try step(dt: stepDt))
        }
        return results
    }

    /// Run and return results keyed by node ID rather than column index.
    ///
    /// This is the preferred way to consume results in application code —
    /// it is immune to column-order changes caused by serialiser updates.
    ///
    /// - Returns: Dictionary mapping node ID → time series of Q values.
    /// - Throws: `GSSKError` on numerical failure.
    public func runNamed(dt: Double? = nil) throws -> [String: [Double]] {
        let raw = try run(dt: dt)
        var result: [String: [Double]] = [:]
        for (col, nodeID) in nodeManifest {
            result[nodeID] = raw.map { $0[col] }
        }
        return result
    }

    /// Reset simulation to initial conditions.
    public func reset() {
        guard let p = instPtr else { return }
        GSSK_Reset(p)
        currentTime = startTime
    }

    /// Read the current state vector (Q values for all nodes).
    public func state() -> [Double] {
        guard let p = instPtr,
              let ptr = GSSK_GetState(p) else { return [] }
        return Array(UnsafeBufferPointer(start: ptr, count: stateSize))
    }

    /// Read the current state keyed by node ID.
    public func namedState() -> [String: Double] {
        let s = state()
        var result: [String: Double] = [:]
        for (col, nodeID) in nodeManifest {
            result[nodeID] = s[col]
        }
        return result
    }

    // MARK: - Node access

    /// Return the ID string for the node at `index` (kernel ordering).
    public func nodeID(at index: Int) -> String? {
        guard let p = instPtr,
              let cStr = GSSK_GetNodeID(p, index) else { return nil }
        return String(cString: cStr)
    }

    /// Find the kernel column index of a node by its string ID.
    /// Returns `nil` if not found.
    public func nodeIndex(id: String) -> Int? {
        guard let p = instPtr else { return nil }
        let idx = id.withCString { GSSK_FindNodeIdx(p, $0) }
        return idx >= 0 ? Int(idx) : nil
    }

    // MARK: - Edge access

    /// Number of edges (flows) in the model.
    public var edgeCount: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetEdgeCount(p))
    }

    /// Get the coefficient `k` for edge at `index`.
    public func edgeK(at index: Int) -> Double {
        guard let p = instPtr else { return 0 }
        return GSSK_GetEdgeK(p, index)
    }

    /// Set the coefficient `k` for edge at `index`.
    /// Use this to implement time-varying flow rates between steps.
    public func setEdgeK(_ k: Double, at index: Int) {
        guard let p = instPtr else { return }
        GSSK_SetEdgeK(p, index, k)
    }

    // MARK: - Private helpers

    private static func map(status: GSSK_Status, message: String) -> GSSKError {
        switch status {
        case GSSK_ERR_INVALID_JSON:     return .invalidJSON
        case GSSK_ERR_MALLOC_FAILED:    return .mallocFailed
        case GSSK_ERR_SCHEMA_VIOLATION: return .schemaViolation(message)
        case GSSK_ERR_DIVERGENCE:       return .divergence
        default:                        return .unknown
        }
    }
}
