import CGSSK
import Foundation

// MARK: - Error type

/// Swift representation of GSSK_Status error codes.
public enum GSSKError: Error, LocalizedError {
    case invalidJSON
    case mallocFailed
    case schemaViolation(String)
    case divergence
    case unknown
    case noInstance

    public var errorDescription: String? {
        switch self {
        case .invalidJSON:              return "GSSK: Invalid JSON input."
        case .mallocFailed:             return "GSSK: Memory allocation failed."
        case .schemaViolation(let msg): return "GSSK: Schema violation — \(msg)"
        case .divergence:               return "GSSK: Numerical divergence (NaN/Inf detected)."
        case .unknown:                  return "GSSK: Unknown error."
        case .noInstance:               return "GSSK: No active instance."
        }
    }
}

// MARK: - Simulator

/// A high-level Swift wrapper around the GSSK ODE simulation kernel.
///
/// Manages the lifecycle of a `GSSK_Instance` (init/free) automatically via
/// `init`/`deinit`. All C pointer casting is handled internally.
///
/// Usage:
/// ```swift
/// let sim = try GSSKSimulator(json: modelJSON)
/// let results = try sim.run()
/// ```
public final class GSSKSimulator {

    // MARK: - Private storage

    // GSSK_Instance is an incomplete C struct (opaque pointer in Swift).
    // We store it as OpaquePointer and cast it to the C function signatures
    // using a typealias for clarity.
    private typealias InstPtr = OpaquePointer

    private var instPtr: InstPtr?

    // MARK: - Public properties

    /// Number of storage nodes in the model.
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

    /// Initialise the simulator from a JSON model string.
    /// - Parameter json: The GSSK model topology JSON.
    /// - Throws: `GSSKError` describing any parse or schema failure.
    public init(json: String) throws {
        var rawPtr: InstPtr? = nil

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

    /// Reset simulation to initial conditions.
    public func reset() {
        guard let p = instPtr else { return }
        GSSK_Reset(p)
        currentTime = startTime
    }

    /// Read the current state vector (Q values for all storage nodes).
    public func state() -> [Double] {
        guard let p = instPtr,
              let ptr = GSSK_GetState(p) else { return [] }
        return Array(UnsafeBufferPointer(start: ptr, count: stateSize))
    }

    // MARK: - Node access

    /// Return the ID string for the node at `index`.
    public func nodeID(at index: Int) -> String? {
        guard let p = instPtr,
              let cStr = GSSK_GetNodeID(p, index) else { return nil }
        return String(cString: cStr)
    }

    /// Find the index of a node by its string ID. Returns `nil` if not found.
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
