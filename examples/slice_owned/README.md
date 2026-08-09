# Slice, `def`, and owned allocation

This example exercises the first semantic foundations added to Margo:

- `def SAMPLE_COUNT = 4` is lowered to a C enum constant, not a textual macro.
- `slice(T)` has a stable C ABI representation: `data`, `len`, and `elem_size`.
  `slice_at(T, value, index)` checks both the element type size and bounds.
- `owned_new(T)` allocates through Margo's libttak-backed allocator. Its result
  is recognized by the transpiler and released automatically at scope exit.

Build and run:

```bash
make
./slice-owned
```

Expected output: `sum=10`.
