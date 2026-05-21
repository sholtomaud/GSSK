// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "GSSK",
    platforms: [
        .macOS(.v13),
        .iOS(.v16),
    ],
    products: [
        // The raw C library — import as `import CGSSK` in Swift.
        .library(
            name: "CGSSK",
            targets: ["CGSSK"]
        ),
        // Higher-level Swift wrapper — import as `import GSSK` in Swift.
        .library(
            name: "GSSK",
            targets: ["GSSK"]
        ),
    ],
    targets: [
        // MARK: - C Library Target
        // SPM requires publicHeadersPath to be within the target's `path`,
        // so we root the target at the repo root and enumerate sources explicitly.
        // main.c is excluded — it contains the CLI entry point.
        .target(
            name: "CGSSK",
            path: ".",
            exclude: [
                // Directories SPM must not traverse
                "Sources",
                "tests",
                "examples",
                "docs",
                "web",
                "assets",
                "scripts",
                "dist",
                "bin",
                "lib",
                ".cache",
                ".github",
                // Individual files not part of the library
                "src/main.c",
                "src/gssk.d.ts",
            ],
            sources: [
                "src/gssk.c",
                "src/advanced.c",
                "src/cJSON.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .define("_POSIX_C_SOURCE", to: "200809L"),
            ]
        ),

        // MARK: - Swift Wrapper Target
        // A thin idiomatic Swift layer over the C API.
        .target(
            name: "GSSK",
            dependencies: ["CGSSK"],
            path: "Sources/GSSK"
        ),

        // MARK: - Tests
        .testTarget(
            name: "GSSKTests",
            dependencies: ["GSSK"],
            path: "Sources/GSSKTests"
        ),
    ]
)
