import CGSSK
import Foundation

// MARK: - Schema version

/// The schema version this wrapper was built against.
/// A serialiser must embed this in the JSON `metadata.schema_version` field.
/// The kernel accepts v2 (with a deprecation warning) and v3 (current).
public let GSSKSchemaVersion: Int = 3

/// Programmatic access to the current schema version.
public let GSSKCurrentSchemaVersion: Int = GSSKSchemaVersion

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

// MARK: - Solver Confidence

/// Dual-solver confidence level (AUTO mode).
public enum GSSKSolverConfidence {
    /// IDC and RK4 agree within tolerance.
    case high
    /// Solvers have diverged — RK4 result was used.
    case degraded
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
    case solverDivergence
    case notFound
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
        case .solverDivergence:
            return "GSSK: IDC vs RK4 solver divergence (exceeds tolerance)."
        case .notFound:
            return "GSSK: Node or edge ID not found."
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
    var instPtr: OpaquePointer?  /* internal — accessible to @testable imports */

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

    /// Current simulation time — reads directly from the kernel clock.
    /// After loading a snapshot JSON this correctly reflects snapshot.t.
    public var currentTime: Double {
        guard let p = instPtr else { return 0 }
        return GSSK_GetCurrentTime(p)
    }

    /// Number of steps taken since the last init or reset.
    public var stepCount: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetStepCount(p))
    }

    /// Current dual-solver confidence level (AUTO mode).
    /// Always `.high` in EULER/RK4/INCIPIENT modes.
    public var solverConfidence: GSSKSolverConfidence {
        guard let p = instPtr else { return .high }
        return GSSK_GetSolverConfidence(p) == GSSK_CONFIDENCE_HIGH ? .high : .degraded
    }

    // MARK: - Metadata (v3)

    /// Schema version of the loaded model (2 or 3).
    public var schemaVersion: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetSchemaVersion(p))
    }

    /// Model name from the `metadata.name` field.
    public var modelName: String {
        guard let p = instPtr, let cStr = GSSK_GetModelName(p) else { return "" }
        return String(cString: cStr)
    }

    /// Model description from `metadata.description`.
    public var modelDescription: String {
        guard let p = instPtr, let cStr = GSSK_GetModelDescription(p) else { return "" }
        return String(cString: cStr)
    }

    /// Kernel version string that created this model (e.g. "3.0.0").
    public var modelKernelVersion: String {
        guard let p = instPtr, let cStr = GSSK_GetModelKernelVersion(p) else { return "" }
        return String(cString: cStr)
    }

    /// SHA256 model hash from `metadata.model_hash`.
    public var modelHash: String {
        guard let p = instPtr, let cStr = GSSK_GetModelHash(p) else { return "" }
        return String(cString: cStr)
    }

    /// Current GSK kernel version string (e.g. "3.0.0").
    public static var kernelVersion: String {
        guard let cStr = GSSK_GetVersionString() else { return "" }
        return String(cString: cStr)
    }

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
            // Accept v2 (kernel auto-migrates with warning) and v3. Reject anything else.
            guard version == 2 || version == GSSKSchemaVersion else {
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
    /// - Throws: `GSSKError.divergence` if NaN/Inf is detected, or `GSSKError.solverDivergence` in AUTO mode.
    @discardableResult
    public func step(dt: Double? = nil) throws -> [Double] {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let stepDt = dt ?? defaultDt
        let status = GSSK_Step(p, stepDt)
        // WARN_SOLVER_DIVERGENCE is handled as success because the kernel
        // automatically falls back to RK4 and continues.
        guard status == GSSK_SUCCESS || status == GSSK_WARN_SOLVER_DIVERGENCE else {
            throw Self.map(status: status, message: "")
        }
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

    // MARK: - Quality Accounting

    /// Transformation ratio (Tr) per node. Only non-empty if quality accounting is enabled.
    public func transformationRatios() -> [Double] {
        guard let p = instPtr,
              let ptr = GSSK_GetTransformationRatio(p) else { return [] }
        return Array(UnsafeBufferPointer(start: ptr, count: stateSize))
    }

    /// Transformation ratio keyed by node ID.
    public func namedTransformationRatios() -> [String: Double] {
        let tr = transformationRatios()
        guard !tr.isEmpty else { return [:] }
        var result: [String: Double] = [:]
        for (col, nodeID) in nodeManifest {
            result[nodeID] = tr[col]
        }
        return result
    }

    /// Quality flow (Tr × flow rate) summed per node.
    public func qualityFlows() -> [Double] {
        guard let p = instPtr,
              let ptr = GSSK_GetQualityFlow(p) else { return [] }
        return Array(UnsafeBufferPointer(start: ptr, count: stateSize))
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

    /// Get the stable ID for the edge at `index`.
    public func edgeID(at index: Int) -> String? {
        guard let p = instPtr,
              let cStr = GSSK_GetEdgeID(p, index) else { return nil }
        return String(cString: cStr)
    }

    /// Find the kernel index of an edge by its string ID.
    public func edgeIndex(id: String) -> Int? {
        guard let p = instPtr else { return nil }
        let idx = id.withCString { GSSK_FindEdgeIdx(p, $0) }
        return idx >= 0 ? Int(idx) : nil
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

    /// Quality flow (Tr × flow) on a specific edge.
    public func edgeQualityFlow(at index: Int) -> Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetEdgeQualityFlow(p, index)
    }

    // MARK: - Topology Mutation

    /// Add a new node at runtime.
    ///
    /// - Parameter json: JSON object conforming to the Node schema.
    /// - Throws: `GSSKError` if the node is invalid or ID is duplicate.
    public func addNode(json: String) throws {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let status = json.withCString { GSSK_AddNode(p, $0) }
        guard status == GSSK_SUCCESS else {
            throw Self.map(status: status, message: "AddNode failed")
        }

        // Refresh manifest for the new node
        let newIdx = stateSize - 1
        if let id = nodeID(at: newIdx) {
            nodeManifest[newIdx] = id
        }
    }

    /// Add a new edge at runtime.
    ///
    /// - Parameter json: JSON object conforming to the Edge schema.
    /// - Throws: `GSSKError` if the edge is invalid or linkage fails.
    public func addEdge(json: String) throws {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let status = json.withCString { GSSK_AddEdge(p, $0) }
        guard status == GSSK_SUCCESS else {
            throw Self.map(status: status, message: "AddEdge failed")
        }
    }

    /// Deactivate an edge by ID (sets k=0, marks inactive).
    public func deactivateEdge(id: String) throws {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let status = id.withCString { GSSK_DeactivateEdge(p, $0) }
        guard status == GSSK_SUCCESS else {
            throw Self.map(status: status, message: "DeactivateEdge failed")
        }
    }

    /// Deactivate a node by ID (zeroes all connected edges).
    public func deactivateNode(id: String) throws {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let status = id.withCString { GSSK_DeactivateNode(p, $0) }
        guard status == GSSK_SUCCESS else {
            throw Self.map(status: status, message: "DeactivateNode failed")
        }
    }

    /// Manually trigger network reclassification (e.g. after changing k values).
    public func reclassify() {
        guard let p = instPtr else { return }
        GSSK_ReclassifyNetwork(p)
    }

    // MARK: - Phase 1 — IDC error estimates and event log

    /// Per-edge relative error between IDC and RK4 flows from the last step.
    /// Returns 0.0 in EULER/RK4 modes or if index is out of range.
    public func edgeErrorEstimate(at index: Int) -> Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetEdgeErrorEstimate(p, index)
    }

    /// Step-level max error: max over all edges of `edgeErrorEstimate(at:)`.
    /// Used by the kernel to decide between IDC and RK4 results.
    public var stepErrorEstimate: Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetStepErrorEstimate(p)
    }

    /// Number of threshold crossing events recorded since last `reset()`.
    public var eventCount: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetEventCount(p))
    }

    /// A threshold crossing event.
    public struct GSSKEvent {
        /// Simulation time when the crossing occurred.
        public let t: Double
        /// ID of the threshold edge that crossed.
        public let edgeID: String
        /// +1 if Q_origin crossed threshold upward, -1 downward.
        public let direction: Int
    }

    /// Return the event at `index` in the event log, or nil if out of range.
    public func event(at index: Int) -> GSSKEvent? {
        guard let p = instPtr, index < eventCount else { return nil }
        let t   = GSSK_GetEventTime(p, index)
        let dir = Int(GSSK_GetEventDirection(p, index))
        guard let cStr = GSSK_GetEventEdgeID(p, index) else { return nil }
        return GSSKEvent(t: t, edgeID: String(cString: cStr), direction: dir)
    }

    // MARK: - Phase 2 — Adaptive Numerics

    /// Advance the simulation by one adaptively-sized DOPRI5 step.
    ///
    /// Uses the internally managed step size (`nextStepSize`, initialised from
    /// `config.dt`).  Updates `currentTime` by the accepted `lastStepSize`.
    /// Only meaningful when method = "adaptive"; for other methods behaves
    /// identically to `step()` using `config.dt`.
    ///
    /// - Returns: Array of node state values after the step.
    /// - Throws: `GSSKError` on NaN/Inf divergence.
    @discardableResult
    public func stepAdaptive() throws -> [Double] {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let status = GSSK_StepAdaptive(p)
        if status == GSSK_ERR_DIVERGENCE {
            throw GSSKError.divergence
        }
        return Array(UnsafeBufferPointer(start: GSSK_GetState(p),
                                         count: Int(GSSK_GetStateSize(p))))
    }

    /// Actual step size h used in the most recently accepted DOPRI5 step.
    /// For fixed-step methods this equals `config.dt`.
    public var lastStepSize: Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetLastStepSize(p)
    }

    /// Suggested h for the next `stepAdaptive()` call, updated by the PI
    /// controller after every accepted DOPRI5 step.
    public var nextStepSize: Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetNextStepSize(p)
    }

    /// Relative change in total storage-Q over the last step.
    /// Near zero for a fully closed system (no source/sink nodes).
    public var conservationError: Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetConservationError(p)
    }

    // MARK: - Phase 3 — Sensitivity Analysis

    /// Enable forward sensitivity tracking for a set of edges (by index).
    ///
    /// After this call, every `step()` / `stepAdaptive()` updates the internal
    /// sensitivity matrix S where S[nodeIdx][paramIdx] = ∂Q_nodeIdx/∂k_j.
    /// - Parameter paramEdgeIndices: Edge indices to track (0-based).
    @discardableResult
    public func enableForwardSensitivity(paramEdgeIndices: [Int]) throws -> GSSK_Status {
        guard let p = instPtr else { throw GSSKError.noInstance }
        let idx = paramEdgeIndices.map { Int($0) }
        return idx.withUnsafeBufferPointer { buf in
            GSSK_EnableForwardSensitivity(p, buf.baseAddress, idx.count)
        }
    }

    /// Disable forward sensitivity tracking and free the sensitivity matrix.
    public func disableForwardSensitivity() {
        guard let p = instPtr else { return }
        GSSK_DisableForwardSensitivity(p)
    }

    /// Read ∂Q[nodeIdx] / ∂k[paramIdx] from the sensitivity matrix.
    /// `paramIdx` is the column index into the array passed to
    /// `enableForwardSensitivity`, not the raw edge index.
    public func getSensitivity(nodeIdx: Int, paramIdx: Int) -> Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetSensitivity(p, nodeIdx, paramIdx)
    }

    /// Compute gradient ∂L/∂k via adjoint (backward) integration.
    ///
    /// Objective: L = ½ Σ_i weight_i · (Q_i(T) − target_i)².
    /// Runs a fresh forward pass from t_start→t_end, then integrates backward.
    /// Restores live state on return.
    ///
    /// - Parameters:
    ///   - targets: Terminal objective terms as `(nodeIdx, targetValue, weight)`.
    ///   - paramEdgeIndices: Edges whose k gradient is returned.
    /// - Returns: Gradient vector ∂L/∂k_j for each j in `paramEdgeIndices`.
    public func runAdjoint(
        targets: [(nodeIdx: Int, targetValue: Double, weight: Double)],
        paramEdgeIndices: [Int]
    ) throws -> [Double] {
        guard let p = instPtr else { throw GSSKError.noInstance }
        var tgts = targets.map {
            GSSK_AdjointTarget(node_idx: $0.nodeIdx,
                               target_value: $0.targetValue,
                               weight: $0.weight)
        }
        let idx  = paramEdgeIndices.map { Int($0) }
        var grad = [Double](repeating: 0.0, count: paramEdgeIndices.count)
        let tgtCount = tgts.count
        let st: GSSK_Status = tgts.withUnsafeMutableBufferPointer { tBuf in
            idx.withUnsafeBufferPointer { iBuf in
                GSSK_RunAdjoint(p, tBuf.baseAddress, tgtCount,
                                iBuf.baseAddress, idx.count, &grad)
            }
        }
        if st == GSSK_ERR_MALLOC_FAILED { throw GSSKError.mallocFailed }
        return grad
    }

    /// ∂Tr[nodeIdx] / ∂k[edgeIdx] via implicit differentiation of the quality
    /// accounting system. Returns 0.0 if quality accounting is disabled.
    public func getTransformitySensitivity(nodeIdx: Int, edgeIdx: Int) -> Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetTransformitySensitivity(p, nodeIdx, edgeIdx)
    }

    // MARK: - Serialization (0.3 Round-trip)

    /// Serialize the current topology to a JSON string.
    ///
    /// The result is a valid model JSON with initial_value ICs and the current
    /// edge k values. Passing it back to `init(json:)` starts a fresh run from
    /// `t_start` with the current (possibly cybernetically adjusted) parameters.
    ///
    /// - Returns: Pretty-printed JSON string.
    /// - Throws: `GSSKError.mallocFailed` if the kernel runs out of memory.
    public func serializeModel() throws -> String {
        guard let p = instPtr else { throw GSSKError.noInstance }
        var ptr: UnsafeMutablePointer<CChar>? = nil
        let status = GSSK_SerializeModel(p, &ptr)
        guard status == GSSK_SUCCESS, let cStr = ptr else {
            throw Self.map(status: status, message: "SerializeModel failed")
        }
        let result = String(cString: cStr)
        GSSK_FreeString(ptr)
        return result
    }

    /// Serialize the current topology and live state to a JSON string.
    ///
    /// The result includes a `snapshot` block with the current Q[], Tr[],
    /// per-edge k, simulation time, and step counter. Passing it back to
    /// `init(json:)` resumes from `t = snapshot.t`, satisfying the round-trip
    /// property: `Init → Step×N → serializeSnapshot → init → Step×M` is
    /// bit-identical to `Init → Step×(N+M)`.
    ///
    /// - Returns: Pretty-printed JSON string.
    /// - Throws: `GSSKError.mallocFailed` if the kernel runs out of memory.
    public func serializeSnapshot() throws -> String {
        guard let p = instPtr else { throw GSSKError.noInstance }
        var ptr: UnsafeMutablePointer<CChar>? = nil
        let status = GSSK_SerializeSnapshot(p, &ptr)
        guard status == GSSK_SUCCESS, let cStr = ptr else {
            throw Self.map(status: status, message: "SerializeSnapshot failed")
        }
        let result = String(cString: cStr)
        GSSK_FreeString(ptr)
        return result
    }

    // MARK: - Phase 4 — Mutation Log & Replay

    /// A recorded topology mutation.
    public struct GSSKMutation {
        public let t: Double
        public let op: String
        public let targetID: String
        public let payload: String
        public let cause: String
    }

    /// Number of mutations recorded since `init` (not cleared by `reset()`).
    public var mutationCount: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetMutationCount(p))
    }

    /// Read the mutation record at `index`, or nil if out of range.
    public func mutation(at index: Int) -> GSSKMutation? {
        guard let p = instPtr else { return nil }
        guard let r = GSSK_GetMutationRecord(p, index) else { return nil }
        let op: String
        switch r.pointee.op {
        case GSSK_MUT_ADD_NODE:         op = "add_node"
        case GSSK_MUT_ADD_EDGE:         op = "add_edge"
        case GSSK_MUT_DEACTIVATE_EDGE:  op = "deactivate_edge"
        case GSSK_MUT_DEACTIVATE_NODE:  op = "deactivate_node"
        default:                        op = "set_edge_k"
        }
        return GSSKMutation(
            t:        r.pointee.t,
            op:       op,
            targetID: withUnsafeBytes(of: r.pointee.target_id) {
                          String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
                      },
            payload:  withUnsafeBytes(of: r.pointee.payload) {
                          String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
                      },
            cause:    withUnsafeBytes(of: r.pointee.cause) {
                          String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
                      }
        )
    }

    /// All mutations in the log.
    public var mutations: [GSSKMutation] {
        (0 ..< mutationCount).compactMap { mutation(at: $0) }
    }

    /// Set the cause string for the next appended mutation.
    /// Automatically cleared after one mutation is appended.
    public func setMutationCause(_ cause: String) {
        guard let p = instPtr else { return }
        cause.withCString { GSSK_SetMutationCause(p, $0) }
    }

    /// Remove all entries from the mutation log (capacity is retained).
    public func clearMutationLog() {
        guard let p = instPtr else { return }
        GSSK_ClearMutationLog(p)
    }

    /// Serialize the mutation log as a JSON array string.
    /// - Throws: `GSSKError.mallocFailed` on OOM.
    public func exportMutationLog() throws -> String {
        guard let p = instPtr else { throw GSSKError.noInstance }
        var ptr: UnsafeMutablePointer<CChar>? = nil
        let status = GSSK_ExportMutationLog(p, &ptr)
        guard status == GSSK_SUCCESS, let cStr = ptr else {
            throw Self.map(status: status, message: "ExportMutationLog failed")
        }
        let result = String(cString: cStr)
        GSSK_FreeString(ptr)
        return result
    }

    /// Replay a simulation from `initialJSON`, applying mutations at their recorded times.
    ///
    /// - Parameters:
    ///   - initialJSON: Topology-only model JSON (no snapshot block).
    ///   - mutationsJSON: JSON array string from `exportMutationLog()`, or nil.
    ///   - targetT: Stop time. Use the model's `t_end` for a full replay.
    /// - Returns: New `GSSKSimulator` at state `targetT`.
    /// - Throws: `GSSKError` on parse or divergence failure.
    public static func replay(
        from initialJSON: String,
        mutations mutationsJSON: String? = nil,
        until targetT: Double
    ) throws -> GSSKSimulator {
        var rawPtr: OpaquePointer? = nil
        let muts = mutationsJSON ?? ""
        let status: GSSK_Status = initialJSON.withCString { ij in
            muts.withCString { mj in
                GSSK_Replay(ij, mj, targetT, &rawPtr)
            }
        }
        guard let ptr = rawPtr else { throw GSSKError.mallocFailed }
        guard status == GSSK_SUCCESS || status == GSSK_WARN_SOLVER_DIVERGENCE else {
            let msg = String(cString: GSSK_GetErrorDescription(ptr)!)
            GSSK_Free(ptr)
            throw Self.map(status: status, message: msg)
        }
        return GSSKSimulator(raw: ptr)
    }

    /// Internal initialiser that wraps an already-owned `GSSK_Instance*`.
    private init(raw ptr: OpaquePointer) {
        self.instPtr = ptr
        self.nodeManifest = [:]
        let n = Int(GSSK_GetStateSize(ptr))
        for i in 0 ..< n {
            if let cStr = GSSK_GetNodeID(ptr, i) {
                nodeManifest[i] = String(cString: cStr)
            }
        }
    }

    // MARK: - Phase 5 — Multi-Carrier Schema

    /// A carrier definition from the top-level `carriers` array.
    public struct GSSKCarrier {
        /// Unique identifier, e.g. "money", "energy", "material".
        public let id: String
        /// Physical unit string, e.g. "AUD", "kWh", "kg".
        public let unit: String
        /// If true, total storage-Q for this carrier is tracked per step.
        public let conserved: Bool
    }

    /// Number of carriers declared in the model's top-level `carriers` array.
    public var carrierCount: Int {
        guard let p = instPtr else { return 0 }
        return Int(GSSK_GetCarrierCount(p))
    }

    /// Return the carrier definition at `index`, or nil if out of range.
    public func carrier(at index: Int) -> GSSKCarrier? {
        guard let p = instPtr,
              let r = GSSK_GetCarrier(p, index) else { return nil }
        return GSSKCarrier(
            id:        withUnsafeBytes(of: r.pointee.id)   { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) },
            unit:      withUnsafeBytes(of: r.pointee.unit) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) },
            conserved: r.pointee.conserved
        )
    }

    /// All carriers declared in the model.
    public var carriers: [GSSKCarrier] {
        (0 ..< carrierCount).compactMap { carrier(at: $0) }
    }

    /// Carrier string declared on the node at `nodeIndex`.
    /// Returns an empty string if no carrier is set on that node.
    public func nodeCarrier(at nodeIndex: Int) -> String {
        guard let p = instPtr,
              let cStr = GSSK_GetNodeCarrier(p, nodeIndex) else { return "" }
        return String(cString: cStr)
    }

    /// Carrier string declared on the edge at `edgeIndex` (Odum Position 1).
    /// Returns an empty string if no carrier is set on that edge.
    public func edgeCarrier(at edgeIndex: Int) -> String {
        guard let p = instPtr,
              let cStr = GSSK_GetEdgeCarrier(p, edgeIndex) else { return "" }
        return String(cString: cStr)
    }

    /// Per-carrier conservation error from the last step (relative change in
    /// total storage-Q for all nodes matching `carriers[carrierIndex].id`).
    /// Returns 0.0 if the carrier is not conserved or no step has been taken.
    public func carrierConservationError(for carrierIndex: Int) -> Double {
        guard let p = instPtr else { return 0.0 }
        return GSSK_GetCarrierConservationError(p, carrierIndex)
    }

    // MARK: - Private helpers

    private static func map(status: GSSK_Status, message: String) -> GSSKError {
        switch status {
        case GSSK_ERR_INVALID_JSON:      return .invalidJSON
        case GSSK_ERR_MALLOC_FAILED:     return .mallocFailed
        case GSSK_ERR_SCHEMA_VIOLATION:  return .schemaViolation(message)
        case GSSK_ERR_DIVERGENCE:        return .divergence
        case GSSK_WARN_SOLVER_DIVERGENCE: return .solverDivergence
        case GSSK_ERR_NOT_FOUND:         return .notFound
        default:                         return .unknown
        }
    }
}
