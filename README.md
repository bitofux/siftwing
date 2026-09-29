# Siftwing

Siftwing is a C++17/Linux document search service being built as a staged, evidence-driven engineering project.

The planned path covers deterministic offline indexing and querying, followed by a real Main-Sub Reactor network service, then reliability, observability, performance, and later distributed evolution. Each stage must remain buildable, testable, debuggable, and explainable before the next stage begins.

## Project status

The repository is currently at the engineering-bootstrap milestone. Search, indexing, protocol, and Reactor capabilities are planned but are not implemented yet.

## Engineering principles

- Prefer correctness and explicit ownership over premature optimization.
- Keep public contracts, failure behavior, and thread boundaries testable.
- Separate verified engineering status from learning status.
- Publish only reproducible build, test, debugging, and performance evidence.

## License

Siftwing is licensed under the MIT License. See [LICENSE](LICENSE).
