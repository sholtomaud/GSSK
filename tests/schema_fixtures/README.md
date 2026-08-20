# Schema fixtures

Models that exist only to keep `gssk.schema.json` honest about corners the
`examples/` never reach. They are validated by `make test-schema` and loaded by
`bin/dump_serialized`, so each one must be both schema-valid and accepted by
`GSSK_Init` — a fixture that stops loading is as much a failure as one that
stops validating.

They are not documentation. Anything meant to be read as an example of how to
model something belongs in `examples/`.
