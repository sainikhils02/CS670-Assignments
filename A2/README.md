# gen_queries: Distributed Point Function (DPF)

This project provides a C++ implementation of a Distributed Point Function (DPF) using a binary-tree construction with correction words and XOR sharing. It includes a `generator` and a `Evalfull` to verify correctness end-to-end.

The implementation uses a cryptographic PRG (AES-128 in CTR mode via OpenSSL EVP) with 256-bit node seeds (first 128 bits as the AES key). At each level, we keep a single 256-bit seed correction `cw_seed[ℓ]` and two 1-bit corrections `cw_tL[ℓ]` and `cw_tR[ℓ]`. The two least significant bits of seeds are cleared before PRG expansion and on child seeds, mirroring the semantics in `dpf.h`.

The program `gen_queries` generates random DPF instances with a random location and value, creates two keys, and verifies that the XOR of evaluations equals the desired point function.

## Quick start

Build (C++17, link with OpenSSL):

```
g++ -std=c++17 -O2 -DNDEBUG -o gen_queries gen_queries.cpp -lssl -lcrypto
```

Notes:
- On Ubuntu/WSL you may need to install OpenSSL dev headers/libraries (optional):
	- sudo apt-get update
	- sudo apt-get install -y libssl-dev
- If you are building on other platforms, ensure OpenSSL is installed and visible to your compiler/linker, and adjust include/library paths as needed.

Run:

```
./gen_queries <DPF_size> <num_DPFs> [--print-evals]
```

Examples:

```
./gen_queries 1024 10
```

Outputs a pass/fail per instance and previews of each key’s root and correction words (a 64-bit preview of each 256-bit value for readability).

To print both parties’ evaluations at every index (v0, v1, and their XOR), add the optional flag:

```
./gen_queries 32 1 --print-evals
```

Note: The per-index printout is O(N) lines. Use a small domain (e.g., 32 or 64) when enabling this flag.

## What is a DPF?

A point function over a domain [N] is a function f such that:

- f(i) = v for exactly one index i = α (the location), and
- f(j) = 0 for all j ≠ α.

A 2-party DPF shares f into two keys k0 and k1 such that, for any index x,

- Eval(k0, x) ⊕ Eval(k1, x) = f(x).

Each key alone reveals nothing about α or v (in a cryptographic construction). The “⊕” denotes bitwise XOR over 64-bit words in this implementation.

## High-level design

We implement a binary-tree DPF with a single seed correction per level, per-direction bit corrections, and a final output correction. Evaluation runs in O(log N) time; keys have O(log N) size.

Key components:

- Pseudorandom Generator (PRG): Deterministically expands a 256-bit seed into two 256-bit child seeds and control bits at each level. We use AES-128 in CTR mode via OpenSSL's EVP API. Domain separation is provided via distinct nonces/counters for child seeds, control bits, and leaf outputs. The two least significant bits of seeds are cleared before/after PRG.
- Correction words: At each level we store one seed correction `cw_seed[ℓ]` and two bit masks `cw_tL[ℓ]`, `cw_tR[ℓ]`. These ensure the XOR of the two evaluations equals the point function while individual keys reveal nothing.
- Final correction: A 64-bit value `cw_out` xored into a party’s output only when its final control bit is 1. Here `cw_out = v ⊕ PRG_out(s0_α) ⊕ PRG_out(s1_α)` where `s*_α` are the leaf seeds reached along the α-path during key generation.

## Math and algorithmic details

Notation:

- Domain size N = 2^d (d = depth). Indices are represented as d-bit strings, MSB first.
- At each level ℓ ∈ {0,…,d−1}, a node labeled by seed s has two children: left and right. We define `(s_L, t_L) = PRG_L(clear_lsb(s))`, `(s_R, t_R) = PRG_R(clear_lsb(s))`, with seeds 256-bit and `t_* ∈ {0,1}`.
- Each party i ∈ {0,1} maintains its own seed `s_i` and control bit `t_i` during generation and evaluation.

Key generation:

