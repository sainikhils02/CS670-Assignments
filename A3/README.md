# CS670 A3 Secure Item Update Workflow

This directory contains a Docker workflow that:

1. `gen_queries` samples additive shares of user vector `U` and item vector `V`, then creates per-party query files containing `(i, k_b)` where `k_b` is a DPF key share for the accessed item index `j`.
2. Parties `P0` and `P1` run the secure protocol with help from dealer `P2` to evaluate each query, compute the update value
   \[ M = u_i (1 - \langle u_i, v_j \rangle) \]
   and privately apply it to their shares of `V` using the FCW-adjustment.

## Layout

```
A3/
├── A3.md                  # Assignment handout (given)
├── Dockerfile             # Builds gen/p0/p1/p2 images
├── docker-compose.yml     # Runs the 3-party workflow
├── common.hpp             # Field ops, share utilities, MPC helpers
├── dpf.hpp                # AES-CTR based DPF implementation + serialization
├── gen_queries.cpp        # Query/share generator
├── p2.cpp                 # Trusted dealer supplying preprocessing
├── server.cpp             # Unified source for P0/P1 roles
└── data/                  # Runtime inputs/outputs (created automatically)
```

## Usage

Inside this directory:

```bash
# Build containers and run the full workflow
docker compose up --build
```

The default compose file generates data for 16 users/items and 6 queries. Adjust the command in `docker-compose.yml` to change sizes (the number of items must be a power of two for the DPF domain).

After a run you will find:
- `data/U{0,1}_shares.txt` and `data/V{0,1}_shares.txt`: initial additive shares.
- `data/V{0,1}_shares_updated.txt`: updated item shares after serving all queries.
- `data/queries_p{0,1}.txt`: party-specific query streams (human-readable) containing serialized DPF keys.

Logs from `p0`/`p1` show per-query progress. To rerun, clean `data/` or simply invoke `docker compose up --build` again to regenerate fresh shares and queries.

## Code flow

1. **Share & query generation (`gen_queries.cpp`)**
   - Samples random field elements for cleartext `U`/`V`, splits them into additive shares, and writes them to `data/U{0,1}_shares.txt` and `data/V{0,1}_shares.txt`.
   - Emits `data/queries_p{0,1}.txt` with `(user_id, DPF key)` pairs. Each query targets a random user/item pair; DPF keys encode the selection vector for the item domain.
   - Writes `data/config.txt` capturing `(num_users, num_items, num_queries)` so every role runs with consistent dimensions.
2. **Dealer preprocessing (`p2.cpp`)**
   - Waits for two clients, identifies P0/P1 via the role handshake, and then serves on-demand preprocessing bundles per dot-product dimension.
   - Each bundle provides correlated randomness `(X_b, Y_b, alpha)` so the online protocol can evaluate secure dot/multiply calls without extra interaction rounds.
3. **Online protocol (`server.cpp`)**
   - Loads local shares from disk, reads its party-specific queries, and establishes sockets to P2 and the peer.
   - For each query: evaluates the DPF key locally, converts the XOR-shared indicator vector into additive shares, computes `v_j` via secure dot product against `V`, evaluates `⟨u_i, v_j⟩`, derives `M = u_i · (1 - ⟨u_i, v_j⟩)`, and securely scales the indicator vector so that only item `j` receives the update.
   - Updated shares are written back to `data/V{0,1}_shares_updated.txt` at the end of the batch.

## Customizable parameters

| Parameter | Where to change | Notes |
| --- | --- | --- |
| `NUM_USERS`, `NUM_ITEMS`, `NUM_QUERIES` | `docker-compose.yml` command for `gen` service (`./gen_queries <users> <items> <queries>`) | `NUM_ITEMS` must be a power of two for DPF. Larger sizes increase DPF depth and runtime proportionally. |
## How to run

1. **One-time setup**
   ```bash
   docker compose up --build
   ```
   Builds all role images, generates fresh shares/queries, and runs gen + P2 + P1 + P0 in one shot.
2. **Rerun with existing data**
   ```bash
   docker compose up
   ```
   Skips rebuilding; `gen_queries` still runs first and regenerates data unless you comment it out in the compose file.
3. **Regenerate everything from scratch**
   ```bash
   rm -rf data
   docker compose up --build
   ```
   Ensures no cached shares remain (useful when comparing different parameter sets).
4. **Inspect logs**
   - `docker compose logs p0` or `p1` shows per-query progress and any warnings (e.g., unexpected DPF domain mismatches).
   - `docker compose logs p2` helps debug preprocessing traffic or connection issues.
5. **Customize parameters**
   - Edit the `gen` service command in `docker-compose.yml` (e.g., `./gen_queries 64 32 20`).
   - Rebuild (`docker compose up --build`) to propagate the new config through all services.

