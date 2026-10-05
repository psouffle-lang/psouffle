# Security

## Source References

- [../README.md:1](../README.md#L1): branch scope.
- [../.gitignore:2](../.gitignore#L2): ignored local outputs.

## Scope

This branch is not a production distribution.

## Data Handling

- Do not commit private datasets, generated outputs, DOT/JSON dumps, or timing logs.
- Treat fact files, probability files, and JSON logs as experiment data.
- Keep large or machine-local runs outside the repository checkout.

## Dependency Hygiene

- The build depends on the system C++ toolchain, CMake, Flex/Bison,
  Python 3, Readline, and CUDD.
- Pin dependency versions in release notes when preparing an archive.