1. Sample independent roots: s0, s1 uniformly at random. Set t0 = lsb(s0), t1 = t0 ⊕ 1, ensuring `t0 ⊕ t1 = 1` initially.
2. For each level ℓ:
	- Expand both parties: `(s0L, t0L; s0R, t0R) ← PRG(clear_lsb(s0))`, `(s1L, t1L; s1R, t1R) ← PRG(clear_lsb(s1))` and clear_lsbs on children.
	- Let a = α[ℓ] (MSB first), keep = a, lose = 1 ⊕ keep.
	- Compute bit masks: `cw_tL[ℓ] = t0L ⊕ t1L ⊕ a ⊕ 1`, `cw_tR[ℓ] = t0R ⊕ t1R ⊕ a`.
	- Seed correction: `cw_seed[ℓ] = (lose == L) ? s0L ⊕ s1L : s0R ⊕ s1R`.
	- Advance each party i on keep:
		- Let `(child, τ)` be the keep child `(s*i, t*i)`.
		- If previous `t_i == 0`, set `child ⊕= cw_seed[ℓ]`. Clear lsbs of child.
		- Update `t_i = τ ⊕ (t_i & (keep ? cw_tR[ℓ] : cw_tL[ℓ]))` and set `s_i = child`.
3. Finalizer: `cw_out = v ⊕ PRG_out(s0_α) ⊕ PRG_out(s1_α)` using the leaf seeds after step 2.

Evaluation:

Given key `(rootSeed, rootT, cw_seed[], cw_tL[], cw_tR[], cw_out)` and index x:

1. Initialize `(s, t) = (rootSeed, rootT)`.
2. For each level ℓ from 0..d−1:
	- Expand with  semantics: `(sL, tL; sR, tR) = PRG(clear_lsb(s))` and clear_lsbs on children.
	- Let b = x[ℓ] (MSB first), choose `(child, τ) = (b ? sR : sL, b ? tR : tL)`.
	- Update `t ← τ ⊕ (t & (b ? cw_tR[ℓ] : cw_tL[ℓ]))`.
	- If previous t was 0, set `child ⊕= cw_seed[ℓ]`. Clear lsbs; set `s ← child`.
3. Let `y = PRG_out(s)`. If `t = 1`, set `y ⊕= cw_out`.

Reconstruction:

For any index x, Eval(k0, x) ⊕ Eval(k1, x) equals:

- v if x = α, because along the α path the control bits ensure exactly one party applies cw_out at the leaf; and
- 0 otherwise, as the two streams cancel level-by-level due to the per-child corrections.


## Implementation mapping (gen_queries.cpp)

- PRG: AES-128-CTR-based via OpenSSL EVP with domain-separated nonces and counters (see AES helpers, `prg_expand_full`, and `prg_leaf_output`).  wrappers `_clear_lsbs` and `prg_expand_` enforce the dpf.h semantics.
- Data structures:
	- `Seed256`: 256-bit seed wrapper (8x32-bit words) with XOR helpers and debug preview.
	- `DPFKey`: `rootSeed`, `rootT`, per-level `cw_seed`, `cw_tL`, `cw_tR`, and `cw_out`, plus `size` and `depth`.
- Generation: `generateDPF_(size, location, value, rng)` implements the  algorithm above.
- Evaluation: `evalDPF_(key, index)` traverses the tree using  corrections.
- Verification: `EvalFull_(keys)` evaluates both keys on all indices, XORs outputs, and checks equality with the point function (v at α, 0 elsewhere).

### API

- `generateDPF_(size_t size, size_t location, uint64_t value, std::mt19937_64 &rng) -> DPFKeys`
	- Inputs: domain size (power of two), location α, target value v, RNG for seeding.
	- Output: Pair of keys (k0, k1) and metadata in `DPFKeys`.
- `evalDPF_(const DPFKey &key, size_t index) -> uint64_t`
	- Input: A key and index x.
	- Output: Share y s.t. y ⊕ y' = f(x) when combined with the other key's share.
- `EvalFull_(const DPFKeys &keys, bool verbose=false) -> bool`
	- Runs evaluation across all indices and checks reconstruction equals the point function.
	- Prints mismatches when `verbose=true`.

## Complexity

- Key size: O(log N) words (one seed correction + two bit corrections per level, plus root seed/bit and final correction).
- Evaluation time: O(log N) PRG expansions per Eval.
- Generation time: O(log N) operations.

## Correctness and testing

- The program randomly picks α and v, generates keys, runs `EvalFull_`, and prints “Test Passed/Failed”.
- For inspection, it prints a short preview of per-level correction words and `cw_out`.

## Domain and types

- Domain size N must be a power of two: N = 2^d.
- Output values are 64-bit words; all operations are XOR over 64-bit words.

## Security considerations

- This code uses OpenSSL's AES-128-CTR via the EVP API as the PRG for seeds, control bits, and leaf outputs. Domain separation is enforced via distinct nonces/counters per purpose (left/right/bits/output).
